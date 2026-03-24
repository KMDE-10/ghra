#!/bin/bash
set -e

source /opt/ros/humble/setup.bash
source /ros2_ws/install/setup.bash

# Launch rosbridge websocket server and the control node
ros2 launch ghra_control ghra_launch.py
