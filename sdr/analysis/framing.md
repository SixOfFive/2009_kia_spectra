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

- **Modulation: OOK_PULSE_PWM**
- `short_width`: 708 µs (= "0" bit's HIGH pulse width)
- `long_width`: 1076 µs (= "1" bit's HIGH pulse width)
- `sync_width`: 1448 µs (= sync pulse at start of packet)
- `reset_limit`: 1532 µs (= inter-packet gap)
- **FIXED CODE — NO KeeLoq, NO rolling counter, NO device key**

Bit encoding per the rtl_433 PWM model: each HIGH pulse encodes one bit
based on its width. Short pulse (≈708 µs) = "0". Long pulse (≈1076 µs) = "1".
LOW gaps between pulses are variable and ignored for bit decoding.

## Packet layout (36 bits, MSB first as transmitted)
_from rtl_433/src/devices/compustar_1wg3r.c — public domain knowledge_

| Field          | Bits | Position | Notes |
|----------------|------|----------|-------|
| Remote ID      | 16   | 0..15    | Constant per FOB (= "serial" in HCS terminology) |
| Always 111     | 3    | 16..18   | Sanity check bits, always 0b111 |
| ~button (inv)  | 8    | 19..26   | Bitwise NOT of button code |
| button code    | 8    | 27..34   | Identifies which button(s) |
| Always 0       | 1    | 35       | Sanity bit |

Integrity check: `button_inverse XOR button_code == 0xFF`.

## Implications for the rest of the project

The KeeLoq cipher (`esp32/src/lib/keeloq.py`), counter management, and
device-key recovery (sdr/07) are **not needed for this FOB**. To replicate
the FOB:

1. Capture once → extract the 16-bit Remote ID + per-button 8-bit button codes
2. Transmit a 36-bit packet (sync + 36 PWM bits) to trigger any button
3. NO counter — same packet is accepted every time

This is much simpler than the original KeeLoq path. The ESP32 firmware
can be substantially trimmed.

## Timing
_measured in URH per step 05, populate after captures_

- TE (bit element time): _____ µs
- Preamble: _____ half-bit cycles
- Header gap: _____ TE periods of silence
- Inter-packet guard: _____ ms
- Repeats per press: _____

(These are encoder-configuration values shared by anyone with the same
HCS variant — not FOB-identifying. OK to commit.)

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
