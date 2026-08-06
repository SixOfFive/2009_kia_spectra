# Framing — capture-session findings (public)

This file holds the **non-sensitive** parts of the SDR analysis: protocol
layout (universal HCS knowledge), timing constants (universal), measured
center frequency (anyone with an SDR sweeping the ISM band would see it
within seconds).

**Per-FOB sensitive values — serial, function-code mapping, captured
hopping codes, counter — live in `sdr/analysis/framing.local.md` which is
gitignored.** See [`framing.local.md.example`](framing.local.md.example)
for the template; copy it to `framing.local.md` before step 06.

## FOB transmit frequency
_measured 2026-05-24_

- Measured center: 433.968 MHz (peak +19.4 dB above noise floor, cluster
  spans ~10 kHz)
- Offset from nominal: +48 kHz from 433.920 MHz — could be the FOB or the
  R828D dongle's non-TCXO crystal (~110 ppm). Doesn't matter for capture;
  use the measured value everywhere.
- Source: `rtl_power -f 433M:434M:1k -i 1 -g 40 -e 60` on the Vomeko / R828D dongle
- All subsequent `rtl_sdr` commands: `-f 433968000`

## PROTOCOL DISCOVERY — this FOB is NOT HCS-KeeLoq

Originally assumed this was a Microchip HCS300/301 with KeeLoq rolling code.
**Wrong assumption.** Confirmed via rtl_433 project's `compustar_1wg3r.c`
decoder (covers 1WG3R-SH, 1WAMR-1900 — same protocol family as 1WSHR-PRO):

- **Modulation: OOK_PULSE_PWM (symmetric — HIGH and LOW both vary)**
- `short_width`: 708 µs (1WG3R spec), measured 732 µs on this FOB
- `long_width`: 1076 µs (1WG3R spec), measured 1100 µs on this FOB
- `sync_width`: 1448 µs (1WG3R spec), measured 1476 µs on this FOB
- `reset_limit`: 1532 µs (1WG3R spec)
- **FIXED CODE — NO KeeLoq, NO rolling counter, NO device key**

Bit encoding — **corrected 2026-08-06** by direct SDR measurement of the FOB
(`sdr/captures/fob-60s-2026-08-06.bin`, 141 clean bits, SD ~4 µs):

| Symbol | HIGH duration | LOW duration | bit PERIOD |
|---|---|---|---|
| "0" bit | ~731 µs (short) | ~762 µs (short) | **~1493 µs** |
| "1" bit | ~1100 µs (long) | ~1140 µs (long) | **~2243 µs** |
| sync   | ~1476 µs        | ~1500 µs         | ~2976 µs |

The HIGH and LOW of a data bit are the **same class**: a "0" is short-HIGH +
short-LOW, a "1" is long-HIGH + long-LOW. The bit is carried by the **PERIOD**
(1493 vs 2243 µs), NOT by a constant-sum pulse pair.

> ⚠️ **Correction — this cost a full bench day.** An earlier version of this
> table (from a mis-paired rtl_433 `-A` reading) claimed "0" = short-HIGH +
> **long**-LOW and "1" = long-HIGH + **short**-LOW, "sum approximately constant
> ~1860 µs". That is WRONG: it makes the 0 and 1 periods nearly identical
> (~1860 µs each), so a receiver that decodes by period/gap cannot tell the
> bits apart. That bad timing was copied into the firmware
> (`CMP_SHORT_LOW_US`/`CMP_LONG_LOW_US` were swapped) and the Compustar brain
> silently ignored every otherwise-perfect replay. An rtl_433-style decoder
> reads only the HIGH pulse, so it "validated" the broken signal — always
> measure the LOW/gap too. Fixed in firmware fw 4.2.

## rtl_433 ground-truth verification

Running rtl_433 (nightly Windows build) with `-A` on our capture:

```
rtl_433 -r cu8:fob-start-lg20-b1.bin -s 250000 -A -v
```

produces a pulse histogram matching the table above. `-R 302` (the
Compustar 1WG3R decoder) does NOT produce JSON output, because our
specific FOB transmits a structural sub-variant: **3-sync-triplet + 35
bits per packet** vs the canonical **1 sync + 36 bits**. Same modulation
and timing, different framing. Since we replay verbatim rather than
decode, the divergence is cosmetic.

## Packet layout — 1WG3R canonical (36 bits)
_from rtl_433/src/devices/compustar_1wg3r.c — public domain knowledge_

| Field          | Bits | Position | Notes |
|----------------|------|----------|-------|
| Remote ID      | 16   | 0..15    | Constant per FOB |
| Always 111     | 3    | 16..18   | Sanity check bits |
| ~button (inv)  | 8    | 19..26   | Bitwise NOT of button code |
| button code    | 8    | 27..34   | Identifies which button(s) |
| Always 0       | 1    | 35       | Sanity bit |

Integrity check: `button_inverse XOR button_code == 0xFF`.

## Packet layout — 1WSHR-PRO observed (35 bits between syncs)

This FOB doesn't match the canonical 1WG3R layout. Observed structure:

- First 16 bits = Remote ID (verified — matches across all four buttons)
- Bits 16..23 = constant `0x08` across all buttons (some header / always-on field)
- Bits 24..34 = per-button code (11 bits, including parity / inversion)

We don't need to fully reverse-engineer the field meanings since the
patterns are FIXED. For replay it's sufficient to store each button's
35-bit pattern verbatim. The per-button patterns live in
`framing.local.md`.

## Implications for the rest of the project

The KeeLoq cipher (`esp32/src/lib/keeloq.py`), counter management, and
device-key recovery (sdr/07) are **not needed for this FOB**. To replicate
the FOB:

1. Capture once → extract the 16-bit Remote ID + per-button 8-bit button codes
2. Transmit a 36-bit packet (sync + 36 PWM bits) to trigger any button
3. NO counter — same packet is accepted every time

This is much simpler than the original KeeLoq path. The ESP32 firmware
can be substantially trimmed.

## Transmit structure — one button press
_measured 2026-08-06 from full-press SDR captures; this is what fw 4.3 replays_

A single physical FOB press is **ONE burst**, shaped like this:

```
[wake-up carrier ~1.44 s][~5 short preamble cells][ 8 × (3 sync + 35 data bits) ][trailing carrier ~0.5 s]
                                                     packets spaced ~1.1 ms apart
```

- **Wake-up carrier**: ~1.44 s of continuous carrier. The receiver is
  duty-cycled (sleeps); without this it never hears the packet. (fw 3.9)
- **Preamble**: ~5 short (~740 µs) on/off cells to settle the slicer/clock.
- **Data**: the 3-sync + 35-bit packet repeated ~8× back-to-back, gaps ~1.1 ms
  (NOT the 39 ms we first used — too wide, receiver drops bit-clock lock). (fw 4.1)
- **Trailing carrier**: ~0.5 s of carrier after the data. (fw 4.1)
- **Bursts per press: exactly 1.** ⚠️ Sending the whole unit 3× (our first
  design, from a long-hold capture) does NOT start the car — a second start
  command ~2.5 s later reads as a re-press and CANCELS the start. One burst
  per press, like the FOB. (fw 4.3 — the change that finally cranked the car)

(These are encoder-configuration values shared by anyone with the same FOB
family — not FOB-identifying. OK to commit.)

## Where the per-FOB secret stuff lives

| Value | Location |
|---|---|
| 28-bit FOB serial | `sdr/analysis/framing.local.md` (gitignored) |
| Function-code mapping | `sdr/analysis/framing.local.md` (gitignored) |
| Captured hopping codes per press | `sdr/analysis/framing.local.md` (gitignored) |
| Counter values | `sdr/analysis/framing.local.md` (gitignored) |
| 64-bit device key | `esp32/src/secrets.py` (gitignored) |

## Notes
_session observations OK to share publicly_

- 2026-05-24: R828D-tuner Vomeko dongle on USB-3-only Win11 PC. Falls
  over at 2 MS/s; works fine at 250 kSps. USB 2.0 hub between dongle
  and PC required for stable streaming.
