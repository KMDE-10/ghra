import rclpy
from rclpy.node import Node
from std_msgs.msg import Float32, Int8, Bool

# Button event encoding from ESP32 #2 remote
BUTTON_FWD_FAST = 1
BUTTON_FWD_SLOW = 2
BUTTON_BWD_SLOW = 3
BUTTON_BWD_FAST = 4

# Speed presets (0.0 - 1.0)
SPEED_FAST = 1.0
SPEED_SLOW = 0.3

# Direction encoding
DIR_FORWARD = 1
DIR_REVERSE = -1
DIR_STOP = 0


class ControlNode(Node):
    def __init__(self):
        super().__init__('ghra_control_node')

        # Parameters
        self.declare_parameter('speed_fast', SPEED_FAST)
        self.declare_parameter('speed_slow', SPEED_SLOW)
        self.declare_parameter('position_min_mm', 100.0)
        self.declare_parameter('position_max_mm', 15000.0)

        # State
        self.current_position = 0.0
        self.enabled = True

        # Subscribers
        self.button_sub = self.create_subscription(
            Int8, '/remote/button_event', self.on_button_event, 10)
        self.position_sub = self.create_subscription(
            Float32, '/carriage/position', self.on_position, 10)
        self.enable_sub = self.create_subscription(
            Bool, '/motor/enable', self.on_enable, 10)

        # Publishers
        self.speed_pub = self.create_publisher(Float32, '/motor/speed_cmd', 10)
        self.direction_pub = self.create_publisher(Int8, '/motor/direction', 10)

        self.get_logger().info('GHRA Control Node started')

    def on_button_event(self, msg: Int8):
        button = msg.data

        # Negative value = button released -> stop
        if button < 0:
            self.publish_command(0.0, DIR_STOP)
            return

        if not self.enabled:
            self.get_logger().warn('Motor disabled, ignoring button press')
            return

        speed_fast = self.get_parameter('speed_fast').value
        speed_slow = self.get_parameter('speed_slow').value
        pos_min = self.get_parameter('position_min_mm').value
        pos_max = self.get_parameter('position_max_mm').value

        if button == BUTTON_FWD_FAST:
            if self.current_position < pos_max:
                self.publish_command(speed_fast, DIR_FORWARD)
            else:
                self.get_logger().warn('Forward limit reached')
                self.publish_command(0.0, DIR_STOP)

        elif button == BUTTON_FWD_SLOW:
            if self.current_position < pos_max:
                self.publish_command(speed_slow, DIR_FORWARD)
            else:
                self.get_logger().warn('Forward limit reached')
                self.publish_command(0.0, DIR_STOP)

        elif button == BUTTON_BWD_SLOW:
            if self.current_position > pos_min:
                self.publish_command(speed_slow, DIR_REVERSE)
            else:
                self.get_logger().warn('Backward limit reached')
                self.publish_command(0.0, DIR_STOP)

        elif button == BUTTON_BWD_FAST:
            if self.current_position > pos_min:
                self.publish_command(speed_fast, DIR_REVERSE)
            else:
                self.get_logger().warn('Backward limit reached')
                self.publish_command(0.0, DIR_STOP)

    def on_position(self, msg: Float32):
        self.current_position = msg.data

        # Safety: stop if out of bounds
        pos_min = self.get_parameter('position_min_mm').value
        pos_max = self.get_parameter('position_max_mm').value
        if self.current_position <= pos_min or self.current_position >= pos_max:
            self.publish_command(0.0, DIR_STOP)

    def on_enable(self, msg: Bool):
        self.enabled = msg.data
        if not self.enabled:
            self.publish_command(0.0, DIR_STOP)
            self.get_logger().info('Motor disabled')
        else:
            self.get_logger().info('Motor enabled')

    def publish_command(self, speed: float, direction: int):
        speed_msg = Float32()
        speed_msg.data = speed
        self.speed_pub.publish(speed_msg)

        dir_msg = Int8()
        dir_msg.data = direction
        self.direction_pub.publish(dir_msg)


def main(args=None):
    rclpy.init(args=args)
    node = ControlNode()
    rclpy.spin(node)
    node.destroy_node()
    rclpy.shutdown()


if __name__ == '__main__':
    main()
