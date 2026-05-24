# 16 — First voltage-triggered auto-start

## Goal

Test the actual reason this project exists: the controller decides
on its own that battery voltage is too low, fires the engine, runs
it for `RUN_DURATION_S`, stops it, and goes back to sleep — all
without you touching anything.

The trick is testing this without actually letting the battery drain
to threshold (which would take days, and which would only test the
trigger once before you have to either jump it or wait for it to
recover). Instead, **temporarily raise `LOW_V_TRIGGER` above the
current resting voltage**, watch the trigger fire, then restore the
threshold to the real production value.

## Prerequisites

- [15 — First live trigger](15-first-trigger.md) passed with 5+
  clean cycles
- Dashboard is reachable from the driver's seat (or from a phone
  on the same WiFi network — you'll be watching it during the test
  rather than touching it)
- Real Compustar FOB on your person (still — the safety story doesn't
  change just because you've done a few starts)
- Compustar valet switch reachable
- Hood open
- Same parking location as step 15

## Why we test with an elevated threshold

Production `LOW_V_TRIGGER` is `12.2 V`. A healthy car battery at
rest reads ~12.6 V; an OBD-loaded battery with the controller in
deep-sleep poll reads ~12.5 V. The system, as configured, will
never trigger unless the battery genuinely drops to 12.2 V — which
takes 2-4 weeks of parking for a healthy battery, and a much
shorter time for a sick one.

To verify the trigger *path* without waiting for that, we raise the
threshold above current voltage. The controller fires immediately
(after `LOW_V_SUSTAIN_S` confirms the reading is stable), runs the
full cycle, and we then restore the production threshold.

This validates: ADC reading, sustain logic, RF transmit, MOSFET
gating, Pi boot, Pi acknowledgment, OBD CAN read, run timer,
auto-stop, shutdown coordination, MOSFET cut, return to deep-sleep
poll. The whole chain. No piece of the auto-trigger path goes
untested.

## Step 1 — Note the current resting voltage

From the dashboard or via MQTT, read the `v_battery` value. Note it
down: this is your baseline.

Example values:

| Reading | Meaning |
|---|---|
| 12.6 V | Healthy battery, key out, no recent crank |
| 12.4 V | Slightly discharged, or recent crank within last hour |
| 12.2 V | Either threshold-low (auto-trigger is about to fire on its own!), or a sick battery |
| <12.0 V | Battery is in trouble independent of this project |

Pick an elevated threshold that is **0.2 V above** the current
reading. So if you're at 12.5 V, set the test threshold to 12.7 V.
This guarantees the trigger fires on the very next sample.

## Step 2 — Set the elevated threshold via the dashboard

The dashboard's settings panel includes a `LOW_V_TRIGGER` editor
(the same one you used in bench validation). From the driver's seat
or your phone:

1. Open the dashboard's settings panel
2. Change `LOW_V_TRIGGER` to the elevated value (e.g. 12.7 V)
3. Apply

The Pi forwards a `set_threshold` COMMAND to the ESP32 via UART. The
ESP32 ACKs and updates its in-memory threshold (not persisted to
NVS by default — see "Restoring the threshold" below).

Confirm the dashboard shows the new threshold value.

Equivalent MQTT command if you prefer:

```yaml
service: mqtt.publish
data:
  topic: vroom/spectra/cmd
  payload: '{"cmd": "set_threshold", "value": 12.7}'
```

## Step 3 — Wait for `LOW_V_SUSTAIN_S` to elapse

The controller doesn't trigger on the first low reading — it requires
`LOW_V_SUSTAIN_S` consecutive low samples to filter out noise from
crank events, alternator load steps, etc. Default is `300` seconds
(5 minutes) but check `esp32/src/config.py` for your actual value.

While you wait, watch the dashboard's events feed. You should see
log lines like:

```
[controller] v=12.51 below threshold 12.70 - sustained: 1/300
[controller] v=12.51 below threshold 12.70 - sustained: 2/300
...
```

(Sampling cadence is `WAKE_INTERVAL_S`, default 60s — so the counter
ticks slowly. If you want a faster test, *also* temporarily reduce
`LOW_V_SUSTAIN_S` to e.g. 30 seconds via the same threshold
command — but remember to restore it. Some teams skip this and just
wait the 5 minutes; it's a one-time test.)

Hood stays open the whole time.

## Step 4 — Observe the trigger fire

When the sustain counter hits the threshold:

| Time | Event |
|---|---|
| t=0 | Sustain counter satisfied; `EVENT: low_voltage_trigger` in feed |
| t=0.1 | ESP32 enables Pi MOSFET (if Pi isn't already running) |
| t=0.5 | ESP32 transmits START packet (8 burst repeats) |
| t=~2 | Engine cranks |
| t=~3 | Engine catches, idle stabilizes |
| t=~5 | OBD data flows; dashboard updates |
| t=~5 | `EVENT: engine_started` published |

This is identical to step 15's manual sequence, except you didn't
push the button — the controller decided on its own.

If anything looks wrong, **flip the valet switch** and review.

## Step 5 — Watch the run + stop cycle

Engine runs for `RUN_DURATION_S` (default 900 = 15 minutes — long
enough to actually warm the engine and load the battery enough to
matter; check your config for your value).

During the run:

- Battery voltage should rise from ~12.5 V (pre-start) to ~14.2-14.5 V
  (alternator charging)
- OBD data flows continuously
- ESP32 continues to poll voltage every `WAKE_INTERVAL_S` — but
  doesn't trigger again because it's already in the `running` state
- Dashboard shows the run-timer counting down

For the first auto-test, **do not let it go the full 15 minutes**.
The point of this step is "trigger fires correctly" not "endurance".
After 60-120 seconds of confirmed-running, manually issue Stop from
the dashboard (same as step 15). You'll do the full-duration run
during long-term stability testing in step 18.

If you let it complete the full duration:

| Time (s after run start) | Event |
|---|---|
| `RUN_DURATION_S` | ESP32 transmits Stop (same Start packet, second press) |
| +2 | Engine shuts down |
| +5 | OBD traffic ceases |
| +10 | ESP32 sends `SHUTDOWN` UART message |
| +30 | Pi has finished `shutdown -h now` |
| +30 | ESP32 cuts MOSFET; Pi powers off; display goes black |
| +30 | ESP32 enters `COOLDOWN` state for `COOLDOWN_S` |
| +`COOLDOWN_S` | ESP32 returns to deep-sleep polling |

## Step 6 — Restore the production threshold

This is the easy-to-forget step. **Do not leave the elevated
threshold in place.** If you do, the next time you park overnight,
the system will helpfully run your engine for you at 3 AM.

From the dashboard settings panel:

1. Change `LOW_V_TRIGGER` back to `12.2 V` (or your production value)
2. Apply
3. Confirm the dashboard shows the original threshold

If you also lowered `LOW_V_SUSTAIN_S` in step 3, restore it to its
production value (typically 300 s) too.

If the threshold is stored in NVS (persisted across reboots), confirm
the new value is in NVS too — pull the OBD pigtail for 30 s, plug
it back in, watch the ESP32 boot, and verify `v_battery` threshold
on the dashboard matches production.

If the threshold is NOT in NVS (in-memory only), then a power cycle
already restored it to the `config.py` default — but verify anyway.

## Step 7 — Sanity check: confirm trigger no longer fires

After restoring the threshold, leave the car for `LOW_V_SUSTAIN_S` +
60 s with the hood still open and your eye on the dashboard. The
sustain counter should stay at 0 (current voltage well below the
restored 12.2 V trigger). No trigger fires. Now you've proven both
directions: trigger fires when it should, and stays put when it
shouldn't.

Close the hood.

## What you should have when done

- One successful auto-trigger fire from an elevated threshold
- Engine run + stop cycle completed (truncated to ~60-120 s for the
  first test; full cycle deferred to step 18)
- Pi shutdown + MOSFET cut + return to deep-sleep all confirmed
- Production threshold restored
- Production sustain time restored
- Verified trigger does not re-fire at production threshold

## Where artifacts go

- `logs/YYYY-MM-DD.md` with the time of trigger fire, the elevated
  threshold used, the resting voltage at the time, run duration
  truncation, and confirmation of threshold restore
- A screenshot of the dashboard at the moment of trigger to
  `logs/images/YYYY-MM-DD/first-auto-trigger.png`

## Troubleshooting

| Symptom | Fix |
|---|---|
| Sustain counter doesn't increment | New threshold not applied. Check the dashboard reads back the new value; if it shows the old value, the COMMAND didn't reach the ESP32. |
| Counter increments but trigger never fires at threshold | `LOW_V_SUSTAIN_S` is higher than you think. Check `esp32/src/config.py` and the in-memory value via REPL. |
| Trigger fires but no RF | Same diagnosis as step 15 — CC1101 init, antenna placement. |
| RF fires but engine doesn't crank | Compustar brain refusing start condition (door open, hood open is fine, gear position issue). Check Compustar status via the FOB's LED indicator. |
| Engine runs the full duration when you wanted to truncate | Dashboard Stop button didn't catch the running state. Either the state machine misses the Stop COMMAND while in the `running` state, or the COMMAND whitelist doesn't permit `stop_engine`. Tap Start instead — same packet, same effect (force-quit the run). |
| Auto-trigger fires again immediately after stop | `COOLDOWN_S` is shorter than the time it takes voltage to recover, and the elevated threshold is still in place. Restore the threshold (step 6) before doing anything else. |
| You forgot to restore the threshold and the engine started overnight | This is exactly the failure mode the explicit step 6 exists to prevent. If it happens once: shorten `RUN_DURATION_S` to 60 s temporarily, restore the threshold immediately, and add an alert (see step 17). If it happens twice: rethink your testing process. |

## Safety callouts

- **Restore the threshold before you walk away from the car.**
  This is the single most important step in this doc. An elevated
  threshold left in place will trigger an auto-start the next time
  you park, and depending on where that is (covered garage, busy
  street, friend's driveway) the consequences range from
  embarrassing to dangerous.
- Until you've done this test, you cannot trust the auto-trigger
  path. After this test, you can trust it enough to leave the car
  parked overnight at home — but treat the next month of operation
  as ongoing observation, not finished work.
- Until step 18 confirms long-term stability, **do not park this
  car somewhere the auto-trigger firing would be unsafe** (closed
  garage, valet lot, attended parking).

## Next

[17 — Home Assistant integration](17-home-assistant-integration.md) —
hook the system into Home Assistant via MQTT for notifications and
unattended monitoring.
