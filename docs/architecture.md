# Architecture

This project ships in two scope tiers:

- **v1 — ESP32-only.** The shipped build. Voltage trigger, RF replay,
  done. Sub-mA average current, no Pi, no display, no Wi-Fi.
- **v2 — optional telematics.** Add-on Pi side with touchscreen, web
  dashboard, MQTT, SNMP, OBD-II logging. Fully built and tested; not
  required for the trigger function. See "v2 — optional telematics
  add-on" further down.

Scope-decision context: 2026-06-01 — the original goal was "battery
never dies in winter," and v1 alone delivers that. v2 grew organically
during the hardware wait and lives in the repo as a documented add-on.

## v1 — division of responsibilities

There's only one box. Everything lives on the ESP32:

| Job | Runs on |
|---|---|
| Always-on voltage monitoring (sub-mA deep sleep) | **ESP32** |
| Wake-up trigger when V drops below threshold | **ESP32** |
| Compustar RF transmit via CC1101 (real-time bit timing) | **ESP32** |
| 15-minute run timer + engine-stop packet | **ESP32** |
| Cooldown gate before re-arm | **ESP32** |
| RTC-memory streak persistence across deep sleeps | **ESP32** |

No coordination, no UART listener, no MOSFET gating, no Pi shutdown
choreography in v1 — those exist in the firmware but are inert when
no Pi is wired up. (The Pi-power MOSFET GPIO toggles harmlessly into
an unconnected pin; the `shutdown_pi` UART writes go nowhere; the
STOPPING-state hard-cap timeout fires cleanly either way.)

## v2 — optional telematics add-on

If the Pi-side hardware from the deferred BOM is connected, the
firmware's UART writes start being consumed and the Pi's
`vroom.service` daemon comes alive. v2 adds:

| Job | Runs on |
|---|---|
| 5" touch display UI | **Pi** |
| Map rendering (cached OSM tiles or live via Wi-Fi) | **Pi** |
| Web UI for home-network monitoring | **Pi** |
| MQTT publisher to Home Assistant / Grafana | **Pi** (the ESP32 could do this too — left to v2 for clean separation) |
| SNMPv2c responder for NMS polling (read-only) | **Pi** (see [docs/20-snmp-integration.md](20-snmp-integration.md)) |
| OBD-II via native CAN (live RPM / speed / coolant logging) | **ESP32** (TWAI peripheral) → publishes to Pi via UART |
| Engine-running confirmation (CAN frame parsing) | **ESP32** → Pi as event |
| Persistent trip log to SD card | **Pi** |
| Clean shutdown on power-down signal | **Pi** (systemd service) |
| SSH for debugging | **Pi** |

Principle: ESP32 stays the always-on watchdog with real-time
superpowers and microamp sleep. Pi is the smart brain that wakes up
only when triggered.

## Pi-side daemon layout (v2)

The Pi runs **one process** (`pi.app.daemon`, systemd unit
`vroom.service`) hosting four background threads plus the Flask
request handlers in the main thread. All threads share state through
`pi/app/state.py`:

| Thread | Module | Job |
|---|---|---|
| (main) | `pi/app/display/server.py` | Flask request handlers (dashboard + `/api/*`) |
| `uart-listener` | `pi/app/comms/uart_listener.py` | Reads ESP32 messages over UART, mutates STATE |
| `mqtt-publisher` | `pi/app/comms/mqtt_publisher.py` | Periodic STATE snapshot → broker |
| `mqtt-subscriber` | `pi/app/comms/mqtt_subscriber.py` | Subscribes to command topics from broker |
| `vroom-snmp` | `pi/app/snmp_responder.py` | Read-only SNMPv2c responder on UDP/1161, OIDs under `1.3.6.1.4.1.99999.7` |

The SNMP thread shares the same in-process `STATE` dict the UART
listener writes to, so `snmpwalk` returns live telemetry, not stale
defaults. See [docs/20-snmp-integration.md](20-snmp-integration.md)
for the OID reference and integration examples.

(There used to be a separate `vroom-snmp.service` running the
responder as its own process — refactored 2026-05-30 to a thread
because module-level dicts are per-process in CPython, so the
separate-process responder served boot-time defaults forever.)

## Communication between them

- **Primary:** UART at 115200 baud, 3 wires (TX, RX, GND)
- **Wake signal:** ESP32 GPIO → AO3401A P-MOSFET → gates 5V to Pi
- **Shutdown coordination:** ESP32 sends a `shutdown_pi` COMMAND over UART (constant `CMD_SHUTDOWN_PI` in both `pi_link.py` ends), Pi systemd service catches it and runs `shutdown -h now`, ESP32 waits `PI_SHUTDOWN_GRACE_S` (default 30s) then cuts MOSFET. Safety hard-cap at `RUN_DURATION_S + STOPPING_HARD_CAP_MULT * PI_SHUTDOWN_GRACE_S` force-cuts power if the Pi never acks.

UART protocol: line-delimited JSON, one object per line at 115200 baud. Six message types (STATUS, OBD, EVENT, COMMAND, ACK, LOG) defined symmetrically in `esp32/src/lib/pi_link.py` and `pi/app/comms/esp32_link.py`. Cross-module constant parity is verified by `esp32/tests/test_integration.py`.

## Power architecture — v1 (ESP32-only)

```
OBD-II Pin 16 (+12V, always-hot) ─── 2A fuse ─── TVS clamp ─── 1A buck ─── 5V rail
                                                                            │
                                                                            ▼
                                                                  ESP32 (always on,
                                                                  deep-sleep <50 µA)
```

Buck sized for: ESP32 (~150 mA peak) + headroom = **1A is plenty**.
v1 doesn't have a Pi, a display, or a power-gate MOSFET — the rail is
just ESP32.

## Power architecture — v2 (Pi telematics added)

```
OBD-II Pin 16 (+12V, always-hot) ─── 2A fuse ─── TVS clamp ─── 5A buck ─── 5V rail
                                                                            │
                                                  ┌─────────────────────────┼─────────────────────────┐
                                                  ▼                         ▼                         ▼
                                            ESP32 (always on)    AO3401A P-MOSFET           5" display
                                                                        │                  (always when buck up,
                                                                        ▼                   ~700mA when on)
                                                                  Pi Zero 2 W
                                                                  (gated, ~400mA peak)
```

v2 upgrades the buck to 5A and adds the MOSFET gate so the Pi only
draws current when the ESP32 has triggered a start cycle. Buck sized
for: ESP32 (~150 mA peak) + Pi (~400 mA peak) + display (~700 mA peak)
+ headroom = 5A is conservative.

## Sleep states

| State | ESP32 | Pi | Display | Total current @ 12V |
|---|-----|-----|-----|-----|
| Deep idle (car parked, healthy battery) | Deep sleep 60s, wake 100ms | Off (MOSFET cut) | Off | ~0.5 mA average |
| Voltage check (every 60s for ~100ms) | Active read | Off | Off | ~30 mA (1% duty) |
| Trigger sequence | Active, TX, then monitor | Booting then logging | Off until Pi boots ready | ~200-600 mA |
| Engine running window (15 min) | Active CAN logging | Active UI + MQTT | Active | ~600-1000 mA |
| Post-shutdown cleanup | Active 30s | Halting | Off | ~150 mA |

Average over a typical parked day with one trigger event: roughly 5-10 mAh from the battery. A 60 Ah car battery loses ~0.01-0.02% per day to the monitor. Effectively zero.

## Trigger sequence timeline

```
t=0:00   Voltage hit threshold (e.g., 12.2V sustained 5 minutes)
t=0:00   ESP32 fires Compustar START packet via CC1101 (captured fixed-code; see sdr/analysis/framing.md)
t=0:00   ESP32 turns on MOSFET → Pi power on
t=0:05   ESP32 sees alternator voltage rise on Pin 16 → start confirmed
t=0:25   Pi finishes boot, opens UART to ESP32
t=0:30   Pi posts "started, V was X.X" via MQTT
t=15:00  ESP32 re-fires Compustar START packet via CC1101 (1WSHR-PRO uses same button to stop)
t=15:05  ESP32 confirms engine off via voltage drop + CAN silence
t=15:05  ESP32 sends shutdown_pi COMMAND over UART to Pi
t=15:15  Pi halts cleanly
t=15:45  ESP32 cuts MOSFET → Pi powerless
t=15:45  ESP32 back to deep-sleep / 60-second polling cadence
```

## Failure modes (planned)

| Failure | Detection | Response |
|---|---|---|
| Keeloq packet sent but engine doesn't start | No alternator V rise within 30s | Retry once (sync may have drifted). If still nothing, alert via MQTT/SMS. |
| Engine starts but stalls within run window | CAN bus goes silent, voltage drops | Send STOP packet. Log incident. Alert. |
| Pi fails to boot | UART silent after 60s | Continue without Pi for this cycle. Log via ESP32-direct MQTT. |
| Pi fails to shut down cleanly | UART still active after 30s | Force MOSFET cut (SD card may need fsck on next boot — accept the loss for safety) |
| Buck converter dies | 5V rail collapses, ESP32 resets | Nothing we can do remotely; battery now safe behind fuse |
| 12V transient (load dump) | TVS clamps | Continue normally |
