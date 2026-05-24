# 15 — First live trigger (manual, parked, hood open)

## Goal

First time you push the Start button on the dashboard from the
fully-installed in-car system and watch the engine crank. The bench
already proved every individual piece works — this step proves they
still work after vibration, heat, ground noise, and the real RF
environment of a car parked next to its own brain.

Treat this the same way you'd treat a first powered test of any
remote-actuated mechanical system: every safety guard in place, the
expectation that something will go wrong, and a clear plan for what
to do when it does.

## Prerequisites

- [12 — Compustar bench validation](12-bench-validation.md) passed
  end-to-end including a successful Lock cycle in the car
- [13 — OBD-II install](13-obd2-install.md) wiring complete, fused,
  TVS-protected
- [14 — Case mounting](14-case-mounting.md) done, antennas placed,
  display mounted, dry-run of full removability passed
- Compustar valet switch located and accessible from the driver's
  seat (you should be able to flip it without unbuckling)
- Your real Compustar FOB **on your person** — fallback if the
  dashboard or RF path fails
- The car's actual key in your hand (not in the ignition)
- Hood open. Yes, the whole time. You want visual confirmation the
  engine started and an immediate eyeball on belt/coolant/oil signs.

## Safety checklist (read out loud before you press anything)

- [ ] Car in **Park** (or Neutral with hand brake fully set, manual
      transmission)
- [ ] Key **out of the ignition**, on your person
- [ ] Hood **open** and propped
- [ ] Parked in a safe location — driveway / quiet lot, not a
      street, not a garage (CO risk)
- [ ] At least 1 m of clearance in front of the car (in case the
      Compustar decides the gear position is wrong and the car
      lurches — it shouldn't, but the bench is not the car)
- [ ] Compustar valet switch reachable
- [ ] Real FOB in your pocket
- [ ] Fire extinguisher within reach if you have one. Optional but
      good practice for any first-fire automotive test.
- [ ] Phone charged with someone-who-knows-where-you-are notified
- [ ] Doors locked or unlocked per the Compustar protocol you
      validated in step 12. The Spectra's brain refuses to start
      if a door is open — if the dashboard reports start-rejected,
      verify all doors closed first.
- [ ] No people, pets, or obstacles in front of or under the car
- [ ] You're not in a hurry. If you're rushed, do this another day.

If any box isn't checked, stop and fix it before continuing.

## Step 1 — Power the system from the OBD pigtail

With the system installed, you don't need to do anything special —
plug-in happened in step 13 and the controller has been running on
parked-state polling since then. Just confirm:

- Display is showing the dashboard (chromium kiosk came up after the
  last reboot)
- Battery gauge reads ~12.4-12.7 V (key out, healthy battery)
- WiFi indicator on the dashboard shows connected
- Last-update timestamp is within the last `WAKE_INTERVAL_S` seconds

If any of those is off, fix that first before triggering anything.

## Step 2 — Walk around the car

Visual inspection of the install from the outside:

- Display visible through the windshield from the driver's seat?
- No cables hanging visibly through the dash gap?
- Antennas not sticking up where they'll catch on a sun visor?

Walk around to the front, hood up:

- Battery terminals tight
- No new wiring you didn't put there (sanity check — the install
  was supposed to be 100% under-dash; if you see anything in the
  engine bay you don't recognize, stop)

## Step 3 — Press Start on the dashboard

From the driver's seat, with the hood open and a clear view of the
engine bay, tap the **Start** button on the touchscreen.

What you should see and hear, in order, within ~5 seconds:

| Time (s) | What | Where to look |
|---|---|---|
| 0.0 | Button highlights, dashboard logs "command: start_engine" | Display |
| 0.1 | UART forwards COMMAND to ESP32 | (Pi journal — invisible at the moment but logged) |
| 0.2 | ESP32 ACK appears in dashboard events feed | Display |
| 0.3 | ESP32 transmits the START packet (8 burst repeats over ~1.5 s) | (Engine bay quiet — no audio of the RF burst) |
| ~2.0 | Compustar brain pulls the starter relay | Audible click + starter motor engage |
| ~2.5 | Engine cranks | Engine bay |
| ~3.0 | Engine catches and idle stabilizes | Engine bay + tachometer if visible |
| ~5.0 | OBD CAN traffic starts; dashboard RPM gauge moves off 0 | Display |
| ~5.5 | MQTT publishes `engine_started` event (if MQTT configured) | (Invisible at the moment unless you're watching Home Assistant) |

If the engine catches and runs steadily within ~5 seconds, **the
in-car install is working**. Stand and watch for at least 60 s.
Don't touch anything. Just verify it keeps running.

## Step 4 — What to verify while it's running

- **Crank duration**: should be 1-2 seconds. The Compustar brain
  will keep cranking up to ~4 seconds before it gives up. Anything
  past 2 s suggests a low battery, fueling issue, or — possibility
  worth checking — that the OBD-II install is loading the battery
  enough that cranking is harder than baseline (it shouldn't be, but
  measure).
- **Idle quality**: should be steady at whatever the Spectra
  normally idles at (~750 RPM cold, ~650 warm). If it surges or
  hunts, something is wrong with the car, not the install — but
  stop and look at it anyway.
- **OBD data flow**: dashboard RPM, coolant temp, MAF should all
  start updating within ~5 s of the engine catching. If RPM is the
  only one that updates, the CAN wiring is fine but the OBD PIDs in
  the Pi-side decoder might be configured for the wrong protocol.
- **MQTT event**: if `MQTT_BROKER` is set in Pi `secrets.py`, the
  `engine_started` event should land on your broker within ~5 s.
  Verify from another device (laptop with `mosquitto_sub`, phone
  with MQTT Explorer, etc.) — *before* you trust this for unattended
  use.
- **No new fault codes**: after ~30 s of idle, grab a scan tool
  through the Y-splitter's free port and confirm no new DTCs.

## Step 5 — Stop the engine

Three ways to stop, in order of preference:

**Preferred — dashboard Stop button:**

Tap **Stop** on the dashboard. The ESP32 transmits the same START
packet again — for the 1-way Compustar protocol, the second press
of Start while the engine is running is interpreted as Stop. Engine
should shut down within ~2 seconds.

**Backup — re-tap Start:**

If for some reason the dashboard's Stop button doesn't fire (UART
glitch, command-whitelist mismatch), tap Start again — same RF
packet, same effect.

**Emergency — Compustar valet switch:**

Flip the valet switch. This cuts the Compustar brain's authority
over the starter and ignition, killing the engine immediately. Use
this if:

- Engine is running rough enough to suggest a real fault
- Engine has been running longer than `RUN_DURATION_S` and the auto-
  stop didn't fire
- You see smoke, hear noise, smell fuel — anything at all unusual
- You're not sure what's happening

The valet switch is the airbag-pyrotechnic-charge equivalent for
this build: if you ever need to ask "should I flip it?", the answer
is yes.

## Step 6 — Confirm clean shutdown

After Stop, watch for:

- RPM drops to 0 on the dashboard
- Engine quiet (visual + audible)
- After ~10 s, OBD traffic stops (PCM goes back to sleep)
- Dashboard reports `engine_stopped` event
- (If MQTT configured) `engine_stopped` event on the broker

The ESP32 stays in active-monitoring mode for `COOLDOWN_S` after the
stop event, so the dashboard remains live for a few minutes. After
that the ESP32 returns to deep-sleep polling and the Pi powers down
via the MOSFET — display goes black. **That is the expected behavior**,
not a fault. The Pi will come back up the next time the ESP32
triggers (manually or automatically).

## Abort conditions (any one of these → flip the valet switch immediately)

- Crank duration > 3 seconds with no catch
- Engine catches but stalls within first 5 seconds
- Idle hunts wildly (>200 RPM oscillation)
- Visible smoke from any part of the engine bay
- Audible knock, grinding, belt squeal that wasn't there before
- Dashboard shows OBD fault codes
- Smell of fuel or coolant
- ESP32 keeps re-transmitting Start packets (visible as repeated
  "tx" events in the dashboard feed)
- Anything you can't explain

After any abort, plug a scan tool into the Y-splitter's free port
and check DTCs before the next attempt. Resolve whatever the car is
complaining about *before* you trust the auto-trigger path.

## Step 7 — Repeat the cycle 5+ times

One successful start/stop is not enough to trust the install. Do at
least five start → idle 60 s → stop cycles, with at least 60 s of
key-out cooldown between cycles. Verify every time:

- Crank duration consistent (1-2 s)
- Idle stabilizes within 5 s
- OBD data flows on every cycle
- MQTT events publish on every cycle
- No new DTCs at the end

If any of the five cycles is flaky, **do not move on to the auto-
trigger test in step 16**. Diagnose first. Common causes after a
clean bench validation:

- RF range marginal in the car (re-aim the 433 MHz antenna)
- Ground noise causing ADS1115 false-low readings (improve ground
  bonding inside the case)
- UART glitches from the ESP32 to Pi when the alternator is loading
  (add a 0.1 µF cap to the UART RX line if scope shows ringing)

## What you should have when done

- 5+ consecutive successful manual Start → 60s idle → Stop cycles
- All OBD data flowing during the run cycle
- All MQTT events publishing within ~5 s of the corresponding
  physical event
- No new DTCs after any of the cycles
- Cooldown + Pi shutdown sequence completes cleanly
- Valet switch confirmed functional as the emergency abort

## Where artifacts go

- `logs/YYYY-MM-DD.md` with timestamps for each of the 5 cycles,
  any anomalies, and any tweaks you made between cycles
- Short video of the first successful cycle (start to stop) to
  `logs/images/YYYY-MM-DD/first-trigger-*.mp4` — handy for the
  inevitable "wait, did this actually work?" moment a month later
- Any tweaks to RF antenna placement noted in the daily log

## Troubleshooting

| Symptom | Fix |
|---|---|
| Cranks but doesn't catch | Cold engine + low battery, or a real fueling issue. Check the basics with the manual key first. |
| Catches then stalls within 2 s | Compustar's anti-theft "tachsense" disengaged too early. This is a Compustar install issue, not the vroom build — consult Compustar setup. |
| RF burst fires but nothing happens | Range or antenna issue. Re-aim 433 MHz vertical; check the brain's antenna is intact and not crushed during your install. |
| Dashboard says ACK but no RF on the SDR | CC1101 not in TX mode. Check `radio.init_433mhz_ook()` was called on boot — `main.py` should do this on every wake. |
| `engine_started` event doesn't publish to MQTT | Broker credentials in Pi `secrets.py` are stale, or the Pi can't reach the broker over WiFi from the car's location. Check from a phone on the same WiFi. |
| Stop button does nothing while running | The COMMAND whitelist in `secrets.py` doesn't include `stop_engine`. Tap Start again — same packet, same effect. |
| Engine shuts down but Pi doesn't power off | ESP32 didn't send the SHUTDOWN UART message, or Pi systemd service didn't catch it. Pi will eventually go down on MOSFET cut (after `COOLDOWN_S`), but cleaner if SHUTDOWN works. |

## Next

[16 — First voltage-triggered auto-start](16-first-auto-start.md) —
test the actual point of the project: the controller fires the
engine on its own when battery voltage drops below threshold.
