"""
MQTT <-> ROS2 bridge for GHRA motor control.
Bridges MQTT topics (from ESP32/dashboard) to ROS2 topics (for control_node).
"""

import os
import rclpy
from rclpy.node import Node
from std_msgs.msg import Float32, Int8, Bool
import paho.mqtt.client as mqtt


class MqttBridge(Node):
    def __init__(self):
        super().__init__('mqtt_bridge')

        broker = os.environ.get('MQTT_BROKER', '127.0.0.1')
        port = int(os.environ.get('MQTT_PORT', '1883'))

        # ROS2 publishers (MQTT -> ROS2)
        self.speed_cmd_pub = self.create_publisher(Float32, '/motor/speed_cmd', 10)
        self.direction_pub = self.create_publisher(Int8, '/motor/direction', 10)
        self.enable_pub = self.create_publisher(Bool, '/motor/enable', 10)

        # ROS2 subscribers (ROS2 -> MQTT)
        self.speed_fb_sub = self.create_subscription(
            Float32, '/motor/speed_feedback', self.on_speed_feedback, 10)
        self.position_sub = self.create_subscription(
            Float32, '/carriage/position', self.on_position, 10)

        # MQTT client
        self.mqtt = mqtt.Client(client_id='ros2_bridge', protocol=mqtt.MQTTv311)
        self.mqtt.on_connect = self.on_mqtt_connect
        self.mqtt.on_message = self.on_mqtt_message
        self.mqtt.connect_async(broker, port)
        self.mqtt.loop_start()

        self.get_logger().info(f'MQTT bridge started (broker: {broker}:{port})')

    def on_mqtt_connect(self, client, userdata, flags, rc):
        self.get_logger().info(f'MQTT connected (rc={rc})')
        client.subscribe('motor/speed_cmd', qos=0)
        client.subscribe('motor/direction', qos=0)
        client.subscribe('motor/enable', qos=0)

    def on_mqtt_message(self, client, userdata, msg):
        try:
            payload = msg.payload.decode()
            if msg.topic == 'motor/speed_cmd':
                ros_msg = Float32()
                ros_msg.data = float(payload)
                self.speed_cmd_pub.publish(ros_msg)
            elif msg.topic == 'motor/direction':
                ros_msg = Int8()
                ros_msg.data = int(payload)
                self.direction_pub.publish(ros_msg)
            elif msg.topic == 'motor/enable':
                ros_msg = Bool()
                ros_msg.data = payload == '1'
                self.enable_pub.publish(ros_msg)
        except Exception as e:
            self.get_logger().error(f'MQTT message error: {e}')

    def on_speed_feedback(self, msg):
        self.mqtt.publish('motor/speed_feedback', f'{msg.data:.2f}', qos=0)

    def on_position(self, msg):
        self.mqtt.publish('carriage/position', f'{msg.data:.1f}', qos=0)

    def destroy_node(self):
        self.mqtt.loop_stop()
        self.mqtt.disconnect()
        super().destroy_node()


def main(args=None):
    rclpy.init(args=args)
    node = MqttBridge()
    rclpy.spin(node)
    node.destroy_node()
    rclpy.shutdown()


if __name__ == '__main__':
    main()
