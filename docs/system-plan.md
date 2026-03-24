# GHRA - ROS2 Motor Control System Plan

## Context

Build a ROS2-based system to control an AC motor on a rail (motorized trolley / Laufkatze on IPE 140 I-beam). The motor speed is controlled via a VFD receiving a 0-10V analog signal from a DAC. Position feedback comes from a laser distance sensor. The system includes a web dashboard and a wireless remote control with 4 buttons.

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
| **Network Switch** (unmanaged, 5+ ports, e.g. TP-Link TL-SG105) | Connects Dell OptiPlex + ESP32 #1 (via W5500) on wired LAN | ~15-25 EUR |
| **WiFi Access Point / Router** | ESP32 #2 (remote) connects via WiFi; Dell OptiPlex needs to be on same network | ~20-40 EUR (or use existing router) |
| **Mirror / Reflector** for laser sensor | Mounted at end of I-beam for distance measurement | ~5-10 EUR |
| **3.3V regulator or separate PSU for laser sensor** | Laser needs 2.5-3.3V @ 300mA; ESP32's 3.3V pin may not supply enough current reliably | ~2-5 EUR |
| **2-wire shielded cable** (for DAC to VFD analog signal) | 0-10V analog signal from DAC to VFD AVI/ACM terminals | ~5 EUR |
| **Wago/lever connectors or additional terminal blocks** | Clean wiring inside enclosures | ~5-10 EUR |

### 1.3 Verified: NOT Needed

| Component | Reason |
|---|---|
| ~~Op-amp / level shifter for DAC~~ | GP8211S outputs 0-10V natively with internal boost circuit |
| ~~USB-to-Serial adapter~~ | ESP32 Plus Kit has USB-C with CH340C onboard |

### 1.4 Future (Out of Scope)

- 2x Emergency Stop buttons (cuts power to motor + motor-control ESP)
- Contactors/Schutze for E-stop circuit (24V)

---

## 2. System Architecture

```
                        ┌─────────────────────────────────────────────┐
                        │         Dell OptiPlex (x86 Linux)           │
                        │         Ubuntu 22.04 + Podman (WSL)         │
                        │                                             │
                        │  ┌──────────────┐  ┌────────────────────┐  │
                        │  │ micro-ROS     │  │ ros2-core          │  │
                        │  │ Agent         │  │ (control node +    │  │
                        │  │ (UDP bridge)  │  │  rosbridge:9090)   │  │
                        │  └──────┬────────┘  └────────┬───────────┘  │
                        │         │  ROS2 DDS          │              │
                        │         └────────┬───────────┘              │
                        │  ┌───────────────┴──────────────────────┐   │
                        │  │ web-dashboard (Vue.js + nginx :8080) │   │
                        │  └──────────────────────────────────────┘   │
                        └─────────────────┬───────────────────────────┘
                                          │ Ethernet
                                    ┌─────┴─────┐
                                    │  Network   │
                                    │  Switch    │
                                    └──┬─────┬──┘
                           Ethernet    │     │    WiFi (via AP/Router)
                                       │     │
         ┌─────────────────────────────┘     └──────────────────┐
         │                                                      │
┌────────┴───────────────┐                    ┌─────────────────┴──────┐
│ ESP32 #1 (Motor Box)   │                    │ ESP32 #2 (Remote)      │
│ micro-ROS over UDP/ETH │                    │ micro-ROS over WiFi    │
│                        │                    │                        │
│ VSPI → W5500 (ETH)    │                    │ GPIO12 ← Green1 (FwdF) │
│ I2C  → GP8211S DAC    │                    │ GPIO13 ← Green2 (FwdS) │
│ UART2 ← Laser Sensor  │                    │ GPIO14 ← Red1   (BwdS) │
│ GPIO → Relays (FWD/REV)│                   │ GPIO27 ← Red2   (BwdF) │
└────────┬───────────────┘                    └────────────────────────┘
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
**Network**: WiFi (same SSID/subnet as Dell OptiPlex)

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

### 4.1 Podman Compose (Dell OptiPlex, WSL)

**OS**: Ubuntu 22.04 (WSL) + Podman + podman-compose

| Container | Image | Purpose | Ports |
|---|---|---|---|
| `micro-ros-agent` | `docker.io/microros/micro-ros-agent:humble` | Bridges micro-ROS (ESP32s) ↔ ROS2 DDS | UDP 8888 |
| `ros2-core` | Custom (ros:humble-ros-base + rosbridge_suite) | Control node + rosbridge WebSocket | WS 9090 |
| `web-dashboard` | Vue.js build + nginx | Speed control UI | HTTP 8080 |

All containers use `network_mode: host` for ROS2 DDS multicast.

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

- **Transport**: micro-ROS over UDP via W5500 Ethernet (ETH.h native driver)
- **DAC library**: DFRobot_GP8XXX (I2C, 15-bit, 0-10V)
- **Laser protocol**: UART2 @ 19200, hex commands:
  - Single shot: `AA 00 00 20 00 01 00 00 21`
  - Continuous: `AA 00 00 20 00 01 00 04 25`
  - Response: 13 bytes, distance = `(byte[8]<<8)|byte[9]` in mm
- **Subscribers**: `/motor/speed_cmd`, `/motor/direction`, `/motor/enable`
- **Publishers**: `/carriage/position` (mm), `/motor/speed_feedback`
- **Control loop**: ~50Hz DAC update, ~10Hz laser read

### 4.4 ESP32 #2 Firmware (Arduino IDE / arduino-cli + micro-ROS)

- **Transport**: micro-ROS over WiFi UDP
- **Publishers**: `/remote/button_event`
- **Logic**: Debounce (20ms), publish on press and release
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

### 4.6 Web Dashboard (Vue.js + roslibjs)

- Connects to rosbridge WebSocket on port 9090
- Speed slider (0-100%) → `/motor/speed_cmd`
- Direction toggle (FWD/REV) → `/motor/direction`
- Enable/disable switch → `/motor/enable`
- Live position readout from `/carriage/position`
- Connection status indicator

---

## 5. Project Directory Structure

```
ghra/
├── hardware/                    # BOM CSV + reference images
├── docker/
│   ├── docker-compose.yml       # Podman-compatible
│   ├── ros2-core/
│   │   ├── Dockerfile
│   │   ├── entrypoint.sh
│   │   └── ghra_control/        # ROS2 Python package
│   │       ├── package.xml
│   │       ├── setup.py
│   │       └── ghra_control/
│   │           ├── __init__.py
│   │           └── control_node.py
│   └── web-dashboard/
│       ├── Dockerfile
│       ├── nginx.conf
│       └── src/                 # Vue.js app
├── firmware/
│   ├── esp32-motor/             # Arduino sketch
│   │   └── esp32-motor.ino
│   └── esp32-remote/            # Arduino sketch
│       └── esp32-remote.ino
└── docs/
    └── system-plan.md           # This file
```

---

## 6. Implementation Order

1. **Phase 1**: Podman compose + micro-ROS agent + ros2-core containers
2. **Phase 2**: ESP32 #1 firmware (W5500 Ethernet + micro-ROS + DAC output)
3. **Phase 3**: Laser sensor UART integration on ESP32 #1
4. **Phase 4**: Web dashboard (Vue.js + rosbridge + roslibjs)
5. **Phase 5**: ESP32 #2 remote firmware (WiFi + 4 buttons)
6. **Phase 6**: Control node (button→speed mapping, safety limits)
7. **Future**: E-stop hardware circuit

---

## 7. Key Notes

- **DAC → VFD is direct**: GP8211S outputs 0-10V natively, connects straight to VFD AVI/ACM. No amplification needed.
- **Laser sensor power**: Needs dedicated 3.3V @ 300mA+ supply, NOT from ESP32's 3.3V regulator (insufficient current). Use a small LDO from the 5V rail or a separate 3.3V DIN rail PSU.
- **VFD direction**: Controlled by relays closing FWD or REV terminals to COM, not by analog signal. Speed magnitude only via 0-10V.
- **micro-ROS agent**: Run with `udp4 --port 8888` to accept both Ethernet and WiFi ESP32 connections.
- **GPIO14 caution**: On ESP32-WROOM-32D, GPIO14 outputs PWM at boot. Use with pull-up so button default state is HIGH (unpressed).
- **ESP32 Arduino core**: v3.3.7 installed. If micro_ros_arduino has compatibility issues, downgrade to v2.0.17 via `arduino-cli core install esp32:esp32@2.0.17`.
