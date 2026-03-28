/*
 * GHRA Motor Controller - ESP32 #1
 *
 * Interfaces:
 *   - W5500 Ethernet (SPI) -> MQTT broker on Dell OptiPlex
 *   - GP8211S DAC (I2C, 0x58) -> 0-10V to VFD AVI input
 *   - Laser distance sensor M01 (SoftwareSerial, 9600 baud) -> carriage position
 *   - 2x Relays (GPIO) -> VFD FWD/REV terminals
 *
 * MQTT Topics:
 *   Subscribes: motor/speed_cmd, motor/direction, motor/enable
 *   Publishes:  motor/speed_feedback, carriage/position
 */

#include <SPI.h>
#include <Ethernet.h>
#include <Wire.h>
#include <DFRobot_GP8XXX.h>
#include <PubSubClient.h>
#include <SoftwareSerial.h>

// ── Pin Definitions ──

#define ETH_CS_PIN    5
#define ETH_RST_PIN   4
#define ETH_SCLK_PIN  18
#define ETH_MISO_PIN  19
#define ETH_MOSI_PIN  23

#define DAC_SDA_PIN   21
#define DAC_SCL_PIN   22

#define LASER_RX_PIN  33
#define LASER_TX_PIN  32

#define RELAY_FWD_PIN 25
#define RELAY_REV_PIN 26

#define ID_PIN 27

// ── Configuration ──

static byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x11 };
static IPAddress ip(192, 168, 1, 11);
static IPAddress gateway(192, 168, 1, 1);
static IPAddress subnet(255, 255, 255, 0);
static IPAddress dns_addr(192, 168, 1, 1);
static IPAddress mqtt_server(192, 168, 1, 100);
static const uint16_t MQTT_PORT = 1883;
static const char* MQTT_CLIENT_ID = "ghra_motor_esp32";

static const uint32_t RELAY_INTERLOCK_DELAY_MS = 50;

// Laser sensor M01 (SoftwareSerial, 9600 baud, continuous mode)
static const uint8_t LASER_CMD_CONTINUOUS[] = {0xAA,0x00,0x00,0x21,0x00,0x01,0x00,0x00,0x22};

// ── Globals ──

DFRobot_GP8211S dac;
SoftwareSerial laserSerial(LASER_RX_PIN, LASER_TX_PIN);
EthernetClient ethClient;
PubSubClient mqtt(ethClient);

// Laser frame parser state
static uint8_t laser_buf[16];
static int laser_pos = 0;
static float laser_distance_mm = -1.0f;

// State
float current_speed = 0.0f;
int8_t current_direction = 0;
bool motor_enabled = false;

// Timers
static uint32_t last_feedback_ms = 0;
static uint32_t last_stats_ms = 0;

// ── DAC Output ──

void updateDac() {
    float speed = current_speed;
    if (speed < 0.0f) speed = 0.0f;
    if (speed > 1.0f) speed = 1.0f;
    uint16_t dac_value = (uint16_t)(speed * 32767.0f);
    dac.setDACOutVoltage(dac_value);
}

// ── Relay Control ──

void setDirection(int8_t dir) {
    if (dir == current_direction) return;
    digitalWrite(RELAY_FWD_PIN, LOW);
    digitalWrite(RELAY_REV_PIN, LOW);
    delay(RELAY_INTERLOCK_DELAY_MS);
    current_direction = dir;
    if (dir > 0) {
        digitalWrite(RELAY_FWD_PIN, HIGH);
    } else if (dir < 0) {
        digitalWrite(RELAY_REV_PIN, HIGH);
    }
}

// ── Laser Sensor ──

static bool laserChecksumOK(const uint8_t* f, int n) {
    if (n < 3) return false;
    uint32_t s = 0;
    for (int i = 1; i < n - 1; i++) s += f[i];
    return ((uint8_t)s) == f[n - 1];
}

static uint32_t laserDecodeBCD(const uint8_t* b) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v = v * 100 + ((b[i] >> 4) & 0x0F) * 10 + (b[i] & 0x0F);
    }
    return v;
}

void laserWake() {
    laserSerial.write(LASER_CMD_CONTINUOUS, sizeof(LASER_CMD_CONTINUOUS));
    laserSerial.flush();
}

// Non-blocking: call from loop(), processes available bytes
void laserProcess() {
    while (laserSerial.available()) {
        uint8_t x = laserSerial.read();
        if (laser_pos == 0 && x != 0xAA) continue;
        laser_buf[laser_pos++] = x;
        if (laser_pos == 13) {
            if (laser_buf[4] == 0x00 && laser_buf[5] == 0x04 && laserChecksumOK(laser_buf, 13)) {
                uint8_t func = laser_buf[3];
                if (func == 0x20 || func == 0x21 || func == 0x22) {
                    laser_distance_mm = (float)laserDecodeBCD(&laser_buf[6]);
                    if (mqtt.connected()) {
                        char buf[16];
                        snprintf(buf, sizeof(buf), "%.0f", laser_distance_mm);
                        mqtt.publish("carriage/position", buf);
                    }
                }
            }
            laser_pos = 0;
        }
        if (laser_pos >= 13) laser_pos = 0;
    }
}

// ── MQTT Callback ──

void mqttCallback(char* topic, byte* payload, unsigned int length) {
    laserWake();

    char buf[32];
    if (length >= sizeof(buf)) length = sizeof(buf) - 1;
    memcpy(buf, payload, length);
    buf[length] = '\0';

    if (strcmp(topic, "motor/speed_cmd") == 0) {
        current_speed = atof(buf);
        Serial.printf("MQTT speed: %.2f\n", current_speed);
        updateDac();
    }
    else if (strcmp(topic, "motor/direction") == 0) {
        int8_t dir = (int8_t)atoi(buf);
        Serial.printf("MQTT dir: %d\n", dir);
        setDirection(dir);
    }
    else if (strcmp(topic, "motor/enable") == 0) {
        bool en = (buf[0] == '1');
        Serial.printf("MQTT enable: %d\n", en);
        motor_enabled = en;
        if (!motor_enabled) {
            current_speed = 0.0f;
            setDirection(0);
            updateDac();
        }
    }
}

// ── MQTT Connect ──

void mqttConnect() {
    if (mqtt.connected()) return;
    Serial.printf("MQTT connecting to %s:%d...\n",
        mqtt_server.toString().c_str(), MQTT_PORT);
    if (mqtt.connect(MQTT_CLIENT_ID)) {
        Serial.println("MQTT connected!");
        mqtt.subscribe("motor/speed_cmd", 0);
        mqtt.subscribe("motor/direction", 0);
        mqtt.subscribe("motor/enable", 0);
    } else {
        Serial.printf("MQTT failed rc=%d\n", mqtt.state());
    }
}

// ── Setup ──

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("\n\n=== GHRA Motor Controller (MQTT) ===");

    pinMode(ID_PIN, OUTPUT);
    digitalWrite(ID_PIN, HIGH);

    pinMode(RELAY_FWD_PIN, OUTPUT);
    pinMode(RELAY_REV_PIN, OUTPUT);
    digitalWrite(RELAY_FWD_PIN, LOW);
    digitalWrite(RELAY_REV_PIN, LOW);

    laserSerial.begin(9600);
    laserWake();

    Wire.begin(DAC_SDA_PIN, DAC_SCL_PIN);
    while (dac.begin() != 0) {
        Serial.println("DAC init failed, retrying...");
        delay(1000);
    }
    dac.setDACOutRange(dac.eOutputRange10V);
    dac.setDACOutVoltage(0);
    Serial.println("DAC OK");

    // W5500 Ethernet
    pinMode(ETH_RST_PIN, OUTPUT);
    digitalWrite(ETH_RST_PIN, LOW);
    delay(50);
    digitalWrite(ETH_RST_PIN, HIGH);
    delay(200);

    SPI.begin(ETH_SCLK_PIN, ETH_MISO_PIN, ETH_MOSI_PIN, ETH_CS_PIN);
    Ethernet.init(ETH_CS_PIN);
    Ethernet.begin(mac, ip, dns_addr, gateway, subnet);

    Serial.printf("ETH IP: %s\n", Ethernet.localIP().toString().c_str());

    if (Ethernet.hardwareStatus() == EthernetNoHardware) {
        Serial.println("W5500 not found!");
    } else if (Ethernet.linkStatus() == LinkOFF) {
        Serial.println("ETH cable not connected");
    } else {
        Serial.println("ETH OK");
    }

    mqtt.setServer(mqtt_server, MQTT_PORT);
    mqtt.setCallback(mqttCallback);
    mqttConnect();

    Serial.println("Running...");
}

// ── Serial Commands ──

void handleSerialCommand() {
    if (!Serial.available()) return;
    String cmd = Serial.readStringUntil('\n');
    cmd.trim();

    if (cmd.startsWith("speed ")) {
        current_speed = cmd.substring(6).toFloat();
        motor_enabled = true;
        updateDac();
        Serial.printf("SET speed=%.2f\n", current_speed);
    }
    else if (cmd == "fwd") { setDirection(1); Serial.println("FWD"); }
    else if (cmd == "rev") { setDirection(-1); Serial.println("REV"); }
    else if (cmd == "stop") {
        current_speed = 0.0f; motor_enabled = false;
        setDirection(0); updateDac();
        Serial.println("STOPPED");
    }
    else if (cmd == "status") {
        Serial.printf("en=%d spd=%.2f dir=%d mqtt=%d\n",
            motor_enabled, current_speed, current_direction, mqtt.connected());
    }
    else if (cmd == "i2c") {
        Serial.println("I2C scan:");
        for (uint8_t addr = 1; addr < 127; addr++) {
            Wire.beginTransmission(addr);
            if (Wire.endTransmission() == 0) Serial.printf("  0x%02X\n", addr);
        }
        dac.setDACOutVoltage(16383);
        Serial.println("DAC -> 5V");
    }
    else if (cmd == "dac0") { dac.setDACOutVoltage(0); Serial.println("DAC -> 0"); }
    else { Serial.println("Cmds: speed <0-1> | fwd | rev | stop | status | i2c | dac0"); }
}

// ── Loop ──

void loop() {
    if (!mqtt.connected()) {
        static uint32_t last_reconnect = 0;
        if (millis() - last_reconnect > 3000) {
            last_reconnect = millis();
            mqttConnect();
        }
    }
    mqtt.loop();

    handleSerialCommand();

    laserProcess();

    // Speed feedback at 5Hz
    if (millis() - last_feedback_ms >= 200) {
        last_feedback_ms = millis();
        if (mqtt.connected()) {
            char buf[16];
            snprintf(buf, sizeof(buf), "%.2f", current_speed);
            mqtt.publish("motor/speed_feedback", buf);
        }
    }

    // Stats
    if (millis() - last_stats_ms > 30000) {
        last_stats_ms = millis();
        Serial.printf("STATS: mqtt=%d en=%d spd=%.2f dir=%d\n",
            mqtt.connected(), motor_enabled, current_speed, current_direction);
    }
}
