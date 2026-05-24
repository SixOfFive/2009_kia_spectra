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

## Packet layout (66 bits, MSB first as transmitted)
_universal HCS300/301 layout; confirmed in step 06_

| Field         | Bits | Position | Notes |
|---------------|------|----------|-------|
| Hopping code  | 32   | 0..31    | Encrypted, changes every press |
| FOB serial    | 28   | 32..59   | Constant for this FOB |
| Function code | 4    | 60..63   | Identifies which button |
| V_LOW         | 1    | 64       | Low-battery indicator |
| Repeat        | 1    | 65       | 0 = first packet, 1 = repeat |

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
