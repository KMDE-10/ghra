# GHRA — Motorized Trolley Control System

> **Reading order for a new maintainer:** §1 *What this is* → §3 *Topology & networking* → §6 *Bringing the system back up after the Ubuntu update* → §9 *Known hiccups*. Then dip into the rest. §12 covers historical context for parts of the tree that look unused.

This repository drives a 230 V AC motorized trolley (TD10A "Laufkatze") that runs on an IPE 140 I‑beam. The trolley speed is set via a 0–10 V analog signal into a frequency inverter (VFD); direction is set by two relays; carriage position is read by a laser distance sensor pointed at a reflector at the rail end. A web dashboard (PWA) and a wired pushbutton remote are the two ways of commanding it.

The repo is split into:

| Path | What it is |
|---|---|
| `firmware/esp32-motor/` | ESP32 #1 — DAC + relays + laser, on the trolley control box |
| `firmware/esp32-remote/` | ESP32 #2 — 3‑button wired remote (FWD / STOP / REV) |
| `firmware/esp32-laser-test/` | Stand‑alone sketch for bench‑testing the M01 laser |
| `docker/` | Three containers: Mosquitto, ROS 2 core + MQTT bridge, Vue dashboard + nginx |
| `docker/ros2-core/ghra_control/` | ROS 2 Python package: `control_node` + `mqtt_bridge` |
| `docker/web-dashboard/` | Vue 3 / Vite PWA, built into nginx static + WebSocket proxy |
| `esp32-wifi-stick/` | **Separate subsystem** — ESP32‑S3 USB‑to‑WiFi gateway giving the Ubuntu host internet access via SLIP+NAPT (used because NIC1 is consumed by the control LAN; see §3.3) |
| `hardware/` | BOM CSV, control‑box wiring SVGs, photos of inverter/wiring |
| `start.sh`, `ghra.service` | Boot/launch glue for the control PC |
| `serial_monitor.py` | Quick read/write helper for `/dev/ttyUSB0` at 115200 |

---

## 1. What it is, in one diagram

```
                     ┌──────────── 192.168.1.0/24  (Control LAN, NIC1) ────────────┐
                     │                                                              │
  Phone (PWA)        │       ┌──────────────────────────────────────┐              │
   browser ──Wi‑Fi───┤       │   Dell OptiPlex — Ubuntu 22.04        │              │
                     │       │   192.168.1.100 (static)              │              │
                     │       │                                       │              │
                     │       │   podman‑compose stack (network=host) │              │
                     │       │   ┌──────────────┐                    │              │
                     │       │   │ mosquitto    │  :1883  MQTT TCP   │              │
                     │       │   │              │  :9001  MQTT WSS   │              │
                     │       │   └──────────────┘                    │              │
                     │       │   ┌──────────────┐                    │              │
                     │       │   │ ros2-core    │  :9090  rosbridge  │              │
                     │       │   │  control_node│         WS         │              │
                     │       │   │  mqtt_bridge │                    │              │
                     │       │   └──────────────┘                    │              │
                     │       │   ┌──────────────┐                    │              │
                     │       │   │ web-dashboard│  :8443  HTTP       │              │
                     │       │   │ nginx + Vue  │  /mqtt  → :9001    │              │
                     │       │   │              │  /rosbridge → 9090 │              │
                     │       │   └──────────────┘                    │              │
                     │       └──────┬────────────────────────────────┘              │
                     │              │ NIC1 (control LAN)                            │
                     │              ▼                                               │
                     │      ┌────────────────┐                                      │
                     │      │  Switch (TP‑Link│                                     │
                     │      │  TL‑SG105 etc.) │                                     │
                     │      └──┬──────┬───────┘                                     │
                     │         │      │                                              │
                     │  ┌──────┘      └──────────┐                                   │
                     │  │                        │                                   │
                     │ ┌┴───────────────────┐   ┌┴────────────────────┐              │
                     │ │ ESP32 #1 — Motor    │   │ ESP32 #2 — Remote    │             │
                     │ │ 192.168.1.11        │   │ 192.168.1.12         │             │
                     │ │  W5500 over SPI     │   │  W5500 over SPI      │             │
                     │ │  GP8211S DAC (I²C)  │   │  3 × pushbuttons     │             │
                     │ │  Laser M01 (UART)   │   │   (FWD / STOP / REV) │             │
                     │ │  2 × Relay → VFD    │   │                      │             │
                     │ └─────────┬───────────┘   └──────────────────────┘             │
                     │           │ 0–10 V + FWD/REV contacts                          │
                     │           ▼                                                    │
                     │      VFD UX‑52  ──3‑phase──►  Trolley motor                   │
                     │           │                                                    │
                     │     ════════════ IPE 140 I‑beam ════════════                  │
                     │           ◄ laser ─── reflector ►                             │
                     └──────────────────────────────────────────────────────────────┘

   Optional: Internet access for the Dell OptiPlex
   ──────────────────────────────────────────────
   ESP32‑S3 USB stick ── SLIP@4M baud ── /dev/ttyACM0 ── espwifi0 (TUN, 10.0.0.0/30)
                                                      │
                                                      └── default route via 10.0.0.1 (NAPT on ESP32 STA)
                                                                                  │
                                                                                  └── iPhone Personal Hotspot ("Secondary Tiphone")
```

---

## 2. Design principles

- **Air‑gapped control LAN.** The `192.168.1.0/24` segment carries motor traffic and nothing else. There is intentionally no router on it. The MQTT broker runs without authentication (`allow_anonymous true`), which is acceptable because nothing else is on the wire and the LAN has no internet path.
- **Static everything.** No DHCP, no mDNS, no service discovery. The OptiPlex is `.100`, the motor ESP is `.11`, the remote ESP is `.12`. If you renumber the LAN you re‑flash both ESP32s.
- **One broker, one schema.** All command and telemetry traffic crosses Mosquitto over MQTT. The ROS 2 side mirrors the relevant topics for debugging and policy enforcement (limits, interlocks). The dashboard speaks MQTT‑over‑WebSocket directly to the broker through an nginx reverse proxy, *not* through ROS.
- **Containerized control plane.** Mosquitto, ROS 2 core, and the dashboard each run in their own Podman container in `network_mode: host` (DDS multicast and the various loopback hops require it). Containers are rebuilt on every `start.sh` so a clean reboot of the OptiPlex always reflects what is in the repo.
- **Two independent ways to drive the motor.** A wired pushbutton remote (low‑level, jogs at 7 % speed) for shop‑floor use, and a phone PWA (full speed range, position readout) for fine control. Both publish to the same MQTT topics — they are peers, not master/slave.
- **Hardware safety lives in firmware, not in ROS.** The 50 ms relay interlock and the 0–32767 DAC clamp are in the motor ESP32. The control_node only adds soft position limits.
- **Internet is a separate, optional path.** The OptiPlex has a second NIC reserved for it; in practice we use the ESP32‑S3 USB WiFi stick instead so the OptiPlex stays portable. Either way, that path is for `apt` and image pulls only — none of the runtime traffic goes near it.

---

## 3. Networking

The entire system lives on a flat IPv4 subnet **192.168.1.0/24** with the Dell OptiPlex as the broker host at **.100**. The control devices have *static* IPs hard‑coded in firmware. After a fresh OS / network‑config wipe, you reproduce the link by:

### 3.1 Static IPs that are baked into firmware (cannot be changed without re‑flashing)

| Device | IP | MAC (set in firmware) | Source |
|---|---|---|---|
| Dell OptiPlex (MQTT broker) | `192.168.1.100/24` | (host NIC, set in netplan) | `firmware/*/esp32-*.ino` → `mqtt_server` |
| ESP32 #1 — motor controller | `192.168.1.11/24` | `DE:AD:BE:EF:00:11` | `firmware/esp32-motor/esp32-motor.ino:43–44` |
| ESP32 #2 — remote | `192.168.1.12/24` | `DE:AD:BE:EF:00:12` | `firmware/esp32-remote/esp32-remote.ino:35–36` |
| Default gateway | `192.168.1.1` | — | both ESP32s, only used for ARP / outbound that never happens |
| DNS server | `192.168.1.1` | — | both ESP32s (unused — they don't resolve hostnames) |

Anything outside that subnet **will not** be reachable by the ESP32s. If you change the OptiPlex IP, you must re‑flash both ESP32s.

### 3.2 Linux side — what to configure on the OptiPlex

The OptiPlex has two NICs by design: one for the control LAN, one (optional) for internet via the ESP32‑S3 stick. The control NIC is the one that physically goes to the TP‑Link switch.

1. **Identify the two NICs** — `ip -br link`.

2. **Configure the control NIC with a static address** (no DHCP). With NetworkManager:

   ```bash
   nmcli con add type ethernet ifname <ctrl-nic> con-name ghra-control \
       ipv4.method manual ipv4.addresses 192.168.1.100/24 \
       ipv4.gateway "" ipv6.method disabled connection.autoconnect yes
   nmcli con up ghra-control
   ```

   *Do not set a default gateway on this connection* — the LAN has no internet and you don't want a black‑hole default route. Pin `ipv4.never-default yes` on the profile so NetworkManager never silently promotes it.

3. **Disable IPv6 / mDNS noise on this NIC** — Avahi can flood the segment and the ESP32s ignore it but the broker logs get noisy.

4. **Verify reachability** before launching containers:

   ```bash
   ping -c2 192.168.1.11    # motor ESP
   ping -c2 192.168.1.12    # remote ESP
   ```

   If pings fail, check (in this order):
   - cable & link LEDs on switch and W5500 modules,
   - ESP32 5 V DIN‑rail PSU,
   - W5500 reset wire to GPIO4 (a stuck reset = "W5500 not found!" in serial monitor),
   - the ESP32 serial console at 115200 (`python3 serial_monitor.py` after editing the device path) — the firmware prints `ETH IP: 192.168.1.x`, `ETH OK`, `MQTT connecting…`, `MQTT connected!`.

5. **Open MQTT locally** to confirm broker reachability:

   ```bash
   mosquitto_sub -h 192.168.1.100 -t '#' -v
   # expect periodic carriage/position from ESP32 #1
   ```

6. **Firewall** — if `ufw` is enabled, allow `1883/tcp`, `9001/tcp`, `8443/tcp` from `192.168.1.0/24`.

### 3.3 Internet on the OptiPlex via the ESP32‑S3 USB stick (`esp32-wifi-stick/`)

The control LAN is intentionally air‑gapped. To give the OptiPlex itself internet access (for `apt`, container builds, etc.) the project ships a homemade USB‑WiFi adapter:

- The ESP32‑S3 dev board runs `wifi_slip/wifi_slip.ino`. Phase 1 it speaks JSON over serial; phase 2 it acts as a SLIP+NAPT gateway.
- `setup_wifi.py` (run as root on the host) opens `/dev/ttyACM0` at **4 000 000 baud**, creates a TUN device `espwifi0`, point‑to‑point `10.0.0.2 ↔ 10.0.0.1`, and adds a default route via the ESP32.
- `start_wifi.sh` is the convenience wrapper: defaults to **SSID `Secondary Tiphone`, password `ganzgeheim`** on `/dev/ttyACM0`. Edit if the hotspot changed.

Things that go wrong here are catalogued in `esp32-wifi-stick/LEARNINGS.md`. The big ones:

- iPhone Personal Hotspot goes to sleep ~90 s after you leave the settings screen — keep that screen open while connecting.
- `/dev/ttyACM0` permissions: user must be in `dialout` (`sudo usermod -a -G dialout control && newgrp dialout`).
- DTR‑on‑open resets the ESP32. Run **once after each plug‑in**: `sudo stty -F /dev/ttyACM0 -hupcl`. The Python script also sets `~HUPCL` via termios as a fallback for baud rates `stty` can't handle.
- ICMP through NAPT is broken (lwIP limitation) — `ping 8.8.8.8` shows 100 % loss, but `curl https://...` works. Don't use ping to verify.
- `apt update` sometimes hangs through this path; workaround is to download `.deb`s with `curl` and `dpkg -i`. Or use `Acquire::ForceIPv4=true` in `/etc/apt/apt.conf.d/`.
- `/etc/resolv.conf` is rewritten by `setup_wifi.py` to prepend the upstream DNS. **`systemd-resolved`** on Ubuntu 22.04 will fight you — disable or mask it if DNS keeps reverting:

  ```bash
  sudo systemctl disable --now systemd-resolved
  sudo rm /etc/resolv.conf            # remove the symlink
  ```

If you need the WiFi stick at boot, mirror `ghra.service` with a separate `wifi-stick.service` that starts before the network‑online target.

### 3.4 No router, no WAN bridging

The control LAN does **not** route to the internet, and the WiFi stick does **not** bridge the two — it only gives the OptiPlex (and only the OptiPlex) outbound NAPT. ESP32s cannot reach the internet and don't need to.

---

## 4. Software versions / pinning reference

Pin these when reproducing the environment.

### 4.1 Ubuntu host

| Component | Version | Notes |
|---|---|---|
| Ubuntu | **22.04 LTS (Jammy)** | also runs under WSL for development |
| Podman | install per `copy.txt` (`devel:kubic:libcontainers:stable` xUbuntu_20.04 keyring) | `podman-compose` from pip in user site is what `start.sh` calls |
| `podman-compose` | whatever `pip3 install --user` resolves; tested with 1.0.x | Path in `ghra.service` is `/home/control/.local/bin/podman-compose` |
| systemd unit user | `control` | `WorkingDirectory=/home/control/ghra` |

### 4.2 Container images

| Image | Tag | Source |
|---|---|---|
| `eclipse-mosquitto` | `2` | Docker Hub. Listens 1883 (TCP) + 9001 (WebSocket), `allow_anonymous true` |
| `ros:humble-ros-base` | (built locally) | Used as base for `ros2-core`. Adds `ros-humble-rosbridge-suite`, `ros-humble-ros2cli`, `python3-pip`, `paho-mqtt` |
| `node` | `20-alpine` | Build stage for the dashboard |
| `nginx` | `alpine` | Runtime stage for the dashboard |

### 4.3 ROS 2 / Python

- ROS 2 distribution: **Humble Hawksbill**.
- DDS profile forced UDPv4 only (no shared memory) — the file is generated at container start in `entrypoint.sh` to `/tmp/fastdds_udp.xml`.
- `ROS_SECURITY_ENABLE=false` in the entrypoint (the broker is the trust boundary, not DDS).
- Python deps: `paho-mqtt` (broker bridge), `setuptools`, `rclpy`, `std_msgs`, `rosbridge_server`.
- Package metadata: `docker/ros2-core/ghra_control/package.xml` (v0.1.0), entry points `control_node` and `mqtt_bridge`.

### 4.4 Web dashboard

| Package | Version |
|---|---|
| `vue` | `^3.4.0` |
| `vite` | `^5.0.0` |
| `@vitejs/plugin-vue` | `^5.0.0` |
| `mqtt` (mqtt.js) | `^5.3.0` |

`docker/web-dashboard/src/package.json` holds the source of truth. Bumping these majors has historically broken the build on Node 20 alpine — re‑pin if you upgrade.

### 4.5 ESP32 firmware

- **Arduino core**: `esp32:esp32` v3.3.7 (latest stable). Fall back to `2.0.17` if a library refuses to compile (`arduino-cli core install esp32:esp32@2.0.17`).
- FQBN motor + remote: `esp32:esp32:esp32` (ESP32‑WROOM‑32D Plus Kit).
- FQBN WiFi stick: `esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=default` — `hwcdc/cdc` does **not** work on this dev board (USB‑C is wired to the CH343, not native USB).
- Libraries (Arduino library manager names):
  - `Ethernet` (W5500) — built‑in via `Ethernet.h`
  - `PubSubClient` — Nick O'Leary, used for MQTT
  - `DFRobot_GP8XXX` v1.1.0 — DAC
  - `SoftwareSerial` (ESP32 fork) — for the laser
  - `WiFi`, `HTTPClient`, `WiFiClientSecure`, `esp_eap_client`, lwIP NAPT — only used by the WiFi stick

---

## 5. ROS 2 / MQTT topic map

The bridge in `mqtt_bridge.py` mirrors selected MQTT topics into ROS 2 and back. Topics are the same name on both sides except for the leading slash on the ROS side.

| MQTT topic | ROS topic | Type | Direction | Producer | Consumer |
|---|---|---|---|---|---|
| `motor/speed_cmd` | `/motor/speed_cmd` | float (0.0–1.0) | dashboard / remote → motor | dashboard, remote ESP | motor ESP, control_node |
| `motor/direction` | `/motor/direction` | int (`-1`/`0`/`1`) | dashboard / remote → motor | dashboard, remote ESP | motor ESP, control_node |
| `motor/enable` | `/motor/enable` | `'1'`/`'0'` | dashboard / remote → motor | dashboard, remote ESP | motor ESP, control_node |
| `motor/speed_feedback` | `/motor/speed_feedback` | float | motor → dashboard (currently unused) | — | dashboard |
| `carriage/position` | `/carriage/position` | float (mm) | sensor → all | motor ESP (laser) | dashboard, control_node |

`control_node.py` (`docker/ros2-core/ghra_control/ghra_control/control_node.py`) enforces position limits (`100 mm` … `15 000 mm`) on `/carriage/position` and re‑publishes a stop. Speed presets `speed_fast=1.0`, `speed_slow=0.3`. These are ROS parameters (`docker/ros2-core/ghra_control/launch/ghra_launch.py:23`).

---

## 6. Bringing the system back up after the Ubuntu update

Recipe for the missing‑networking situation:

1. **Verify host shell**: `whoami` should be `control`; `~` should be `/home/control`. Group: `id` includes `dialout`. If not: `sudo usermod -a -G dialout control` and re‑login.
2. **Re‑install Podman** if `podman --version` fails. Snippet from `copy.txt` is the working set of repository keys (the Kubic libcontainers source).
3. **Re‑install `podman-compose`**: `pip3 install --user podman-compose`. Confirm the binary at `/home/control/.local/bin/podman-compose` (this is what `ghra.service` expects).
4. **Re‑apply control LAN config** (§3.2).
5. **Bring up the stack**: `bash /home/control/ghra/start.sh`. The script tears down old containers, builds, launches, and runs three TCP probes (MQTT 1883, dashboard 8443).
6. **(Optional) Internet via WiFi stick**: in a separate terminal `sudo /home/control/esp32-wifi-stick/start_wifi.sh`. Persist the ESP32‑S3 anti‑reset:
   ```bash
   sudo stty -F /dev/ttyACM0 -hupcl
   ```
   This needs to be re‑run after every USB re‑plug.
7. **Re‑enable the systemd unit** (`ghra.service` lives in repo root — it expects to be at `/etc/systemd/system/ghra.service`):
   ```bash
   sudo cp /home/control/ghra/ghra.service /etc/systemd/system/
   sudo systemctl daemon-reload
   sudo systemctl enable --now ghra
   ```
8. **Phone access**: connect the phone to the same control LAN (the project assumes a separate Wi‑Fi AP cabled into the switch — the AP is *not* configured by anything in this repo, see §10), then open `http://192.168.1.100:8443/`. "Add to Home Screen" for the PWA experience.

---

## 7. Wiring quick reference

### 7.1 ESP32 #1 — motor controller (`firmware/esp32-motor/esp32-motor.ino`)

| Function | ESP32 GPIO | Peripheral pin |
|---|---|---|
| W5500 SCLK | 18 | SCK |
| W5500 MISO | 19 | MISO |
| W5500 MOSI | 23 | MOSI |
| W5500 CS | 5 | CS |
| W5500 RST | 4 | RST (active LOW pulse 50 ms at boot) |
| GP8211S DAC SDA | 21 | SDA (I²C addr `0x58`) |
| GP8211S DAC SCL | 22 | SCL |
| Laser M01 RX (data **from** laser) | 33 | TXD (yellow) |
| Laser M01 TX (data **to** laser) | 32 | RXD (green) |
| Relay FWD (active HIGH) | 25 | VFD FWD ↔ COM |
| Relay REV (active HIGH) | 26 | VFD REV ↔ COM |
| ID strap | 27 | held HIGH at boot — used to identify motor box |

Direction switching uses a 50 ms both‑off interlock (`RELAY_INTERLOCK_DELAY_MS`). Speed updates push a 15‑bit value (0–32767) into the DAC, which outputs 0–10 V on its `VOUT` straight to VFD `AVI`; DAC `GND` to VFD `ACM`.

### 7.2 ESP32 #2 — wired remote (`firmware/esp32-remote/esp32-remote.ino`)

Same W5500 wiring. Buttons (active LOW, internal pull‑ups, 20 ms debounce):

| Button | GPIO | MQTT effect |
|---|---|---|
| FWD | 12 | `motor/direction=1`, `speed_cmd=0.07`, `enable=1` |
| STOP | 14 | `speed_cmd=0`, `enable=0` |
| REV | 27 | `motor/direction=-1`, `speed_cmd=0.07`, `enable=1` |

⚠ GPIO 14 outputs PWM during boot. The internal pull‑up combined with the debounce window suppresses the spurious pulse, but if STOP ever appears to "self‑press" right after boot, the candidate cause is documented in §9.

### 7.3 VFD UX‑52 parameters

Set on the keypad before the first run:
- speed source = analog AVI (0–10 V),
- run source = terminals (FWD/REV),
- max frequency = whatever matches the trolley gearbox.

Terminals: `R,T` ← 230 V mains; `U,V,W` → motor (delta for 220 V); `AVI ← DAC VOUT`, `ACM ← DAC GND`; `FWD ← Relay1 NO`, `REV ← Relay2 NO`, both relays' COM tied to VFD COM; `PE` to earth.

### 7.4 Laser M01 power (don't skip this)

The M01 draws ≈300 mA peak, which the ESP32's onboard 3.3 V LDO cannot supply reliably. Use a separate 3.3 V DIN‑rail PSU or LDO from the 5 V rail. Symptom of an under‑powered laser: brown‑outs on the ESP32 mid‑measurement, distance reads `0` or `-1`, or the W5500 link light dies because the 3.3 V rail collapsed.

---

## 8. Building, deploying, debugging

### 8.1 Containers (control PC)

```bash
cd /home/control/ghra
./start.sh                       # tear down, rebuild, bring up
podman ps                        # confirm 3 containers running
podman logs -f ghra-ros2-core    # live log of MQTT bridge + control_node
podman logs -f ghra-mosquitto
podman logs -f ghra-web-dashboard
```

### 8.2 Firmware (motor / remote)

Both sketches use Arduino IDE or `arduino-cli`:

```bash
arduino-cli compile --fqbn esp32:esp32:esp32 firmware/esp32-motor
arduino-cli upload  --fqbn esp32:esp32:esp32 -p /dev/ttyUSB0 firmware/esp32-motor
```

The CH340C on these ESP32 Plus Kits enumerates as `/dev/ttyUSB*` (different from the WiFi‑stick's CH343 on `/dev/ttyACM*`).

### 8.3 Talking to the running motor ESP

After flash, the firmware accepts these single‑word commands on the USB serial console at 115200 baud (see `handleSerialCommand()` in `esp32-motor.ino:252`):

| Cmd | Effect |
|---|---|
| `speed 0.5` | force `current_speed=0.5`, `motor_enabled=true`, push DAC |
| `fwd` / `rev` / `stop` | direction control bypassing MQTT |
| `status` | print `en=… spd=… dir=… mqtt=…` |
| `i2c` | scan I²C bus, then drive DAC to 5 V |
| `dac0` | drive DAC to 0 V |

Useful for bench testing without the broker. There is no equivalent CLI on the remote (it's just buttons).

### 8.4 Watching MQTT

```bash
mosquitto_sub -h 192.168.1.100 -t 'motor/#' -t 'carriage/#' -v
```

You should see `carriage/position <mm>` at ~1 Hz from the laser. Pressing remote buttons should produce a flurry of `motor/direction`, `motor/speed_cmd`, `motor/enable`.

---

## 9. Hiccups (the field guide)

These are real things that have bitten the project. Adding to this list when something new bites is the easiest form of bug tracking.

### 9.1 Networking

- **`192.168.1.100` is the only address the firmware will talk to.** If you renumber the LAN, you must re‑flash both ESP32s (`firmware/*/esp32-*.ino`, the `mqtt_server` line). There is no DHCP, no mDNS discovery.
- **Two NICs on the OptiPlex must not both have a default gateway.** Only the WiFi‑stick TUN should provide one; the control NIC has none.
- **`network_mode: host`** is mandatory for all three containers — the dashboard loops back to `127.0.0.1:9001` and `127.0.0.1:9090`, and Mosquitto listens on `0.0.0.0`. Switching to a Podman pod or bridge network will silently break MQTT‑over‑WebSocket from the browser.
- **Mosquitto runs anonymous.** Anyone on the LAN can publish to `motor/*`. Acceptable only because the LAN has no other devices and no internet path. Re‑adding ACLs is `mosquitto.conf`'s `password_file` directive.

### 9.2 ESP32

- **GPIO 14 PWM at boot** on ESP32‑WROOM‑32D. Used as the STOP button on ESP32 #2. Internal pull‑up + 20 ms debounce currently masks it. If STOP ever appears spurious immediately after power‑up, add an external 10 kΩ pull‑up and a 100 nF cap to ground.
- **W5500 reset wire**. If `Ethernet.hardwareStatus() == EthernetNoHardware` is logged, GPIO 4 may not have pulsed the W5500's RST line at boot — verify the wire and that the module's own LDO regulator isn't bypassed.
- **5 V supply current**. The ESP32 + W5500 + relays + laser pull >500 mA peaks. Use the 15 W DIN PSU, not a 5 W phone charger.
- **Arduino core 3.x vs 2.x.** Today's MQTT path is core‑version agnostic, but if a library complains, downgrade to 2.0.17.
- **CH340 vs CH343.** The motor/remote ESP32 Plus Kits use **CH340C** (`/dev/ttyUSB0`); the WiFi‑stick ESP32‑S3 uses **CH343** (`/dev/ttyACM0`). They are not interchangeable in udev rules or scripts.

### 9.3 Laser sensor (M01)

- The sensor talks **9600 baud** over `SoftwareSerial` (`esp32-motor.ino:210`, `esp32-laser-test.ino:36`).
- The firmware re‑sends the *continuous mode* command every 10 s (`last_laser_wake_ms` in `loop()`), because the M01 silently times out otherwise. Don't shorten this faster than the response window or you'll corrupt frames.
- Distance is BCD‑encoded across `buf[6..9]`. Frames start with `0xAA`, length 13 bytes, checksum is the low byte of the sum of bytes `1..n‑2`. Drop frames where `buf[4]==0x00, buf[5]==0x04, func ∈ {0x20,0x21,0x22}`.

### 9.4 ESP32‑S3 WiFi stick

`esp32-wifi-stick/LEARNINGS.md` is the long‑form list. Headline issues:

- DTR resets ESP32 on every serial open — fix with `stty -F /dev/ttyACM0 -hupcl` once after each plug‑in (see §3.3).
- Max stable baud is **4 000 000**. 4 608 000 / 5M return nothing; 6M corrupts under sustained traffic.
- ICMP (ping) does not get NATed back through lwIP NAPT — this is *expected*, not a bug. Test internet with `curl https://example.com`.
- iPhone hotspot sleeps after ≈90 s. Keep the Personal Hotspot screen open while connecting.
- `apt update` through this path can hang; download `.deb`s via `curl` and `dpkg -i`. `Acquire::ForceIPv4=true` did *not* help.
- `/etc/resolv.conf` will be re‑clobbered by `systemd-resolved`. Either disable it (§3.3 step) or push the upstream DNS into the resolved config instead.
- AppIndicator visibility: GNOME Shell does not natively show TUN devices. Cosmetic only.

### 9.5 Containers / podman

- **Cross‑container DDS discovery** required disabling shared memory transport (`fastdds_udp.xml` in `entrypoint.sh:9`). Don't undo this — Podman containers cannot share `/dev/shm` reliably without extra mounts.
- The DDS profile file is recreated on every container start, so editing it on the host has no effect.
- `colcon build` runs at container build time; for fast iteration on `control_node.py`, mount the package as a volume instead of rebuilding.

### 9.6 Ubuntu update aftermath — what to recheck

Things the in‑place Ubuntu apt upgrade has touched in the past on similar systems:

- **`systemd-resolved`** re‑enabled, `/etc/resolv.conf` reverted to a 127.0.0.53 stub. Symptom: WiFi stick connects but DNS fails.
- **Group memberships** dropped for the `control` user (`dialout`, `plugdev`).
- **Podman / runc** ABI bump → containers won't start. Reinstall both from the Kubic repo (`copy.txt`).
- **`podman-compose` user pip install** wiped if Python minor version bumped. Re‑install with `pip3 install --user`.
- **NetworkManager** taking over the control NIC and adding a default route. Pin `ipv4.never-default yes ipv4.gateway ""` on the `ghra-control` profile.
- **Firewall**: `ufw` re‑enabled by some upgrade chains. If `mosquitto_sub` works on the host but not from another LAN device, check `sudo ufw status`. Allow `1883/tcp`, `9001/tcp`, `8443/tcp` from `192.168.1.0/24`.
- **udev rules** for serial devices may need refreshing if the kernel module names changed (CH343 has historically lived in both `cdc_acm` and a vendor module).

---

## 10. Things that are NOT in this repository (and should be backed up elsewhere)

When somebody tells you "the networking is missing", these are the off‑repo pieces worth checking the backup of:

- The **netplan / NetworkManager profile** for the OptiPlex's two NICs (control LAN + WiFi‑stick TUN). Stored in `/etc/netplan/*.yaml` or `/etc/NetworkManager/system-connections/`.
- The **Wi‑Fi access point** that the phone joins (model + admin password). The repo assumes one exists, cabled into the control switch. Anything that bridges Wi‑Fi to the wired control LAN works (a cheap travel router in AP mode, ideally with no internet uplink).
- The **systemd unit for the WiFi stick** if you want internet at boot. There is no template in the repo.

> ⚠ **`copy.txt`** in the repo root contains two GitHub Personal Access Tokens. They should be considered leaked — revoke them on github.com → Settings → Developer settings → Personal access tokens, and remove from the file. (They are not consumed by anything in the codebase.)

---

## 11. Bug‑tracking workflow

There is no issue tracker wired in. The recommended local flow:

1. Reproduce. Capture the relevant serial console (`python3 serial_monitor.py` after editing `/dev/ttyUSB0` to match) **and** the matching `podman logs` excerpt.
2. Locate the rough area:
   - "motor doesn't move" → `firmware/esp32-motor/esp32-motor.ino` + `mosquitto_sub motor/#`
   - "remote does nothing" → `firmware/esp32-remote/esp32-remote.ino` + same sub
   - "dashboard never connects" → browser DevTools console (look for `wss://… /mqtt` failure) + `podman logs ghra-mosquitto`
   - "carriage position is 0" → laser power supply + `podman logs ghra-mosquitto | grep carriage`
3. Add an entry to §9 of this README with the symptom, the root cause, and the fix.
4. For anything that could cost a re‑flash or take an ESP32 offline, push a commit on a topic branch first; `git log --oneline` is currently the only audit trail.

---

## 12. Historical development

This section exists to demystify files and references in the tree that look orphaned. None of this is part of the running system — it is here to spare future‑you a wild goose chase.

### 12.1 The original ROS 2 / DDS / SROS2 plan

`docs/system-plan.md` is the project's *original* design document. It describes a system built on **micro‑ROS over UDP DDS**, secured end‑to‑end with **SROS2** (mutual TLS between every ROS node, including the ESP32s), with the dashboard served over **HTTPS + basic auth** and the rosbridge connection tunneled through **WSS**. That is *not* what runs today — but the document was the project's north star for several months and is still useful for understanding why certain pieces exist.

The pivot to the current MQTT‑based stack happened in commit `45688bd` ("changed everything to mqtt instead of dds for ros"). Reasons in roughly the order they mattered:

- micro‑ROS over Ethernet on the ESP32 was painful to keep stable across `arduino-esp32` 2.x → 3.x.
- DDS multicast across Podman containers needed `network_mode: host` *and* a custom Fast‑DDS XML profile to suppress shared‑memory transport — this part survived the pivot (see `entrypoint.sh`).
- The dashboard side needed roslibjs + rosbridge + WSS proxy to reach DDS; replacing all of that with the browser's native `mqtt.js` was a one‑evening simplification.
- SROS2 keystore management (`ros2 security create_enclave …` per node, plus baking matching keys into ESP32 firmware) was disproportionately heavy for a single air‑gapped LAN.

### 12.2 The 4‑button wireless remote

The original plan had **ESP32 #2 connect over Wi‑Fi to a dedicated control AP** and present **four buttons**: FwdFast / FwdSlow / BwdSlow / BwdFast, each with LED feedback. Speed was meant to be selected by which button was held; releasing any button stopped the motor. The control_node was responsible for mapping `BUTTON_*` events to `(speed, direction)` pairs.

The remote was rewired to **W5500 wired Ethernet with three buttons** (FWD / STOP / REV) at a fixed 7 % speed because:

- Wi‑Fi roaming / reconnect timing was unreliable inside the steel structure.
- A wired remote is one fewer power‑management problem in a battery‑less enclosure.
- Three buttons map cleanly to the three things an operator actually does at the rail.

`control_node.py` still has subscriber code for the old `/remote/button_event` topic and the `BUTTON_FWD_FAST` / `BUTTON_FWD_SLOW` / `BUTTON_BWD_SLOW` / `BUTTON_BWD_FAST` constants. Nothing publishes that topic anymore — the wired remote writes MQTT directly. The code is harmless; left in place as a starting point if a multi‑speed remote is ever revived.

### 12.3 The JRT laser sensor

`docs/system-plan.md` calls for a JRT‑style laser at **UART2 / 19200 baud** with a hex command set documented inline. The actually‑procured sensor turned out to be an **M01** that uses **9600 baud** with a different (BCD‑encoded) framing. The bench sketch in `firmware/esp32-laser-test/` was written specifically to characterise the M01 protocol; the result is reflected in `esp32-motor.ino`'s `laserProcess()`.

### 12.4 SROS2 keystore (`docker/ros2-core/security/keystore-old/`)

These are the certificates generated for the original SROS2 plan: a CA, governance/permissions PKCS#7 envelopes, and per‑node enclaves for `/ghra_control_node`, `/ghra_motor_esp32`, `/ghra_remote_esp32`, `/rosbridge_websocket`. They are **not loaded** by the current `entrypoint.sh` (`ROS_SECURITY_ENABLE=false` is set explicitly). They are kept under the `keystore-old/` name so a future re‑enable can reuse them as a starting point. The generator (`generate_sros2_keys.sh`) still works.

### 12.5 TLS / basic auth on the dashboard

`docker/web-dashboard/certs/generate_tls_certs.sh` and `docker/web-dashboard/generate_htpasswd.sh` produce the artefacts a hardened dashboard would need. They are *not wired into* `nginx.conf` (which currently `listen 8443` plain HTTP, no `auth_basic`). To re‑enable:

- regenerate `server.crt` / `server.key` and `.htpasswd`,
- mount them into the container (extend `Dockerfile` and `docker-compose.yml`),
- change the nginx server block to `listen 8443 ssl;` and add `ssl_certificate*` + `auth_basic` directives.

Trade‑off: the phone PWA stops being installable without a trusted certificate, so this is only worth doing if a real CA cert (Let's Encrypt via DNS‑01, or an internal CA distributed to phones) is available.

### 12.6 First‑gen WiFi stick firmware (`esp32-wifi-stick/wifi_stick/`)

`wifi_stick.ino` was the first attempt at a USB‑WiFi adapter — it spoke a JSON command/response protocol over serial (`scan`, `connect`, `http`, `dns`, `ping`). `wifi_host.py` is its companion CLI. It worked but wasn't a real network device on the host: every TCP connection had to be marshalled through JSON.

`wifi_slip/wifi_slip.ino` (the **current** firmware on the stick) replaced it: phase‑1 it speaks the same JSON for scanning/connecting, phase‑2 it switches to SLIP and acts as a NAT'd network interface (`espwifi0`) on the host. The first‑gen `wifi_stick.ino` and `wifi_host.py` are kept around for one‑off WiFi diagnostics — `wifi_host.py scan` is occasionally useful to see what's in the air without bringing the TUN device up.

### 12.7 Git history quick map

For context when you `git log`:

- `4d9efcb Initial commit` — empty scaffolding.
- `9556d4a Add ROS2 motor control system` … `b6f3ce2 Update system plan` — the original DDS / SROS2 / HTTPS design landing.
- `cf03f43 Update firmware for new ESP32 network API …` — port to arduino‑esp32 3.x.
- `b8f0312` … `4f5d741 new files for stick` — birth of the WiFi stick.
- `c981b88 worked on it working` … `4f033b5 ...working but slow` — first‑gen WiFi stick + JSON CLI.
- `45688bd changed everything to mqtt instead of dds for ros` — **the pivot**. Everything before this commit assumes the DDS plan; everything after assumes MQTT.
- `acdaea7 moved files`, `f910ea2 added start.sh` — post‑pivot tidy.
- `42d7e5b final for today` … `683a38c worked on the remote` — ongoing reliability work on the inverter, laser, and the wired remote.
