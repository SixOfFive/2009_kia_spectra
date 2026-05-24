# Power budget analysis

Does the architecture actually meet the "weeks parked without draining the battery" goal? This doc walks through the numbers.

## Tl;dr

Parked, our system adds ~7-12 mA average draw to the car. A 2009 Spectra battery is ~50 Ah, the car's own quiescent draw is ~30-50 mA, so the combined total is ~37-62 mA — call it 50 mA worst-case. That gives **~41 days from a full battery to a "no-start" voltage drop** before our trigger ever fires.

Once we do fire, the engine runs for 15 minutes and the alternator dumps ~7-8 Ah back in. So the system is **net restorative**, not depleting.

## Per-component current draw

### Always-on path (when parked)

| Component | State | Current |
|---|---|---|
| LM2596 buck converter | quiescent (lightly loaded) | ~8 mA |
| ESP32-WROOM-32U | deep sleep | 10-50 µA (board-dependent; counterfeit boards leak more) |
| ESP32-WROOM-32U | awake (sample + sleep transitions) | ~80 mA |
| ADS1115 | continuous off + single-shot @ 1/min | ~75 µA average |
| CC1101 | SLEEP state | <0.2 µA |
| SN65HVD230 | silent mode (driver disabled) | ~200 nA |
| TVS SMBJ24CA | leakage | <1 µA |

### When ESP32 wakes briefly to sample (~1 s out of every WAKE_INTERVAL_S)

ESP32 awake current spike: ~80 mA × 1 s per minute = duty cycle 1/60.

```
Average ESP32 = (deep_sleep * 59 + awake * 1) / 60
              = (50 µA  * 59 + 80 mA * 1)  / 60
              = (2.95 + 80) mA·s / 60 s
              = ~1.38 mA average
```

### Total parked draw

```
LM2596 quiescent     ~8.0 mA
ESP32 sampling avg   ~1.4 mA
ADS1115 single-shot  ~0.1 mA
CC1101 sleep         negligible
SN65HVD230 silent    negligible
─────────────────────────────
Total from us         ~9.5 mA
```

Combined with the 2009 Spectra's own parasitic draw (typical ~30 mA — alarm, ECU keepalive, radio memory), worst case the car total is ~40-50 mA parked.

## Days to "no-start" voltage

A typical lead-acid battery at 50 Ah delivers usable energy to ~30 Ah before voltage drops below the ~10.5 V starting threshold (lead-acid loses voltage non-linearly as capacity depletes).

```
days_until_no_start = usable_Ah / parked_mA / 24 hr
                    = 30,000 mAh / 50 mA / 24
                    = 25 days  (worst case, 50 mA total parked draw)

                    = 30,000 mAh / 40 mA / 24
                    = 31 days  (typical, 40 mA total)
```

This is the no-trigger scenario. Our `LOW_V_TRIGGER` defaults to **12.2 V** which corresponds to ~50% state of charge (~25 Ah remaining). Hitting that takes:

```
25,000 mAh / 40 mA / 24 = ~26 days   (typical)
```

So we expect the controller to fire its first trigger roughly **3-4 weeks** into a sustained parked period.

## When the engine runs

Once we trigger, the engine runs for `RUN_DURATION_S = 900 s` (15 min). The Compustar receiver provides ~12 min of actual cranked-and-running time after a couple of seconds of crank + idle stabilization.

Alternator output (Spectra 2.0L): nominally ~85 A at idle, derated to ~30 A at typical idle RPM with no other loads. Net battery charge:

```
charged_Ah = (alternator_A - engine_load_A - system_load_A) * minutes / 60
           = (30 - 5 - 1) * 12 / 60
           = ~4.8 Ah per 15-min cycle
```

That's ~10% of the battery's nominal capacity restored per cycle. After one trigger we're well above 80% state of charge again.

## RF transmit power impact

During an actual trigger:

| Phase | Duration | Current |
|---|---|---|
| Pi boot + handshake | ~30 s | ~250 mA (Pi + display backlight) |
| RF burst (4 repeats × ~80 ms) | ~320 ms | CC1101 TX ~33 mA + ESP32 80 mA + Pi 250 mA |
| OBD polling (RUNNING phase) | 15 min | ~25 mA CC1101 idle RX + ~17 mA CAN listening + ESP32 + Pi |
| RF stop burst | ~320 ms | ~360 mA |
| Pi shutdown grace | 30 s | ~250 mA |
| MOSFET cut | thereafter | back to parked draw |

Energy used by the controller during a trigger cycle: ~250 mA × 16 min ≈ **65 mAh**. That's 0.2% of the battery — negligible compared to the 4.8 Ah we put back.

## Sensitivity: what if the buck quiescent is worse than expected?

The LM2596 isn't the most efficient choice — its 8 mA quiescent dominates our parked budget. Swapping to a switching-mode regulator with <0.5 mA quiescent (e.g. TPS62291) drops the total to ~3 mA parked. Days-to-no-start nearly doubles.

For v1, the LM2596 is fine. If the field-tested parked drain is higher than predicted, that's the first thing to swap.

## What invalidates this analysis

- A bad cell in the battery — actual capacity could be 20 Ah instead of 50 Ah
- Counterfeit ESP32 module that doesn't reach proper deep sleep (50µA → 5 mA)
- Other accessories left on (interior light, dash cam, OBD-port-powered tracker)
- Cold winter — lead-acid capacity drops ~30% at -20°C
- A bad MOSFET that leaks current to the Pi while "off"

The bench validation in `docs/12` should include a multimeter inline measurement of actual parked current. If it's >15 mA, suspect ESP32 deep sleep or buck quiescent.

## Numbers worth tracking in the field

The dashboard's events feed records every transition. Useful production metrics:

- Time between triggers (should increase as winter -> summer)
- Voltage at trigger time (lower = battery degrading)
- Voltage 5 min into the run (alternator regulation working?)
- Voltage 1 hour after engine stop (recovery curve)

After a few months of data, you can fit a battery-aging model and predict failure ahead of time.
