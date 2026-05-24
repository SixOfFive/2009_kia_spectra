# 2009 Kia Spectra — voltage-triggered remote start + telematics

An in-car ESP32 + Raspberry Pi Zero 2 W unit that:

- Monitors battery voltage continuously at sub-mA average draw (deep sleep)
- Triggers the factory-installed Compustar remote start when voltage drops below a configurable threshold for a sustained period
- Runs the engine for 15 minutes, then shuts it off
- Streams live OBD-II data (RPM, speed, coolant, fuel, voltage, etc.) over CAN
- Displays gauges + status on a 5" touchscreen
- Exposes state to the home network via WiFi (web UI + MQTT)

The Compustar trigger is performed by software synthesis of the KeeLoq rolling-code RF signal — the ESP32 plays the role of the FOB, allowing the in-car system to stay self-contained (FOB remains in the house).

## Build status

**Bench-ready software, awaiting parts.** As of the most recent commit:

- 74 unit + integration tests passing across the project ([CI](.github/workflows/tests.yml) runs on every push)
- All hardware-independent firmware written (KeeLoq cipher, HCS packet builder, OBD-II, UART protocol, state machine, drivers for CC1101 / ADS1115 / TWAI)
- Pi-side daemon, dashboard, MQTT publisher, systemd unit, and provisioning script all written
- Phase 1 (SDR walkthrough) and Phase 2 (bench-prototype docs) complete and reproducible
- Deep-sleep + RTC-memory streak persistence wired into the controller

Remaining work is hardware-bound: smoke-test each module on a breadboard, follow the SDR walkthrough to recover the KeeLoq device key from the FOB, then do the in-car Lock test before any Start attempt.

Track day-by-day progress in [`logs/`](logs/).

## Hardware architecture

```
                       +12V (OBD-II Pin 16, always-hot)
                                  │
                       [1A inline fuse] + [TVS SMBJ24CA]
                                  │
                   [12V → 5V buck converter, 5A]
                                  │
              ┌───────────────────┼───────────────────┐
              │                   │                   │
            5V to             5V to                5V to
           ESP32           Pi (gated by         5" HDMI
        (always on)        AO3401A MOSFET       touchscreen
                           controlled by
                           ESP32 GPIO)

   ┌────────────────────────┐         ┌─────────────────────────┐
   │ ESP32-WROOM-32U        │ UART    │ Raspberry Pi Zero 2 W   │
   │ - ADS1115 voltage mon  │◄───────►│ - 5" HDMI touch display │
   │ - CC1101 RF transmit   │  3-wire │ - Web UI + MQTT         │
   │ - SN65HVD230 CAN ↔ OBD │  JSON   │ - Persistent logging    │
   │ - Deep sleep <10 µA    │  115200 │ - chromium kiosk        │
   └────────────────────────┘         └─────────────────────────┘
              ▲                                  ▲
              │ CAN-H, CAN-L, GND, +12V         │ HDMI, USB (touch)
              │                                  │
   OBD-II passive Y-splitter ── pigtail to case
```

## Repository layout

| Directory | Contents |
|---|---|
| `docs/` | BOM, architecture, Compustar research, reproduction walkthroughs (steps 08-12) |
| `esp32/src/` | MicroPython firmware: controller state machine, drivers, KeeLoq + HCS, UART protocol |
| `esp32/tests/` | CPython unit tests for all of the above (run without hardware) |
| `pi/app/` | Pi daemon: shared STATE, UART listener, MQTT publisher, Flask dashboard |
| `pi/systemd/` + `pi/setup/` | systemd service unit + `provision.sh` for fresh Pi setup |
| `pi/tests/` | Pi-side unit tests |
| `sdr/` | RTL-SDR capture walkthrough (steps 01-07), helper scripts, framing template |
| `logs/` | Day-by-day build journal |
| `braindump.md` | Compaction-safe context dump for picking up where the last session left off |

## Reproducing this build

This documentation describes a **single** unit. The full path from "nothing" to "engine running on a voltage trigger":

1. [`docs/BOM.md`](docs/BOM.md) — what to order (~$260 CAD per unit)
2. [`docs/architecture.md`](docs/architecture.md) — how the pieces fit together
3. [`docs/reproduce.md`](docs/reproduce.md) — the master walkthrough index
4. Phase 1: [`sdr/README.md`](sdr/README.md) — capture and reverse-engineer the FOB
5. Phase 2: [`docs/08-flash-micropython.md`](docs/08-flash-micropython.md) → [`docs/12-keeloq-bench-validation.md`](docs/12-keeloq-bench-validation.md) — bench prototype + first car test
6. Phase 3-4: in-car install + polish (written as those phases happen)

## Running the tests

```powershell
# ESP32 (firmware) tests — pure CPython, no hardware required
for ($t = 0; $t -lt 1; $t++) { foreach ($f in Get-ChildItem esp32/tests/test_*.py) { python $f.FullName } }

# Pi tests
foreach ($f in Get-ChildItem pi/tests/test_*.py) { python $f.FullName }
```

Or just push — CI runs them across Python 3.10/3.11/3.12.

## License

Personal project. Not licensed for redistribution yet — to be decided once it ships.
