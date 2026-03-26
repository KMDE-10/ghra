/*
 * GHRA Motor Controller - ESP32 #1
 *
 * Interfaces:
 *   - W5500 Ethernet (SPI) → micro-ROS agent on Dell OptiPlex
 *   - GP8211S DAC (I2C, 0x58) → 0-10V to VFD AVI input
 *   - Laser distance sensor (UART2, 19200 baud) → carriage position
 *   - 2x Relays (GPIO) → VFD FWD/REV terminals
 *
 * ROS2 Topics:
 *   Subscribes: /motor/speed_cmd, /motor/direction, /motor/enable
 *   Publishes:  /carriage/position, /motor/speed_feedback
 */

#include <SPI.h>
#include <ETH.h>
#include <NetworkInterface.h>
#include <Wire.h>
#include <DFRobot_GP8XXX.h>

#include <micro_ros_arduino.h>
#include <rcl/rcl.h>
#include <rclc/rclc.h>
#include <rclc/executor.h>
#include <std_msgs/msg/float32.h>
#include <std_msgs/msg/int8.h>
#include <std_msgs/msg/bool.h>

// ── Pin Definitions ──

#define ETH_CS_PIN    5
#define ETH_RST_PIN   4
#define ETH_SCLK_PIN  18
#define ETH_MISO_PIN  19
#define ETH_MOSI_PIN  23

#define DAC_SDA_PIN   21
#define DAC_SCL_PIN   22

#define LASER_RX_PIN  16
#define LASER_TX_PIN  17

#define RELAY_FWD_PIN 25
#define RELAY_REV_PIN 26

// Board identification: GPIO32 HIGH = this is the motor board
#define ID_PIN 32

// ── Configuration ──

// Dell OptiPlex (micro-ROS agent)
static const char* AGENT_IP = "192.168.1.100";
static const uint16_t AGENT_PORT = 8888;

// Static IP for this ESP32
static const IPAddress STATIC_IP(192, 168, 1, 11);
static const IPAddress GATEWAY(192, 168, 1, 1);
static const IPAddress SUBNET(255, 255, 255, 0);
static const IPAddress DNS(192, 168, 1, 1);

// Direction relay interlock delay (ms)
static const uint32_t RELAY_INTERLOCK_DELAY_MS = 50;

// Laser sensor UART
static const uint32_t LASER_BAUD = 19200;
static const uint8_t LASER_CMD_SINGLE[] = {0xAA, 0x00, 0x00, 0x20, 0x00, 0x01, 0x00, 0x00, 0x21};
static const size_t LASER_CMD_LEN = sizeof(LASER_CMD_SINGLE);
static const size_t LASER_RESP_LEN = 13;

// ── Globals ──

DFRobot_GP8211S dac;

// micro-ROS
rcl_allocator_t allocator;
rclc_support_t support;
rcl_node_t node;
rclc_executor_t executor;

// Subscribers
rcl_subscription_t speed_cmd_sub;
rcl_subscription_t direction_sub;
rcl_subscription_t enable_sub;
std_msgs__msg__Float32 speed_cmd_msg;
std_msgs__msg__Int8 direction_msg;
std_msgs__msg__Bool enable_msg;

// Publishers
rcl_publisher_t position_pub;
rcl_publisher_t speed_feedback_pub;
std_msgs__msg__Float32 position_msg;
std_msgs__msg__Float32 speed_feedback_msg;

// Timers
rcl_timer_t laser_timer;

// State
float current_speed = 0.0f;
int8_t current_direction = 0;
bool motor_enabled = false;
bool eth_connected = false;

// ── Ethernet Event Handler ──

void onEthEvent(arduino_event_id_t event) {
    switch (event) {
        case ARDUINO_EVENT_ETH_CONNECTED:
            Serial.println("ETH connected");
            break;
        case ARDUINO_EVENT_ETH_GOT_IP:
            Serial.printf("ETH IP: %s\n", ETH.localIP().toString().c_str());
            eth_connected = true;
            break;
        case ARDUINO_EVENT_ETH_DISCONNECTED:
            Serial.println("ETH disconnected");
            eth_connected = false;
            break;
        default:
            break;
    }
}

// ── DAC Output ──

void updateDac() {
    // Map speed (0.0 - 1.0) to DAC value (0 - 32767 for 15-bit, 0-10V)
    float speed = motor_enabled ? current_speed : 0.0f;
    if (speed < 0.0f) speed = 0.0f;
    if (speed > 1.0f) speed = 1.0f;

    uint16_t dac_value = (uint16_t)(speed * 32767.0f);
    dac.setDACOutVoltage(dac_value);

    // Publish feedback
    speed_feedback_msg.data = speed;
    rcl_publish(&speed_feedback_pub, &speed_feedback_msg, NULL);
}

// ── Relay Control ──

void setDirection(int8_t dir) {
    if (dir == current_direction) return;

    // Interlock: turn both off first
    digitalWrite(RELAY_FWD_PIN, LOW);
    digitalWrite(RELAY_REV_PIN, LOW);
    delay(RELAY_INTERLOCK_DELAY_MS);

    current_direction = dir;

    if (dir > 0) {
        digitalWrite(RELAY_FWD_PIN, HIGH);
    } else if (dir < 0) {
        digitalWrite(RELAY_REV_PIN, HIGH);
    }
    // dir == 0: both stay off (stopped)
}

// ── Laser Sensor ──

float readLaserDistance() {
    // Flush any stale data
    while (Serial2.available()) Serial2.read();

    // Send single-shot measurement command
    Serial2.write(LASER_CMD_SINGLE, LASER_CMD_LEN);

    // Wait for response (timeout 500ms)
    uint32_t start = millis();
    while (Serial2.available() < (int)LASER_RESP_LEN) {
        if (millis() - start > 500) return -1.0f;
        delay(1);
    }

    uint8_t resp[LASER_RESP_LEN];
    Serial2.readBytes(resp, LASER_RESP_LEN);

    // Distance is in bytes 8 and 9, in mm
    uint16_t distance_mm = ((uint16_t)resp[8] << 8) | resp[9];
    return (float)distance_mm;
}

// ── micro-ROS Callbacks ──

void speedCmdCallback(const void* msgin) {
    const std_msgs__msg__Float32* msg = (const std_msgs__msg__Float32*)msgin;
    current_speed = msg->data;
    updateDac();
}

void directionCallback(const void* msgin) {
    const std_msgs__msg__Int8* msg = (const std_msgs__msg__Int8*)msgin;
    setDirection(msg->data);
}

void enableCallback(const void* msgin) {
    const std_msgs__msg__Bool* msg = (const std_msgs__msg__Bool*)msgin;
    motor_enabled = msg->data;
    if (!motor_enabled) {
        current_speed = 0.0f;
        setDirection(0);
        updateDac();
    }
}

void laserTimerCallback(rcl_timer_t* timer, int64_t last_call_time) {
    (void)last_call_time;
    if (timer == NULL) return;

    float distance = readLaserDistance();
    if (distance >= 0.0f) {
        position_msg.data = distance;
        rcl_publish(&position_pub, &position_msg, NULL);
    }
}

// ── Setup ──

void setup() {
    Serial.begin(115200);
    Serial.println("GHRA Motor Controller starting...");

    // Board ID pin: measure 3.3V between GPIO32 and GND to identify motor board
    pinMode(ID_PIN, OUTPUT);
    digitalWrite(ID_PIN, HIGH);

    // Relay pins
    pinMode(RELAY_FWD_PIN, OUTPUT);
    pinMode(RELAY_REV_PIN, OUTPUT);
    digitalWrite(RELAY_FWD_PIN, LOW);
    digitalWrite(RELAY_REV_PIN, LOW);

    // Laser UART
    Serial2.begin(LASER_BAUD, SERIAL_8N1, LASER_RX_PIN, LASER_TX_PIN);

    // I2C for DAC
    Wire.begin(DAC_SDA_PIN, DAC_SCL_PIN);
    while (dac.begin() != 0) {
        Serial.println("DAC init failed, retrying...");
        delay(1000);
    }
    dac.setDACOutRange(dac.eOutputRange10V);
    dac.setDACOutVoltage(0);
    Serial.println("DAC initialized (0-10V mode)");

    // Ethernet (W5500 via SPI)
    Network.onEvent(onEthEvent);
    SPI.begin(ETH_SCLK_PIN, ETH_MISO_PIN, ETH_MOSI_PIN, ETH_CS_PIN);
    ETH.begin(ETH_PHY_W5500, 1, ETH_CS_PIN, -1, ETH_RST_PIN, SPI);
    ETH.config(STATIC_IP, GATEWAY, SUBNET, DNS);

    // Wait for Ethernet
    Serial.println("Waiting for Ethernet...");
    while (!eth_connected) {
        delay(100);
    }
    Serial.printf("Ethernet ready — IP: %s\n", ETH.localIP().toString().c_str());

    // micro-ROS setup (reuse wifi UDP transport — works over ETH on ESP32 shared lwIP stack)
    static struct micro_ros_agent_locator locator;
    locator.address.fromString(AGENT_IP);
    locator.port = AGENT_PORT;

    rmw_uros_set_custom_transport(
        false,
        (void *) &locator,
        arduino_wifi_transport_open,
        arduino_wifi_transport_close,
        arduino_wifi_transport_write,
        arduino_wifi_transport_read
    );

    allocator = rcl_get_default_allocator();
    rclc_support_init(&support, 0, NULL, &allocator);
    rclc_node_init_default(&node, "ghra_motor_esp32", "", &support);

    // Subscribers
    rclc_subscription_init_default(
        &speed_cmd_sub, &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Float32),
        "/motor/speed_cmd");

    rclc_subscription_init_default(
        &direction_sub, &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Int8),
        "/motor/direction");

    rclc_subscription_init_default(
        &enable_sub, &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Bool),
        "/motor/enable");

    // Publishers
    rclc_publisher_init_default(
        &position_pub, &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Float32),
        "/carriage/position");

    rclc_publisher_init_default(
        &speed_feedback_pub, &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Float32),
        "/motor/speed_feedback");

    // Timer for laser readings (100ms = 10Hz)
    rclc_timer_init_default(
        &laser_timer, &support,
        RCL_MS_TO_NS(100),
        laserTimerCallback);

    // Executor (3 subs + 1 timer)
    rclc_executor_init(&executor, &support.context, 4, &allocator);
    rclc_executor_add_subscription(&executor, &speed_cmd_sub, &speed_cmd_msg, &speedCmdCallback, ON_NEW_DATA);
    rclc_executor_add_subscription(&executor, &direction_sub, &direction_msg, &directionCallback, ON_NEW_DATA);
    rclc_executor_add_subscription(&executor, &enable_sub, &enable_msg, &enableCallback, ON_NEW_DATA);
    rclc_executor_add_timer(&executor, &laser_timer);

    Serial.println("micro-ROS initialized, spinning...");
}

// ── Loop ──

void loop() {
    rclc_executor_spin_some(&executor, RCL_MS_TO_NS(10));
}
