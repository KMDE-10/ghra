/*
 * ESP32-S3 USB WiFi Adapter — SLIP Gateway with NAPT
 *
 * Two-phase operation:
 *   Phase 1 (Config): JSON commands over serial to scan/connect WiFi
 *   Phase 2 (SLIP):   SLIP packet forwarding with NAPT — appears as network device on host
 *
 * Host setup: slattach creates sl0 interface, then configure IP routing.
 */

#include <WiFi.h>
#include "esp_wifi.h"
#include "esp_eap_client.h"
#include "esp_netif.h"
#include "lwip/netif.h"
#include "lwip/ip.h"
#include "lwip/pbuf.h"
#include "lwip/ip4_napt.h"
#include "lwip/lwip_napt.h"
#include "lwip/dns.h"
#include "lwip/tcpip.h"

// ── Config ──────────────────────────────────────────────────────────────────
#define SERIAL_BAUD   4000000
#define SLIP_MTU      1500
#define WIFI_TIMEOUT  20000

// SLIP special bytes (RFC 1055)
#define SLIP_END      0xC0
#define SLIP_ESC      0xDB
#define SLIP_ESC_END  0xDC
#define SLIP_ESC_ESC  0xDD

// Point-to-point addresses for the SLIP link
#define SLIP_LOCAL_IP   "10.0.0.1"   // ESP32 side
#define SLIP_PEER_IP    "10.0.0.2"   // Host side
#define SLIP_NETMASK    "255.255.255.252"

// ── Globals ─────────────────────────────────────────────────────────────────
static bool slipMode = false;
static struct netif slip_netif;
static uint8_t slip_rx_buf[SLIP_MTU + 100];
static int slip_rx_len = 0;
static bool slip_esc_flag = false;

// Config mode
static char cmdBuffer[4096];
static int cmdIndex = 0;

// ── SLIP output: lwIP → Serial ──────────────────────────────────────────────
static err_t slip_netif_output(struct netif *nif, struct pbuf *p, const ip4_addr_t *ipaddr) {
  Serial.write(SLIP_END);
  for (struct pbuf *q = p; q != NULL; q = q->next) {
    uint8_t *data = (uint8_t *)q->payload;
    for (uint16_t i = 0; i < q->len; i++) {
      switch (data[i]) {
        case SLIP_END:
          Serial.write(SLIP_ESC);
          Serial.write(SLIP_ESC_END);
          break;
        case SLIP_ESC:
          Serial.write(SLIP_ESC);
          Serial.write(SLIP_ESC_ESC);
          break;
        default:
          Serial.write(data[i]);
          break;
      }
    }
  }
  Serial.write(SLIP_END);
  return ERR_OK;
}

static err_t slip_netif_init(struct netif *nif) {
  nif->name[0] = 's';
  nif->name[1] = 'l';
  nif->output = slip_netif_output;
  nif->mtu = SLIP_MTU;
  nif->flags |= NETIF_FLAG_LINK_UP | NETIF_FLAG_UP;
  return ERR_OK;
}

// ── SLIP input: Serial → lwIP ───────────────────────────────────────────────
static void slip_process_packet(uint8_t *data, int len) {
  if (len <= 0) return;
  struct pbuf *p = pbuf_alloc(PBUF_RAW, len, PBUF_RAM);
  if (p != NULL) {
    memcpy(p->payload, data, len);
    // Use tcpip_input to dispatch through the lwIP thread properly
    if (tcpip_input(p, &slip_netif) != ERR_OK) {
      pbuf_free(p);
    }
  }
}

static void slip_serial_rx() {
  while (Serial.available()) {
    uint8_t c = Serial.read();
    if (slip_esc_flag) {
      slip_esc_flag = false;
      switch (c) {
        case SLIP_ESC_END: c = SLIP_END; break;
        case SLIP_ESC_ESC: c = SLIP_ESC; break;
        default: break;
      }
      if (slip_rx_len < SLIP_MTU) {
        slip_rx_buf[slip_rx_len++] = c;
      }
    } else {
      switch (c) {
        case SLIP_END:
          if (slip_rx_len > 0) {
            slip_process_packet(slip_rx_buf, slip_rx_len);
            slip_rx_len = 0;
          }
          break;
        case SLIP_ESC:
          slip_esc_flag = true;
          break;
        default:
          if (slip_rx_len < SLIP_MTU) {
            slip_rx_buf[slip_rx_len++] = c;
          }
          break;
      }
    }
  }
}

// ── Start SLIP netif and enable NAPT ────────────────────────────────────────
static bool start_slip() {
  ip4_addr_t ip, mask, gw;
  ip4addr_aton(SLIP_LOCAL_IP, &ip);
  ip4addr_aton(SLIP_NETMASK, &mask);
  ip4addr_aton("0.0.0.0", &gw);

  // Add the SLIP netif to lwIP (must hold TCPIP core lock)
  LOCK_TCPIP_CORE();
  struct netif *result = netif_add(&slip_netif, &ip, &mask, &gw, NULL, slip_netif_init, tcpip_input);
  if (result != NULL) {
    netif_set_up(&slip_netif);
    netif_set_link_up(&slip_netif);
  }
  UNLOCK_TCPIP_CORE();

  if (result == NULL) {
    return false;
  }

  // Enable NAPT on the WiFi STA netif — packets from SLIP (10.0.0.x)
  // get NATed to the WiFi STA IP before going out to the internet
  LOCK_TCPIP_CORE();
  // Find the WiFi STA netif (not our slip_netif)
  struct netif *sta_nif;
  for (sta_nif = netif_list; sta_nif != NULL; sta_nif = sta_nif->next) {
    if (sta_nif != &slip_netif && (sta_nif->flags & NETIF_FLAG_UP)) {
      ip_napt_enable_netif(sta_nif, 1);
      break;
    }
  }
  UNLOCK_TCPIP_CORE();

  // Enable IP forwarding
  // (lwIP handles this when NAPT is enabled)

  return true;
}

// ── JSON helper ─────────────────────────────────────────────────────────────
String jsonGet(const String &json, const String &key) {
  String search = "\"" + key + "\":";
  int idx = json.indexOf(search);
  if (idx == -1) return "";
  idx += search.length();
  while (idx < (int)json.length() && json[idx] == ' ') idx++;
  if (json[idx] == '"') {
    int start = idx + 1;
    int end = json.indexOf('"', start);
    if (end == -1) return "";
    return json.substring(start, end);
  } else {
    int start = idx;
    int end = start;
    while (end < (int)json.length() && json[end] != ',' && json[end] != '}' && json[end] != ' ') end++;
    return json.substring(start, end);
  }
}

// ── Config mode commands ────────────────────────────────────────────────────
void cmdScan() {
  Serial.println("{\"event\":\"scanning\"}");
  int n = WiFi.scanNetworks();
  String r = "{\"action\":\"scan\",\"networks\":[";
  for (int i = 0; i < n; i++) {
    if (i > 0) r += ",";
    r += "{\"ssid\":\"" + WiFi.SSID(i) + "\",\"rssi\":" + String(WiFi.RSSI(i))
       + ",\"ch\":" + String(WiFi.channel(i))
       + ",\"enc\":" + String(WiFi.encryptionType(i)) + "}";
  }
  r += "],\"count\":" + String(n) + "}";
  Serial.println(r);
  WiFi.scanDelete();
}

void cmdConnect(const String &ssid, const String &pass) {
  WiFi.disconnect();
  delay(100);
  if (pass.length() > 0)
    WiFi.begin(ssid.c_str(), pass.c_str());
  else
    WiFi.begin(ssid.c_str());

  Serial.println("{\"event\":\"connecting\",\"ssid\":\"" + ssid + "\"}");
  unsigned long t0 = millis();
  while (!WiFi.isConnected() && (millis() - t0) < WIFI_TIMEOUT) delay(250);

  if (WiFi.isConnected()) {
    Serial.println("{\"action\":\"connect\",\"status\":\"connected\""
      ",\"ip\":\"" + WiFi.localIP().toString() + "\""
      ",\"gw\":\"" + WiFi.gatewayIP().toString() + "\""
      ",\"dns\":\"" + WiFi.dnsIP().toString() + "\""
      ",\"rssi\":" + String(WiFi.RSSI()) + "}");
  } else {
    WiFi.disconnect();
    Serial.println("{\"action\":\"connect\",\"status\":\"failed\"}");
  }
}

void cmdConnectEnterprise(const String &ssid, const String &user, const String &pass, const String &identity) {
  WiFi.disconnect(true);
  delay(200);
  WiFi.mode(WIFI_STA);

  esp_eap_client_set_identity((uint8_t *)user.c_str(), user.length());
  if (identity.length() > 0) {
    esp_eap_client_set_identity((uint8_t *)identity.c_str(), identity.length());
  }
  esp_eap_client_set_username((uint8_t *)user.c_str(), user.length());
  esp_eap_client_set_password((uint8_t *)pass.c_str(), pass.length());
  esp_wifi_sta_enterprise_enable();

  WiFi.begin(ssid.c_str());
  Serial.println("{\"event\":\"connecting\",\"ssid\":\"" + ssid + "\",\"mode\":\"enterprise\"}");

  unsigned long t0 = millis();
  while (!WiFi.isConnected() && (millis() - t0) < WIFI_TIMEOUT) delay(250);

  if (WiFi.isConnected()) {
    Serial.println("{\"action\":\"connect\",\"status\":\"connected\",\"mode\":\"enterprise\""
      ",\"ip\":\"" + WiFi.localIP().toString() + "\""
      ",\"gw\":\"" + WiFi.gatewayIP().toString() + "\""
      ",\"dns\":\"" + WiFi.dnsIP().toString() + "\""
      ",\"rssi\":" + String(WiFi.RSSI()) + "}");
  } else {
    WiFi.disconnect(true);
    esp_wifi_sta_enterprise_disable();
    Serial.println("{\"action\":\"connect\",\"status\":\"failed\",\"mode\":\"enterprise\"}");
  }
}

void cmdStartSlip() {
  if (!WiFi.isConnected()) {
    Serial.println("{\"error\":\"not_connected\"}");
    return;
  }

  // Gather DNS info to pass to host before switching to SLIP
  String dns1 = WiFi.dnsIP(0).toString();
  String dns2 = WiFi.dnsIP(1).toString();

  if (start_slip()) {
    Serial.println("{\"action\":\"startslip\",\"status\":\"ok\""
      ",\"slip_ip\":\"" SLIP_LOCAL_IP "\""
      ",\"peer_ip\":\"" SLIP_PEER_IP "\""
      ",\"mask\":\"" SLIP_NETMASK "\""
      ",\"dns1\":\"" + dns1 + "\""
      ",\"dns2\":\"" + dns2 + "\""
      ",\"wifi_ip\":\"" + WiFi.localIP().toString() + "\""
      "}");
    Serial.flush();
    delay(100);
    slipMode = true;
  } else {
    Serial.println("{\"action\":\"startslip\",\"status\":\"failed\"}");
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
    WiFi.disconnect();
    Serial.println("{\"action\":\"disconnect\",\"status\":\"ok\"}");
  } else if (action == "status") {
    if (WiFi.isConnected()) {
      Serial.println("{\"action\":\"status\",\"connected\":true"
        ",\"ip\":\"" + WiFi.localIP().toString() + "\""
        ",\"rssi\":" + String(WiFi.RSSI()) + "}");
    } else {
      Serial.println("{\"action\":\"status\",\"connected\":false}");
    }
  } else if (action == "startslip") {
    cmdStartSlip();
  } else if (action == "ping") {
    Serial.println("{\"action\":\"ping\",\"response\":\"pong\"}");
  } else {
    Serial.println("{\"error\":\"unknown_action\"}");
  }
}

// ── Arduino setup/loop ──────────────────────────────────────────────────────
void setup() {
  Serial.begin(SERIAL_BAUD);
  while (!Serial) delay(10);

  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);

  Serial.println("{\"event\":\"ready\",\"firmware\":\"wifi-slip\",\"version\":\"2.0\",\"baud\":" + String(SERIAL_BAUD) + "}");
}

void loop() {
  if (slipMode) {
    // Phase 2: SLIP packet forwarding
    slip_serial_rx();
  } else {
    // Phase 1: JSON config commands
    while (Serial.available()) {
      char c = Serial.read();
      if (c == '\n' || c == '\r') {
        if (cmdIndex > 0) {
          cmdBuffer[cmdIndex] = '\0';
          processCommand(String(cmdBuffer));
          cmdIndex = 0;
        }
      } else if (cmdIndex < (int)sizeof(cmdBuffer) - 1) {
        cmdBuffer[cmdIndex++] = c;
      }
    }
  }
  if (!slipMode) delay(1);
}
