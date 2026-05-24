# 13 — Wire to the OBD-II port (Y-splitter + pigtail)

## Goal

Get +12V always-hot power and CAN-H / CAN-L into the controller through
the car's OBD-II connector without permanently modifying the harness.
A passive Y-splitter keeps scan tools usable; a fused, TVS-protected
pigtail keeps the rest of the system safe from the worst that an
automotive 12V rail can throw at it.

When this step is done the controller can be powered up in the car,
but **do not bench-test with the engine running** until the install
is fully verified (step 15).

## Prerequisites

- [12 — Compustar bench validation](12-bench-validation.md) passed end
  to end. RF, UART, dashboard, voltage trigger all confirmed on the
  bench — no electrical surprises left to find at this step.
- Hardware from the [BOM](BOM.md):
  - OBD-II passive Y-splitter (Bafx OBD2 pass-through or similar
    dual-female passive splitter — see "Y-splitter selection" below)
  - OBD-II to bare-wire pigtail (16-pin male, ~1 m of pigtail leads)
  - SMBJ24CA TVS diode (bidirectional, 24 V working voltage)
  - 2 A ATM mini blade fuse + inline fuse holder
  - 12V→5V buck converter, 5A (from [BOM](BOM.md) row 13)
  - SN65HVD230 CAN transceiver, already smoke-tested
    (see [09 — bench smoke tests](09-bench-smoke-tests.md))
  - Twisted-pair hookup wire, 22 AWG for power + 24 AWG twisted for CAN
- Multimeter, wire strippers, soldering iron, heat-shrink.

## OBD-II pinout cheat sheet (J1962 connector, looking at the car's socket)

| Pin | Signal | Use in this build |
|----|----|----|
| 4 | Chassis GND | Single-point ground — controller GND ties here |
| 5 | Signal GND | Tie to pin 4 at the connector if separate (Spectra connects them internally) |
| 6 | CAN-H | → SN65HVD230 CAN-H |
| 14 | CAN-L | → SN65HVD230 CAN-L |
| 16 | +12 V battery (always hot) | → 2 A fuse → TVS → buck input |

All other pins are unused. **Do not touch pin 1 / 9 / 8 / 12** —
some cars expose ignition-switched lines, K-line, or J1850 there, and
on the Spectra they're either NC or hooked to things you don't want
to load.

## Step 1 — Y-splitter selection

Use a **passive** pass-through splitter only. Avoid any product that
calls itself "OBD-II hub", "smart splitter", or "switched splitter" —
those contain MCUs that can hold the bus dominant or interfere with
arbitration when the controller and a scan tool are both connected.

Tested-good products (any one of these works):

- Bafx Products OBD2 Y-splitter (passive)
- iCar Pro / VEEPEAK passive Y-cable
- Generic "OBD-II 1-to-2 male-to-2-female passive cable" from
  AliExpress (cheap; verify it really is passive by measuring
  continuity from a male pin to **both** female sockets in the
  same position)

Plug one female socket into the car's OBD-II port. The other female
socket stays accessible for a Veepeak / scan tool. The male side
becomes our pigtail mounting point.

## Step 2 — Wire the pigtail

The pigtail is a male OBD-II connector with ~1 m of unterminated wire.
Cut the wires you don't need flush with the connector body and heat-
shrink the stubs so they can't short.

Keep:

| Wire | Goes to | Notes |
|----|----|----|
| Pin 16 (+12 V) | 2 A fuse → TVS anode → buck V+ | Use the heaviest gauge wire in the pigtail (usually red) |
| Pin 4 (GND) | Common ground bus inside the case | 22 AWG, tie pin 5 to pin 4 here too if your pigtail breaks them out separately |
| Pin 6 (CAN-H) | SN65HVD230 CAN-H | Twist with CAN-L from this point onward |
| Pin 14 (CAN-L) | SN65HVD230 CAN-L | Twist with CAN-H from this point onward |

Strain-relieve all four wires at the OBD plug itself — a small zip
tie around the cable bundle, anchored to a notch in the case wall or
to the plug's strain-relief boot. The OBD connector hangs straight
down off the dash; you do not want the conductor itself taking the
weight of the wire.

## Step 3 — Power-side protection

```
Pin 16 (+12V) ──┬── 2 A ATM mini fuse ──┬── SMBJ24CA TVS ──┬── buck V+
                │                       │                  │
                │                       │                  └── 10 µF tantalum (already in BOM row 15)
                │                       │
                │                       └── (TVS cathode here, anode to GND bus)
                │
                └── (no other taps — single power feed only)
```

Layout notes:

- **Fuse first**, before anything else. If a downstream component
  fails short, the fuse blows before the OBD harness wire heats up
  and before you push 60 A back into the battery.
- **2 A is the right size**: peak load is ESP32 + Pi + display ≈
  150 mA on the 12 V side after the buck (because the buck steps up
  the current draw on the 5 V side, the 12 V side stays well under
  500 mA even at full burn). 2 A leaves room for inrush and a
  safety margin; anything bigger and you've defeated the fuse.
- **TVS goes across +12 V to GND**, bidirectional, **after** the fuse
  and **before** the buck. The SMBJ24CA clamps at ~38.9 V — well
  above normal alternator transients (~14.5 V) and load-dump
  transients (up to ~36 V on a properly clamped car) but well below
  the buck's absolute-max input.
- **10 µF tantalum** across the buck input (already specified in BOM
  row 15) absorbs the high-frequency edges that the TVS doesn't
  catch.

## Step 4 — Ground strategy

Single-point ground, period.

- All controller GND (ESP32 GND, Pi GND, buck output GND, CC1101 GND,
  SN65HVD230 GND, ADS1115 GND, display GND) tie to **one** bus inside
  the case.
- That bus has **exactly one wire** out to the OBD pigtail — pin 4.
- If your pigtail breaks pin 5 (Signal GND) out separately, tie pin 4
  and pin 5 together right at the pigtail, not inside the case. The
  Spectra connects them internally but other cars don't; tying them
  at the pigtail makes the install vehicle-portable.

Do **not** ground the case to the car body, the dash bracket, or
anything else. Ground loops via the chassis are a classic source of
phantom CAN errors and ADC noise.

## Step 5 — CAN wiring

CAN-H (pin 6) and CAN-L (pin 14) **must be twisted** from the OBD plug
all the way to the SN65HVD230. Loosely twisted 24 AWG (one full twist
per 2-3 cm) is fine. Untwisted CAN runs of more than ~10 cm pick up
enough noise to push the bus-error counter into passive error state
in some cars.

- No terminator needed at the controller end. The two ECUs that the
  Spectra exposes on this bus (PCM and TCM) already provide 60 Ω of
  termination each at their ends; adding 120 Ω here would put the
  bus at 40 Ω and de-tune everything.
- Keep the CAN pair physically away from the CC1101 antenna feedline.
  433 MHz RF coupling into CAN edges is rare but it's a fun bug to
  not have.
- SN65HVD230's `Rs` pin (slew-rate control) ties to GND through a
  10 kΩ resistor on the module — leave the module configuration as
  shipped.

## Step 6 — First power-up in the car (system off)

With the OBD-II pigtail unplugged from the splitter, do a final
bench check on the install:

```powershell
# multimeter checks at the case end before plugging in
# (probe at the female header that mates with the pigtail)
```

- 12 V wire to GND: should read open circuit (∞ Ω) with the buck and
  ESP32 disconnected, ~few kΩ with everything connected.
- CAN-H to CAN-L: should read ~60 kΩ (SN65HVD230 input impedance) —
  **not** 60 Ω. If you see 120 Ω, you accidentally populated the
  termination resistor on the breakout board; lift it.

Plug the pigtail into the splitter. Plug the splitter into the car's
OBD-II port.

- Multimeter probe between pin 16 wire and pin 4 wire at the case
  end: should read battery voltage (~12.6 V key out).
- The buck's output rail should come up to 5 V immediately.
- ESP32's onboard LED should blink the boot sequence within ~1 s.
- After ~10 s, `journalctl -u vroom.service -f` on the Pi (over WiFi
  this time, no longer wired to your dev machine) should show
  STATUS messages with `v_battery ≈ 12.6` flowing.

If any of those check fails, **unplug the OBD pigtail** before
debugging. Do not debug an automotive install with the harness live.

## Step 7 — Confirm OBD CAN traffic (only if `machine.CAN` is in your firmware)

In Thonny over WiFi (`webrepl`) or via a temporary USB cable to the
ESP32 (you'll need to pop the case for this):

```python
>>> from lib.twai_can import Can
>>> can = Can(tx_pin=5, rx_pin=4, baudrate=500_000)
>>> can.receive(timeout_ms=2000)
```

Should return a `(arbitration_id, data)` tuple with the ID being one
of the Spectra's broadcast frames (0x316 PCM RPM, 0x329 wheel speed,
etc.). If it times out:

- Engine off + key out = no traffic on most cars. Turn the key to
  ON (accessory, not crank) and retry — the PCM wakes up and starts
  broadcasting.
- CAN-H / CAN-L swapped — try swapping the wires at the pigtail.

## What you should have when done

- OBD-II pigtail wired through a passive Y-splitter, pin 16 + pin 4 +
  pin 6 + pin 14 only, all other pins terminated short
- 2 A fuse on the +12 V line, SMBJ24CA TVS across +12 V to GND, both
  inside the case
- Single-point ground bus with one wire out to pin 4
- CAN pair twisted from plug to SN65HVD230
- Strain relief at the OBD plug
- Bench multimeter checks all pass with the pigtail disconnected
- Controller comes up cleanly with the pigtail plugged in, key out
- (Optional) CAN frames received from the PCM with key on

## Where artifacts go

- Photos of the as-built pigtail to
  `logs/images/YYYY-MM-DD/obd2-pigtail-*.jpg` (per the daily-log
  convention)
- Any deviation from the BOM (substitute splitter, different fuse,
  etc.) noted in that day's `logs/YYYY-MM-DD.md`

## Troubleshooting

| Symptom | Fix |
|---|---|
| Fuse blows immediately on plug-in | Buck shorted, or pin 16 tied to GND somewhere; unplug, ohm out the 12 V rail to GND |
| Buck output is 5 V but ESP32 doesn't boot | ESP32 VIN line broken, or 5 V getting injected on a 3.3 V pin somewhere |
| STATUS messages stop after 30 s | WiFi credentials in `secrets.py` are for your dev bench network, not your home network. Update them. |
| CAN receive times out with key on | CAN-H / CAN-L swapped at the pigtail, or the SN65HVD230 module has its onboard 120 Ω terminator populated. |
| Random ESP32 resets while engine cranks | TVS missing or in the wrong place. Cranking pulls the 12 V rail down to ~6 V then snaps back; without the TVS + tantalum, the buck can oscillate. |

## Safety callouts

- **Do not** tap into ignition-switched lines for power. OBD-II pin
  16 is the only correct power source for an always-on monitor — it's
  fused at the body harness end and it's expected to draw a small
  parasitic load. Anything else either dies when the key turns off
  (defeats the whole project) or backfeeds into circuits that don't
  expect to see external loads.
- **Do not** try to power the Pi directly from pin 16 without the
  buck converter. Pin 16 is 12 V nominal but swings 9-15 V (cranking
  to alternator) and can spike to 60+ V during load dump. A Pi sees
  that and dies.
- **Do not bench-test with the car running** until the install is
  verified end to end in step 15. A misrouted wire is much easier to
  diagnose with everything cold than at 14.4 V with the alternator
  pumping current.

## Next

[14 — Mount the case and run the display ribbon to the dash](14-case-mounting.md)
— physically install the controller under the dash, route the
display, place the antennas.
