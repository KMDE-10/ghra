from launch import LaunchDescription
from launch_ros.actions import Node


def generate_launch_description():
    return LaunchDescription([
        # Rosbridge WebSocket server (port 9090) — kept for ROS2 tooling
        Node(
            package='rosbridge_server',
            executable='rosbridge_websocket',
            name='rosbridge_websocket',
            parameters=[{
                'port': 9090,
                'unregister_timeout': 86400.0,
            }],
            arguments=['--ros-args', '--enclave', '/rosbridge_websocket'],
        ),
        # GHRA control node
        Node(
            package='ghra_control',
            executable='control_node',
            name='ghra_control_node',
            parameters=[{
                'speed_fast': 1.0,
                'speed_slow': 0.3,
                'position_min_mm': 100.0,
                'position_max_mm': 15000.0,
            }],
            arguments=['--ros-args', '--enclave', '/ghra_control_node'],
        ),
        # MQTT <-> ROS2 bridge
        Node(
            package='ghra_control',
            executable='mqtt_bridge',
            name='mqtt_bridge',
        ),
    ])
