/*
 * GHRA Remote Controller - ESP32 #2
 *
 * Interfaces:
 *   - WiFi → micro-ROS agent on Dell OptiPlex
 *   - 4x Buttons (GPIO, active LOW with internal pull-ups)
 *
 * Buttons:
 *   Green 1 (GPIO12) = Forward Fast   → event +1
 *   Green 2 (GPIO13) = Forward Slow   → event +2
 *   Red 1   (GPIO14) = Backward Slow  → event +3
 *   Red 2   (GPIO27) = Backward Fast  → event +4
 *   Released = negative event value
 *
 * ROS2 Topics:
 *   Publishes: /remote/button_event (std_msgs/Int8)
 */

#include <WiFi.h>

#include <micro_ros_arduino.h>
#include <rcl/rcl.h>
#include <rclc/rclc.h>
#include <rclc/executor.h>
#include <std_msgs/msg/int8.h>

// ── Pin Definitions ──

#define BTN_FWD_FAST_PIN 12
#define BTN_FWD_SLOW_PIN 13
#define BTN_BWD_SLOW_PIN 14
#define BTN_BWD_FAST_PIN 27

// ── Button Encoding ──

static const int8_t BTN_EVENT_FWD_FAST = 1;
static const int8_t BTN_EVENT_FWD_SLOW = 2;
static const int8_t BTN_EVENT_BWD_SLOW = 3;
static const int8_t BTN_EVENT_BWD_FAST = 4;

// ── Configuration ──

// TODO: Change these to your network credentials and Dell OptiPlex IP
static char WIFI_SSID[] = "YOUR_WIFI_SSID";
static char WIFI_PASS[] = "YOUR_WIFI_PASSWORD";
static char AGENT_IP[]  = "192.168.1.100";
static const uint16_t AGENT_PORT = 8888;

static const uint32_t DEBOUNCE_MS = 20;

// ── Button State ──

struct Button {
    uint8_t pin;
    int8_t event_id;
    bool last_state;       // true = pressed (LOW)
    uint32_t last_change;  // millis of last state change
};

Button buttons[] = {
    {BTN_FWD_FAST_PIN, BTN_EVENT_FWD_FAST, false, 0},
    {BTN_FWD_SLOW_PIN, BTN_EVENT_FWD_SLOW, false, 0},
    {BTN_BWD_SLOW_PIN, BTN_EVENT_BWD_SLOW, false, 0},
    {BTN_BWD_FAST_PIN, BTN_EVENT_BWD_FAST, false, 0},
};
static const size_t NUM_BUTTONS = sizeof(buttons) / sizeof(buttons[0]);

// ── micro-ROS ──

rcl_allocator_t allocator;
rclc_support_t support;
rcl_node_t node;
rcl_publisher_t button_pub;
std_msgs__msg__Int8 button_msg;

// ── Publish Button Event ──

void publishButtonEvent(int8_t event) {
    button_msg.data = event;
    rcl_publish(&button_pub, &button_msg, NULL);
    Serial.printf("Button event: %d\n", event);
}

// ── Setup ──

void setup() {
    Serial.begin(115200);
    Serial.println("GHRA Remote Controller starting...");

    // Configure button pins with internal pull-ups
    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        pinMode(buttons[i].pin, INPUT_PULLUP);
    }

    // Connect WiFi
    Serial.printf("Connecting to WiFi: %s\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASS);
    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\nWiFi connected, IP: %s\n", WiFi.localIP().toString().c_str());

    // micro-ROS setup
    set_microros_wifi_transports(WIFI_SSID, WIFI_PASS, AGENT_IP, AGENT_PORT);

    allocator = rcl_get_default_allocator();
    rclc_support_init(&support, 0, NULL, &allocator);
    rclc_node_init_default(&node, "ghra_remote_esp32", "", &support);

    // Publisher
    rclc_publisher_init_default(
        &button_pub, &node,
        ROSIDL_GET_MSG_TYPE_SUPPORT(std_msgs, msg, Int8),
        "/remote/button_event");

    Serial.println("micro-ROS initialized, polling buttons...");
}

// ── Loop ──

void loop() {
    uint32_t now = millis();

    for (size_t i = 0; i < NUM_BUTTONS; i++) {
        bool pressed = (digitalRead(buttons[i].pin) == LOW);  // Active LOW

        if (pressed != buttons[i].last_state && (now - buttons[i].last_change) > DEBOUNCE_MS) {
            buttons[i].last_state = pressed;
            buttons[i].last_change = now;

            if (pressed) {
                publishButtonEvent(buttons[i].event_id);        // Positive = pressed
            } else {
                publishButtonEvent(-buttons[i].event_id);       // Negative = released
            }
        }
    }

    delay(5);  // ~200Hz polling
}
