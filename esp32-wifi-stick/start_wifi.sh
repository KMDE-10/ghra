#!/bin/bash
# ESP32-S3 WiFi Adapter — Quick start script
# Usage: sudo ./start_wifi.sh
set -e

SSID="${1:-Secondary Tiphone}"
PASS="${2:-ganzgeheim}"
PORT="${3:-/dev/ttyACM0}"
BAUD=115200

echo "[*] Starting ESP32-S3 WiFi adapter..."
echo "[*] SSID: $SSID"
echo "[*] Port: $PORT"

# Wait for ESP32 to be ready
sleep 6

exec python3 /home/control/esp32-wifi-stick/setup_wifi.py \
    --ssid "$SSID" --pass "$PASS" -p "$PORT"
