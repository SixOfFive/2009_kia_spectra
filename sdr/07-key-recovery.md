# 07 — Recover the KeeLoq device key + replicate the FOB

## Goal

Get the 64-bit KeeLoq device key embedded in your FOB so the ESP32 can encrypt new (counter, function) pairs and produce valid hopping codes the receiver will accept.

**This is the step that turns the project from "we can decode the FOB" into "we can replicate the FOB."** Without this, you have observation; with it, you have control.

## Why this is hard, and why it's possible

Each FOB has a 64-bit secret device key, programmed into its HCS chip's EEPROM at the factory. The receiver in the car was paired to your FOB by storing this same key. Every transmission encrypts the counter (plus a few extra bits) with this key, so the 32-bit hopping code looks random unless you know the key.

Without the key, you cannot:
- Generate new valid hopping codes (so you can't replay because the receiver tracks the counter and rejects old codes)
- Decrypt past captures to learn the counter values
- Re-sync if the counter ever drifts

**The good news**: there are three practical paths to recover this key when you legitimately own the FOB. Pick whichever fits your tools.

## Path A — Manufacturer-key database (easiest, free, no hardware)

Many manufacturers don't pick a fresh device key per FOB. Instead they:

1. Have one **manufacturer master key** (shared across all FOBs the manufacturer ships)
2. Derive each device key by combining the master key with the FOB's serial number using a known function (usually `device_key = decrypt(serial_padded, manufacturer_key)` or `device_key = manufacturer_key XOR f(serial)`)

If your manufacturer uses this scheme and the master key has been extracted by someone in the security research community, you can compute your device key with just:
- Your FOB's serial (you got this in step 06)
- Their manufacturer key
- The published derivation function

### The Flipper Zero Unleashed database

The Flipper Zero community has extracted manufacturer keys for many vendors and ships them in the **Unleashed firmware's keeloq_mfcodes file**. See:

- [`Flipper-XFW/unleashed-firmware`](https://github.com/DarkFlippers/unleashed-firmware) — repo, look in `applications/main/subghz/assets/keeloq_mfcodes`
- The file is a simple text mapping: `manufacturer_name : 64-bit-key-hex : derivation_type`

For Compustar / Firstech specifically — at time of writing the situation is mixed:
- Some older Compustar models use a known manufacturer key
- Newer (post-2018) models reportedly use per-device keys that defeat this approach

**Action**: download keeloq_mfcodes from a current Unleashed firmware build, search for entries matching "Compustar", "Firstech", or "1WSHR", and try each candidate manufacturer key with the standard derivation functions.

The helper [`sdr/scripts/try-mfkeys.py`](scripts/try-mfkeys.py) does the search:

1. Loads candidate manufacturer keys from a Flipper-format file
2. For each (key, derivation_type) pair, computes the device key from your FOB serial
3. Decrypts one captured hopping code with it
4. Checks if the plaintext is plausible (counter < 50000, discrimination nibble matches the low nibble of the serial, function nibble nonzero)
5. Prints any hits

```powershell
python sdr/scripts/try-mfkeys.py --serial 0xABCDEF1 --hopping 0xDEADBEEF --mfkeys path/to/keeloq_mfcodes
```

If you get exactly one hit, **that's your device key**. Drop into `secrets.py` and confirm with `validate-key.py` (see "Validate" below). If you get many hits, you'll narrow it down by running `validate-key.py` with 3+ consecutive captures — only the right key produces counters that increment by 1.

## Path B — Read the chip's EEPROM directly (most reliable, ~$30)

If Path A fails (no manufacturer key works), the next best route is to physically dump the EEPROM of the HCS encoder chip inside the FOB. This works on every Compustar FOB ever made — the chip's contents include the device key, counter, and serial in plaintext, and Microchip's HCS300/301/361/362 chips have minimal read-protection.

### What you need

- **PICkit programmer** (PICkit 3 or 4, ~$30 on Amazon) — or any PIC programmer that supports HCS3xx chips. SiLabs USBee, Microchip MPLAB Snap, Adafruit's PICkit-compatible programmers all work.
- **Microchip MPLAB X IDE** (free, [microchip.com](https://www.microchip.com/en-us/tools-resources/develop/mplab-x-ide))
- Five thin wires to bridge from the programmer's header to the HCS chip's pins
- A precision flathead screwdriver to open the FOB

### Procedure

1. Open the FOB case (single screw under the keyring, or pry along the seam)
2. Identify the HCS chip — small 8-pin SOIC, marked `HCS300`, `HCS301`, `HCS361`, `HCS362`, or `40HCS301` etc.
3. Pinout (per HCS300 datasheet):
   ```
        +---v---+
   S0 1 |       | 8 VCC
   S1 2 |       | 7 PWM
   S2 2 |       | 6 LED
   S3 4 |       | 5 VSS (GND)
        +-------+
   ```
   - Programming uses S2 = CLK, S3 = DATA, VCC + GND
4. Solder thin (30 AWG) wire-wrap wires to:
   - Pin 2 (S2 / PGC clock)
   - Pin 3 (S3 / PGD data)
   - Pin 8 (VCC, ~5V)
   - Pin 5 (GND)
   - Pin 7 (PWM, only needed for "verify the chip is actually being programmed" — optional)
5. Connect to PICkit per its standard 6-pin header (MCLR=not used for HCS, PGC, PGD, VCC, GND)
6. Open MPLAB X → Programmer → Connect to PICkit → select target = HCS301 (or whatever marking)
7. **Read** the chip's memory
8. The device key, counter, and serial appear in the EEPROM dump as labeled fields (see Microchip AN642 application note for the exact layout)

After dumping, you have:
- 64-bit device key — paste into `secrets.py` as `COMPUSTAR_DEVICE_KEY`
- 16-bit counter — paste as `COMPUSTAR_COUNTER` (set to current value + 10 so you have margin)
- 28-bit serial — should match what you measured in step 06 (sanity check!)

## Path C — Cryptanalytic attack (research interest, slow, complete)

If both A and B fail, the academic attacks against KeeLoq still work:

- **Bogdanov 2007 slide attack**: ~2^37 work, runs in a few hours on a modern GPU. Requires ~2^16 captured ciphertexts (which means you'd need to press the FOB 65,000 times — automatable with a servo-pressed FOB but very tedious)
- **Eisentrager 2008 practical attack**: ~65 min data collection + 7.8 days on 64 CPU cores. Same massive plaintext requirement.

In practice this is academic interest, not a real production path. If A and B both fail, the realistic answer is: replace the FOB with one whose manufacturer-key is known, or buy a fresh Compustar FOB and pair it (then dump its chip).

## Validate the recovered key

Once you have a candidate device key (from any of the paths above), confirm it with the [`sdr/scripts/validate-key.py`](scripts/validate-key.py) helper using 3+ consecutive Start captures:

```powershell
python sdr/scripts/validate-key.py `
    --device-key 0xAABBCCDDEEFF0011 `
    --serial 0xABCDEF1 `
    0xHOP1 0xHOP2 0xHOP3
```

The script decrypts each capture and prints the counter, discrimination, and function values. The gold-standard confirmation is:

- All rows say `yes` in the ok? column (discrimination matches serial low nibble, function nonzero, counter reasonable)
- Counter deltas are `[1, 1]` — meaning the captures are 3 consecutive presses and the key recovers them correctly

If you see `All deltas = 1. Key is almost certainly correct.` — done. Drop the values into `secrets.py` and move on.

## Replicate the FOB from the ESP32

Once you have:
- Device key (`COMPUSTAR_DEVICE_KEY` in `secrets.py`)
- Serial (`COMPUSTAR_SERIAL` in `secrets.py`)
- Current counter + safety margin (`COMPUSTAR_COUNTER` in `secrets.py`)
- Function codes (`compustar.Function.START` etc. in `esp32/src/lib/compustar.py`)
- TE timing (`RF_TE_US` etc. in `esp32/src/config.py`)

…the existing `esp32/src/lib/compustar.py` will generate valid packets the receiver accepts. The flow on the ESP32:

```python
from lib import compustar
from config import COMPUSTAR_DEVICE_KEY, COMPUSTAR_SERIAL

# Read current counter from persisted flash storage
counter = load_counter_from_flash()  # e.g. 1234

# Build packet
packet = compustar.build_packet(
    serial=COMPUSTAR_SERIAL,
    function_code=compustar.Function.START,
    counter=counter,
    device_key=COMPUSTAR_DEVICE_KEY,
)

# Render to PWM pulses for CC1101
pulses = compustar.packet_to_pulses(packet["bits"])

# Send via CC1101 driver (created in a later step)
cc1101.transmit_ook(pulses, frequency=433_920_000)

# Persist incremented counter
save_counter_to_flash(counter + 1)
```

### Counter sync notes

The receiver tracks the counter you sent. If you skip ahead too far (e.g. >16 missed presses), it'll go into "re-sync" mode and require two consecutive valid codes before accepting again. The ESP32 firmware handles this by transmitting **two packets** if a single one is rejected (detected by lack of expected response).

Persist the counter to flash after every successful TX so a power cycle doesn't re-send an old value.

## Initial bench validation (before driving the car)

**Do not test against a moving car.** Bench validation first:

1. Park the car in a safe location, hood open, hand on the kill switch (Compustar provides a valet switch you can flip to disable the system)
2. Power up the ESP32 with the recovered values
3. Trigger one packet at the **Lock** function code (lower risk than Start)
4. Observe: did the car's lock cycle? If yes, your replication works
5. Repeat with Unlock — confirms two-way functioning
6. Only after Lock + Unlock both work consistently, try Start

If Lock works but Start doesn't, the function code mapping may be off — re-check step 06's function code extraction.

## What you should have when done

- 64-bit device key, validated by successful decryption of captured hopping codes
- The decrypted plaintext shows sensible counter, discrimination, and function values
- Counter incrementing by 1 between consecutive captures
- Updated `secrets.py` with all three FOB values
- A successful Lock test from the ESP32 to the real car

## Where artifacts go

- `esp32/src/secrets.py` (gitignored) — the recovered key + serial + counter
- `sdr/analysis/framing.local.md` (gitignored) — serial, function-code
  mapping, captured hopping codes per press, counter values
- `sdr/analysis/key-recovery-session.local.md` (gitignored, *.local.md
  pattern) — narrative record of which path worked, against what FOB,
  any oddities encountered. Keep locally as your reference.

## Security and safety

- **Never commit the device key.** `secrets.py` is in `.gitignore`. Verify with `git status` before any commit.
- **Never commit your serial, hopping codes, or counter either.** They go
  in `framing.local.md` and `secrets.py`, both gitignored. Verify both
  files do NOT appear in `git status` before committing anything from this
  session.
- **Never put any FOB-identifying value in a public log file** under
  `logs/YYYY-MM-DD.md`. The auto-commit/push rule pushes those to GitHub.
- **The recovered key is no less sensitive than the physical FOB itself.** Treat it the same way you treat your car keys.
- **Disable the system before any in-car testing.** Compustar has a valet switch for exactly this reason.

## Next

After this step you've completed the SDR phase. The rest of the build is hardware integration:
- Wire the CC1101 to the ESP32
- Load the recovered key + serial into secrets
- Flash the firmware
- Test on the bench, then in the car
