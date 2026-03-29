/*
 * GHRA Remote Controller - ESP32 #2
 *
 * Interfaces:
 *   - W5500 Ethernet (SPI) -> MQTT broker on Dell OptiPlex
 *   - 3x Buttons (GPIO, active LOW with internal pull-ups)
 *
 * Buttons:
 *   FWD  (GPIO12) = Forward at 7% speed
 *   STOP (GPIO14) = Stop motor
 *   REV  (GPIO27) = Reverse at 7% speed
 *
 * MQTT Topics:
 *   Publishes: motor/speed_cmd, motor/direction, motor/enable
 */

#include <SPI.h>
#include <Ethernet.h>
#include <PubSubClient.h>

// ── Pin Definitions ──

#define ETH_CS_PIN    5
#define ETH_RST_PIN   4
#define ETH_SCLK_PIN  18
#define ETH_MISO_PIN  19
#define ETH_MOSI_PIN  23

#define BTN_FWD_PIN   12
#define BTN_STOP_PIN  14
#define BTN_REV_PIN   27

// ── Configuration ──

static byte mac[] = { 0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0x12 };
static IPAddress ip(192, 168, 1, 12);
static IPAddress gateway(192, 168, 1, 1);
static IPAddress subnet(255, 255, 255, 0);
static IPAddress dns_addr(192, 168, 1, 1);
static IPAddress mqtt_server(192, 168, 1, 100);
static const uint16_t MQTT_PORT = 1883;
static const char* MQTT_CLIENT_ID = "ghra_remote_esp32";

static const uint32_t DEBOUNCE_MS = 20;

// ── Globals ──

EthernetClient ethClient;
PubSubClient mqtt(ethClient);

// ── Button State ──

struct Button {
    uint8_t pin;
    bool last_state;
    uint32_t last_change;
};

Button btn_fwd  = {BTN_FWD_PIN,  false, 0};
Button btn_stop = {BTN_STOP_PIN, false, 0};
Button btn_rev  = {BTN_REV_PIN,  false, 0};

// ── MQTT Connect ──

void mqttConnect() {
    if (mqtt.connected()) return;
    Serial.printf("MQTT connecting to %s:%d...\n",
        mqtt_server.toString().c_str(), MQTT_PORT);
    if (mqtt.connect(MQTT_CLIENT_ID)) {
        Serial.println("MQTT connected!");
    } else {
        Serial.printf("MQTT failed rc=%d\n", mqtt.state());
    }
}

// ── Button Actions ──

void cmdForward() {
    Serial.println("BTN: FWD");
    mqtt.publish("motor/direction", "1");
    mqtt.publish("motor/speed_cmd", "0.07");
    mqtt.publish("motor/enable", "1");
}

void cmdStop() {
    Serial.println("BTN: STOP");
    mqtt.publish("motor/speed_cmd", "0");
    mqtt.publish("motor/enable", "0");
}

void cmdReverse() {
    Serial.println("BTN: REV");
    mqtt.publish("motor/direction", "-1");
    mqtt.publish("motor/speed_cmd", "0.07");
    mqtt.publish("motor/enable", "1");
}

// ── Button Polling ──
// Returns: 1 = just pressed, -1 = just released, 0 = no change

int8_t debounce(Button &btn) {
    bool pressed = (digitalRead(btn.pin) == LOW);
    uint32_t now = millis();
    if (pressed != btn.last_state && (now - btn.last_change) > DEBOUNCE_MS) {
        btn.last_state = pressed;
        btn.last_change = now;
        return pressed ? 1 : -1;
    }
    return 0;
}

// ── Setup ──

void setup() {
    Serial.begin(115200);
    delay(2000);
    Serial.println("\n\n=== GHRA Remote Controller (MQTT) ===");

    pinMode(BTN_FWD_PIN, INPUT_PULLUP);
    pinMode(BTN_STOP_PIN, INPUT_PULLUP);
    pinMode(BTN_REV_PIN, INPUT_PULLUP);

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
    mqttConnect();

    Serial.println("Running...");
}

// ── Loop ──

void loop() {
    Ethernet.maintain();

    if (!mqtt.connected()) {
        static uint32_t last_reconnect = 0;
        if (millis() - last_reconnect > 3000) {
            last_reconnect = millis();
            Serial.printf("MQTT state=%d, ETH link=%d\n", mqtt.state(), Ethernet.linkStatus());
            mqttConnect();
        }
    }
    mqtt.loop();

    int8_t fwd = debounce(btn_fwd);
    int8_t stp = debounce(btn_stop);
    int8_t rev = debounce(btn_rev);

    if (fwd == 1)  cmdForward();
    if (fwd == -1) cmdStop();
    if (rev == 1)  cmdReverse();
    if (rev == -1) cmdStop();
    if (stp == 1)  cmdStop();

    delay(5);
}
