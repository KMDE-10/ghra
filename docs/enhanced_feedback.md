# Enhanced Feedback — Plan to make the trolley crisp

> Sister doc to `system-plan.md`. Where that one describes what was *built*, this one describes what makes it *feel slow* and how to fix it. Status as of 2026-05-06, after the user reported intermittent long latency and "sticky" buttons during hardware work.

## 1. Goal

Make every operator action — pushbutton press, pushbutton release, dashboard tap, slider drag — produce a visible motor reaction within a single perceptual frame (≤ 100 ms typical, never > 250 ms), and ensure that a stop *always* arrives even when the network glitches.

Two acceptance bars:

1. **Crispness:** end-to-end latency from button-press to relay-close, measured with a logic analyser, has a median under 50 ms and a 99th percentile under 200 ms.
2. **No stuck motor:** under simulated single-message loss anywhere in the chain, the motor reaches `speed=0, dir=0` within 600 ms.

## 2. Symptom inventory

What "not crisp" looks like in the field:

| Symptom | What the operator sees | Likely class |
|---|---|---|
| Long latency | Button pressed, motor reacts after a noticeable beat | Loop starvation, blocking I/O, MQTT keepalive churn |
| Sticky buttons | Released remote button, motor keeps running until pressed STOP | Lost release publish at QoS 0, no deadman |
| Slider lag | Dashboard slider thumb moves but motor speed lags | Unthrottled `@input` floods, I²C DAC writes serialised |
| Cold-boot weirdness | First action after power-up is delayed | MQTT reconnect window, container start order |
| Random pauses every ~10 s | Brief unresponsive moment that recurs | Laser re-wake (`laserWake()` every 10 s), SoftwareSerial blocking |

## 3. Root cause map (ranked by likely contribution)

ROS is **not** in the command path. `mqtt_bridge.py` only mirrors MQTT → ROS one-way; `control_node.py`'s soft-limit stops never reach the motor. Removing the ROS containers entirely would change nothing for crispness. The real causes, in priority order:

| # | Cause | Where | Impact |
|---|---|---|---|
| 1 | `SoftwareSerial` for the M01 laser starves the main loop on the motor ESP32 | `firmware/esp32-motor/esp32-motor.ino:60, 126-147` | High — bit-banged 9600-baud RX disables interrupts during bit timing; sustained continuous-mode streaming → MQTT loop starved → keepalive/reconnect churn → dropped commands |
| 2 | `delay(50)` blocking interlock on every direction change | `esp32-motor.ino:94` | High — 50 ms freeze per FWD↔REV swap during which nothing else runs |
| 3 | All MQTT publishes are QoS 0, no retain | every ESP32 + dashboard | High — single-packet loss leaves motor running (the "sticky" symptom) |
| 4 | No deadman / heartbeat on the motor | `esp32-motor.ino` `loop()` | High — motor runs indefinitely until told to stop, so any lost release = stuck motor |
| 5 | Slider publishes on every `@input` event, unthrottled | `docker/web-dashboard/src/src/App.vue:46-52, 166-168` | Medium — floods the motor's inbound queue at the screen's polling rate, each one triggering an I²C write |
| 6 | I²C DAC at default 100 kHz | `esp32-motor.ino:213` | Medium — every `dac.setDACOutVoltage()` is a few ms of blocking I²C; matters when slider spams |
| 7 | STOP-button release event is unhandled | `firmware/esp32-remote/esp32-remote.ino:165-173` | Low symptomatically (STOP-press already stops) but inconsistent and confusing |
| 8 | Remote loop has `delay(5)` and a blocking `mqtt.publish()` | `esp32-remote.ino:175, 80-95` | Low — small per-loop, but stacks during MQTT congestion |
| 9 | No re-publish of held button state on reconnect | `esp32-remote.ino` | Low — if MQTT drops while user holds FWD, press is forgotten |
| 10 | Nginx WebSocket proxy: no explicit `proxy_buffering off`, no `tcp_nodelay on` | `docker/web-dashboard/nginx.conf:14-21` | Low — usually defaults are fine, but worth pinning |
| 11 | OS-level: NIC offloads / EEE / Wake-on-LAN, Avahi/mDNS noise on control NIC | host config (off-repo) | Low/situational — capable of injecting tens-of-ms delays sporadically |
| 12 | Mosquitto runs with default keepalive; ESP32 reconnect is 3 s | `mosquitto.conf`, `esp32-*.ino:155-162` / `:290-295` | Low — only matters when something else has already pushed the system into reconnect state |

## 4. Concrete changes, by component

Each section: **what**, **why**, **file/line**, **risk**.

### 4.1 Motor ESP32 — `firmware/esp32-motor/esp32-motor.ino`

#### 4.1.1 Move laser from `SoftwareSerial` to `HardwareSerial Serial2` (UART2)

- **What:** Replace `SoftwareSerial laserSerial(33, 32)` with `HardwareSerial &laserSerial = Serial2;` and call `Serial2.begin(9600, SERIAL_8N1, 16, 17);`. Re-wire laser yellow→GPIO16, green→GPIO17.
- **Why:** UART2 is hardware-buffered, interrupt-driven, doesn't disable global interrupts, and doesn't block the loop while a frame is incoming. Single biggest win.
- **Files:** `esp32-motor.ino:33-34, 60, 210`. Update wiring SVG `hardware/images/esp32-motor-wiring.svg` and README §7.1.
- **Risk:** Wiring change required. GPIO 32/33 free up — make sure nothing else expects them. The `laserProcess()` parser logic is unchanged because the byte stream is identical; only the read source changes.

#### 4.1.2 Replace `delay(50)` interlock with non-blocking state machine

- **What:** Convert `setDirection()` to enqueue a transition (record target dir + timestamp), drive both relays low immediately, and complete the transition on the next `loop()` iteration that observes `millis() - relay_off_ts >= 50`. Reject new direction commands until the transition completes (or queue the latest one).
- **Why:** Eliminates the 50 ms freeze. While transitioning, the loop still services MQTT, laser, and serial commands.
- **Files:** `esp32-motor.ino:90-101`. Add two state vars: `int8_t pending_direction; uint32_t relay_off_ts;`.
- **Risk:** The 50 ms both-off window is a *safety* property of the VFD wiring; do not shorten it, only stop blocking the loop during it. Verify the new code never closes a relay before 50 ms have elapsed.

#### 4.1.3 Add a deadman / command timeout

- **What:** Add `uint32_t last_command_ms` updated whenever a `motor/speed_cmd`, `motor/direction`, or `motor/enable` message arrives. If `motor_enabled && (millis() - last_command_ms > 600)`, force `current_speed=0`, `setDirection(0)`, `motor_enabled=false`. (600 ms = 3 missed 200 ms heartbeats with margin.)
- **Why:** A single dropped MQTT message no longer leaves the motor running. This is the core "sticky button" fix.
- **Files:** `esp32-motor.ino` callback (`mqttCallback`, line 151) and `loop()` (line 289).
- **Risk:** Requires the command-source side to publish a heartbeat while a button is held (see §4.2.3 and §4.3.2). Without that, a held FWD button would auto-stop after 600 ms.

#### 4.1.4 Subscribe with QoS 1 and accept retained messages

- **What:** Change `mqtt.subscribe("motor/speed_cmd", 0)` etc. to QoS 1. Don't bother with retain-on-subscribe (PubSubClient doesn't filter retained messages out).
- **Why:** QoS 1 makes the broker re-deliver if the ESP32 hasn't ACKed. Combined with §4.1.3 deadman, this is belt-and-braces.
- **Files:** `esp32-motor.ino:187-189`.
- **Risk:** PubSubClient's QoS 1 implementation is minimal; under heavy load it can stall. Keep `MQTT_MAX_PACKET_SIZE` at default (256). Test under 5 Hz heartbeat load before deploying.

#### 4.1.5 Stop streaming the laser when the motor is disabled

- **What:** Only re-issue `LASER_CMD_CONTINUOUS` while `motor_enabled == true` (or while a measurement is recently needed). When disabled, let the M01's auto-sleep kick in. On enable, send the wake command once.
- **Why:** Removes ~all SoftwareSerial / UART2 RX traffic when the trolley isn't moving. Less interrupt load, less jitter on cold-start of motion.
- **Files:** `esp32-motor.ino:303-307` (the 10 s re-wake block) and `mqttCallback` for `motor/enable`.
- **Risk:** Position telemetry stops when motor is idle. If the dashboard expects a continuous position read while stopped, drop in a 1 Hz single-shot poll (`func 0x20`) instead of full continuous mode.

#### 4.1.6 Bump I²C clock to 400 kHz

- **What:** After `Wire.begin(...)`, call `Wire.setClock(400000);`.
- **Why:** Each `dac.setDACOutVoltage()` writes ~3 bytes; at 100 kHz that's ~300 µs of I²C; at 400 kHz it's ~75 µs. Matters when slider spams.
- **Files:** `esp32-motor.ino:213`.
- **Risk:** GP8211S supports up to 400 kHz per its datasheet. Verify by I²C scanning (`i2c` serial command) after the change — DAC should still appear at 0x58.

#### 4.1.7 Don't publish `carriage/position` from the MQTT-callback context

- **What:** `laserProcess()` is called from `loop()` so this is fine, but ensure `mqtt.publish("carriage/position", ...)` is *not* called inside `mqttCallback`. (Currently it is not — keep it that way.)
- **Why:** Reentering PubSubClient from its own callback can deadlock the W5500 socket buffer.
- **Risk:** None if left as-is. Documented to prevent regressions.

#### 4.1.8 Enable the ESP32 hardware watchdog

- **What:** `esp_task_wdt_init(5, true); esp_task_wdt_add(NULL);` in setup; `esp_task_wdt_reset();` in `loop()`.
- **Why:** If anything genuinely hangs (W5500 SPI lock-up, I²C hold-low) the ESP32 reboots itself. Better than a wedged motor controller.
- **Files:** `esp32-motor.ino:197` (setup) and top of `loop()`.
- **Risk:** A 5 s WDT triggers if any single operation blocks longer than that. Test before enabling, especially with the slow `delay(2000)` startup banner — that needs to come before WDT init or the timeout adjusted.

### 4.2 Remote ESP32 — `firmware/esp32-remote/esp32-remote.ino`

#### 4.2.1 Use QoS 1 for command publishes

- **What:** `PubSubClient` doesn't expose QoS on publish — the entire library is QoS 0 for `publish()`. To get QoS 1, swap to a library that supports it (e.g. `arduino-mqtt` by Joël Gähwiler) **or** keep PubSubClient and apply §4.2.2 below as the workaround.
- **Why:** Same reason as §4.1.4 on the motor — guarantee delivery at the protocol level.
- **Risk:** Library swap is broader than a one-liner. If avoiding it, lean on §4.2.2 instead.

#### 4.2.2 Re-send button events 3× at 30 ms intervals

- **What:** When a button transitions, instead of a single `mqtt.publish()`, schedule three publishes at `t=0, 30, 60 ms`. Implement with a tiny pending-publish queue checked each `loop()` iteration.
- **Why:** Cheap, library-agnostic mitigation for QoS-0 packet loss. The motor's idempotent handlers don't care about duplicates.
- **Files:** `esp32-remote.ino:78-96` (`cmdForward`, `cmdStop`, `cmdReverse`) and `loop()`.
- **Risk:** Triples MQTT traffic during transitions. Negligible at this scale.

#### 4.2.3 Heartbeat while a button is held

- **What:** While `btn_fwd.last_state == true`, publish `motor/direction=1, speed_cmd=0.07, enable=1` every 200 ms. Same for REV. Stops on release.
- **Why:** Pairs with the motor deadman (§4.1.3). If any heartbeat reaches the motor, motor stays running. If they all stop reaching it (cable cut, broker died, ESP32 crashed), motor stops within 600 ms. **This is the proper sticky-button fix.**
- **Files:** `esp32-remote.ino` `loop()` (line 152).
- **Risk:** Slight increase in steady-state MQTT traffic when a button is held. ~5 messages/s per direction. Mosquitto won't notice.

#### 4.2.4 Handle STOP release

- **What:** Add `if (stp == -1) cmdStop();` (or just leave it; STOP-press already publishes stop). Decision: explicitly comment that release is intentionally a no-op so a future reader doesn't think it was forgotten.
- **Files:** `esp32-remote.ino:173`.
- **Risk:** None.

#### 4.2.5 Make MQTT connect non-blocking

- **What:** `mqtt.connect()` blocks up to ~15 s on TCP timeout if the broker isn't reachable. During `setup()` that's fine; in `loop()` it freezes the remote on every failed reconnect attempt. Replace the `mqttConnect()` call inside the reconnect window with a non-blocking `EthernetClient::connect(host, port)` followed by an explicit `mqtt.connect()` only when the TCP connect has succeeded.
- **Why:** A briefly-unreachable broker should not freeze button polling.
- **Files:** `esp32-remote.ino:65-74, 155-162`.
- **Risk:** More state to manage. Test with broker pulled cold.

#### 4.2.6 Drop the `delay(5)` at the bottom of `loop()`

- **What:** Replace with a non-blocking time check: only poll buttons when `millis() - last_btn_poll_ms >= 5`. Keeps the loop free for `mqtt.loop()`.
- **Files:** `esp32-remote.ino:175`.
- **Risk:** None.

### 4.3 Dashboard — `docker/web-dashboard/src/src/App.vue`

#### 4.3.1 Throttle the slider

- **What:** Wrap `publishSpeed()` in a 50 ms trailing-edge throttle (or `requestAnimationFrame`-batched). Currently every `@input` event publishes — a slider drag spams 60+ messages/s.
- **Why:** The motor ESP32 spends I²C time on every one. Throttling matches the actual analog-control rate the system can use.
- **Files:** `App.vue:46-52, 166-168`.
- **Risk:** None. Operator won't perceive 50 ms throttle.

#### 4.3.2 Heartbeat while a directional button is held

- **What:** Same idea as §4.2.3 but for the dashboard. While the user holds FWD/REV (touchstart→touchend), publish at 200 ms cadence. Currently the dashboard only publishes on click — fine for tap-and-release UI, but pair with the motor deadman by ensuring a touch-and-hold gesture maintains heartbeats.
- **Why:** If the operator holds FWD and the WiFi briefly drops, motor must stop. With this + deadman, it does.
- **Files:** `App.vue:155-164`.
- **Risk:** Small. Confirm `touchstart`/`touchend` events fire correctly on iOS PWA mode.

#### 4.3.3 Use QoS 1 for command publishes

- **What:** `this.client.publish(topic, payload, { qos: 1 })` for `motor/enable`, `motor/direction`. Speed slider can stay at QoS 0 (it's continuous; loss is self-healing).
- **Files:** `App.vue:170-174`.
- **Risk:** mqtt.js handles QoS 1 fine. Slight increase in WS traffic.

#### 4.3.4 Use `retain: true` on `motor/enable`

- **What:** When toggling enable, publish with `retain: true`. Motor ESP32 reconnect picks up current enable state.
- **Files:** `App.vue:150`.
- **Risk:** Retained messages also cause the broker to send the last enable to *new* subscribers. If a debug tool subscribes mid-run, no harm. Keep `motor/speed_cmd` and `motor/direction` non-retained — those are stateful only when paired with the deadman heartbeat.

### 4.4 Mosquitto — `docker/mosquitto/mosquitto.conf`

Mosquitto is fine. Two optional tweaks:

#### 4.4.1 Pin `keepalive` and `max_inflight_messages`

- **What:** Add `max_keepalive 30` and `max_inflight_messages 20`.
- **Why:** Defaults work, but pinning avoids surprises if the image bumps majors.
- **Risk:** None.

#### 4.4.2 Don't enable `persistence`

- **What:** Currently `persistence false`. Keep it that way.
- **Why:** Persistence forces fsync on retained messages. We have one retained message (`motor/enable`) at low rate — not worth the I/O.

### 4.5 Nginx — `docker/web-dashboard/nginx.conf`

#### 4.5.1 Pin WebSocket transparency

- **What:** Inside `location /mqtt { ... }`, add:
  ```
  proxy_buffering off;
  tcp_nodelay on;
  proxy_set_header X-Real-IP $remote_addr;
  ```
- **Why:** Defaults usually do the right thing for WebSocket upgrades, but explicit beats implicit. `proxy_buffering off` ensures Mosquitto pings/pongs aren't held in nginx buffers.
- **Files:** `nginx.conf:14-21`.
- **Risk:** None.

#### 4.5.2 Drop the `/rosbridge` location

- **What:** Remove `location /rosbridge { ... }` block (`nginx.conf:24-31`). The dashboard does not use rosbridge — App.vue only opens an MQTT WS.
- **Why:** Less surface area; rosbridge isn't part of the runtime path.
- **Risk:** None — confirmed by grepping `App.vue` and `main.js` for `rosbridge` (no references). If a future tool needs it, re-add.

### 4.6 OS / host (OptiPlex)

These are off-repo. Worth a one-time check:

- **NIC EEE / power-saving on the control NIC** — known to inject 10s of ms when waking. Disable: `ethtool --set-eee <ctrl-nic> eee off`, also `ethtool -K <ctrl-nic> tx off rx off gso off tso off` if seeing weirdness. Persist via NetworkManager dispatcher or netplan.
- **`avahi-daemon` on the control LAN** — README §3.A.2 step 3 mentions it. Disable: `sudo systemctl disable --now avahi-daemon` *or* restrict it to the WiFi-stick TUN with `allow-interfaces=espwifi0` in `/etc/avahi/avahi-daemon.conf`.
- **`systemd-resolved`** — already documented in README §3.3 as something to disable for the WiFi stick. No effect on MQTT TCP path (broker addressed by IP literal), but kills `apt`-time confusion.
- **`ufw`** — if enabled, allow `1883/tcp 9001/tcp 8443/tcp` from `192.168.1.0/24`. README §9.6 already says this; double-check after the next Ubuntu upgrade.

### 4.7 ROS / control_node — what to do with it

The mqtt_bridge + control_node combination currently:
- Adds CPU and RAM overhead on the OptiPlex (one Python process spinning on rclpy + paho).
- Does **not** participate in the command path (verified: bridge is one-way MQTT→ROS for commands; ROS-side `/motor/speed_cmd` publishes go nowhere).
- Hosts soft position-limit logic that is functionally dead (the limit-stop publish has no path back to MQTT).

Three options:

| Option | Effort | Effect on crispness |
|---|---|---|
| **A. Leave as-is** | 0 | None — already not in the path |
| **B. Fix the bridge** so ROS→MQTT works for `motor/speed_cmd` and `motor/direction`, then the soft limits do something | Moderate | Adds ~ms of bridge latency to commands originating from ROS, which is none of them today; only matters if you start trusting the limits |
| **C. Delete ROS containers entirely**, move position limits into motor firmware (cleaner anyway) | Larger | Frees ~100 MB RAM, simplifies `start.sh`, removes dead code surface |

Recommendation: **C**, but only after the firmware-side fixes (§4.1) are in and proven. The position-limit logic is a hardware-safety property — it belongs in `esp32-motor.ino`'s `mqttCallback` for `motor/speed_cmd`, gated on the laser distance. Do not pull ROS until that's moved.

## 5. Phased rollout

Don't try to land all of this at once. Order matters because some changes assume others.

**Phase 1 — Firmware crispness (one evening, biggest win)**
1. §4.1.1 laser → HardwareSerial Serial2 (rewire + code)
2. §4.1.2 non-blocking 50 ms interlock
3. §4.1.6 I²C → 400 kHz
4. §4.2.6 drop `delay(5)` on remote
5. Smoke test: drive trolley, verify no regressions, observe latency feel

**Phase 2 — Resilience (one evening, fixes sticky buttons)**
1. §4.1.3 motor deadman (600 ms)
2. §4.2.3 remote heartbeat at 200 ms while held
3. §4.3.2 dashboard heartbeat while held
4. §4.2.2 3× re-send on button transitions (or library swap §4.2.1)
5. §4.3.4 retain on `motor/enable`
6. Pull-the-cable test: hold FWD, yank Ethernet from remote — motor must stop within 1 s

**Phase 3 — Polish**
1. §4.3.1 slider throttle
2. §4.3.3 dashboard QoS 1 for enable/direction
3. §4.5.1 nginx pinning, §4.5.2 drop /rosbridge
4. §4.1.5 stop streaming laser when disabled
5. §4.1.8 watchdog
6. §4.6 host-side checks

**Phase 4 — Optional cleanup**
1. §4.7 option C: delete ROS, move limits into firmware

## 6. How to measure improvement

Without numbers this is faith-based. Three cheap probes:

1. **End-to-end latency probe.** Wire a logic analyser to (a) the remote's FWD button GPIO and (b) the motor's `RELAY_FWD_PIN`. Press button, measure delta. Capture before any change as baseline; re-capture after each phase.

2. **MQTT loss simulator.** On the OptiPlex, briefly block one direction with `sudo iptables -I INPUT -s 192.168.1.12 -j DROP`, then drop the rule. Verify deadman triggers. Reverse for ESP32→broker direction.

3. **Slider stress.** Open the dashboard, drag the slider rapidly for 10 s. Watch the motor ESP32's serial log (`STATS:` line every 30 s). Should not see MQTT disconnects or queue overruns. Pre-fix this often shows reconnects; post-fix it should stay clean.

Log all three before and after each phase. Add a row to a small table in this file as you go — it's the simplest way to tell whether the next change is worth doing.

## 7. What NOT to do

- **Don't blame ROS first.** It's not in the command path. Fixing the firmware fixes the perceived ROS latency.
- **Don't shorten the 50 ms relay interlock.** That's a hardware safety property.
- **Don't move to QoS 2 anywhere.** PubSubClient doesn't support publishing it well; mqtt.js does but the round-trip cost isn't worth it for this workload.
- **Don't enable Mosquitto persistence** for retained-message durability. Power-cycle of the OptiPlex re-establishes state in seconds anyway.
- **Don't add `auth_basic` to nginx or `password_file` to Mosquitto** as part of this work. README §3.A.5 trust model holds: the air-gapped LAN is the trust boundary. Mixing auth changes with crispness work confuses two unrelated regressions.
- **Don't move the laser to USB or I²C.** It's a UART device; HardwareSerial is the right answer.

## 8. Open questions for the operator

Things I wasn't sure about while writing this:

1. Does the dashboard need continuous position telemetry while the motor is stopped (relevant to §4.1.5)? If yes, switch to 1 Hz single-shot rather than killing telemetry entirely.
2. Is the M01 auto-sleep timeout known? (We re-wake every 10 s defensively — could be tightened or relaxed if the timeout is documented somewhere we haven't found.)
3. Is anyone using the rosbridge endpoint (§4.5.2)? Default assumption: no, based on App.vue.
4. Is there a logic analyser available for §6.1, or should the latency probe be done with serial timestamps?
