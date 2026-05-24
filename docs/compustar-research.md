# Compustar system research notes

What we've established about the factory-installed aftermarket remote
start in the car. This file is a snapshot of facts about the
1WSHR-PRO FOB and its receiver brain. The protocol-level decisions
that turn those facts into the firmware design live in
[`sdr/analysis/framing.md`](../sdr/analysis/framing.md) (committed,
public) and [`sdr/06-framing-extraction.md`](../sdr/06-framing-extraction.md).

## What's installed

- **FOB:** Compustar 1WSHR-PRO (4-button, 1-way) — FCC ID **7087A-R762A433**
- **RF frequency:** 433.92 MHz nominal (per FCC filing). This specific FOB
  measured at 433.886 MHz via `rtl_433 -A`; receivers tolerate hundreds of
  kHz of offset so the nominal 433.92 figure works for transmit configuration.
- **Brain:** Unknown Compustar / Firstech CM-series module, tucked behind
  the dash near the driver-side ECU. Visible in install photo but label
  not readable without disassembly.
- **Antenna:** Small black 3-LED windshield-mounted unit, top center of
  windshield. Cable runs back to the brain.
- **Bypass cartridge:** Likely an iDatalink/Compustar BLADE-AL or similar —
  handles the Kia immobilizer challenge using a sacrificed chipped key
  inside the module.

## Why we don't need to identify the brain

The original plan was to find the brain's hardwire start input. We
can't safely access the brain without dash disassembly, so we pivoted
to RF synthesis — render the genuine FOB's packets ourselves and let
the brain treat us as the remote.

## Protocol discovery summary

The original plan assumed the FOB used the standard Microchip
HCS300/301 KeeLoq rolling-code protocol. **That turned out to be
wrong.** SDR analysis cross-referenced against the rtl_433 project's
`compustar_1wg3r.c` decoder confirmed:

- The 1WSHR-PRO belongs to the **Compustar 1WG3R fixed-code family**
  (same family as 1WG3R-SH, 1WAMR-1900).
- Modulation: OOK with symmetric PWM. Each bit is a `(HIGH_us, LOW_us)`
  pulse pair where HIGH and LOW both vary per bit.
- Per-bit timing (rtl_433 `-A` measured):
  - "0" bit = HIGH ~732 µs + LOW ~1136 µs
  - "1" bit = HIGH ~1100 µs + LOW ~756 µs
  - sync = HIGH ~1476 µs + LOW ~1500 µs
- Packet shape (this specific 1WSHR-PRO sub-variant): 3-sync-triplet
  followed by 35 data bits, repeated ~8 times per button press.
- **FIXED CODE.** Every press of a given button transmits the same
  35-bit pattern on the wire. No rolling counter, no encryption, no
  device key.

So the firmware does not need a KeeLoq cipher, a device-key recovery
path, or counter management. Capture each button's 35-bit pattern
once via SDR, store it verbatim in `secrets.py`, and replay through
the CC1101. See [`docs/12-bench-validation.md`](12-bench-validation.md)
for the on-bench validation procedure.

`keeloq.py` is retained in the firmware tree as a fallback for readers
working with a Compustar variant that does use HCS-KeeLoq. See
[`sdr/07-key-recovery.md`](../sdr/07-key-recovery.md) for that path.

## RF transmission plan

- CC1101 module configured for 433.92 MHz OOK in async serial mode
  (`init_433mhz_ook()` in `esp32/src/lib/cc1101.py`)
- ESP32 SPI master + bit-bangs the GDO0 pin per the rendered
  pulse train from `compustar.build_pulses_for_button()`
- No FIFO, no packet engine — direct OOK pulse train control gives
  microsecond-precise timing across the 700–1500 µs pulse range

## Why a thief can't exploit this

- The brain validates the on-air packet, but the car still requires
  the **mechanical key in the cylinder** to release the steering
  column lock
- Without the steering lock released, the car can idle but cannot be
  driven
- This is the same security model as factory remote start on modern
  cars — the system is no less secure than the existing FOB

The captured 35-bit patterns are FOB-identifying values and live in
`sdr/analysis/framing.local.md` (gitignored) — not the committed
`framing.md`. They have the same security weight as the physical FOB:
treat them as you'd treat your car keys.
