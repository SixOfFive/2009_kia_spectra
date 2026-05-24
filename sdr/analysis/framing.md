# Framing — capture-session findings

This file collects every per-FOB value you'll eventually need to plug into
`esp32/src/secrets.py` and `esp32/src/config.py`. Fill it in as you progress
through SDR steps 03-07. **Do NOT put the device key in this file** — that
goes in the gitignored `secrets.py` only.

## FOB transmit frequency
_filled in after step 03_

- Measured center: 433.____ MHz
- Bandwidth: roughly ±____ kHz
- Source: rtl_power sweep on YYYY-MM-DD with `-g 40 -i 1 -e 60`
- Capture file: `sdr/captures/fob-frequency-sweep.csv`

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
