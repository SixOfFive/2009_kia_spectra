# How to reproduce this build

This is the master index of step-by-step instructions for reproducing the
voltage-triggered remote start on a 2009 Kia Spectra (or any aftermarket
remote-start equipped car with a Compustar / Microchip HCS-family FOB).

The pages below are numbered in the order you'd actually do them. Each
page is self-contained and tells you what to have on hand, exact commands
to run, what success looks like, and where to put any artifacts you
generate.

Pages marked **WIP** are scheduled to be filled in as we get there in
the build. The repo grows along with the project.

## Phase 0 — Prerequisites

- [ ] [Hardware bill of materials](BOM.md) — what to order
- [ ] [Architecture overview](architecture.md) — how the pieces fit together
- [ ] [What we learned about the Compustar install](compustar-research.md)
- [ ] [Power budget analysis](power-budget.md) — predicted parked draw, days-to-trigger, net energy effect

## Phase 1 — SDR capture and analysis (do this with the FOB before the rest of the parts arrive)

- [ ] [01 — Install SDR software](../sdr/01-software-setup.md)
- [ ] [02 — Verify the dongle works](../sdr/02-hardware-verification.md)
- [ ] [03 — Confirm the FOB transmit frequency](../sdr/03-frequency-confirmation.md)
- [ ] [04 — Record clean captures of each button](../sdr/04-recording-captures.md)
- [ ] [05 — Demodulate captures to bit sequences](../sdr/05-demodulation.md)
- [ ] [06 — Capture per-button packet patterns](../sdr/06-framing-extraction.md)
- [ ] [07 — Recover the KeeLoq device key](../sdr/07-key-recovery.md) (HCS-KeeLoq FOBs only — skip for Compustar 1WG3R-family)

## Phase 2 — Bench prototype (after parts arrive)

**Take-to-the-bench cheat sheet**: [`day-one.md`](day-one.md) — combines steps 08-12 into a printable checklist with triage table and "when things go wrong" troubleshooting.

- [ ] [08 — Flash MicroPython on the ESP32](08-flash-micropython.md)
- [ ] [09 — Bench smoke-test each module](09-bench-smoke-tests.md) (ADS1115, CC1101, CAN, ESP32 WiFi)
- [ ] [10 — Set up the Raspberry Pi Zero 2 W](10-pi-setup.md) (provision.sh, dashboard, kiosk)
- [ ] [11 — Wire ESP32 ↔ Pi UART link](11-uart-link.md)
- [ ] [12 — Compustar bench validation + first car Lock test](12-bench-validation.md)

## Phase 3 — In-car install — WIP

- [ ] 13 — Wire to the OBD-II port (Y-splitter + pigtail)
- [ ] 14 — Mount the case and run the display ribbon to the dash
- [ ] 15 — First live trigger (manual, parked, hood open)
- [ ] 16 — First voltage-triggered auto-start

## Phase 4 — Polish — WIP

- [ ] 17 — Home network monitoring (MQTT to Home Assistant)
- [ ] 18 — Long-term stability tests
- [ ] 19 — Cellular alerting (optional)
