# Compustar system research notes

What we've established about the factory-installed aftermarket remote start in the car.

## What's installed

- **FOB:** Compustar 1WSHR-PRO (4-button, 1-way) — FCC ID **7087A-R762A433**
- **RF frequency:** 433.92 MHz (inferred from FCC ID, to be confirmed by SDR sweep)
- **Brain:** Unknown Compustar / Firstech CM-series module, tucked behind the dash near the driver-side ECU. Visible in install photo but label not readable without disassembly.
- **Antenna:** Small black 3-LED windshield-mounted unit, top center of windshield. Cable runs back to the brain.
- **Bypass cartridge:** Likely an iDatalink/Compustar BLADE-AL or similar — handles the Kia immobilizer challenge using a sacrificed chipped key inside the module.

## Why we don't need to identify the brain

The original plan was to find the brain's hardwire start input. But since we can't safely access the brain without dash disassembly, we pivoted to RF synthesis — generate the Keeloq packet ourselves.

## Keeloq cipher status

- Original Microchip HCS-family cipher, ~32-bit block, 64-bit key
- **Academically broken** (Eisenträger et al., Eurocrypt 2008): practical key recovery in ~65 minutes data collection + 7.8 days on 64 cores
- For owner-access scenarios (we have the FOB), the attack is dramatically easier — possibly direct EEPROM read of the HCS encoder chip in the FOB

## Likely encoder chip in the 1WSHR-PRO

- Microchip HCS300, HCS301, HCS361, or HCS362 (TBD — visual inspection of FOB internals will tell)
- All are 8-pin SOIC, OOK modulation, PWM bit encoding
- Standard transmission format:
  - ~12 cycles preamble at TE rate
  - Header gap (~10×TE)
  - 32-bit encrypted hopping code
  - 28-bit serial (fixed per FOB)
  - 4-bit function code (which button)
  - 2-bit status (V-low, repeat)

## Attack approach (in priority order)

1. **SDR capture + framing analysis** (URH, free)
   - Confirm 433.92 MHz, confirm OOK, identify TE bit timing
   - Capture multiple presses of each button — identify which bits are fixed (serial) vs hopping (encrypted) vs function code
   - Stop here for documentation purposes if Step 2 not needed

2. **Manufacturer-key database lookup** (no extra hardware)
   - Flipper Zero "Unleashed" firmware has a published database of extracted manufacturer keys for many vendors
   - If Compustar's manufacturer key is known: device_key = derive(manufacturer_key, FOB_serial)
   - Quick win if it works

3. **Direct EEPROM read of FOB encoder chip** (PICkit ~$30 — deferred)
   - Open FOB, identify HCS300/301/361/362
   - Connect PIC programmer, read EEPROM
   - Extract device key, counter, serial directly
   - Most reliable fallback

## RF transmission plan (once we have the key)

- CC1101 module configured for 433.92 MHz, OOK, baudrate matching the FOB's TE
- ESP32 SPI master driving CC1101
- MicroPython encoder runs Keeloq forward with stored device key + incrementing counter
- Constructs full HCS packet, hands to CC1101 FIFO for transmission

## Things to verify with SDR captures

- [ ] Confirm exact frequency (sweep 433-434 MHz)
- [ ] Confirm modulation (OOK expected)
- [ ] Measure TE (bit period, expected ~400 µs)
- [ ] Count bits in a single packet (expected 66)
- [ ] Identify preamble length and pattern
- [ ] Identify header gap length
- [ ] Identify guard time between packets in a repeat burst
- [ ] Extract 28-bit serial from constant bits across presses
- [ ] Extract 4-bit function codes by comparing Start vs Lock vs Unlock captures
- [ ] Confirm 32 bits of hopping code change every press

## Why a thief can't exploit this

- The brain validates Keeloq codes, but the car still requires the **mechanical key in the cylinder** to release the steering column lock
- Without the steering lock released, the car can idle but cannot be driven
- This is the same security model as factory remote start on modern cars
- The system is no less secure than the existing FOB
