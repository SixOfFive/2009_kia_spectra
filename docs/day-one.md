# Day one with parts — operating order

The boxes arrived. This page is the take-to-the-bench cheat sheet that
combines all of docs/08-12 into one ordered checklist. Print it or have
it open on a second screen.

## Before you open any box

- [ ] Soldering iron warmed up, multimeter, flush cutters, fresh CR2032
- [ ] Thonny installed (`pip install thonny`)
- [ ] mpremote installed (`pip install mpremote`)
- [ ] RTL-SDR + Universal Radio Hacker working (Phase 1 done)
- [ ] Run `python tools/preflight.py` — should report 27 OK + 3 warnings (no secrets, no paho-mqtt). Errors here = fix before continuing.
- [ ] FOB battery confirmed strong (LED bright when pressed)

## Triage as you unbox

For each module, do a 30-second visual + multimeter check:

| Module | Check | Pass criterion |
|---|---|---|
| ESP32-WROOM-32U | Is the ESP module + IPEX connector present? | Yes (the AliExpress scam ships antenna-only; if you only got an antenna, you got bait-variant) |
| Pi Zero 2 W | Header pre-soldered? | If yes, easier. If no, you'll need to solder pins (already in BOM kit) |
| ADS1115 | ADDR pin solder bridge or floating? | Floating = address 0x48 (what we want) |
| CC1101 | Antenna connector present? Crystal visible? | Yes to both |
| SN65HVD230 | 8-pin SOIC chip present? | Yes |
| OBD-II Y-splitter | Continuity from each pin in to both pins out (Pin 16, 4, 6, 14) | Yes for all four |
| OBD-II pigtail | Confirm it's MALE (plugs into car-side female socket) | Yes (NOT female — that was the original bad order; uxcell variant is male) |
| Buck converter | Adjust trimpot to output 5.0V (apply 12V in, measure out) | 5.00 ± 0.05V |
| Display | HDMI + USB-A both plug in cleanly? | Yes |

Anything that fails: set aside, photograph, decide return vs continue.

## Phase 2 order of operations

Follow these in order. Each step is its own doc — link goes deeper.

### Day 1 morning — software setup

1. [`docs/08-flash-micropython.md`](08-flash-micropython.md) — flash ESP32 via Thonny
2. [`esp32/scripts/install.ps1 COM7`](../esp32/scripts/install.ps1) — copy esp32/src/* to the board
3. Smoke-test from Thonny REPL:
   ```python
   >>> from machine import Pin; import time
   >>> [Pin(2, Pin.OUT).value(i % 2) or time.sleep(0.2) for i in range(10)]
   ```

### Day 1 afternoon — bench smoke tests

4. [`docs/09-bench-smoke-tests.md`](09-bench-smoke-tests.md) — wire each module to the ESP32 on a breadboard, one at a time:
   - ADS1115 over I2C (3V3 known voltage → ~26400 raw)
   - CC1101 over SPI (partnum=0x00, version=0x14)
   - SN65HVD230 CAN loopback (if `machine.CAN` is in your build)

### Day 1 evening — Pi setup

5. [`docs/10-pi-setup.md`](10-pi-setup.md) — Raspberry Pi Imager, first boot, `sudo bash provision.sh`, dashboard reachable on LAN
6. Plug display + verify chromium kiosk

### Day 2 morning — wire it together

7. [`docs/11-uart-link.md`](11-uart-link.md) — three wires between ESP32 and Pi
8. Confirm STATUS messages flow into the dashboard

### Day 2 afternoon — SDR phase (if not done already)

9. Run through [`sdr/01-...`](../sdr/01-software-setup.md) through [`sdr/07-key-recovery.md`](../sdr/07-key-recovery.md)
10. Recover the KeeLoq device key
11. Validate with `python sdr/scripts/validate-key.py ...` → all deltas should be 1
12. Update `esp32/src/secrets.py` and `compustar.Function` codes per `sdr/analysis/framing.md`

### Day 2 evening — the moment of truth

13. [`docs/12-keeloq-bench-validation.md`](12-keeloq-bench-validation.md) — synthesize → SDR-confirm → first car Lock cycle
14. Re-run `python tools/preflight.py` — should now report 0 warnings (secrets populated)
15. **Lock works** in the car → onto Unlock → onto Start (in that order, with valet switch flipped)

### Day 3+ — perfboard + in-car install

16. Move from breadboard to perfboard (Phase 3 — docs to be written)
17. Mount in case, wire to OBD-II via Y-splitter
18. First in-car voltage-triggered auto-start

## When things go wrong

### `tools/preflight.py` fails an import

Something is genuinely broken. Fix before continuing.

### Bench smoke test fails on a specific module

Suspect that module. Don't proceed to the next test until the failing one is resolved — debugging "module 3 doesn't work" with three other module changes in play is much harder than debugging it in isolation.

### URH demodulates to nonsense bits

Re-do the frequency sweep (`sdr/03`). FOB might be at 313.5 MHz instead of 433.92 MHz (some Compustar 1-way kits are).

### KeeLoq Path A (Flipper Zero DB) finds no match

Fall back to Path B (PICkit EEPROM dump). The chip-in-FOB has the device key in plaintext.

### Synthesized RF burst is visible on SDR but car doesn't respond to Lock

Three usual suspects:
- Counter too far behind/ahead (FOB has been used recently → ESP32 counter is stale)
- Function code mapping wrong (re-verify via `diff-bits.py` between Lock and Unlock captures)
- TE timing off (measure preamble cycle width precisely in URH)

### Start works but engine cuts out after a few seconds

Probably the immobilizer bypass isn't engaging. Check that the Compustar BLADE module is still functioning and the sacrificed key inside hasn't dislodged.

## Don't forget

- Auto-commit every change. The repo is `https://github.com/SixOfFive/2009_kia_spectra`.
- Update `logs/YYYY-MM-DD.md` at end of each session.
- `secrets.py` is gitignored — keep your local copy backed up somewhere safe (1Password, encrypted USB).
- The valet switch on the Compustar bypass is your safety net. Know where it is before powering anything up in the car.
