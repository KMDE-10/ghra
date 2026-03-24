#!/bin/bash
set -e

source /opt/ros/humble/setup.bash
source /ros2_ws/install/setup.bash

# Enable SROS2 if keystore is mounted
if [ -d "/security/keystore" ]; then
    export ROS_SECURITY_KEYSTORE=/security/keystore
    export ROS_SECURITY_ENABLE=true
    export ROS_SECURITY_STRATEGY=Enforce
    echo "SROS2 security ENABLED (Enforce mode)"
else
    echo "WARNING: No keystore found at /security/keystore"
    echo "Running WITHOUT DDS security. To enable:"
    echo "  1. Run: generate_sros2_keys.sh /security/keystore"
    echo "  2. Mount keystore volume in docker-compose.yml"
fi

# Launch rosbridge websocket server and the control node
ros2 launch ghra_control ghra_launch.py
