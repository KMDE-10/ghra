#!/bin/bash
# GHRA Motor Control - Start everything
#
# Usage:
#   ./start.sh             — boot/idempotent path (no rebuild, no teardown, no internet needed)
#   ./start.sh --rebuild   — full down + build + up (REQUIRES internet for image build)
#
# The default path is what systemd runs at boot. It assumes the container images
# are already built locally (localhost/docker_ros2-core:latest, ...-web-dashboard:latest)
# and just brings them up if not running. Safe to re-run on a healthy stack — no-op.

export PATH="$HOME/.local/bin:$PATH"
cd "$(dirname "$0")/docker"

if [[ "$1" == "--rebuild" ]]; then
    echo "=== GHRA: REBUILD (internet required) ==="
    podman-compose down 2>/dev/null || true
    if ! podman-compose build; then
        echo "Build FAILED. Stack is now down. Fix internet/Dockerfiles and retry."
        exit 1
    fi
fi

echo "=== GHRA: starting (idempotent, offline-safe) ==="

# podman-compose 1.4.1 unconditionally recreates containers on `up`, which
# breaks idempotence (errors with "name already in use" on re-runs).
# So we manage state directly: invoke `up` only when a container is missing,
# otherwise just `podman start` any that aren't running.

expected=(ghra-mosquitto ghra-ros2-core ghra-web-dashboard)

missing=()
for c in "${expected[@]}"; do
    podman container exists "$c" 2>/dev/null || missing+=("$c")
done

if [[ ${#missing[@]} -gt 0 ]]; then
    echo "Containers missing (${missing[*]}) — creating via podman-compose up..."
    if ! podman-compose up -d --no-build; then
        echo "podman-compose up FAILED."
        exit 1
    fi
else
    for c in "${expected[@]}"; do
        state=$(podman inspect -f '{{.State.Status}}' "$c" 2>/dev/null)
        if [[ "$state" != "running" ]]; then
            echo "Starting $c (was $state)..."
            podman start "$c" >/dev/null || { echo "Failed to start $c"; exit 1; }
        fi
    done
fi

sleep 3

echo
echo "--- Container Status ---"
podman ps --format "table {{.Names}}\t{{.Status}}" | grep ghra

echo
echo "--- Service Health ---"
for spec in "MQTT-broker  127.0.0.1 1883" \
            "MQTT-WS      127.0.0.1 9001" \
            "ROSbridge    127.0.0.1 9090" \
            "Dashboard    127.0.0.1 8443"; do
    set -- $spec; name=$1; ip=$2; port=$3
    if timeout 2 bash -c "echo > /dev/tcp/$ip/$port" 2>/dev/null; then
        printf "  %-12s OK   (%s:%s)\n" "$name" "$ip" "$port"
    else
        printf "  %-12s FAIL (%s:%s)\n" "$name" "$ip" "$port"
    fi
done

echo
echo "Dashboard: http://192.168.1.100:8443"
echo "Done. ESPs reconnect automatically via MQTT."
