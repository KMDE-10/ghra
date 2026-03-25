# ESP32-S3 USB WiFi Adapter — Learnings

## Hardware

### Board Identification
- The ESP32-S3 dev board uses a **CH343 USB-to-UART bridge** (VID `1a86`, PID `55d3`), appearing as `/dev/ttyACM0` via the `cdc_acm` driver.
- This is NOT the ESP32-S3's native USB — it's an external UART chip. The native USB pins (GPIO19/20) are on headers but not connected to the USB-C port.
- `lsusb` shows `QinHeng Electronics` for CH343 devices.
- The iPhone connected on the same USB bus shows as `Apple, Inc. iPhone5/5C/5S/6` (VID `05ac`, PID `12a8`).

### Serial Port Permissions
- `/dev/ttyACM0` is owned by `root:dialout`. User `control` was NOT in the `dialout` group.
- Fix: `sudo usermod -a -G dialout control` (permanent, needs re-login) + `sudo chmod 666 /dev/ttyACM0` (immediate).

### CH343 Baud Rate Limits
- **4,000,000 baud is the maximum stable rate** for the CH343 on this board.
- 2,000,000 works perfectly.
- 4,608,000 — no response.
- 5,000,000 — no response.
- 6,000,000 — responds to ping but crashes under sustained SLIP traffic (data corruption).
- The CH343 datasheet claims up to 6 Mbps, but real-world stability caps at 4M on this setup.

## Serial / DTR Reset Problem

### The Problem
The CH343 has DTR connected to the ESP32's EN (enable/reset) pin. Every time the serial port is opened or closed, pyserial toggles DTR, causing the ESP32 to **hard reset**.

### Failed Fixes
- `dsrdtr=False, rtscts=False` on `serial.Serial()` — inconsistent, sometimes still resets.
- `ser.setDTR(False)` after open — too late, the reset already happened during open.
- `ser.dtr = None` — doesn't work as intended.

### Working Fix
**`stty -F /dev/ttyACM0 -hupcl`** (disable hangup-on-close) — set this ONCE before any serial access. After this, opening/closing the port no longer toggles DTR. Works for both regular user and sudo.

**For baud rates > 4M that `stty` doesn't support**, use termios directly in Python:
```python
import termios
fd = ser.fileno()
attrs = termios.tcgetattr(fd)
attrs[2] = attrs[2] & ~termios.HUPCL
termios.tcsetattr(fd, termios.TCSANOW, attrs)
```

### Critical Implication
- If the ESP32 enters SLIP mode and the serial port is closed (triggering reset), the ESP32 reboots back to config mode. The host-side bridge loses connection.
- With `-hupcl`, the ESP32 stays in whatever mode it's in across serial port open/close cycles.
- But if the ESP32 IS stuck in SLIP mode and you need to reset it, you must re-upload firmware via `arduino-cli upload` (which uses esptool, which forces a reset via RTS).

## ESP32-S3 Arduino / ESP-IDF

### USB Mode Configuration
- `USBMode=default, CDCOnBoot=default` — Serial goes through UART0 (CH343). This is what works for our board.
- `USBMode=hwcdc, CDCOnBoot=cdc` — Serial goes through native USB CDC. Does NOT work on this board because the USB-C port is wired to the CH343, not native USB.
- When HW CDC is enabled, the boot ROM messages still come through UART0/CH343, but `Serial.println()` in the sketch goes to the native USB (which is unconnected). Result: you see boot messages but no firmware output.

### FQBN (Fully Qualified Board Name)
- Generic ESP32-S3: `esp32:esp32:esp32s3`
- With options: `esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default`

### lwIP Thread Safety
- **`netif_add()` MUST be called with `LOCK_TCPIP_CORE()` held.** Without it, you get:
  ```
  assert failed: netif_add /IDF/components/lwip/lwip/src/core/netif.c:297
  (Required to lock TCPIP core functionality!)
  ```
  This causes a hard crash and reboot.

### lwIP Packet Input
- When creating a custom netif, use `tcpip_input` (not `ip_input`) as the input function in `netif_add()`. This ensures packets are dispatched through the lwIP thread properly.
- For incoming packets, use `pbuf_alloc(PBUF_RAW, len, PBUF_RAM)` — not `PBUF_IP`.

### NAPT (Network Address Port Translation)
- ESP32 Arduino core 3.x (ESP-IDF 5.x) has NAPT support in lwIP.
- Header: `#include "lwip/lwip_napt.h"`
- Available APIs:
  ```c
  ip_napt_enable(u32_t addr, int enable);
  ip_napt_enable_no(u8_t number, int enable);
  ip_napt_enable_netif(struct netif *netif, int enable);  // cleanest
  ```
- Also available: `esp_netif_napt_enable(esp_netif_t *netif)` from `esp_netif.h`.
- NAPT works for **TCP and UDP** but **ICMP (ping) replies don't get NATed back** — this is a known lwIP NAPT limitation. Pings to external IPs show 100% loss, but TCP (curl, etc.) works fine.
- To enable: find the WiFi STA netif (not the SLIP netif) and call `ip_napt_enable_netif(sta_nif, 1)`.

### WPA2-Enterprise (eduroam)
- Old API (deprecated): `#include "esp_wpa2.h"` with `esp_wifi_sta_wpa2_ent_*` functions.
- New API: `#include "esp_eap_client.h"` with:
  ```c
  esp_eap_client_set_identity(...)
  esp_eap_client_set_username(...)
  esp_eap_client_set_password(...)
  esp_wifi_sta_enterprise_enable()
  ```
- Must call `WiFi.disconnect(true)` and `WiFi.mode(WIFI_STA)` before configuring enterprise auth.

## SLIP Protocol (RFC 1055)

### Framing
```
SLIP_END     = 0xC0  (frame delimiter)
SLIP_ESC     = 0xDB  (escape byte)
SLIP_ESC_END = 0xDC  (escaped END)
SLIP_ESC_ESC = 0xDD  (escaped ESC)
```
- Send `END` before and after each packet.
- Replace `END` bytes in payload with `ESC ESC_END`.
- Replace `ESC` bytes in payload with `ESC ESC_ESC`.

### TUN Device on Linux
- Open `/dev/net/tun` with `O_RDWR`.
- `ioctl(fd, TUNSETIFF, ...)` with `IFF_TUN | IFF_NO_PI` to create a TUN (IP-layer) device.
- `IFF_NO_PI` is critical — without it, each packet has a 4-byte header prepended that breaks IP parsing.
- Packets written to the TUN fd must be valid IP packets (IPv4 version nibble = 4, or IPv6 = 6). Invalid packets cause `OSError: [Errno 22] Invalid argument`.

### Bridge Architecture
```
Host App -> TUN (espwifi0) -> Python bridge -> SLIP encode -> Serial (CH343)
    -> ESP32 UART -> SLIP decode -> lwIP netif -> NAPT -> WiFi STA -> Internet
```
Return path is the reverse.

## iPhone Hotspot Behavior

- **iPhone hotspots go to sleep aggressively.** The SSID stops broadcasting ~90 seconds after you leave the Personal Hotspot settings screen.
- The ESP32 scan may show the hotspot (e.g., at -28 dBm), but connection attempts fail if the hotspot fell asleep between scan and connect.
- **Fix:** Keep the Personal Hotspot settings screen open on the iPhone during connection.
- Once connected, the hotspot stays alive as long as there's an active client.
- iPhone hotspot uses subnet `172.20.10.0/28`, gateway `172.20.10.1`, DHCP range starts at `.2`.
- DNS server is the gateway itself (`172.20.10.1`).

## NetworkManager Integration

### What Works
- NM automatically detects the TUN device and shows it in `nmcli device status` as `tun / connected`.
- Creating an NM connection profile with `nmcli connection add type tun ifname espwifi0 ...` registers it.

### What Doesn't Work
- **GNOME Shell's network indicator (top-right) does NOT show TUN devices.** It only renders WiFi, Ethernet, VPN, and Mobile Broadband device types.
- Using `nmcli connection up` on a TUN connection that's already manually configured can cause the bridge process to crash (NM reconfigures the interface, breaking the TUN fd).
- **AppIndicator3** (`gir1.2-appindicator3-0.1`) is needed for a custom system tray icon but was not installed and couldn't be fetched via apt (no network on main interface, and apt didn't route through the ESP32 bridge).

### Recommendation
- Configure the interface manually with `ip addr` / `ip route`.
- Create the NM connection profile with `connection.autoconnect no` just for visibility in `nmcli`.
- Don't call `nmcli connection up` — let manual config handle it.
- For GUI visibility, a custom AppIndicator script is needed (requires `gir1.2-appindicator3-0.1`).

## apt Routing Issue
- Even with `default via 10.0.0.1 dev espwifi0 metric 100` as the only default route, apt connections to `de.archive.ubuntu.com` timed out.
- `curl` worked fine through the same route.
- Likely cause: apt may be trying IPv6 first, or the NAPT doesn't handle apt's connection pattern well. Using `-o Acquire::ForceIPv4=true` didn't help.
- Workaround: download `.deb` files manually with `curl` and install with `dpkg -i`.

## Python / sudo Gotchas

### pyserial Under sudo
- `sudo python3` uses root's Python environment, which may not have `pyserial` installed.
- Fix: `sudo python3 -m pip install pyserial`.

### Buffered Output
- When running a Python script in the background via sudo, stdout may be fully buffered (no output visible).
- Fix: use `python3 -u` (unbuffered) flag.

### pip on Python 3.8
- `get-pip.py` from `bootstrap.pypa.io` requires Python 3.9+.
- For Python 3.8: use `https://bootstrap.pypa.io/pip/3.8/get-pip.py`.
- Install with `--user` flag to avoid needing root: `python3 get-pip.py --user`.

## arduino-cli

### Installation
- Install to user directory: `curl -fsSL https://raw.githubusercontent.com/arduino/arduino-cli/master/install.sh | BINDIR=~/.local/bin sh`
- The install script fails if you pass flags like `--dest` as part of the URL — use `BINDIR` env var instead.

### ESP32 Board Support
- Additional board URL: `https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/package_esp32_index.json`
- Core install: `arduino-cli core install esp32:esp32` (~311 MB download + toolchains).
- Board detection: `arduino-cli board list` — shows "Unknown" for ESP32-S3 on CH343, but upload works fine with explicit FQBN.

### Upload Failures
- "device reports readiness to read but returned no data" — another process has the serial port open. Fix: `fuser -k /dev/ttyACM0`.
- "The chip stopped responding" — ESP32 was busy (e.g., in SLIP mode sending data). Wait a moment and retry.
- "Invalid head of packet: Possible serial noise" — serial port in bad state. Wait and retry.

## Performance Summary

| Baud Rate | Throughput | Status |
|-----------|-----------|--------|
| 115,200 | ~11 KB/s | Works, too slow |
| 921,600 | ~90 KB/s | Failed after DTR reset (worked before hupcl fix) |
| 2,000,000 | ~200 KB/s | Stable |
| 4,000,000 | ~400 KB/s | **Max stable — use this** |
| 4,608,000 | ~460 KB/s | No response |
| 5,000,000 | ~500 KB/s | No response |
| 6,000,000 | ~600 KB/s | Ping works, crashes under load |

## File Structure

```
~/esp32-wifi-stick/
  wifi_stick/wifi_stick.ino    — Original serial-command firmware (v1, JSON protocol)
  wifi_slip/wifi_slip.ino      — SLIP gateway firmware (v2, creates network device)
  wifi_host.py                 — Interactive CLI for v1 firmware
  setup_wifi.py                — Host setup script for v2 (TUN bridge + routing)
```
