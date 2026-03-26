#!/bin/bash
# GHRA Motor Control - Start everything
# Usage: ./start.sh

set -e
cd "$(dirname "$0")/docker"

echo "=== GHRA Motor Control ==="

# Stop any existing containers
echo "Stopping old containers..."
podman-compose down 2>/dev/null || true

# Build and start containers
echo "Building containers..."
podman-compose build --quiet

echo "Starting Mosquitto + ROS2 + Dashboard..."
podman-compose up -d

# Wait for services
echo "Waiting for services..."
sleep 3

# Check health
echo ""
echo "--- Container Status ---"
podman ps --format "table {{.Names}}\t{{.Status}}" | grep ghra

# Check MQTT broker
if timeout 2 bash -c "echo > /dev/tcp/127.0.0.1/1883" 2>/dev/null; then
    echo "MQTT broker:  OK (port 1883)"
else
    echo "MQTT broker:  FAILED"
fi

# Check dashboard
if timeout 2 bash -c "echo > /dev/tcp/127.0.0.1/8443" 2>/dev/null; then
    echo "Dashboard:    OK (port 8443)"
else
    echo "Dashboard:    FAILED"
fi

echo ""
echo "--- Dashboard ---"
echo "https://192.168.1.100:8443"
echo ""

# Open browser if available
if command -v xdg-open &>/dev/null; then
    xdg-open "https://192.168.1.100:8443" 2>/dev/null &
elif command -v firefox &>/dev/null; then
    firefox "https://192.168.1.100:8443" 2>/dev/null &
fi

echo "Done. ESP32 connects automatically via MQTT."
