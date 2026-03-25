# GHRA - ROS2 Motor Control System Plan

## Context

Build a ROS2-based system to control an AC motor on a rail (motorized trolley / Laufkatze on IPE 140 I-beam). The motor speed is controlled via a VFD receiving a 0-10V analog signal from a DAC. Position feedback comes from a laser distance sensor. The system includes a web dashboard (PWA for phone access), a wireless 4-button remote, and full TLS encryption on all communications.

**Development**: Podman on WSL (Windows)
**Deployment**: Ubuntu 22.04 on Dell OptiPlex x86

---

## 1. Hardware Components (with verified specs)

### 1.1 Already Ordered (from CSV)

| Component | Specs | Qty | Role |
|---|---|---|---|
| Motorized Trolley TD10A | 230V, 1000kg capacity | 1 | AC motor on IPE 140 rail |
| **VFD** MCU UX-52 | 750W/1HP, 176-264V in, 3-phase 0-220V out, **0-10V analog input on AVI terminal**, FWD/REV digital inputs | 1 | Motor speed control |
| **DAC** DFRobot GP8211S (DFR1071) | 15-bit, I2C addr **0x58**, **0-10V output native** (built-in boost), 3.3-5V supply, library: DFRobot_GP8XXX | 1 | Analog signal to VFD |
| **ESP32-WROOM-32D** Plus Kit | Dual-core 240MHz, WiFi+BT, 2x SPI, I2C, 3x UART, USB-C (CH340C) | 4 | Microcontrollers |
| **W5500** Ethernet Module | SPI (up to 80MHz), 10/100Mbps, 3.3V/130mA, RJ45 w/ magnetics | 5 | Wired Ethernet for ESP32 |
| **Laser Distance Sensor** (JRT-type) | 0.03-40m, +/-1-2mm, **UART 19200 8N1**, **2.5-3.3V supply ~300mA**, hex protocol | 2 | Carriage position via UART |
| Red LED Pushbutton | Momentary, LED | 2 | Remote: Backward Slow + Backward Fast |
| Green LED Pushbutton | Momentary, LED | 2 | Remote: Forward Fast + Forward Slow |
| DIN Rail PSU 5V (HDR-15-5) | 5V output | 5 | Power for ESP32s |
| DIN Rail PSU 24V (HDR-15-24) | 24V output | 3 | Power for contactors |
| DIN Rail Relay 5V | 5V coil | 4 | VFD FWD/REV/enable control |
| Enclosures (large + small) | IP65 | 2+4 | Control boxes |
| Ethernet Cable 30m | Cat5e/6 | 2 | Network wiring |
| RJ45 Panel Mount Ports | | 4 | Enclosure connectors |
| Cable Throughhole Fittings | | 2 | Cable entry |
| Terminal Blocks (DIN rail) | | 1 set | Wiring |
| 220V Rubber Cable 30m | 1.5mm2 | 1 | Power to trolley |
| DIN Rail Breaker 16A | | 2 | Circuit protection |

### 1.2 Still Needed (NOT in CSV)

| Component | Why | Est. Cost |
|---|---|---|
| **Network Switch** (unmanaged, 5+ ports, e.g. TP-Link TL-SG105) | Connects Dell OptiPlex + ESP32 #1 (via W5500) on control LAN | ~15-25 EUR |
| **WiFi Access Point** (dedicated for control network) | ESP32 #2 (remote) + phone connect via WiFi; isolated from internet | ~20-40 EUR |
| **Mirror / Reflector** for laser sensor | Mounted at end of I-beam for distance measurement | ~5-10 EUR |
| **3.3V regulator or separate PSU for laser sensor** | Laser needs 2.5-3.3V @ 300mA; ESP32's 3.3V pin may not supply enough current reliably | ~2-5 EUR |
| **2-wire shielded cable** (for DAC to VFD analog signal) | 0-10V analog signal from DAC to VFD AVI/ACM terminals | ~5 EUR |
| **Wago/lever connectors or additional terminal blocks** | Clean wiring inside enclosures | ~5-10 EUR |

### 1.3 Verified: NOT Needed

| Component | Reason |
|---|---|
| ~~Op-amp / level shifter for DAC~~ | GP8211S outputs 0-10V natively with internal boost circuit |
| ~~USB-to-Serial adapter~~ | ESP32 Plus Kit has USB-C with CH340C onboard |
| ~~Bluetooth module / app~~ | Phone accesses web dashboard as PWA over control WiFi |

### 1.4 Future (Out of Scope)

- 2x Emergency Stop buttons (cuts power to motor + motor-control ESP)
- Contactors/Schutze for E-stop circuit (24V)

---

## 2. System Architecture

```
   Phone (PWA)──────────┐
   https://<ip>          │ Control WiFi
                         │
                        ┌─────────────────────────────────────────────────────┐
                        │         Dell OptiPlex (x86 Linux)                   │
                        │         Ubuntu 22.04 + Podman                       │
                        │         NIC1: Control net  NIC2: Internet           │
                        │                                                     │
                        │  ┌──────────────┐  ┌──────────────────────────┐    │
                        │  │ micro-ROS     │  │ ros2-core                │    │
                        │  │ Agent         │  │  control_node            │    │
                        │  │ (UDP:8888)    │  │  rosbridge (WS:9090)     │    │
                        │  └──────┬────────┘  └────────┬─────────────────┘    │
                        │         │  ROS2 DDS (SROS2 TLS encrypted)    │      │
                        │         └────────┬───────────┘               │      │
                        │  ┌───────────────┴────────────────────────┐  │      │
                        │  │ web-dashboard (Vue.js PWA + nginx)     │  │      │
                        │  │  HTTPS :443 + basic auth               │  │      │
                        │  │  WSS proxy → rosbridge :9090           │  │      │
                        │  └────────────────────────────────────────┘  │      │
                        └─────────────────┬────────────────────────────┘      │
                                          │ Ethernet (Control net)            │
                                    ┌─────┴─────┐     ┌─────────────┐        │
                                    │  Network   │     │ WiFi AP     │────────┘
                                    │  Switch    │─────│ (Control)   │
                                    └──┬─────┬──┘     └──────┬──────┘
                           Ethernet    │     │          WiFi  │
                                       │     │                │
         ┌─────────────────────────────┘     │      ┌─────────┘
         │                                   │      │
┌────────┴───────────────┐      ┌────────────┴──────┴──────┐
│ ESP32 #1 (Motor Box)   │      │ ESP32 #2 (Remote)        │
│ micro-ROS over UDP/ETH │      │ micro-ROS over WiFi      │
│                        │      │                           │
│ VSPI → W5500 (ETH)    │      │ GPIO12 ← Green1 (FwdF)   │
│ I2C  → GP8211S DAC    │      │ GPIO13 ← Green2 (FwdS)   │
│ UART2 ← Laser Sensor  │      │ GPIO14 ← Red1   (BwdS)   │
│ GPIO → Relays (FWD/REV)│     │ GPIO27 ← Red2   (BwdF)   │
└────────┬───────────────┘      └──────────────────────────┘
         │ 0-10V (VOUT → AVI)
         │ GND   (GND  → ACM)
         │ Relay → FWD terminal
         │ Relay → REV terminal
         ▼
   ┌───────────┐        ┌─────────────┐
   │  VFD       │──3ph──→│  AC Motor   │
   │  UX-52     │        │ (Trolley)   │
   │  750W      │        └──────┬──────┘
   └───────────┘               │
                          ═══════════  IPE 140 I-Beam
                         ◄────────────►
                         Laser ← Mirror
```

---

## 3. Wiring Details

### 3.1 ESP32 #1 (Motor Control Box)

**Power**: DIN Rail 5V PSU → ESP32 VIN/5V

**W5500 (VSPI)**:
| W5500 Pin | ESP32 GPIO |
|---|---|
| MISO | GPIO19 |
| MOSI | GPIO23 |
| SCLK | GPIO18 |
| CS | GPIO5 |
| RST | GPIO4 (optional) |
| 3.3V | 3.3V |
| GND | GND |

**GP8211S DAC (I2C, addr 0x58)**:
| DAC Pin | Connection |
|---|---|
| VCC | 3.3V or 5V from ESP32 |
| GND | GND |
| SDA | GPIO21 |
| SCL | GPIO22 |
| VOUT | VFD AVI terminal |
| GND | VFD ACM terminal |

**Laser Sensor (UART2, 19200 baud)**:
| Laser Pin | Connection |
|---|---|
| TX | ESP32 GPIO16 (RX2) |
| RX | ESP32 GPIO17 (TX2) |
| VCC | **Separate 3.3V regulator** (300mA+, NOT ESP32 3.3V pin) |
| GND | Common GND |

**Relays (VFD direction control)**:
| Relay | ESP32 GPIO | VFD Terminal |
|---|---|---|
| Relay 1 (FWD) | GPIO25 | FWD → COM (close = forward) |
| Relay 2 (REV) | GPIO26 | REV → COM (close = reverse) |

### 3.2 ESP32 #2 (Wireless Remote)

**Power**: DIN Rail 5V PSU or battery pack
**Network**: WiFi (control network AP)

| Button | Color | Function | ESP32 GPIO | Pull-up |
|---|---|---|---|---|
| 1 | Green | Forward Fast | GPIO12 | Internal pull-up |
| 2 | Green | Forward Slow | GPIO13 | Internal pull-up |
| 3 | Red | Backward Slow | GPIO14 | Internal pull-up |
| 4 | Red | Backward Fast | GPIO27 | Internal pull-up |

Note: Buttons connect GPIO to GND when pressed (active LOW).

### 3.3 VFD UX-52 Configuration

**Required parameter settings** (via VFD keypad):
- Speed command source = analog input (AVI)
- Run command source = terminal (FWD/REV)
- AVI input range = 0-10V
- Max frequency = as needed for trolley speed

**Terminal connections**:
- R, T → 230V AC mains (single phase)
- U, V, W → Motor (delta config for 220V)
- AVI → DAC VOUT (0-10V speed reference)
- ACM → DAC GND (analog common)
- FWD → Relay 1 NO → COM
- REV → Relay 2 NO → COM
- PE → Earth ground

---

## 4. Software Stack

### 4.1 Podman Compose (Dell OptiPlex)

**Dev**: Podman on WSL (Windows)
**Deploy**: Podman on Ubuntu 22.04 (Dell OptiPlex)

| Container | Image | Purpose | Ports |
|---|---|---|---|
| `micro-ros-agent` | `docker.io/microros/micro-ros-agent:humble` | Bridges micro-ROS (ESP32s) ↔ ROS2 DDS | UDP 8888 |
| `ros2-core` | Custom (ros:humble-ros-base + rosbridge_suite + SROS2) | Control node + rosbridge WebSocket | WS 9090 (internal) |
| `web-dashboard` | Vue.js PWA build + nginx | HTTPS dashboard + WSS reverse proxy | HTTPS 443 |

All containers use `network_mode: host` for ROS2 DDS multicast.
SROS2 keystore mounted read-only into micro-ros-agent and ros2-core.

### 4.2 ROS2 Topics

| Topic | Type | Publisher | Subscriber |
|---|---|---|---|
| `/motor/speed_cmd` | `std_msgs/Float32` | Dashboard, Control node | Motor ESP |
| `/motor/speed_feedback` | `std_msgs/Float32` | Motor ESP | Dashboard |
| `/motor/direction` | `std_msgs/Int8` | Control node | Motor ESP |
| `/carriage/position` | `std_msgs/Float32` | Motor ESP (laser) | Dashboard, Control node |
| `/remote/button_event` | `std_msgs/Int8` | Remote ESP | Control node |
| `/motor/enable` | `std_msgs/Bool` | Dashboard | Motor ESP |

**Direction encoding**: +1 = FWD, -1 = REV, 0 = stop
**Button encoding**: 1=FwdFast, 2=FwdSlow, 3=BwdSlow, 4=BwdFast, negative=released

### 4.3 ESP32 #1 Firmware (Arduino IDE / arduino-cli + micro-ROS)

- **Build tool**: Arduino IDE or `arduino-cli` (FQBN: `esp32:esp32:esp32`)
- **Libraries**: micro_ros_arduino (v2.0.8-humble), DFRobot_GP8XXX (v1.1.0), Wire, SPI, ETH (built-in)
- **Transport**: micro-ROS over UDP via W5500 Ethernet (ETH.h native driver)
- **DAC**: DFRobot_GP8XXX I2C, 15-bit, 0-10V output
- **Laser protocol**: UART2 @ 19200, hex commands:
  - Single shot: `AA 00 00 20 00 01 00 00 21`
  - Continuous: `AA 00 00 20 00 01 00 04 25`
  - Response: 13 bytes, distance = `(byte[8]<<8)|byte[9]` in mm
- **Subscribers**: `/motor/speed_cmd`, `/motor/direction`, `/motor/enable`
- **Publishers**: `/carriage/position` (mm), `/motor/speed_feedback`
- **Relay interlock**: 50ms both-off delay before switching FWD/REV
- **Control loop**: ~50Hz DAC update, ~10Hz laser read

### 4.4 ESP32 #2 Firmware (Arduino IDE / arduino-cli + micro-ROS)

- **Build tool**: Arduino IDE or `arduino-cli` (FQBN: `esp32:esp32:esp32`)
- **Libraries**: micro_ros_arduino (v2.0.8-humble), WiFi (built-in)
- **Transport**: micro-ROS over WiFi UDP
- **Publishers**: `/remote/button_event`
- **Logic**: Debounce (20ms), publish on press (+) and release (-)
- 4 buttons → 4 GPIO with internal pull-ups, active LOW

### 4.5 ROS2 Control Node (Python, runs in ros2-core container)

- Subscribes to `/remote/button_event`
- Maps button presses to speed + direction:
  - FwdFast → direction=+1, speed=1.0
  - FwdSlow → direction=+1, speed=0.3
  - BwdSlow → direction=-1, speed=0.3
  - BwdFast → direction=-1, speed=1.0
  - Release → speed=0.0
- Publishes `/motor/speed_cmd` and `/motor/direction`
- Safety: position boundary checks using `/carriage/position`

### 4.6 Web Dashboard (Vue.js PWA + roslibjs)

- **Served via**: nginx over HTTPS (port 443) with self-signed TLS cert
- **Auth**: Basic auth (username/password via .htpasswd)
- **ROS2 connection**: WSS via nginx reverse proxy (`wss://<host>/rosbridge` → `ws://localhost:9090`)
- **PWA**: Installable on phone home screen via manifest.json
- **Features**:
  - Speed slider (0-100%) → `/motor/speed_cmd`
  - Direction toggle (FWD/STOP/REV) → `/motor/direction`
  - Enable/disable switch → `/motor/enable`
  - Live position readout from `/carriage/position`
  - Speed feedback readout
  - Connection status indicator

---

## 5. Security

### 5.1 DDS Security (SROS2)

All ROS2 DDS traffic is encrypted and authenticated via SROS2:
- **Mutual TLS** between all ROS2 nodes (containers + ESP32s)
- **Per-node certificates** with access control policies
- **Enforce mode**: unauthenticated nodes are rejected

Environment variables (set in docker-compose.yml):
- `ROS_SECURITY_KEYSTORE=/security/keystore`
- `ROS_SECURITY_ENABLE=true`
- `ROS_SECURITY_STRATEGY=Enforce`

### 5.2 Web Dashboard Security

- **HTTPS (TLS)**: Self-signed cert via nginx, all HTTP traffic encrypted
- **Basic Auth**: Username/password required to access dashboard
- **WSS**: rosbridge WebSocket proxied through nginx TLS (wss:// not ws://)

### 5.3 Network Separation

- **Control network**: Dedicated switch + WiFi AP for ROS2 traffic, ESP32s, phone access
- **Internet network**: Separate WiFi (existing home/office router) for internet only
- Dell OptiPlex has two NICs: one for each network
- Phone connects to control WiFi AP to access the dashboard

### 5.4 First-Time Security Setup

Run these **once** before first deployment:

```bash
# 1. Generate SROS2 keys (inside ros2-core container)
podman-compose run ros2-core generate_sros2_keys.sh /security/keystore

# 2. Generate TLS certificate for web dashboard
cd docker/web-dashboard/certs && bash generate_tls_certs.sh

# 3. Generate dashboard login password
cd docker/web-dashboard && bash generate_htpasswd.sh admin
```

Files generated by these scripts are gitignored and must not be committed.

---

## 6. Project Directory Structure

```
ghra/
├── .gitignore                           # Excludes keystore, certs, passwords
├── hardware/                            # BOM CSV + reference images
├── docker/
│   ├── docker-compose.yml               # Podman-compatible, SROS2 + TLS volumes
│   ├── ros2-core/
│   │   ├── Dockerfile                   # ROS2 Humble + rosbridge + SROS2
│   │   ├── entrypoint.sh               # Auto-enables SROS2 if keystore present
│   │   ├── security/
│   │   │   ├── generate_sros2_keys.sh  # One-time SROS2 key generation
│   │   │   └── keystore/               # (gitignored) Generated SROS2 keys
│   │   └── ghra_control/               # ROS2 Python package
│   │       ├── package.xml
│   │       ├── setup.py
│   │       ├── setup.cfg
│   │       ├── resource/ghra_control
│   │       ├── launch/ghra_launch.py
│   │       └── ghra_control/
│   │           ├── __init__.py
│   │           └── control_node.py
│   └── web-dashboard/
│       ├── Dockerfile                   # Vue.js build + nginx
│       ├── nginx.conf                   # HTTPS, basic auth, WSS proxy
│       ├── generate_htpasswd.sh         # One-time password generation
│       ├── .htpasswd                    # (gitignored) Dashboard password
│       ├── certs/
│       │   ├── generate_tls_certs.sh   # One-time TLS cert generation
│       │   ├── server.crt              # (gitignored)
│       │   └── server.key              # (gitignored)
│       └── src/                         # Vue.js PWA app
│           ├── index.html              # PWA meta tags
│           ├── package.json
│           ├── vite.config.js
│           ├── public/
│           │   └── manifest.json       # PWA manifest
│           └── src/
│               ├── main.js
│               └── App.vue             # Dashboard UI + WSS connection
├── firmware/
│   ├── esp32-motor/                     # Arduino sketch (ESP32 #1)
│   │   └── esp32-motor.ino
│   └── esp32-remote/                    # Arduino sketch (ESP32 #2)
│       └── esp32-remote.ino
└── docs/
    └── system-plan.md                   # This file
```

---

## 7. Implementation Order

1. **Phase 1**: Podman compose + micro-ROS agent + ros2-core containers
2. **Phase 2**: ESP32 #1 firmware (W5500 Ethernet + micro-ROS + DAC output)
3. **Phase 3**: Laser sensor UART integration on ESP32 #1
4. **Phase 4**: Web dashboard (Vue.js PWA + HTTPS + rosbridge WSS)
5. **Phase 5**: ESP32 #2 remote firmware (WiFi + 4 buttons)
6. **Phase 6**: Control node (button-to-speed mapping, safety limits)
7. **Phase 7**: SROS2 key generation + enforce mode across all nodes
8. **Future**: E-stop hardware circuit

---

## 8. Key Notes

- **DAC → VFD is direct**: GP8211S outputs 0-10V natively, connects straight to VFD AVI/ACM. No amplification needed.
- **Laser sensor power**: Needs dedicated 3.3V @ 300mA+ supply, NOT from ESP32's 3.3V regulator (insufficient current). Use a small LDO from the 5V rail or a separate 3.3V DIN rail PSU.
- **VFD direction**: Controlled by relays closing FWD or REV terminals to COM, not by analog signal. Speed magnitude only via 0-10V.
- **Relay interlock**: Always turn both relays off and wait 50ms before switching direction. Never close FWD and REV simultaneously.
- **micro-ROS agent**: Run with `udp4 --port 8888` to accept both Ethernet and WiFi ESP32 connections.
- **GPIO14 caution**: On ESP32-WROOM-32D, GPIO14 outputs PWM at boot. Use with pull-up so button default state is HIGH (unpressed).
- **ESP32 Arduino core**: v3.3.7 installed. If micro_ros_arduino has compatibility issues, downgrade to v2.0.17 via `arduino-cli core install esp32:esp32@2.0.17`.
- **Phone access**: Connect phone to control WiFi, open `https://<optiplex-ip>`, log in with dashboard credentials, "Add to Home Screen" for PWA experience.
