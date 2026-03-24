FROM ros:humble-ros-base

# Install rosbridge suite and Python dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    ros-humble-rosbridge-suite \
    python3-pip \
    && rm -rf /var/lib/apt/lists/*

# Copy the control node package
COPY ghra_control /ros2_ws/src/ghra_control

# Build the ROS2 workspace
WORKDIR /ros2_ws
RUN . /opt/ros/humble/setup.sh && \
    colcon build --packages-select ghra_control

# Entrypoint: source workspace and launch both rosbridge + control node
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENTRYPOINT ["/entrypoint.sh"]
