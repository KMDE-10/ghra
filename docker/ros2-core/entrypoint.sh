#!/bin/bash
set -e

source /opt/ros/humble/setup.bash
source /ros2_ws/install/setup.bash

# Force UDP-only DDS transport (no shared memory) for cross-container discovery
export FASTRTPS_DEFAULT_PROFILES_FILE=/tmp/fastdds_udp.xml
cat > /tmp/fastdds_udp.xml << 'XMLEOF'
<?xml version="1.0" encoding="UTF-8" ?>
<profiles xmlns="http://www.eprosima.com/XMLSchemas/fastRTPS_Profiles">
    <transport_descriptors>
        <transport_descriptor>
            <transport_id>udp_transport</transport_id>
            <type>UDPv4</type>
        </transport_descriptor>
    </transport_descriptors>
    <participant profile_name="default_participant" is_default_profile="true">
        <rtps>
            <useBuiltinTransports>false</useBuiltinTransports>
            <userTransports>
                <transport_id>udp_transport</transport_id>
            </userTransports>
        </rtps>
    </participant>
</profiles>
XMLEOF

# SROS2 disabled until agent has matching security keys
# To re-enable: set ROS_SECURITY_ENABLE=true and ROS_SECURITY_STRATEGY=Enforce
# after configuring the micro-ros-agent with matching SROS2 keys
export ROS_SECURITY_ENABLE=false
echo "SROS2 security DISABLED (agent has no security keys yet)"

# Launch rosbridge websocket server and the control node
ros2 launch ghra_control ghra_launch.py
