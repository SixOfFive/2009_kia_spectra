# braindump — everything I know about this project

Context-preservation document. A future Claude (or future-me) should be
able to read this file and pick up where we left off without
reconstructing from chat history.

**Last updated: 2026-05-24 (late-day, post-firmware-rewrite + doc cleanup).**

> **State note for future Claude**: Section 4 steps 1-6 are COMPLETE.
> The firmware was rewritten to the fixed-code path in commits
> `0df2936` / `80bef8d` / `1bebd81` and the docs were updated in this
> round. Only Step 7 (hardware bench validation) remains, and it's
> blocked on parts arrival. Don't re-do steps 1-6.

---

## 0. READ THIS FIRST

The most important development in this project is in **section 2.5**:
the FOB protocol is **NOT KeeLoq rolling-code**. It's **fixed code**.
This invalidates a lot of the original architecture (KeeLoq cipher,
device-key recovery, counter management) and dramatically simplifies
what the ESP32 firmware needs to do.

Most of the code in `esp32/src/lib/keeloq.py` and the
`build_hopping_code()` function in `esp32/src/lib/compustar.py` is
not needed for this specific FOB. **Don't waste time on the KeeLoq
path.** Jump to section 4 ("Next steps") for the simplified path.

---

## 1. The project in one paragraph

**Goal**: voltage-triggered remote starter for a 2009 Kia Spectra. An
in-car ESP32 monitors battery voltage; when it drops below a threshold
for sustained time, the ESP32 transmits a Compustar FOB packet at
433.968 MHz via a CC1101 module. The factory-installed Compustar brain
receives this as if it were the FOB, and starts the engine. A Pi Zero 2
W drives a 5" HDMI touchscreen + WiFi dashboard. Everything plugs into
the OBD-II port (power + CAN bus), reversible install.

- **Repo**: https://github.com/SixOfFive/2009_kia_spectra
- **Local path**: `C:\Users\sixoffive\Documents\Claude_Projects\vroom`
- **Main branch**: `main`

---

## 2. The car and FOB

- **Vehicle**: 2009 Kia Spectra sedan, 2.0L I4, automatic, VIN `KNAFE221495635751`
- **FOB**: Compustar 1WSHR-PRO, FCC ID `7087A-R762A433`, 4-button 1-way
- **Compustar brain**: under dash, not visually accessible (confirmed)
- **Remote ID**: stored in `sdr/analysis/framing.local.md` (gitignored)

## 2.5. PROTOCOL DISCOVERY (the big news)

After extensive SDR capture + analysis, cross-referenced against the
**rtl_433 project's `compustar_1wg3r.c` decoder** which covers
1WG3R-SH and 1WAMR-1900 (same protocol family as 1WSHR-PRO), we found:

- **Modulation**: OOK_PULSE_PWM
- `short_width`: 708 µs ("0" bit HIGH pulse)
- `long_width`: 1076 µs ("1" bit HIGH pulse)
- `sync_width`: 1448 µs (sync pulse)
- `reset_limit`: 1532 µs (inter-packet gap)
- **Packet length per rtl_433 spec**: 36 bits. Our empirical measurement
  shows 35 bits between sync triplets — slight variant.
- **NO KeeLoq, NO rolling counter, NO device key.** Pure fixed code
  with a bit-inversion integrity check.

### What this means

We don't need the KeeLoq cipher infrastructure for THIS FOB. To
replicate any button press, we just store the captured 35-bit pattern
for that button and transmit it verbatim (with proper PWM pulse widths
and sync triplet at the start). The car will accept it every time.

This was empirically confirmed: all 10 presses of the same button
produce IDENTICAL bit patterns (no variation), and different buttons
produce predictable bit-pattern differences in the trailing 11 bits.

### Source of truth

`https://github.com/merbanan/rtl_433/blob/master/src/devices/compustar_1wg3r.c`
— this is the canonical decoder. Future me: if confused about any
detail, READ THAT FILE, it's the ground truth.

---

## 3. Repository state (as of this snapshot)

```
vroom/
├── README.md                  Project overview + architecture
├── braindump.md              THIS FILE
├── SECURITY.md                Threat model
├── .gitignore                 Excludes secrets.py, captures, framing.local.md, etc.
├── docs/
│   ├── BOM.md                Single-unit BOM (~$260 CAD)
│   ├── architecture.md        Hybrid ESP32+Pi design
│   ├── compustar-research.md  What we know about the install
│   ├── reproduce.md           Master walkthrough TOC
│   ├── power-budget.md        Predicted parked draw, days-to-trigger
│   ├── day-one.md             Take-to-bench cheat sheet
│   ├── 08-flash-micropython.md
│   ├── 09-bench-smoke-tests.md
│   ├── 10-pi-setup.md
│   ├── 11-uart-link.md
│   └── 12-keeloq-bench-validation.md  ← NEEDS RENAMING+REWRITE (no longer KeeLoq)
├── esp32/
│   ├── src/
│   │   ├── config.py          Tunable values
│   │   ├── secrets.py.example Template
│   │   ├── controller.py      State machine
│   │   ├── main.py            Boot entrypoint
│   │   └── lib/
│   │       ├── keeloq.py      ← NOT NEEDED FOR THIS FOB (keep for HCS variants)
│   │       ├── compustar.py   ← NEEDS REWRITE (build 36-bit fixed-code, not HCS66+KeeLoq)
│   │       ├── obd2.py        OBD-II PID dict
│   │       ├── pi_link.py     UART JSON protocol
│   │       ├── cc1101.py      Radio driver
│   │       ├── ads1115.py     ADC driver
│   │       ├── twai_can.py    CAN wrapper
│   │       └── persistence.py RTC memory helpers
│   ├── scripts/
│   │   ├── install.sh + .ps1  mpremote install helpers
│   │   └── simulate.py        CPython state-machine simulator
│   └── tests/                 12 controller + 6 cc1101 + 8 ads1115 + 5 persistence + 4 keeloq + 6 compustar + 9 obd2 + 9 integration = 59 tests
├── pi/
│   ├── app/                   Flask dashboard + UART listener + MQTT pub/sub
│   ├── systemd/
│   ├── setup/provision.sh
│   └── tests/                 6 esp32_link + 9 uart_listener + 9 mqtt_subscriber = 24 tests
├── sdr/
│   ├── README.md
│   ├── 01-software-setup.md
│   ├── 02-hardware-verification.md
│   ├── 03-frequency-confirmation.md
│   ├── 04-recording-captures.md
│   ├── 05-demodulation.md     ← Python-only, no URH required
│   ├── 06-framing-extraction.md
│   ├── 07-key-recovery.md     ← NEEDS UPDATE OR DELETION (no key to recover)
│   ├── captures/              (gitignored — IQ .bin files)
│   ├── scripts/
│   │   ├── plot-power-csv.py
│   │   ├── inspect-capture.py
│   │   ├── trim-burst.py
│   │   ├── demod-ook.py       Generic OOK demodulator (PWM-by-ratio + envelope-sampling)
│   │   ├── demod-compustar.py NEW — Compustar-specific decoder
│   │   ├── scan-compustar.py  NEW — brute-force find valid packets at any offset
│   │   ├── debug-envelope.py
│   │   ├── consensus-bits.py  Majority-vote across multiple decodes
│   │   ├── analyze-framing.py Classify HOP/FN/SER positions across buttons
│   │   ├── diff-bits.py
│   │   ├── try-mfkeys.py      ← NOT NEEDED for fixed-code FOB
│   │   └── validate-key.py    ← NOT NEEDED for fixed-code FOB
│   └── analysis/
│       ├── framing.md         Generic packet structure (committed, public)
│       ├── framing.local.md.example  Template (committed)
│       └── framing.local.md   GITIGNORED — has Remote ID + per-button patterns
├── tools/
│   └── preflight.py           Config + secrets sanity check
├── logs/
│   ├── 2026-05-23.md
│   └── 2026-05-24.md
└── .github/workflows/tests.yml CI on every push (Py 3.10/3.11/3.12)
```

---

## 4. NEXT STEPS (where to pick up)

Numbered in order. Steps 1-6 are DONE as of 2026-05-24 evening. Only
Step 7 remains, blocked on parts arrival.

### ✅ Step 1 — Install rtl_433 on Windows for ground-truth validation

The rtl_433 project has the working compustar_1wg3r decoder. Install it
to verify our findings before rewriting any code.

```powershell
# Download from https://github.com/merbanan/rtl_433/releases
# Get the NIGHTLY pre-release (release 25.12 doesn't have the
# compustar_1wg3r decoder; it was added in nightly post-25.12).
# File: rtl_433-win-msvc-x64-25.12.zip is for 25.12. Need a newer nightly
# from the "nightly" pre-release at the top of the releases page.
```

Then run rtl_433 on the user's captures:

```powershell
# Each capture is a .bin file (rtl_sdr unsigned-8-bit IQ format)
# Tell rtl_433 to interpret it as cu8 (= unsigned 8-bit complex)
rtl_433 -r cu8:sdr\captures\fob-start-lg20-b1.bin -s 250000 -f 433968000 -R 302
# -R 302 = enable Compustar 1WG3R decoder protocol number
```

Expected output: `{"model": "Compustar-1WG3R", "id": "XXXX", "button_code": ..., "button_str": "Start", ...}` per packet, where the `id` field is your FOB's 16-bit Remote ID.

If rtl_433 outputs valid packets, we have GROUND TRUTH for:
- The Remote ID
- The exact 8-bit button code for each button (Start, Lock, Unlock, Trunk)

If rtl_433 fails to decode (different protocol variant), we fall back
to bit-pattern replay using the captures we already have.

### ✅ Step 2 — Update `framing.local.md` with rtl_433's output

Whatever rtl_433 extracts, write to `sdr/analysis/framing.local.md`:
- Remote ID (16-bit hex)
- Per-button code (8-bit hex)
- Sanity: button_inverse byte if rtl_433 also prints it
- Note: if rtl_433 says "1WSHR-PRO is not 1WG3R-compatible", then
  fall back to bit-pattern replay (the existing patterns in
  framing.local.md are still valid for verbatim transmission)

### ✅ Step 3 — Rewrite `esp32/src/lib/compustar.py`

Strip out KeeLoq and replace with a 36-bit fixed-code builder:

```python
# New API:
def build_packet(remote_id, button_code):
    """Build the 36-bit Compustar 1WG3R-family packet.
    
    Format: IIIIIIIIIIIIIIII xxx bbbbbbbb iiiiiiii z
    - I: 16-bit remote ID (MSB first)
    - xxx: 3 bits "always 111"
    - b: 8-bit ~button (= NOT button_code, for integrity)
    - i: 8-bit button_code
    - z: 1 bit "always 0"
    Returns: list of 36 ints (0 or 1) MSB first.
    """
    ...

def packet_to_pulses(bits, short_us=708, long_us=1076, sync_us=1448, gap_us=720):
    """Render 36-bit packet to OOK pulse list for CC1101 transmission.
    
    Each bit i in packet:
      bit 0 -> HIGH(short_us), LOW(gap_us)
      bit 1 -> HIGH(long_us),  LOW(gap_us)
    Prefix with sync triplet: 3x [HIGH(sync_us), LOW(gap_us)]
    """
    ...
```

The new tests should verify the bit packing matches what we captured
empirically. **Keep `keeloq.py` and its tests** — leave them alone, they
just become unused code for this specific FOB. (Future-proof for HCS
variants.)

### ✅ Step 4 — Update `esp32/src/controller.py`

The state machine doesn't change shape, but the RF transmit logic
simplifies dramatically. Replace the keeloq counter-update logic with
just "look up button code, transmit packet". Counter persistence in
`persistence.py` becomes optional / unused.

### ✅ Step 5 — Update `esp32/src/secrets.py.example`

```python
# Compustar 1-way fixed code remote (1WG3R protocol family).
# Capture once via SDR + rtl_433, never changes.
COMPUSTAR_REMOTE_ID = 0x____  # 16-bit ID (set this from framing.local.md)
COMPUSTAR_BUTTON_CODES = {
    "START":  0x__,
    "LOCK":   0x__,
    "UNLOCK": 0x__,
    "TRUNK":  0x__,
}
# (Remove COMPUSTAR_DEVICE_KEY and COMPUSTAR_COUNTER — no longer used.)
```

### ✅ Step 6 — Update reproduction docs

Done in commits during the doc-cleanup round:

- `docs/12-keeloq-bench-validation.md` renamed to `docs/12-bench-validation.md`
  and rewritten for the fixed-code capture-store-replay path.
- `sdr/07-key-recovery.md` now opens with a "NOT NEEDED for Compustar
  1WG3R-family FOBs" callout. Content preserved for any HCS-KeeLoq
  reader.
- `sdr/05-demodulation.md` points readers at `demod-compustar.py`
  first; `demod-ook.py` documented as the generic HCS fallback. Added
  an rtl_433 `-A` cross-check section.
- `sdr/06-framing-extraction.md` simplified to "capture 35-bit pattern
  per button + Remote ID". HCS-KeeLoq diff-based discovery moved to
  an appendix.
- Cross-references updated in `docs/reproduce.md`, `docs/day-one.md`,
  `docs/11-uart-link.md`, `README.md`.

### Step 7 — Test on hardware

After parts arrive (per the original schedule in section 9 below):
1. Smoke-test each module (docs/09)
2. Pi setup (docs/10) — unchanged
3. UART link (docs/11) — unchanged
4. **Compustar bench validation** (revised docs/12):
   - Load the captured packets into `secrets.py`
   - Transmit Start pattern via CC1101 at 433.968 MHz
   - Confirm car cycles (start with Lock first as safety per the
     existing valet-switch protocol)

---

## 5. What captures we have

The user did extensive captures at `-g 20` (after discovering AGC
compression at `-g 40` was killing the signal). In `sdr/captures/`:

| File | Button | Notes |
|---|---|---|
| `fob-start-lg20.bin` + b1 | Start (1 press) | very clean 132x peak/floor |
| `fob-start-001.bin` | Start | OLD, captured at -g 40 — AGC-compressed, NOT USABLE |
| `fob-start-002.bin` | Start (3 presses) | -g 20, clean |
| `fob-start-003.bin` | Start (3 presses) | -g 20, clean |
| `fob-start-004.bin` | Start (10 presses) | -g 20, clean |
| `fob-lock-001.bin` | Lock (3 presses) | -g 20, b1 is partial (started mid-press) |
| `fob-unlock-001.bin` | Unlock (3 presses) | -g 20, clean |
| `fob-unlock-002.bin` | Unlock (10 presses) | -g 20, clean |
| `fob-trunk-001.bin` | Trunk (3 presses) | -g 20, clean |
| `fob-trunk-002.bin` | Trunk (10 presses) | -g 20, clean |

Each "press" produces multiple identical packet repeats inside the burst.
All trimmed via `trim-burst.py` into per-burst `.bin` files. All
decoded via `demod-ook.py` into per-packet `.bits` files (gitignored).

**The 4 verified button bit-patterns are in `sdr/analysis/framing.local.md`** (gitignored, never commit).

---

## 6. Hardware architecture (unchanged from earlier)

**Hybrid ESP32 + Pi Zero 2 W:**
- ESP32: always-on, deep sleep, RF transmit, voltage monitor, CAN bus
- Pi Zero 2 W: 5" HDMI touch, WiFi, MQTT, maps, web UI
- ESP32 power-gates Pi via AO3401A P-MOSFET
- UART line-delimited JSON between them at 115200

**OBD-II connection** (only):
- Pin 16 (+12V always-hot) → fuse → TVS → buck → 5V rail
- Pin 4/5 ground
- Pin 6 CAN-H → SN65HVD230
- Pin 14 CAN-L → SN65HVD230

---

## 7. What the user has experienced today (SDR session)

In order, with fixes applied to docs:

1. **rtl-sdr Windows binary URL was wrong** → fixed in sdr/01, points at
   osmocom FTP + rtl-sdr-blog fork releases
2. **URH won't install on Python 3.14** → URH dropped as dependency,
   replaced by our own Python demodulator (sdr/05 updated, demod-ook.py
   written)
3. **Vomeko dongle with R828D tuner is unstable on USB-3-only host** →
   fixed with USB 2.0 hub between dongle and PC, lowered sample rate to
   250 kSps. Documented in sdr/01, sdr/04.
4. **`-g 40` AGC-compressed the signal to <1% modulation depth** →
   recommended `-g 20`. Documented in sdr/04.
5. **My PWM-by-ratio decoder gave ~80% bit accuracy** because the FOB's
   pulse-pair shapes don't match standard HCS PWM. → Rewrote demodulator
   with bit-clock recovery (decode_bits_from_envelope), then discovered
   the protocol isn't HCS at all and built demod-compustar.py.
6. **Protocol discovery via rtl_433 source** → THIS is the key finding.

User has been patient and persistent. They captured a lot of data and
the bit patterns are solid. They asked me to use Chrome MCP to research
the protocol online — Cloudflare blocked fccid.io, but the rtl_433
GitHub source gave us everything we needed.

---

## 8. Decisions made today

| Decision | Reason |
|---|---|
| Drop URH as dependency | Won't install on Python 3.14, freezes on multi-press files. Pure Python demod is sufficient + reproducible. |
| Use `-g 20` instead of `-g 40` | AGC at high gain compresses OOK signal beyond demod recovery. |
| Use 250 kSps instead of 2 MSps | R828D dongle + USB-3-only host isn't stable at 2 MSps; 250 kSps has 2.5x Nyquist margin for our 50 kHz signal. |
| USB 2.0 hub between dongle and PC | Forces USB 2.0 negotiation, isolates dongle from USB-3 controller noise. |
| **Compustar 1WSHR-PRO is fixed-code (NOT KeeLoq)** | Confirmed via rtl_433 1WG3R decoder + empirical bit-pattern stability across 10 presses. |
| Skip device-key recovery (sdr/07) | No KeeLoq → no device key → no key recovery needed. |
| Store-and-replay approach | Simpler firmware, smaller code, same functional result. |
| Keep `framing.local.md` gitignored | Has Remote ID + button patterns; user explicitly wants those private. |

---

## 9. Things NOT to do (avoid wasted time)

- **Don't try to recover a KeeLoq device key for this FOB.** There is no
  KeeLoq encryption. The "rolling code" assumption was wrong.
- **Don't try to make URH work on Python 3.14.** Use our Python decoder.
- **Don't bump gain back up to `-g 40`.** AGC compression at high gain
  kills the OOK modulation in our captured data.
- **Don't try the `decode_bits_from_runs` PWM-ratio decoder.** It gives
  ~80% per-bit accuracy on this FOB's encoding (which isn't standard
  HCS PWM 1:2 ratio). Use `demod-compustar.py` instead.
- **Don't commit `sdr/analysis/framing.local.md`.** It has the Remote ID
  and button patterns which the user wants kept off public GitHub.
- **Don't commit `compustar_counter.json`** if it appears (runtime state,
  already in .gitignore).

---

## 10. Commit history (recent, in reverse chronological order)

```
9904b3f  Add sdr/scripts/scan-compustar.py — brute-force packet finder
f1f8d29  PROTOCOL DISCOVERY: Compustar 1WSHR-PRO is fixed-code, not KeeLoq
a3c64f6  demod-ook.py: add bit-clock-recovery envelope decoder
2aafcab  demod-ook.py: accept preamble without header gap; bump max-packets default
5a44bd3  Add consensus-bits.py + analyze-framing.py for noisy-bit analysis
4507ba6  Add sdr/scripts/inspect-capture.py — pre-URH burst finder
25adb8c  Add sdr/scripts/trim-burst.py — slice long captures into per-burst files
79e8686  Drop URH as a dependency; the project's own Python demodulator replaces it
7323993  sdr/05: match URH sample rate to whatever rtl_sdr captured at
f0bae96  Document 250 kSps fallback for USB-3-only PCs / R828D dongles
982a051  Document R828D tuner variant + i2c -9 USB-power troubleshooting
750880e  Correct URH Python-version guidance: Windows wheel is 3.13 only
e9af934  Document Python 3.13+ URH install gotcha
bb93ead  Fix sdr/01: osmocom GitHub mirror has no releases
4e172a8  sdr/analysis: record measured FOB frequency = 433.968 MHz
ca891a0  Split framing.md into public + local files; ignore stray counter state
```

---

## 11. Test totals

85+ tests passing across the project. CI runs on every push.
Detailed breakdown in section 3 above.

---

## 12. Where to find the per-FOB sensitive values

`sdr/analysis/framing.local.md` (gitignored). Contains:
- 16-bit Remote ID (the FOB's serial number)
- Per-button 35-bit bit patterns
- Pulse timing parameters (measured)

When future-me writes the new `compustar.py` library, those values go
into `esp32/src/secrets.py` (also gitignored) as plain Python constants.

---

## 13. Quick "I'm coming back fresh" cheat sheet

1. Read this file (you're doing it). Section 4 steps 1-6 are DONE.
2. Read `sdr/analysis/framing.local.md` for per-FOB values (16-bit
   Remote ID + 4 captured 35-bit patterns; these are gitignored).
3. `git log --oneline -20` to see recent commits. Key landmarks:
   - `0fa7dc8` — protocol-discovery braindump
   - `0df2936` — framing.md updated with rtl_433 verification
   - `80bef8d` — firmware rewrite (KeeLoq path removed)
   - `1bebd81` — daily log update
   - Doc cleanup follows after that.
4. Verify the firmware passes tests:
   `cd esp32 && python tests/test_compustar.py && python tests/test_controller.py && python tests/test_integration.py`
   Expected: 23 + 17 + 9 tests passing.
5. The only remaining work is **Step 7 — hardware bench validation**,
   per `docs/12-bench-validation.md`. It's blocked on parts arrival
   (CC1101 / ESP32 / Pi / ADS1115 / OBD-II pigtails).
6. When parts arrive, walk `docs/day-one.md` end to end. The "moment
   of truth" steps are Lock cycle (safest first button) → Unlock →
   Start.

The user is willing to iterate — don't be afraid to ask clarifying
questions or commit partial progress.

---

End of braindump. Compaction-safe.
