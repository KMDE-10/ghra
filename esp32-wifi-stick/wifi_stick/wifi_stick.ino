/*
 * ESP32-S3 WiFi Stick Firmware
 *
 * Turns the ESP32-S3 into a USB WiFi adapter controlled via serial commands.
 * The host sends JSON commands over serial, the ESP32 executes WiFi operations
 * and returns results.
 *
 * Serial protocol (115200 baud):
 *   Host -> ESP32: JSON command
 *   ESP32 -> Host: JSON response
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include "esp_eap_client.h"
#include "esp_wifi.h"

#define SERIAL_BAUD 115200
#define MAX_CMD_LEN 4096
#define WIFI_TIMEOUT_MS 15000

char cmdBuffer[MAX_CMD_LEN];
int cmdIndex = 0;

// Current connection state
bool wifiConnected = false;
String currentSSID = "";
String assignedIP = "";

void setup() {
  Serial.begin(SERIAL_BAUD);
  while (!Serial) { delay(10); }

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  Serial.println("{\"event\":\"ready\",\"firmware\":\"wifi-stick\",\"version\":\"1.0\"}");
}

void loop() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdIndex > 0) {
        cmdBuffer[cmdIndex] = '\0';
        processCommand(String(cmdBuffer));
        cmdIndex = 0;
      }
    } else if (cmdIndex < MAX_CMD_LEN - 1) {
      cmdBuffer[cmdIndex++] = c;
    }
  }

  // Monitor WiFi connection state changes
  static bool lastConnected = false;
  bool nowConnected = WiFi.isConnected();
  if (lastConnected && !nowConnected) {
    wifiConnected = false;
    Serial.println("{\"event\":\"disconnected\"}");
  }
  lastConnected = nowConnected;

  delay(1);
}

// Simple JSON value extractor (avoids needing ArduinoJson library)
String jsonGet(const String &json, const String &key) {
  String search = "\"" + key + "\":";
  int idx = json.indexOf(search);
  if (idx == -1) return "";
  idx += search.length();

  // Skip whitespace
  while (idx < json.length() && json[idx] == ' ') idx++;

  if (json[idx] == '"') {
    // String value
    int start = idx + 1;
    int end = json.indexOf('"', start);
    if (end == -1) return "";
    return json.substring(start, end);
  } else {
    // Number or boolean
    int start = idx;
    int end = start;
    while (end < json.length() && json[end] != ',' && json[end] != '}' && json[end] != ' ') end++;
    return json.substring(start, end);
  }
}

void processCommand(const String &cmd) {
  String action = jsonGet(cmd, "action");

  if (action == "scan") {
    cmdScan();
  } else if (action == "connect") {
    String ssid = jsonGet(cmd, "ssid");
    String pass = jsonGet(cmd, "pass");
    String user = jsonGet(cmd, "user");
    String identity = jsonGet(cmd, "identity");
    if (user.length() > 0) {
      cmdConnectEnterprise(ssid, user, pass, identity);
    } else {
      cmdConnect(ssid, pass);
    }
  } else if (action == "disconnect") {
    cmdDisconnect();
  } else if (action == "status") {
    cmdStatus();
  } else if (action == "http") {
    String url = jsonGet(cmd, "url");
    String method = jsonGet(cmd, "method");
    String body = jsonGet(cmd, "body");
    if (method == "") method = "GET";
    cmdHttp(url, method, body);
  } else if (action == "dns") {
    String host = jsonGet(cmd, "host");
    cmdDns(host);
  } else if (action == "ping") {
    cmdPing();
  } else {
    Serial.println("{\"error\":\"unknown_action\",\"action\":\"" + action + "\"}");
  }
}

void cmdScan() {
  Serial.println("{\"event\":\"scanning\"}");
  int n = WiFi.scanNetworks();

  String response = "{\"action\":\"scan\",\"networks\":[";
  for (int i = 0; i < n; i++) {
    if (i > 0) response += ",";
    response += "{\"ssid\":\"" + WiFi.SSID(i) + "\""
              + ",\"rssi\":" + String(WiFi.RSSI(i))
              + ",\"channel\":" + String(WiFi.channel(i))
              + ",\"encryption\":" + String(WiFi.encryptionType(i))
              + "}";
  }
  response += "],\"count\":" + String(n) + "}";
  Serial.println(response);
  WiFi.scanDelete();
}

void cmdConnect(const String &ssid, const String &pass) {
  if (ssid == "") {
    Serial.println("{\"error\":\"missing_ssid\"}");
    return;
  }

  WiFi.disconnect();
  delay(100);

  if (pass.length() > 0) {
    WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    WiFi.begin(ssid.c_str());
  }

  Serial.println("{\"event\":\"connecting\",\"ssid\":\"" + ssid + "\"}");

  unsigned long start = millis();
  while (!WiFi.isConnected() && (millis() - start) < WIFI_TIMEOUT_MS) {
    delay(250);
  }

  if (WiFi.isConnected()) {
    wifiConnected = true;
    currentSSID = ssid;
    assignedIP = WiFi.localIP().toString();

    String response = "{\"action\":\"connect\",\"status\":\"connected\""
                    + String(",\"ssid\":\"") + ssid + "\""
                    + ",\"ip\":\"" + assignedIP + "\""
                    + ",\"gateway\":\"" + WiFi.gatewayIP().toString() + "\""
                    + ",\"dns\":\"" + WiFi.dnsIP().toString() + "\""
                    + ",\"rssi\":" + String(WiFi.RSSI())
                    + ",\"mac\":\"" + WiFi.macAddress() + "\""
                    + "}";
    Serial.println(response);
  } else {
    WiFi.disconnect();
    Serial.println("{\"action\":\"connect\",\"status\":\"failed\",\"ssid\":\"" + ssid + "\"}");
  }
}

void cmdConnectEnterprise(const String &ssid, const String &user, const String &pass, const String &identity) {
  if (ssid == "" || user == "") {
    Serial.println("{\"error\":\"missing_ssid_or_user\"}");
    return;
  }

  WiFi.disconnect(true);
  delay(200);
  WiFi.mode(WIFI_STA);

  // Configure WPA2-Enterprise (PEAP/MSCHAPv2 — standard for eduroam)
  esp_eap_client_set_identity((uint8_t *)user.c_str(), user.length());
  if (identity.length() > 0) {
    esp_eap_client_set_identity((uint8_t *)identity.c_str(), identity.length());
  }
  esp_eap_client_set_username((uint8_t *)user.c_str(), user.length());
  esp_eap_client_set_password((uint8_t *)pass.c_str(), pass.length());
  esp_wifi_sta_enterprise_enable();

  WiFi.begin(ssid.c_str());

  Serial.println("{\"event\":\"connecting\",\"ssid\":\"" + ssid + "\",\"mode\":\"enterprise\"}");

  unsigned long start = millis();
  while (!WiFi.isConnected() && (millis() - start) < 20000) {
    delay(250);
  }

  if (WiFi.isConnected()) {
    wifiConnected = true;
    currentSSID = ssid;
    assignedIP = WiFi.localIP().toString();

    String response = "{\"action\":\"connect\",\"status\":\"connected\",\"mode\":\"enterprise\""
                    + String(",\"ssid\":\"") + ssid + "\""
                    + ",\"ip\":\"" + assignedIP + "\""
                    + ",\"gateway\":\"" + WiFi.gatewayIP().toString() + "\""
                    + ",\"dns\":\"" + WiFi.dnsIP().toString() + "\""
                    + ",\"rssi\":" + String(WiFi.RSSI())
                    + ",\"mac\":\"" + WiFi.macAddress() + "\""
                    + "}";
    Serial.println(response);
  } else {
    WiFi.disconnect(true);
    esp_wifi_sta_enterprise_disable();
    Serial.println("{\"action\":\"connect\",\"status\":\"failed\",\"ssid\":\"" + ssid + "\",\"mode\":\"enterprise\"}");
  }
}

void cmdDisconnect() {
  WiFi.disconnect();
  wifiConnected = false;
  currentSSID = "";
  assignedIP = "";
  Serial.println("{\"action\":\"disconnect\",\"status\":\"ok\"}");
}

void cmdStatus() {
  String status;
  if (WiFi.isConnected()) {
    status = "{\"action\":\"status\",\"connected\":true"
           + String(",\"ssid\":\"") + WiFi.SSID() + "\""
           + ",\"ip\":\"" + WiFi.localIP().toString() + "\""
           + ",\"gateway\":\"" + WiFi.gatewayIP().toString() + "\""
           + ",\"dns\":\"" + WiFi.dnsIP().toString() + "\""
           + ",\"rssi\":" + String(WiFi.RSSI())
           + ",\"mac\":\"" + WiFi.macAddress() + "\""
           + ",\"channel\":" + String(WiFi.channel())
           + "}";
  } else {
    status = "{\"action\":\"status\",\"connected\":false,\"mac\":\"" + WiFi.macAddress() + "\"}";
  }
  Serial.println(status);
}

void cmdHttp(const String &url, const String &method, const String &body) {
  if (!WiFi.isConnected()) {
    Serial.println("{\"error\":\"not_connected\"}");
    return;
  }

  HTTPClient http;

  if (url.startsWith("https://")) {
    WiFiClientSecure *client = new WiFiClientSecure;
    client->setInsecure(); // Skip cert verification for general use
    http.begin(*client, url);
  } else {
    http.begin(url);
  }

  http.setTimeout(10000);

  int httpCode;
  if (method == "POST") {
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    httpCode = http.POST(body);
  } else if (method == "PUT") {
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    httpCode = http.PUT(body);
  } else {
    httpCode = http.GET();
  }

  String response = "{\"action\":\"http\",\"code\":" + String(httpCode);
  if (httpCode > 0) {
    String payload = http.getString();
    // Escape quotes in payload for JSON
    payload.replace("\\", "\\\\");
    payload.replace("\"", "\\\"");
    payload.replace("\n", "\\n");
    payload.replace("\r", "\\r");
    // Truncate if too large for serial
    if (payload.length() > 2048) {
      payload = payload.substring(0, 2048) + "...[truncated]";
    }
    response += ",\"body\":\"" + payload + "\"";
  }
  response += "}";
  Serial.println(response);

  http.end();
}

void cmdDns(const String &host) {
  if (!WiFi.isConnected()) {
    Serial.println("{\"error\":\"not_connected\"}");
    return;
  }

  IPAddress ip;
  if (WiFi.hostByName(host.c_str(), ip)) {
    Serial.println("{\"action\":\"dns\",\"host\":\"" + host + "\",\"ip\":\"" + ip.toString() + "\"}");
  } else {
    Serial.println("{\"action\":\"dns\",\"host\":\"" + host + "\",\"error\":\"resolve_failed\"}");
  }
}

void cmdPing() {
  Serial.println("{\"action\":\"ping\",\"response\":\"pong\",\"uptime\":" + String(millis()) + "}");
}
