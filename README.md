# 2009 Kia Spectra — voltage-triggered remote start

Goal: **never come out to a dead battery in winter.** When the battery
voltage drops below a sustained threshold, an in-car ESP32 fires the
factory-installed Compustar remote-start by replaying its FOB's RF
packet over a CC1101 transmitter. Engine runs for 15 minutes (the
alternator tops the battery off), then shuts off. Repeats only after a
cooldown so it can't loop. Average current draw is sub-mA between
checks (deep sleep).

That's it for v1 — one ESP32, one CC1101, one battery sensor, and
enough enclosure + wiring to live behind the OBD-II port. **No Pi, no
display, no MQTT** in the shipped v1 build (see "v2 — optional
telematics" below).

## Scope decision 2026-06-01

This project briefly grew a full Raspberry Pi + 5" touchscreen + web
dashboard + MQTT + SNMP + map view side ("telematics") on top of the
core trigger. **Pulled back to ESP32-only as v1.** The Pi side is
parked at the existing quality level (it works, all 199 tests pass,
provisioning is documented) and can be opted into later if the
telematics half becomes interesting. The split is purely a scope
choice — the ESP32 firmware was always self-sufficient for the trigger.

## How the trigger works

1. ESP32 sleeps. Every `WAKE_INTERVAL_S` (default 60 s) it wakes
   briefly, reads battery voltage via an ADS1115 + voltage divider,
   goes back to sleep. Average current: sub-mA.
2. If voltage is below `LOW_V_TRIGGER` (default 12.2 V) for a sustained
   `LOW_V_SUSTAIN_S` (default 300 s — five consecutive low samples at
   the default interval), the controller transitions to STARTING.
3. STARTING sends the captured 1WSHR-PRO "Start" packet 8 times over
   the CC1101 at 433.92 MHz. The Compustar brain accepts the replay
   exactly the way it accepts a real FOB press — engine cranks.
4. RUNNING holds for `RUN_DURATION_S` (default 15 min). Alternator
   tops the battery off.
5. STOPPING sends the "Lock" / engine-stop packet, then transitions to
   COOLDOWN.
6. COOLDOWN holds for `START_COOLDOWN_S` (default 2 h) before the
   trigger can re-arm. Prevents loop-on-bad-sensor.

The Compustar trigger is performed by **software synthesis of the
FOB's RF signal** — the ESP32 plays the role of the remote, so the
original FOB stays in the house. SDR analysis confirmed the specific
1WSHR-PRO model is a Compustar 1WG3R-family **fixed-code** FOB: same
press = identical 35-bit packet on the wire, so the ESP32 just
captures each button's pattern once and replays verbatim. No KeeLoq
encryption or device-key recovery required for this FOB. (The KeeLoq
cipher module is retained in `esp32/src/lib/keeloq.py` for any
HCS-KeeLoq Compustar variants other readers might be working with.)

## Build status

**Bench-ready software, awaiting parts.** As of the most recent commit:

- **199 unit + integration tests** passing across the project
  ([CI](.github/workflows/tests.yml) runs every push across Py
  3.10/3.11/3.12; tested locally on 3.13 as well) — covers ESP32
  firmware, Pi-side daemon (parked v2), and the SDR analysis pipeline
- All v1-essential firmware written: Compustar 1WG3R-family packet
  replay, OBD-II PIDs (parked v2), UART protocol (parked v2), state
  machine, deep sleep + RTC-memory streak persistence, watchdog,
  drivers for CC1101 + ADS1115 + TWAI (parked v2)
- Phase 1 (SDR walkthrough) and Phase 2 (bench-prototype docs 08-12)
  complete and reproducible
- Tooling: simulator (`esp32/scripts/simulate.py`), pre-flight check
  (`tools/preflight.py`), mpremote install scripts, SDR analysis
  helpers

Remaining v1 work is hardware-bound: see
[`docs/day-one.md`](docs/day-one.md) for the take-to-the-bench
checklist. Day-by-day progress in [`logs/`](logs/).

## v1 hardware architecture (the shipped build)

```
                +12V (OBD-II Pin 16, always-hot)
                           │
              [2A blade fuse] + [TVS SMBJ24CA]
                           │
                [12V → 5V buck converter, 1A]
                           │
                          5V
                           │
       ┌───────────────────┴────────────────────┐
       │     ESP32-WROOM-32U                     │
       │       - ADS1115 over I²C → battery V    │
       │       - CC1101 over SPI → 433 MHz RF    │
       │       - Deep sleep <50 µA between polls │
       └─────────────────────────────────────────┘
                           │
                  CC1101 + 433 MHz whip
                           │
                          ⟶ Compustar brain (already in dash)
                              fires factory remote-start
```

Single board, one enclosure, drops into the existing OBD-II passive
Y-splitter for a non-permanent install.

## v2 — optional telematics (deferred)

The repo also ships a Raspberry Pi-side daemon that adds, on top of
v1:

- 5" touchscreen dashboard (gauges + map toggle + manual controls)
- Web UI / `/api/state` JSON over Wi-Fi
- MQTT publisher + subscriber (Home Assistant integration ready)
- SNMPv2c responder (LibreNMS / Cacti integration)
- Persistent OBD-II trip log + sparklines
- mosquitto broker for the LAN

All of that is **fully built and tested** on a bench Pi, but **not
required for v1** — the ESP32 trigger works standalone with no Pi
wired up. To opt into v2, add the Pi-side hardware from the deferred
BOM, deploy per [`docs/10-pi-setup.md`](docs/10-pi-setup.md), and
connect the UART link per [`docs/11-uart-link.md`](docs/11-uart-link.md).

The slypi (bench Pi) deployment is the reference v2 install; see the
private vault note for runtime details that can't go on a public
remote.

## Repository layout

| Directory | Contents | v1 needs it? |
|---|---|---|
| `docs/` | BOM, architecture, Compustar research, reproduction walkthroughs | Yes |
| `esp32/src/` | MicroPython firmware: controller, drivers, UART, KeeLoq, packet replay | **Yes — this is the v1 build** |
| `esp32/tests/` | CPython unit tests for the firmware (no hardware needed) | Yes |
| `sdr/` | RTL-SDR capture walkthrough, demod scripts, synth-driven tests | Yes (one-time FOB capture) |
| `pi/app/` | Pi daemon: STATE, UART listener, MQTT, Flask dashboard, SNMP responder | No — v2 only |
| `pi/systemd/` + `pi/setup/` | Pi systemd unit + idempotent `provision.sh` | No — v2 only |
| `pi/tests/` | Pi-side unit tests | No — v2 only |
| `logs/` | Day-by-day build journal | Reference |
| `braindump.md` | Compaction-safe context dump (gitignored) | Reference |

## Reproducing this build (v1 — ESP32-only)

The full path from "nothing" to "engine running on a voltage trigger":

1. [`docs/BOM.md`](docs/BOM.md) — what to order. ~$95 CAD for v1
   essentials; ~$120 more if you also want v2 telematics.
2. [`docs/architecture.md`](docs/architecture.md) — how the v1 pieces
   fit together (and the v2 add-on architecture for reference).
3. [`docs/reproduce.md`](docs/reproduce.md) — the master walkthrough
   index.
4. **Phase 1**: [`sdr/README.md`](sdr/README.md) — capture and decode
   your FOB once. Produces the 35-bit pattern strings that the firmware
   replays.
5. **Phase 2**: [`docs/08-flash-micropython.md`](docs/08-flash-micropython.md)
   → [`docs/12-bench-validation.md`](docs/12-bench-validation.md) —
   bench prototype, then first car test.
6. **Phase 3 (v1 install)**: docs 13-16 cover the in-car install. Docs
   17-20 are v2 (MQTT + Home Assistant + long-term-stability + SNMP),
   skip them for v1.

## Running the tests + linter

```powershell
# ESP32 (firmware) tests — pure CPython, no hardware required
foreach ($f in Get-ChildItem esp32/tests/test_*.py) { python $f.FullName }

# Pi tests (v2 telematics — skip if you're v1-only)
foreach ($f in Get-ChildItem pi/tests/test_*.py) { python $f.FullName }

# SDR tests — synth-driven round-trips through the demod pipeline
foreach ($f in Get-ChildItem sdr/tests/test_*.py) { python $f.FullName }

# Lint (catches real bugs — unused imports, undefined names, etc.)
python -m pip install ruff
python -m ruff check .
```

Or just push — CI runs tests + lint across Python 3.10/3.11/3.12. See
[`pyproject.toml`](pyproject.toml) for the ruff config (conservative
ruleset: `F`, `E9`, `W6`).

## License

Personal project. Not licensed for redistribution yet — to be decided
once it ships.
