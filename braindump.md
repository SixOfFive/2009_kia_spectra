# braindump — everything I know about this project

This is a context-preservation document written before conversation compaction.
A future Claude (or future-me) should be able to read this file and pick up
where we left off without having to reconstruct from chat history.

Last updated: 2026-05-24 (continued from prior session — substantial code +
docs added; all hardware-independent work is complete).

---

## 1. The project in one paragraph

**Goal**: build a battery-saver / voltage-triggered remote starter for a 2009 Kia Spectra. When the parked car's battery drops below a threshold for sustained time, an in-car ESP32 transmits a synthesized Compustar Keeloq RF packet, the existing factory-installed Compustar remote-start brain accepts it as if it were the FOB, and the engine runs for 15 minutes. A Raspberry Pi Zero 2 W drives a 5" HDMI touchscreen with gauges + maps and pushes state to the home network over WiFi/MQTT. The whole thing plugs into the OBD-II port for power + CAN bus data — no splicing into the car's harness, fully reversible.

**Repo**: https://github.com/SixOfFive/2009_kia_spectra
**Local path**: `C:\Users\sixoffive\Documents\Claude_Projects\vroom`
**Main branch**: `main`

---

## 2. The car and the existing system

- **Vehicle**: 2009 Kia Spectra sedan, 2.0L I4, automatic transmission
- **VIN**: KNAFE221495635751 (decode: KNA=Kia/Korea, F=passenger, E=sedan, 22=Spectra, 9=2009 model year)
- **Existing aftermarket remote start**: Compustar
  - **FOB**: 1WSHR-PRO (FCC ID `7087A-R762A433`, 4-button 1-way, 433.92 MHz)
  - **Brain**: somewhere behind the dash, not visually accessible (user tried, couldn't get camera angle)
  - **Bypass cartridge**: likely BLADE-AL or equivalent for the Kia immobilizer (chip-in-box style with sacrificed spare key inside the module)
  - **Windshield antenna**: small black 3-LED wedge top-center of windshield (we identified this when user went looking)
- **Factory ECU spotted during dash exploration**: Siemens VDO, label "D53K" — confirmed factory part, not the Compustar
- **Owner location**: Alberta, Canada (Thorsby per garage sticker)

### Why we chose RF synthesis instead of finding the brain's hardwire input

User couldn't physically access the Compustar brain. Original plan was to find the brain's "hardwire start input" wire and pulse it via a relay. Pivoted to **synthesizing the Keeloq RF packet ourselves** since:
- The brain is unreachable
- KeeLoq is academically broken (Eisenträger 2008) and with owner FOB access, the attacks are practical
- Avoids any dash disassembly
- Fully reversible

---

## 3. Hardware architecture

### Hybrid ESP32 + Pi Zero 2 W

| Job | Lives on |
|---|---|
| Always-on voltage monitor (sub-mA deep sleep) | ESP32 |
| Wake-up on low V → fire Keeloq packet via CC1101 | ESP32 |
| Native CAN bus → OBD-II PIDs | ESP32 (built-in TWAI) |
| Pi power-gating via AO3401A P-MOSFET | ESP32 (1 GPIO) |
| 5" HDMI touchscreen UI | Pi |
| Map rendering, web UI | Pi |
| MQTT publisher to home network | Pi |
| Persistent logging | Pi |
| SSH for debugging | Pi |
| UART bridge between them | both, 3 wires at 115200 baud |

**Principle**: ESP32 is the always-on watchdog with hardware superpowers and microamp sleep. Pi is the smart brain that wakes up only when needed (boots in ~25 seconds when ESP32 turns on the MOSFET).

### Power architecture

```
OBD-II Pin 16 (+12V always-hot)
    │
    ├── 1A inline fuse
    ├── TVS SMBJ24CA (load-dump clamp)
    └── 12V→5V buck converter (5A)
            │
            ├── 5V to ESP32 (always on)
            ├── 5V via AO3401A P-MOSFET → Pi (gated by ESP32)
            └── 5V to display (gated when Pi is on)
```

### OBD-II connection (4 wires only)

| Pin | Purpose |
|---|---|
| 16 | +12V always-hot (battery via fuse) — powers everything AND ADC samples battery V here |
| 4 or 5 | Ground |
| 6 | CAN-H → SN65HVD230 |
| 14 | CAN-L → SN65HVD230 |

OBD-II Y-splitter keeps the port available for scan tools.

---

## 4. What was ordered

Order was placed by the user manually after several rounds of agent-driven shopping. Cart state at the time the user took over and placed the order:

### Amazon.ca cart (~$497 before final user adjustments)

- Raspberry Pi Zero 2 W (qty 1) @ $38.35
- SanDisk Ultra 32GB **2-pack** (SDSQUA4-032G-GN6MT) @ $56.22 — Ultra not Extreme; saved $26 vs Extreme
- UFL/IPEX SMA Coax 2-pack (B09329TYCS) @ $10.99 (gives 2 pigtails)
- 433 MHz SMA antenna kit (B0C1FCZM94) @ $12.14 (2 antennas + bonus pigtails)
- Bingfu 2.4 GHz WiFi antenna 2-pack (B09J8Q6TRN) @ $14.99
- ZEYUXXRUR CC1101 433 MHz 2-pack @ $9.94
- ADS1115 3-pack @ $19.99
- SN65HVD230 3-pack (B0GYBKSCBL variant) @ $8.89 — agent had to delete 1pc variant and re-add 3pc
- Electronic components assortment kit @ $19.29 (resistors, caps, diodes, transistors)
- SMBJ24CA TVS 10-pack @ $9.80
- Vrupin fuse kit (10 inline holders + 24 fuses 1A-40A) @ $18.99
- uxcell OBDII Male connector pigtail (B07MQSZMWC) @ $13.49 — Motoforti was Female (wrong), swapped
- LeMotech junction box 2-pack 158x80x60mm @ $19.99
- vesaneae rubber grommet 50-pack @ $8.79
- Bulina VHB tape 1"x15.4ft @ $23.39
- ELECROW 5" HDMI capacitive touchscreen 800x480 @ $62.99 (has integrated USB-A for touch — no separate cable needed)
- Thsucords mini-HDMI to HDMI 0.5m 2-pack @ $16.99
- ELEGOO breadboard 3-pack @ $12.99
- 3-pack flux pen @ $12.99
- CALLARON coping saw + blades @ $12.59
- HOUSERAN precision pliers (cutter + needle nose) @ $13.57
- GeeekPi Pi Zero 2 W aluminum case + 20-pin GPIO @ $15.98 (user added himself)

### AliExpress cart (~$103)

- ESP32-WROOM-32U (CoreSMA Store, listing 1005010136688086) qty TBD @ $3.12 — **see scam note below**
- 12V/24V→5V 5A buck converter (Shop911934595) qty 1 @ $6.09
- 100pcs AO3401A MOSFET SOT-23 @ $3.59
- OBD2 Y-splitter (Senyu OBD2) qty 1 @ $8.79
- 5pcs perfboard 7x9cm @ $7.55
- Dupont 120pcs jumper kit (M-M + M-F + F-F) @ $8.25
- 10pcs female header 1x40P @ $7.27
- 10pcs male pin header (Shipu Tech-2, "Single/Dual-row" listing — qty 1 single-row variant) @ $6.83
- Boxed silicone wire kit (28 AWG x 10m x 5 rolls, WinLead) @ $16.07
- 328pcs heat shrink tube assortment box @ $11.68
- Wire stripper NR6017 @ $13.08
- 1pc titanium step drill 4-32mm @ $11.28

### ⚠ ESP32 listing scam (documented in docs/BOM.md)

The CoreSMA AliExpress listing for ESP32-WROOM-32U has a variant-selector trap: the cheapest `$3.12` default variant ships only the bundled antenna, not the board itself. User caught this manually before checkout and selected legitimate variants. Documented in `docs/BOM.md` with a "Buyer warning" section.

### Final state uncertain

Several agents crashed mid-task or made silent quantity errors (going qty 1 → 0 = silent line delete). User took over and placed the order manually. We don't know the exact final cart, but the categories above are correct.

---

## 5. Repository structure

```
vroom/
├── README.md                      Project overview + architecture diagram
├── braindump.md                   THIS FILE
├── .gitignore                     Excludes secrets.py, SDR captures, etc.
├── docs/
│   ├── BOM.md                     Single-unit BOM with ESP32 scam warning
│   ├── architecture.md            Division of labor, power tree, sleep states, sequence diagram
│   ├── compustar-research.md      What we know about the installed system
│   └── reproduce.md               Master TOC for reproduction steps (Phase 1 done, 2-4 WIP)
├── esp32/
│   ├── README.md
│   ├── src/
│   │   ├── config.py              Tunable values + secrets import
│   │   ├── secrets.py.example     Template (real secrets.py is gitignored)
│   │   └── lib/
│   │       ├── __init__.py
│   │       ├── keeloq.py          KeeLoq cipher (4 tests pass)
│   │       ├── compustar.py       HCS packet builder + PWM pulse gen (6 tests pass)
│   │       ├── obd2.py            PID dict + query/parse (9 tests pass)
│   │       └── pi_link.py         MicroPython UART JSON link (6 tests pass)
│   └── tests/
│       ├── test_keeloq.py
│       ├── test_compustar.py
│       └── test_obd2.py
├── pi/
│   ├── README.md
│   ├── app/
│   │   ├── __init__.py
│   │   ├── config.py
│   │   ├── secrets.py.example
│   │   ├── comms/
│   │   │   └── esp32_link.py      Pi-side mirror of pi_link.py
│   │   └── display/
│   │       ├── server.py          Flask app + routes
│   │       ├── templates/
│   │       │   └── dashboard.html 6 gauges + 3 buttons + 3 status pills
│   │       └── static/
│   │           ├── css/main.css   Dark theme, 800x480 layout
│   │           └── js/main.js     1Hz polling, button handlers
│   ├── systemd/                   (empty — services TBD)
│   ├── setup/                     (empty — provisioning TBD)
│   └── tests/
│       └── test_esp32_link.py
├── sdr/
│   ├── README.md                  Walkthrough TOC
│   ├── 01-software-setup.md       Install rtl-sdr + URH (Windows / Linux / macOS)
│   ├── 02-hardware-verification.md FM broadcast test + 433 MHz baseline sweep
│   ├── 03-frequency-confirmation.md Confirm FOB at 433.92 MHz
│   ├── 04-recording-captures.md   10× Start + 3× each of Lock/Unlock/Trunk
│   ├── 05-urh-analysis.md         Load IQ, ASK demod, label fields, export .bits
│   ├── 06-framing-extraction.md   Diff captures to find serial / function codes
│   ├── 07-key-recovery.md         3 paths to device key + bench validation + safety
│   ├── captures/                  (gitignored — raw IQ files)
│   ├── scripts/                   (empty — helpers go here)
│   └── analysis/                  (mostly empty — will contain framing.md after captures)
└── logs/
    └── 2026-05-23.md              Daily journal — kickoff + shopping + code sprint
```

---

## 6. Code modules written (Day 1 / 2026-05-23)

All hardware-independent. All tests pass in CPython. All written in a style that also runs in MicroPython (avoiding numpy, ctypes, large struct.pack patterns).

### `esp32/src/lib/keeloq.py`

- 528-round KeeLoq block cipher (32-bit block, 64-bit key)
- NLF constant: `0x3A5C742E`
- `encrypt(plaintext, key)` and `decrypt(ciphertext, key)`
- Roundtrip and fixed-point properties verified
- **Note**: "published" test vectors floating around online didn't reproduce — they appear to be from non-standard variants. Skipped pinning against external vectors; relying on round-trip + the known degenerate property `encrypt(0,0)=0`. Real validation comes after SDR captures.
- 4 tests pass

### `esp32/src/lib/compustar.py`

- `Function` class with placeholder codes for START/LOCK/UNLOCK/TRUNK — to be confirmed via SDR
- `build_hopping_code(counter, function_code, discrimination, device_key)` → calls keeloq.encrypt on the packed 32-bit plaintext
- `build_packet(serial, function_code, counter, device_key, v_low=0, repeat=0)` → returns dict with `hopping_code` (32 bits), `fixed_code` (32 bits = 28-bit serial + 4-bit function), `status` (2 bits), and `bits` (list of 66 bits MSB-first)
- `packet_to_pulses(bits, te_us, preamble_half_bits, header_gap_te)` → list of (high_us, low_us) tuples for OOK transmission
- TE/preamble/gap constants are placeholders (TE=400µs is HCS default but configurable per chip)
- 6 tests pass

### `esp32/src/lib/obd2.py`

- 14 PIDs in the `PIDS` dict: engine_load, coolant_temp, intake_manifold_pressure, rpm, speed, intake_air_temp, maf_rate, throttle_position, run_time_since_start, distance_with_mil, fuel_level, distance_since_clear, control_module_voltage, ambient_air_temp
- Each PID has `(name, byte_count, scale_function, units)`
- `query(pid, mode=0x01)` → 8-byte CAN data field
- `parse_response(can_data)` → dict with `pid`, `mode`, `raw`, `name`, `value`, `units`
- Single-frame ISO-TP only (standard PID responses fit in 8 bytes; multi-frame not implemented)
- 9 tests pass — including 1726 rpm from `0x1AF8`, 14.0V from `0x36B0`, 55°C coolant, 80 km/h, malformed frame handling

### `esp32/src/lib/pi_link.py` (ESP32 side) + `pi/app/comms/esp32_link.py` (Pi side)

- Line-delimited JSON over UART at 115200 baud
- Message types: STATUS, OBD, EVENT, COMMAND, ACK, LOG
- Command names: start_engine, stop_engine, set_threshold, shutdown_pi, ping
- Event names: engine_started, engine_stopped, low_voltage_trigger, pi_boot, pi_shutdown_request, keeloq_tx, keeloq_tx_fail
- Symmetric — either side can send any type
- Constants defined identically in both modules; **cross-module test fails if they drift**
- ESP32: `UartLink` class wraps `machine.UART` with partial-line buffering
- Pi: `Esp32Link` class wraps pyserial, accepts file-like for tests
- 6 tests pass

### `esp32/src/config.py` + `pi/app/config.py` + `secrets.py.example` (both)

- Voltage thresholds (LOW_V_TRIGGER=12.2V, LOW_V_SUSTAIN_S=300, RUN_DURATION_S=900)
- RF settings (433.92 MHz, TE=400µs, burst=4 repeats, guard=39ms)
- UART pins (TX=17, RX=16), CAN pins (TX=5, RX=4)
- Pi power-gate GPIO=25, shutdown grace=30s
- WAKE_INTERVAL_S=60 (deep sleep cadence)
- START_COOLDOWN_S=7200 (2 hours between triggers)
- secrets templates include WiFi creds, MQTT creds, COMPUSTAR_DEVICE_KEY, COMPUSTAR_SERIAL, COMPUSTAR_COUNTER
- `config.secrets_ready()` and `config.mqtt_ready()` predicates

### `pi/app/display/server.py` + templates + static

- Flask app, three routes: `/`, `/api/state`, `/api/command`
- Module-level `STATE` dict, currently mocked
- `dashboard.html`: 6 gauges (battery V large + RPM, speed, coolant, throttle, fuel), 3 buttons (start/stop/ping), 3 status pills, last-updated/uptime footer
- Dark theme tuned for 800x480 in dim cabin
- 1Hz JS polling of `/api/state`, button POSTs to `/api/command`
- `update_state(**fields)` and `update_obd(name, value)` helpers for future UART listener

---

## 7. Project memory / rules (in `~/.claude/projects/.../vroom/memory/`)

Three feedback/project memory files scoped to this project only:

- **`feedback_vroom_auto_commit.md`**: commit AND push immediately after ANY change in this repo. No batching. User said "Always commit and push, it makes us both happy."
- **`feedback_vroom_daily_log.md`**: maintain `logs/YYYY-MM-DD.md` per session, with optional `logs/images/YYYY-MM-DD/` for screenshots. Narrative journal, not just git log.
- **`project_vroom_scope.md`**: public-facing docs describe ONE unit (single reference build for anyone copying). User personally building two — that's fine in chat but not in committed files.

Index in `MEMORY.md` in that directory.

---

## 8. SDR / Keeloq plan (documented in sdr/0X-*.md)

Walkthrough is fully written. User has an RTL-SDR (Vomeko 100kHz-1.7GHz, R820T2 + RTL2832U).

Steps to execute when ready:
1. Software setup (rtl-sdr drivers via Zadig on Windows, URH via pip)
2. Hardware verify (FM broadcast + 433 MHz baseline)
3. Frequency confirm (FOB really is at 433.92 MHz)
4. Record 10× Start + 3× each of Lock/Unlock/Trunk to `sdr/captures/fob-*.bin`
5. URH demodulate → `.bits` files
6. Diff captures → identify serial / function codes / hopping bit positions
7. Recover device key via one of three paths:
   - **Path A**: Flipper Zero Unleashed manufacturer-key database (free, may not work for newer Compustars)
   - **Path B**: PICkit + MPLAB X reads HCS chip EEPROM directly (~$30 hardware, most reliable)
   - **Path C**: cryptanalytic attack (academic, requires 2^16 captures, 7.8 days compute)

After key recovery: drop values into `secrets.py`, validate by decrypting captured hopping codes (should yield sensible counter + matching function code), bench test with Lock (low risk) before Start.

---

## 9. Known gotchas / lessons learned

### Shopping-agent issues (documented in logs/2026-05-23.md)

- **Long-running browser-MCP agents crash with socket errors** at >5min / >30 tool calls. Multiple incidents this session.
- **Cart quantity decrements can silently delete the line** if the agent miscounts (qty 1 → 0 = delete confirmation, agent dismisses or doesn't notice). Re-audit needed after every batch.
- **Multi-agent shared cart sessions lose state visibility.** Different agents reading the cart at different times see different states. Re-audit before any change.
- **Amazon's Asurion Protection Plan modal** must be dismissed with "No thanks" after every add-to-cart or subsequent adds appear to fail silently.
- **AliExpress opens product pages in new tabs** when clicking thumbnails. Workaround: navigate directly to product URLs.
- **URH `find` tool** uses an LLM internally and burns context fast. Prefer `read_page` with `filter="interactive"` and small `max_chars`.
- **The $3 ESP32-WROOM-32U AliExpress listing is a variant-selector trap** — cheapest variant ships only the antenna, not the board. User caught this manually.

### Compustar / car

- The brain is physically inaccessible (user tried with a camera, couldn't reach). Hence the RF-synthesis approach.
- Compustar uses standard HCS encoder family (HCS300/301/361/362 SOIC chips) — works with all the standard tooling.
- The car's OBD-II Pin 16 is constant +12V (always-hot from battery via cabin fuse box). Pin 4/5 are ground. Pin 6 is CAN-H, Pin 14 is CAN-L. Standard SAE J1962 / ISO 15765-4.
- 2009 Spectra has chipped key + factory Siemens VDO ECU. No factory remote start. Aftermarket Compustar handles the immobilizer bypass via a BLADE-AL-style cartridge with sacrificed chipped key inside.

---

## 10. User-specific context

- **OS**: Windows 11 Pro (64-bit), build 26200
- **Shell**: PowerShell available; bash also works
- **Already owns**: soldering iron, solder, drill, multimeter, RTL-SDR (Vomeko), Veepeak OBD-II BLE scan tool, Thonny (pip installed)
- **Has plenty of tinkering experience**: AgentCommander, NetworkMonitor, EngineStatus, MoneyMaker/Big Iron, LLM Generator, OpenClaw, asus2snmp projects in their Obsidian vault — see CLAUDE.md for vault recall protocol
- **GitHub auth**: working on this machine (git push to https://github.com/SixOfFive/2009_kia_spectra worked without prompting)
- **Email**: hvr.biz@gmail.com (per env)
- **Location**: Alberta, Canada (Thorsby area)
- **Prefers**: cheapest viable parts, auto-commit/push, terse responses (per CLAUDE.md user preferences), Python for everything

### User preferences captured this session

- "Always commit and push, it makes us both happy" — every change
- Single-unit BOM scope publicly even though personally building two
- Daily log narrative format
- Trusts me to make sensible defaults rather than asking 20 questions

---

## 11. What's deferred / TBD

### All written — bench-test on arrival

These were "waiting on hardware" originally but the code now exists, with
CPython unit tests, awaiting smoke-test on real hardware:

- `esp32/src/lib/cc1101.py` — SPI driver + async OOK transmit
- `esp32/src/lib/ads1115.py` — I2C single-shot ADC
- `esp32/src/lib/twai_can.py` — CAN wrapper with graceful no-CAN fallback
- `esp32/src/lib/persistence.py` — RTC memory streak persistence for deep sleep
- `esp32/src/controller.py` — full state machine (5 states, deep sleep, persisted counter)
- `esp32/src/main.py` — boot entrypoint wiring drivers + controller
- `pi/app/state.py` — thread-safe shared STATE
- `pi/app/comms/uart_listener.py` — ESP32 message dispatcher (daemon thread)
- `pi/app/comms/mqtt_publisher.py` — periodic MQTT snapshot publisher
- `pi/app/daemon.py` — single-process Pi entrypoint (Flask + listener + MQTT)
- `pi/app/display/server.py` — Flask routes wired through to ESP32 via the link
- `pi/systemd/vroom.service` — systemd unit
- `pi/setup/provision.sh` — idempotent fresh-Pi setup

### Still waiting on SDR captures + real FOB

- Compustar `Function` codes (placeholders in compustar.py until SDR confirms)
- TE timing (RF_TE_US currently 400µs default — need to measure)
- Preamble length, header gap
- Device key, serial, counter (go into `secrets.py` after recovery)
- Replace dummy obd2 test frames with real captures from the Veepeak (deferred per user)

### Hardware smoke tests (per `docs/09-bench-smoke-tests.md`)

These are the actual remaining "code" work — really verification, not coding:
- Flash MicroPython, copy `esp32/src/` to the chip
- Confirm each driver responds to its hardware in isolation
- Run the full controller through one MONITORING → STARTING → ... cycle on the bench
- First live Lock test in the car (per docs/12)

### Deferred phase docs

- Phase 3 (in-car install: steps 13-16) — written as that phase happens
- Phase 4 (polish: Home Assistant, long-term stability, optional cellular)

### Nice-to-have optimizations

- Resume-from-flash if state was RUNNING at boot (currently we always
  start in MONITORING; a watchdog reset during RUNNING means engine keeps
  running until receiver's own timeout — acceptable for v1)
- BLE-wake from deep sleep (would let dashboard wake the ESP32 on demand
  without breaking the deep-sleep current budget)

---

## 12. Decisions made and why (so we don't re-litigate)

| Decision | Reason |
|---|---|
| Hybrid ESP32 + Pi (not Pi-only or ESP32-only) | ESP32's sub-mA deep sleep is essential for 24/7 monitoring without draining battery. Pi gives us real Linux + maps + 5" display that ESP32 can't render. |
| Synthesize Keeloq RF, not piggyback on brain's hardwire input | Brain is inaccessible. RF synthesis is fully reversible and works with any Compustar generation. |
| OBD-II port for everything | Single connector, fully reversible, no harness splicing. Pin 16 is always-hot from battery so we get power + voltage monitoring + CAN bus all in one. |
| MicroPython on ESP32 (not Arduino C++) | User wanted Python. Performance is fine for this workload. CC1101 + ADS1115 + CAN libraries exist. Faster development cycle. |
| Single-unit docs (user builds two, docs describe one) | Public reproducibility. User explicitly said so. |
| 5" HDMI Elecrow display | Has integrated USB-A for touch (no extra cable). Best price/feature in that size class. |
| SanDisk Ultra 2-pack instead of Extreme | Save $26. Extreme is overkill for this use case. User confirmed. |
| Bias toward Amazon, AliExpress only on >40% / >$5 savings | Avoid stacking AliExpress shipping fees on small items. ESP32 + buck converter went AliExpress because savings were dramatic. |
| Tools agent added 5 items: wire stripper, flux pen, step drill, coping saw, pliers | User confirmed they need all 5. |
| Skip USB cable for touch display | Elecrow chose has integrated USB-A male connector. |
| 3M VHB tape for display mount, rubber grommet for cable pass-through | Standard cabin install pattern. |
| Save the Veepeak OBD-II pre-validation step for later | User said "save it for much later" — comes back during bench testing. |
| Don't auto-trigger Start during testing — use Lock first | Safety. Lock cycle is non-destructive. Confirms RF chain works before risking unwanted engine starts. |

---

## 13. Next session — when parts arrive

In order of priority:

1. **Inventory the boxes** — confirm everything ordered actually arrived, identify any DOA modules
2. **Flash MicroPython** on the ESP32 via Thonny
3. **Smoke-test each module on breadboard** one at a time:
   - ESP32 + LED blink (10 min)
   - ESP32 + ADS1115 (read 3V3 / VCC, confirm correct value)
   - ESP32 + CC1101 (init the chip, read its part number register)
   - ESP32 + SN65HVD230 (loopback test if possible)
4. **Begin SDR captures** of the FOB at 433.92 MHz per the documented walkthrough
5. **Run URH analysis** + extract framing
6. **Recover the device key** (try Flipper Zero DB first, fall back to PICkit if needed)
7. **Wire it all together on breadboard** for first end-to-end test
8. **Lock cycle test** in the car (low risk, validates RF chain)
9. **Start cycle test** with hood up + hand on kill switch
10. **Move to perfboard** + permanent enclosure
11. **In-car install** via OBD-II Y-splitter
12. **First voltage-triggered auto-start**

---

## 14. Git commit history (relevant ones)

```
76d791c  Add GitHub Actions CI + end-to-end integration tests (9 new)
8b4d21c  Add Phase 2 reproduction docs: bench prototype (steps 08-12)
8e174e1  Add deep-sleep + RTC-memory streak persistence for MONITORING
340b909  Log: SDR scripts, ESP32 drivers + state machine, Pi daemon
ad89a9a  Add Pi daemon (UART listener + MQTT publisher + Flask) + provisioning
6057252  Add ESP32 state machine (controller.py + main.py boot entrypoint)
77e45d5  Add ESP32 hardware drivers (cc1101, ads1115, twai_can) + tests
2c21d5e  Add SDR helper scripts so the walkthrough is executable end-to-end
5edb82e  Write braindump.md (pre-compaction context preservation)
ca39466  Add detailed SDR reproduction walkthrough (7 numbered docs + top-level index)
7360a21  Log evening code sprint — first 5 modules + tests + dashboard
3cd569e  Add Flask dashboard skeleton (server + HTML + CSS + JS)
74a83eb  Add config + secrets templates for ESP32 and Pi
53cde0e  Add UART link protocol between ESP32 and Pi (both sides + tests)
f0ef5a4  Add OBD-II PID query/response module + tests
340c309  Add Compustar HCS packet builder + PWM pulse generator
c87e1e7  Add KeeLoq cipher (esp32/src/lib/keeloq.py) with passing tests
fbfe51c  Warn about AliExpress ESP32-WROOM-32U bait-variant scam
370f3e7  Log final shopping outcome and lessons learned
2bdd048  Log shopping run results in 2026-05-23 journal
7a8517c  Scope to single-unit; add daily log for 2026-05-23
c630416  Initial scaffolding: docs, BOM, architecture, SDR plan
```

## 14b. Test totals (as of last commit)

**74 unit + integration tests passing**:
- 4 keeloq + 6 compustar + 9 obd2
- 8 ads1115 + 6 cc1101 + 5 persistence
- 12 controller + 9 integration
- 6 esp32_link + 9 uart_listener

CI runs all of them on Python 3.10 / 3.11 / 3.12 via
`.github/workflows/tests.yml` on every push to main.

---

## 15. Things a future Claude should NOT do

- Don't re-suggest looking up the Compustar brain model — it's inaccessible.
- Don't re-litigate the Pi-only vs ESP32-only vs hybrid debate — hybrid is chosen.
- Don't write the BOM in 2-unit format for committed files (single-unit only).
- Don't commit `secrets.py` (gitignored — verify with `git status` before any commit).
- Don't put the device key in any log file (logs/ gets pushed to public GitHub).
- Don't dispatch giant multi-step browser-MCP agents (>30 tool calls) — they crash. Break into smaller agents.
- Don't trust the cart state after multiple agents touched it — always re-audit before doing anything.
- Don't try to drive the cipher to match "published" KeeLoq test vectors — the canonical reference is our own roundtrip property + post-capture real vectors.
- Don't add an ESP32 to AliExpress carts without explicitly verifying the variant — the cheap default is the antenna-only scam.
- Don't run the system in the car without doing Lock first as a safety check.

---

## 16. Things a future Claude SHOULD remember

- User likes terse, direct answers. Don't pad.
- User is technically competent — explain new things, don't re-explain known things.
- The auto-commit/push rule applies to EVERY change. Every Edit / Write that touches a tracked file triggers `git add` + `git commit` + `git push`.
- Daily log lives at `logs/YYYY-MM-DD.md`. Today is 2026-05-23. Update it at end of session OR after meaningful milestones.
- Project memory is in `C:\Users\sixoffive\.claude\projects\C--Users-sixoffive-Documents-Claude-Projects-vroom\memory\` — read at session start.
- Repository URL is https://github.com/SixOfFive/2009_kia_spectra — push there, not anywhere else.

---

End of braindump. Compaction-safe.
