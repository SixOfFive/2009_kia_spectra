# Framing — capture-session findings

This file collects every per-FOB value you'll eventually need to plug into
`esp32/src/secrets.py` and `esp32/src/config.py`. Fill it in as you progress
through SDR steps 03-07. **Do NOT put the device key in this file** — that
goes in the gitignored `secrets.py` only.

## FOB transmit frequency
_measured 2026-05-24_

- **Measured center: 433.968 MHz** (peak at 433.9678 MHz, +19.4 dB above noise floor)
- **Bandwidth: roughly ±5 kHz** (cluster spans 433.961-433.971 MHz at +4 dB)
- Offset from nominal: 433.920 nominal → +48 kHz higher. Could be the FOB
  itself or the R828D dongle's non-TCXO crystal (~110 ppm). Doesn't matter
  for capture — use the measured value everywhere.
- Source: `rtl_power -f 433M:434M:1k -i 1 -g 40 -e 60` on the Vomeko / R828D dongle
- Capture file: `sdr/captures/fob-frequency-sweep.csv`
- **Use `-f 433968000` in all subsequent rtl_sdr commands.**

## Packet layout (66 bits, MSB first as transmitted)
_filled in / confirmed after step 06_

| Field         | Bits | Position | Notes |
|---------------|------|----------|-------|
| Hopping code  | 32   | 0..31    | Encrypted, changes every press |
| FOB serial    | 28   | 32..59   | Constant for this FOB |
| Function code | 4    | 60..63   | Identifies which button |
| V_LOW         | 1    | 64       | Low-battery indicator |
| Repeat        | 1    | 65       | 0 = first packet, 1 = repeat |

## Timing
_measured in URH per step 05_

- TE (bit element time): _____ µs
- Preamble: _____ half-bit cycles
- Header gap: _____ TE periods of silence
- Inter-packet guard: _____ ms
- Repeats per press: _____

## FOB-specific values
_filled in after step 06_

- **Serial** (28 bits): `0x_______`
- **Function codes**:
  - Start:  `0x_`
  - Lock:   `0x_`
  - Unlock: `0x_`
  - Trunk:  `0x_`

## Captured hopping codes
_populate as you go — used as inputs to `validate-key.py`_

| Capture file              | Button | Hopping (hex) | Counter (decrypted) | Function (decrypted) |
|---------------------------|--------|---------------|---------------------|----------------------|
| fob-start-001.bits        | Start  | 0x________    |                     |                      |
| fob-start-002.bits        | Start  | 0x________    |                     |                      |
| fob-start-003.bits        | Start  | 0x________    |                     |                      |
| fob-lock-001.bits         | Lock   | 0x________    |                     |                      |
| fob-unlock-001.bits       | Unlock | 0x________    |                     |                      |

## Device key recovery
_filled in after step 07; **NEVER put the actual key in this file**_

- Path used: A (Flipper DB) / B (PICkit) / C (cryptanalytic)
- Date recovered: YYYY-MM-DD
- Validation result: counter increments by 1 across N consecutive captures? yes / no
- Device key lives in: `esp32/src/secrets.py` (gitignored)

## Notes
_free-form observations from the session_

-
