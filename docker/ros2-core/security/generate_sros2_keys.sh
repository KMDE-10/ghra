#!/bin/bash
# Generate SROS2 security keys for all ROS2 nodes
# Run this ONCE before first deployment, then distribute keys
#
# Usage: ./generate_sros2_keys.sh [output_dir]
#
# The generated keystore must be mounted into every container/device
# that participates in the ROS2 DDS network.

set -e

KEYSTORE_DIR="${1:-./keystore}"

echo "=== SROS2 Key Generation ==="
echo "Output: $KEYSTORE_DIR"
echo ""

# Source ROS2
source /opt/ros/humble/setup.bash

# Create keystore
ros2 security create_keystore "$KEYSTORE_DIR"
echo "Keystore created."

# Generate keys for each node in the system
NODES=(
    "/ghra_control_node"
    "/ghra_motor_esp32"
    "/ghra_remote_esp32"
    "/rosbridge_websocket"
)

for node in "${NODES[@]}"; do
    echo "Generating keys for: $node"
    ros2 security create_enclave "$KEYSTORE_DIR" "$node"
done

echo ""
echo "=== Key generation complete ==="
echo ""
echo "Keystore contents:"
find "$KEYSTORE_DIR" -type f | sort
echo ""
echo "Next steps:"
echo "  1. Copy keystore to Dell OptiPlex: docker/ros2-core/security/keystore/"
echo "  2. Copy node-specific keys to each ESP32 firmware (for micro-ROS SROS2)"
echo "  3. Set ROS_SECURITY_ENABLE=true in docker-compose.yml (already configured)"
echo ""
echo "IMPORTANT: Keep the CA private key safe. Do NOT commit keystore/ to git."
