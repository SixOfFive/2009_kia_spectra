# Power budget analysis

Does the architecture actually meet the "weeks parked without draining the
battery" goal?

**For the as-built v1 hardware: not as well as this document used to claim.**

> **Status: ESTIMATES, not measurements.** Every figure below is derived from
> datasheets, the firmware's own reported state, and the bench log. **Nobody
> has yet put a meter inline on the 12 V feed.** [Section 5](#5--how-to-measure-this-instead-of-estimating-it)
> says how to replace all of it with real readings in about 30 minutes. The
> honest spread on the headline number is roughly ±35 %.

## Why the previous numbers were wrong

Earlier revisions budgeted **~9.5 mA parked / ~41 days to no-start**. That
described a design we did not build: an ESP32-WROOM-32U in **deep sleep**,
waking ~1 s per minute (a 1/60 duty cycle), sampling through an ADS1115.

The shipped build is an **ESP32-S3-WROOM-1-N16R8 that never sleeps.** It holds
a WiFi STA association up continuously to serve the dashboard, the SNMP agent
and OTA. `CONFIG_PM_ENABLE` is not set in the Arduino-ESP32 3.3.10 prebuilt
libraries, so automatic light sleep is *compiled out* as well. There is no duty
cycle anywhere in this firmware.

The old budget was not slightly optimistic. It modelled a different machine.

## 1. Per-component parked draw, referred to the 12 V battery

Operating point: ESP32-S3 at **80 MHz** (persisted in NVS), WiFi **STA
associated with modem-sleep on** (`WiFi.setSleep(true)`, the shipped default),
no browser open, 25 °C, 12.6 V resting.

Power path is `12.6 V -> LM2596 -> 5 V -> AMS1117 LDO -> 3.3 V -> module`. Two
things about that chain are easy to get wrong:

- **The AMS1117 is an LDO, not a converter.** It passes the *same* current it
  delivers and burns the 1.7 V as heat. 40 mA at 3.3 V is 40 mA drawn at 5 V —
  no voltage-ratio saving, only its ~5 mA ground current on top.
- **The LM2596 module behaves as `I_in(mA) ~= 9.0 + 0.44 x I_out(mA)`** at
  12.6 V in / 5 V out. The 9 mA is fixed (IC quiescent plus the module's own LED
  and feedback divider). At light load, terminal-to-terminal efficiency is only
  about **55 %**.

| Component | Rail | mA at rail | **mA at 12.6 V** | Range |
|---|---|---:|---:|---|
| ESP32-S3 chip @80 MHz, modem-sleep | 3.3 V | ~24 | **11.0** | 8–13 |
| in-module 16 MB flash + 8 MB OPI PSRAM | 3.3 V | ~3 | **1.4** | 0.9–2.1 |
| WiFi radio, DTIM beacon wakes + LAN broadcast | 3.3 V | ~12 | **5.5** | 1.5–9 |
| CC1101, IDLE | 3.3 V | ~1.5 | **0.7** | 0.4–1.1 |
| AMS1117-3.3 quiescent | 5 V | ~5.0 | **2.3** | 1.8–4.2 |
| Two always-on red board LEDs | 3.3 V | ~5.0 | **2.3** | 1.3–4.2 |
| RGB LED controller (draws even when set black) | 3.3 V | ~0.6 | **0.3** | 0.2–0.4 |
| CH343 USB-serial, suspended | 5 V | ~0.1 | **0.05** | 0.04–0.6 |
| Sense divider + ESD leakage | — | ~0.03 | **0.02** | 0.01–0.3 |
| **LM2596 fixed overhead** | 12.6 V | — | **9.0** | 7–11 |
| **TOTAL — normal parked** | | ~52 mA @5 V | **~33 mA** | **25–45 mA** |

**Read that total honestly:** of ~33 mA, only about **11 mA-equivalent is the
electronics doing work.** The rest is the LM2596's fixed 9 mA, the LDO's 2.3 mA,
~8.6 mA of conversion loss, and 2.6 mA of LEDs. **Two-thirds of the parked draw
is power path and decoration.**

### States the board can enter by itself

| State | How you get there | **12.6 V total** | Range |
|---|---|---:|---|
| **A. Normal parked** | STA, modem-sleep, 80 MHz, nobody watching | **~33 mA** | 25–45 |
| **B. Being watched** | dashboard tab open and/or Cacti polling | **~38 mA** | 29–51 |
| **C. Radio never sleeps** | PS-off toggle, **or the automatic SoftAP fallback** | **~61 mA** | 48–82 |
| **D. Worst plausible** | PS off *and* 240 MHz | **~72 mA** | 57–95 |

> **State C is the trap.** After 5 minutes offline the firmware falls back to
> SoftAP — and **AP mode has no power save at all.** Park out of range of the
> home AP (a car park, a friend's driveway, a street two blocks over) and the
> board silently moves from 33 mA to ~61 mA. The only indication is the
> dashboard's mode field, which you cannot see, because you are out of range.
> See mitigation 4.

Going 80 -> 240 MHz costs about **+16 mA at 12 V**. The persisted 80 MHz setting
is correct and already banked.

## 2. Days to voltage threshold

Battery ~50 Ah. Using the flooded-lead-acid resting map
**SoC % = (V - 11.70) / 0.010** — 12.7 V = 100 %, 12.6 = 90 %, 12.4 = 70 %,
12.2 = 50 %, 12.0 = 30 %, 11.8 = 10 % — so **100 mV of resting voltage = 5 Ah.**
Starting from 12.6 V.

| Scenario | mA | -> 12.4 V | -> 12.2 V (marginal crank) | -> 11.8 V (won't crank) |
|---|---:|---:|---:|---:|
| Our board alone, State A | 33 | 12.6 d | 25.3 d | 50.5 d |
| Car alone, healthy | 30 | 13.9 d | 27.8 d | 55.6 d |
| **Car + board, State A** | **63** | **6.6 d** | **13.2 d** | 26.5 d |
| **Car + board, SoftAP fallback** | **91** | **4.6 d** | **9.2 d** | 18.3 d |
| Car + board + 150 mA fault | 213 | 2.0 d | 3.9 d | 7.8 d |
| Car + board + 300 mA fault | 363 | 1.1 d | 2.3 d | 4.6 d |
| *After mitigations 1–5 + 8* | *43* | *9.7 d* | *19.4 d* | *38.8 d* |

**Derating — apply these, they are not fine print:**

- **Aged battery** (this car has already killed two): **x 0.65**
- **Cold**, ~30 % capacity lost at -20 °C: **x 0.7** — and winter is exactly
  when you need the start
- Both: **x 0.46**. The 13.2-day row becomes **~6 days** in a January cold snap.

**What the trigger does about it.** `AS_DEF_VOLTS` ships at **12.40 V**, which is
the right threshold: 12.2 V is exactly 50 % SoC, where cycle damage begins, so
firing there is firing too late. At 63 mA the trigger would fire around day 6.6,
and a 15-minute run returns ~4.5 Ah against a ~1.5 Ah/day burn — roughly **one
auto-start every 3 days** holds the line indefinitely.

Two caveats that cut the other way:

1. **Auto-start ships disabled.** The drain is on by default; the protection is
   not. Arm it before leaving the car parked.
2. A 15-minute idle does not fully recharge a deeply discharged battery —
   alternator output tapers as voltage rises. It holds a line; it does not
   resurrect a flat battery.

## 3. How much worse than the old figure

| | Old doc | Corrected | Factor |
|---|---:|---:|---:|
| Our contribution, parked | 9.5 mA | **~33 mA** | **3.5x** |
| ...in SoftAP fallback | — | **~61 mA** | **6.4x** |
| Time to "won't reliably crank" | *claimed ~41 d* | **~13 d** | **1/3** |
| Same, cold + aged battery | — | **~6 d** | |

**Plainly:** the doc promised "weeks parked without draining the battery." The
as-built system gets **under two weeks** to the point a cold engine may not
crank, and closer to **one week** in winter on an abused battery.

It is also a regression against the project's own goal in a specific, awkward
way: **33 mA is more than the entire normal quiescent draw of the car** (~30 mA).
We more than double the vehicle's parked load, and land above the 30–50 mA
threshold a shop would use to declare a parasitic drain *present*. The
diagnostic tool has become a comparable fault to the one it was built to find.

That is fine for bench bring-up. It is **not** fine for permanent installation
in a car with a known drain that has already destroyed two batteries.

But keep proportion: against a 150–300 mA fault, our 33 mA is only 10–18 % of
the total. **The fault alone reaches 11.8 V in 5–8 days.** Finding it is worth
more than every milliamp below. The realistic failure mode is not "the board
flattened the battery" — it is "the board flattened an already-marginal battery
three days sooner, while auto-start was disabled, and got blamed for it."

## 4. Mitigations, cheapest first

Savings at the 12 V side, from the ~33 mA State A baseline.

| # | Action | Saves | Effort | Costs you |
|---|---|---:|---|---|
| 1 | **Keep CPU at 80 MHz** (already persisted) | ~16 mA *banked* | none | nothing |
| 2 | **Keep WiFi power-save ON** (shipped default) | ~28 mA *banked* | none | ~10–100 ms first-packet latency |
| 3 | Don't leave a dashboard tab open; Cacti at 300 s not 60 s | 4–5 mA | none | coarser graphs |
| 4 | **Time-limit the SoftAP fallback** — retry STA and sleep the radio instead of holding an AP up forever | **~28 mA in the failure case** | ~20 lines | no dashboard when out of AP range, which is exactly when you can't reach it anyway. **Best value per effort.** |
| 5 | Lift the series resistors on the two power LEDs; cut the RGB LED's VDD | 2.5–3 mA | 15 min hot air | no visual power indication |
| 6 | ~~Add `delay()` to `loop()`~~ | **~0.5 mA** | 1 line | **Not worth doing.** `WebServer::handleClient()` already calls `delay(1)` every clientless pass, so `loop()` is already tick-limited — core 1 measures 10–13 %, not 100 %. There is no duty cycle to recover. |
| 7 | Replace the LM2596 with a low-Iq synchronous buck (LM5164/LM5165, ~10 µA Iq, or Pololu D36V6F5) | 8–9 mA | $8–15 | nothing |
| 8 | **Better: one stage.** 12 V -> 3.3 V low-Iq buck straight to the 3V3 pin, **with the AMS1117 removed first** (back-feeding an LDO pushes current backwards through the pass element) | **17–18 mA** | $10–15 + SOT-223 rework | lose USB-5 V powering unless jumpered |
| 9 | Duty-cycle the radio (WiFi up 60 s per 10 min) | 5–6 mA | moderate | poor value — the radio is only ~5.5 mA with modem-sleep on |
| 10 | **Deep sleep**, wake ~5 s per 5 min | ~18 mA stock path | large rewrite | no live dashboard, SNMP or OTA outside the window; CPU-load and net counters become meaningless. This is what the *original* budget assumed. |
| 11 | Hard disconnect (latching relay / high-side FET), or just pull the fuse for storage > 1 week | everything | design change | can't auto-start while disconnected |

**Running totals:** items 1–3 (free, today) ~29 mA · 1–5 + 7 → ~22 mA ·
**1–5 + 8 → ~13 mA**, i.e. the old doc's promised budget with no firmware sleep
at all · 1–5 + 8 + 10 → ~1 mA.

**The power path (7–8) is worth more than every firmware trick short of deep
sleep, and costs nothing in functionality.**

> Note: the "~8 mA deep sleep" row in `esp32-s3/docs/voltage-monitor.md` is the
> dev board's LDO+LED floor, **not** the chip's ~8 µA deep-sleep spec — a factor
> of 1000 apart. Deep sleep on this dev board without items 5 and 8 lands around
> 15 mA. Microamps need a bare module and a low-Iq regulator.

## 5. How to measure this instead of estimating it

### 5a. Differential drain rate — free, the firmware already does it

The dashboard's **drain rate** tile (`drain_mvph` in `/json`, SNMP `.26.0`) is a
least-squares fit of voltage against time over the most recent parked window.
Convert with:

```
I (mA) = |drain mV/h| x C (Ah)
```

At 50 Ah nominal, **multiply the dashboard reading by 50 to get milliamps.**

Fuse-pull procedure:

1. Drive >= 20 min, park, lock. The window auto-restarts whenever voltage
   exceeds 13.2 V, so it begins by itself.
2. **Ignore the first 4–12 h.** Surface charge redistributing after a drive
   produces a large apparent drain that is not real.
3. Let it run a **full 24 h**. Require **r2 > 0.9** before writing it down.
4. Pull one fuse. Repeat for another full 24 h. The difference x 50 is that
   circuit's current.

**Limits — be honest about them:**

- `DRAIN_MIN_N` = 30 means the tile goes "ok" after 30 minutes. That is far too
  short to trust. Treat `ok` as "the maths ran", not "the answer is right" —
  which is why r2 is displayed.
- **There is no temperature compensation.** Lead-acid OCV tempco is about
  -1 to -4 mV/°C, so a 15 °C day/night swing is 15–60 mV of pure artefact,
  comparable to the real signal. **Always fit over whole multiples of 24 h** so
  the window starts and ends at the same point in the thermal cycle.
- ADC resolution is ~5.5 mV per LSB at the battery; averaging helps, r2 gates it.
- Any gap > 10 min, any reboot and any OTA **ends the window**.
- **Practical resolution ~±1 mV/h, i.e. ~±50 mA.** Plenty to find a 150–300 mA
  fault. *Not* enough to tell State A from State C. Don't tune firmware with it.

### 5b. DMM inline — 30 minutes, and it settles this whole document

1. **Safety first:** the bench log records this clone board's "5 V" pin reading
   **3.25 V**. Ring it out against the AMS1117 VIN pin before feeding 5 V in —
   if it is tied to the 3.3 V rail, injecting 5 V destroys the module.
2. Break the 12 V feed, DMM in series, start on the 10 A range and step down.
3. Read State A. Toggle WiFi PS off and read again. Then 240 MHz. Then let it
   fall into SoftAP. **Four readings replace all of section 1.**
4. To split module from board overhead, **jumper EN to GND** — the module in
   reset is well under 1 mA, so what remains *is* the board overhead.

### 5c. INA226 inline shunt — ~$4, makes it permanent

- 0.1 Ω 1 % >= 0.25 W shunt in the 12 V feed to the board. At 33 mA that is
  3.3 mV; the INA226's 2.5 µV LSB gives ~**0.025 mA resolution**, full scale
  819 mA.
- Bus-voltage input is **36 V max** — fine at rest, not during load dump. Keep
  the TVS upstream and put ~100 Ω in series with each IN pin.
- The INA226 itself costs ~0.15 mA at 12 V, about 0.5 % of what it measures.
- Wire to any two free GPIOs as I²C, publish `ma` in `/json` beside
  `drain_mvph` plus a new SNMP OID. Every estimate here then becomes obsolete,
  which is the point.

## 6. Expected dashboard mV/h per scenario

Drain shows as **negative** mV/h. `C` is *actual* capacity — a battery this car
has already half-killed may be 30–35 Ah, not 50.

| Scenario | Total mA | **mV/h @ 50 Ah** | mV/h @ 35 Ah |
|---|---:|---:|---:|
| Fuse out, self-discharge only | 3 | -0.06 | -0.09 |
| Car alone, healthy | 30 | -0.60 | -0.86 |
| **Car + board, State A** | 63 | **-1.26** | -1.80 |
| **Car + board, SoftAP fallback** | 91 | **-1.82** | -2.60 |
| **Car + board + 150 mA fault** | 213 | **-4.26** | -6.09 |
| **Car + board + 300 mA fault** | 363 | **-7.26** | -10.4 |

**Reading your own dashboard:**

- **Worse than -2.5 mV/h** -> the fault is present and dominates everything in
  this document. Stop optimising firmware and go pull fuses.
- **-1.2 to -1.9 mV/h** -> car plus our board, roughly as designed. You cannot
  distinguish State A from State C at this resolution — check the mode field and
  the WiFi power-save state instead, both of which the dashboard reports.
- **-0.6 to -1.0 mV/h** -> our board is off or its fuse is out; that's the car alone.
- **Better than -0.3 mV/h, or positive** -> the fit hasn't settled, or you're in
  the first 12 h after a drive, or the sun is on the car. Wait for a full 24 h
  and r2 > 0.9.

Cross-check against the projection the firmware already computes: at -1.26 mV/h
from 12.6 V, `drainHoursToFlat` should read ~636 h (26.5 days), matching the
"car + board State A" row in section 2. If those disagree, the fit hasn't
converged.

## When the engine runs

Once triggered, the engine runs ~12–15 min. Alternator output on the 2.0L is
nominally ~85 A, derated to ~30 A at idle with no other loads:

```
charged_Ah = (30 - 5 - 1) * 12 / 60 = ~4.8 Ah per cycle
```

About 10 % of nominal capacity restored per cycle — comfortably more than the
~1.5 Ah/day the combined parked load burns. The system is **net restorative**,
provided auto-start is actually armed.

Energy spent by the controller during a trigger cycle is on the order of
100 mAh — negligible against the 4.8 Ah returned.

## What invalidates this analysis

- **A meter.** Every number in sections 1–3 is an estimate. Measure it.
- A bad cell — actual capacity could be 25 Ah, not 50
- The board sitting in SoftAP fallback without anyone noticing (see mitigation 4)
- Cold — lead-acid loses ~30 % at -20 °C
- Other accessories (dash cam, OBD-port tracker) sharing the port
- The unlocated parasitic fault, which likely dwarfs all of the above

## Numbers worth tracking in the field

- Drain rate (mV/h) with r2 > 0.9 over 24 h windows, before and after each fuse pull
- Time between auto-starts (should lengthen winter -> summer)
- Voltage at trigger time (falling over months = battery degrading)
- Voltage 1 h after engine stop (the recovery curve)

The start log and the SNMP tree already record all of these.
