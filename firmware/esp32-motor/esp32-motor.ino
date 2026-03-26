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

#include <lwip/sockets.h>

#include <micro_ros_arduino.h>
#include <rcl/rcl.h>
#include <rclc/rclc.h>
#include <rclc/executor.h>
#include <std_msgs/msg/float32.h>
#include <std_msgs/msg/int8.h>
#include <std_msgs/msg/bool.h>

// ── Custom UDP Transport for micro-ROS over Ethernet (raw lwIP sockets) ──

static int uros_sock = -1;
static struct sockaddr_in agent_addr;
static uint32_t transport_tx_count = 0;
static uint32_t transport_rx_count = 0;
static uint32_t transport_rx_timeout_count = 0;

extern "C" bool custom_transport_open(struct uxrCustomTransport * transport) {
    struct micro_ros_agent_locator * locator = (struct micro_ros_agent_locator *) transport->args;

    uros_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (uros_sock < 0) {
        Serial.printf("TRANSPORT: socket() failed: %d\n", errno);
        return false;
    }

    // Do NOT bind to a specific port — let the OS assign an ephemeral port.
    // sendto() and recvfrom() use the same socket, so the agent will reply
    // to whatever source port it sees in our packets.
    // (Binding to 8888 reports success via getsockname but agent sees a different port)

    // Verify bound port
    struct sockaddr_in bound_addr;
    socklen_t addr_len = sizeof(bound_addr);
    getsockname(uros_sock, (struct sockaddr *)&bound_addr, &addr_len);
    Serial.printf("TRANSPORT: bound to %s:%d (requested %d)\n",
        inet_ntoa(bound_addr.sin_addr), ntohs(bound_addr.sin_port), locator->port);

    // Set non-blocking
    fcntl(uros_sock, F_SETFL, O_NONBLOCK);

    // Prepare agent address for sendto
    memset(&agent_addr, 0, sizeof(agent_addr));
    agent_addr.sin_family = AF_INET;
    agent_addr.sin_port = htons(locator->port);
    agent_addr.sin_addr.s_addr = (uint32_t)locator->address;

    Serial.printf("TRANSPORT: agent at %s:%d\n",
        inet_ntoa(agent_addr.sin_addr), locator->port);
    return true;
}

extern "C" bool custom_transport_close(struct uxrCustomTransport * transport) {
    if (uros_sock >= 0) {
        close(uros_sock);
        uros_sock = -1;
    }
    Serial.println("TRANSPORT: closed");
    return true;
}

extern "C" size_t custom_transport_write(struct uxrCustomTransport * transport, const uint8_t *buf, size_t len, uint8_t *errcode) {
    (void)errcode;
    int sent = sendto(uros_sock, buf, len, 0,
        (struct sockaddr *)&agent_addr, sizeof(agent_addr));
    if (sent < 0) {
        if (transport_tx_count < 5) Serial.printf("TRANSPORT: sendto failed: %d\n", errno);
        return 0;
    }
    transport_tx_count++;
    return (size_t)sent;
}

extern "C" size_t custom_transport_read(struct uxrCustomTransport * transport, uint8_t *buf, size_t len, int timeout, uint8_t *errcode) {
    (void)errcode;

    // Use select() to properly wait for data — busy-poll with recvfrom(MSG_DONTWAIT)
    // misses packets on ESP32 W5500 Ethernet.
    fd_set readfds;
    struct timeval tv;
    tv.tv_sec = timeout / 1000;
    tv.tv_usec = (timeout % 1000) * 1000;

    FD_ZERO(&readfds);
    FD_SET(uros_sock, &readfds);

    int sel = select(uros_sock + 1, &readfds, NULL, NULL, &tv);
    if (sel > 0 && FD_ISSET(uros_sock, &readfds)) {
        struct sockaddr_in from_addr;
        socklen_t from_len = sizeof(from_addr);
        int got = recvfrom(uros_sock, buf, len, MSG_DONTWAIT,
            (struct sockaddr *)&from_addr, &from_len);
        if (got > 0) {
            transport_rx_count++;
            if (transport_rx_count <= 5) {
                Serial.printf("TRANSPORT: RX %d bytes from %s:%d\n",
                    got, inet_ntoa(from_addr.sin_addr), ntohs(from_addr.sin_port));
            }
            return (size_t)got;
        }
    }

    transport_rx_timeout_count++;
    return 0;
}

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
    float speed = current_speed;
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
// Agent floods duplicates — only act on actual value changes

// Debounce — agent floods hundreds of duplicates per publish.
// Only process one callback per topic per 50ms window.
static uint32_t last_speed_cb_ms = 0;
static uint32_t last_dir_cb_ms = 0;
static uint32_t last_enable_cb_ms = 0;
static const uint32_t CB_DEBOUNCE_MS = 50;

void speedCmdCallback(const void* msgin) {
    uint32_t now = millis();
    if (now - last_speed_cb_ms < CB_DEBOUNCE_MS) return;
    last_speed_cb_ms = now;
    const std_msgs__msg__Float32* msg = (const std_msgs__msg__Float32*)msgin;
    current_speed = msg->data;
    Serial.printf("ROS speed_cmd: %.2f\n", msg->data);
    updateDac();
}

void directionCallback(const void* msgin) {
    uint32_t now = millis();
    if (now - last_dir_cb_ms < CB_DEBOUNCE_MS) return;
    last_dir_cb_ms = now;
    const std_msgs__msg__Int8* msg = (const std_msgs__msg__Int8*)msgin;
    Serial.printf("ROS direction: %d\n", msg->data);
    setDirection(msg->data);
}

void enableCallback(const void* msgin) {
    uint32_t now = millis();
    if (now - last_enable_cb_ms < CB_DEBOUNCE_MS) return;
    last_enable_cb_ms = now;
    const std_msgs__msg__Bool* msg = (const std_msgs__msg__Bool*)msgin;
    Serial.printf("ROS enable: %d\n", msg->data);
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
    delay(3000);  // Wait for serial monitor to connect
    Serial.println("\n\n=== GHRA Motor Controller starting ===");

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

    // micro-ROS setup with custom UDP transport over Ethernet
    static struct micro_ros_agent_locator locator;
    locator.address.fromString(AGENT_IP);
    locator.port = AGENT_PORT;

    rmw_uros_set_custom_transport(
        false,
        (void *) &locator,
        custom_transport_open,
        custom_transport_close,
        custom_transport_write,
        custom_transport_read
    );

    allocator = rcl_get_default_allocator();

    rcl_ret_t rc;
    rc = rclc_support_init(&support, 0, NULL, &allocator);
    Serial.printf("INIT support: %d\n", (int)rc);

    rc = rclc_node_init_default(&node, "ghra_motor_esp32", "", &support);
    Serial.printf("INIT node: %d\n", (int)rc);

    // Subscribers — use best-effort QoS to avoid repeated delivery of stale messages
    rc = rclc_subscription_init_best_effort(
        &speed_cmd_sub, &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Float32),
        "/motor/speed_cmd");
    Serial.printf("INIT sub speed_cmd: %d\n", (int)rc);

    rc = rclc_subscription_init_best_effort(
        &direction_sub, &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Int8),
        "/motor/direction");
    Serial.printf("INIT sub direction: %d\n", (int)rc);

    rc = rclc_subscription_init_best_effort(
        &enable_sub, &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Bool),
        "/motor/enable");
    Serial.printf("INIT sub enable: %d\n", (int)rc);

    // Publishers
    rc = rclc_publisher_init_default(
        &position_pub, &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Float32),
        "/carriage/position");
    Serial.printf("INIT pub position: %d\n", (int)rc);

    rc = rclc_publisher_init_default(
        &speed_feedback_pub, &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Float32),
        "/motor/speed_feedback");
    Serial.printf("INIT pub speed_fb: %d\n", (int)rc);

    // Timer for laser readings (100ms = 10Hz)
    rc = rclc_timer_init_default(
        &laser_timer, &support,
        RCL_MS_TO_NS(100),
        laserTimerCallback);
    Serial.printf("INIT timer: %d\n", (int)rc);

    // Executor (3 subs + 1 timer)
    rc = rclc_executor_init(&executor, &support.context, 4, &allocator);
    Serial.printf("INIT executor: %d\n", (int)rc);
    rc = rclc_executor_add_subscription(&executor, &speed_cmd_sub, &speed_cmd_msg, &speedCmdCallback, ON_NEW_DATA);
    Serial.printf("INIT exec+sub speed: %d\n", (int)rc);
    rc = rclc_executor_add_subscription(&executor, &direction_sub, &direction_msg, &directionCallback, ON_NEW_DATA);
    Serial.printf("INIT exec+sub dir: %d\n", (int)rc);
    rc = rclc_executor_add_subscription(&executor, &enable_sub, &enable_msg, &enableCallback, ON_NEW_DATA);
    Serial.printf("INIT exec+sub enable: %d\n", (int)rc);
    rc = rclc_executor_add_timer(&executor, &laser_timer);
    Serial.printf("INIT exec+timer: %d\n", (int)rc);

    Serial.println("micro-ROS initialized, pinging agent...");

    // Verify session is alive
    rmw_ret_t ping_rc = rmw_uros_ping_agent(1000, 3);
    Serial.printf("PING agent: %s (rc=%d)\n", ping_rc == RMW_RET_OK ? "OK" : "FAIL", (int)ping_rc);

    Serial.printf("TRANSPORT after init: tx=%u rx=%u rx_timeout=%u\n",
        transport_tx_count, transport_rx_count, transport_rx_timeout_count);

    Serial.println("Spinning...");
}

// ── Serial Test Commands ──

void handleSerialCommand() {
    if (!Serial.available()) return;

    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd.startsWith("speed ")) {
        float speed = cmd.substring(6).toFloat();
        current_speed = speed;
        motor_enabled = true;
        updateDac();
        Serial.printf("SET speed=%.2f, dac_value=%d\n", speed, (int)(speed * 32767.0f));
    }
    else if (cmd == "fwd") {
        setDirection(1);
        Serial.println("SET direction=FWD (pin 25 HIGH)");
    }
    else if (cmd == "rev") {
        setDirection(-1);
        Serial.println("SET direction=REV (pin 26 HIGH)");
    }
    else if (cmd == "stop") {
        current_speed = 0.0f;
        motor_enabled = false;
        setDirection(0);
        updateDac();
        Serial.println("STOPPED (relays OFF, DAC=0)");
    }
    else if (cmd == "status") {
        Serial.printf("enabled=%d speed=%.2f dir=%d\n", motor_enabled, current_speed, current_direction);
    }
    else if (cmd == "i2c") {
        Serial.println("I2C scan:");
        int found = 0;
        for (uint8_t addr = 1; addr < 127; addr++) {
            Wire.beginTransmission(addr);
            uint8_t err = Wire.endTransmission();
            if (err == 0) {
                Serial.printf("  0x%02X OK\n", addr);
                found++;
            }
        }
        Serial.printf("Found %d devices\n", found);

        // Test DAC write
        Serial.println("DAC test: writing 16383 (~5V)...");
        dac.setDACOutVoltage(16383);
        Serial.println("DAC test: done - measure voltage now");
    }
    else if (cmd == "dac0") {
        dac.setDACOutVoltage(0);
        Serial.println("DAC set to 0");
    }
    else {
        Serial.println("Commands: speed <0.0-1.0> | fwd | rev | stop | status | i2c | dac0");
    }
}

// ── Loop ──

static uint32_t last_stats_ms = 0;

void loop() {
    rclc_executor_spin_some(&executor, RCL_MS_TO_NS(10));

    handleSerialCommand();

    // Print transport stats every 30 seconds
    if (millis() - last_stats_ms > 30000) {
        last_stats_ms = millis();
        Serial.printf("STATS: tx=%u rx=%u timeout=%u\n",
            transport_tx_count, transport_rx_count, transport_rx_timeout_count);
    }

    delay(1);
}
