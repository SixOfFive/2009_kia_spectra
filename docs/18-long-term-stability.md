# 18 — Long-term stability

## Goal

Confirm the system is actually trustworthy over weeks and seasons,
not just over a single afternoon of testing. Define what "normal"
looks like for parked-current draw, false-trigger rate, MQTT volume,
Pi log growth, and ESP32 watchdog reboots — and define what
"abnormal" looks like so you can catch problems before they strand
the car.

This is a doc about **observing** the system, not changing it. The
hardest part of any always-on automotive build is the slow drift
problems — they don't show up in bench testing, they show up after
three months of cold mornings.

## Prerequisites

- [17 — Home Assistant integration](17-home-assistant-integration.md)
  done; ideally InfluxDB + Grafana too so you have trend data to
  look at, not just current state
- At least 2 weeks of installed operation
- A spare set of eyes on the car at least once a week during the
  observation window (parking-spot inspection, listen for unexpected
  starts, check the dashboard)

## What "normal" looks like

After your first month, you should expect roughly:

| Metric | Normal range | Concerning |
|----|----|----|
| Parked battery V (key out, controller in poll) | 12.4 - 12.7 | < 12.2 |
| Auto-triggers per week (mild weather) | 0 - 1 | 2+ |
| Auto-triggers per week (-20 °C cold snap) | 1 - 3 | 5+ |
| False triggers (engine starts but no real low-V condition) | 0 | any |
| MQTT publishes per day | ~2800 (state every 30s) + few events | 10x baseline |
| `/var/log/vroom/*.log` daily growth | 2-10 MB | >50 MB |
| Pi unscheduled reboots per month | 0 | 1+ |
| ESP32 watchdog reboots per month | 0 - 3 | 5+ |
| ESP32 case temperature, summer | <55 °C | >60 °C |
| ESP32 case temperature, winter | -20 to 25 °C | controller stops booting |
| Run-cycle completion rate | 100% | any failed cycle |

The bottom row is the most important: every auto-trigger should
complete cleanly. If a single cycle ends with the engine still
running after `RUN_DURATION_S + 30 s`, the failure mode is the
worst kind — a parked car that won't turn off — and you need to
investigate immediately.

## Step 1 — Recheck parked-current draw after a month

The bench-time power budget assumed an LM2596 with typical
no-load draw of ~10 mA on the 12 V side. Real LM2596 modules vary
by ±50% depending on the specific PWM controller and inductor.
After a month of installed operation, you should recheck the actual
draw.

Method:

1. Pull the OBD pigtail out of the splitter
2. Place a clamp meter (DC, mA range) around the pin-16 wire of the
   pigtail
3. Re-plug the pigtail
4. Wait `WAKE_INTERVAL_S * 2` (~2 minutes) for the controller to
   stabilize back into deep-sleep polling
5. Record the steady-state current (in deep-sleep polling, not
   active sample)

Expected: 5-15 mA. If you read >30 mA, the LM2596 is dissipating
more than it should — possibly running in pulse-skipping mode at
the wrong duty cycle. Worth swapping for a newer one (or upgrading
to a TPS54331-based buck which has much lower no-load draw).

If you can't get a clamp meter on it, you can estimate from the
battery's slow discharge between auto-triggers — but a direct
measurement is much better.

Cross-reference against [docs/power-budget.md](power-budget.md) and
update that doc if your actual numbers are meaningfully different
from the predicted ones.

## Step 2 — False-trigger rate

A false trigger is the engine starting when battery voltage hadn't
actually crossed the threshold in production conditions. With
`LOW_V_SUSTAIN_S` at the default 300, false triggers should be
**zero** — the sustain logic specifically exists to filter out
transient noise.

If you see a trigger fire when the InfluxDB voltage trend shows
voltage was above threshold the whole time:

- Check `journalctl -u vroom.service` on the Pi for the trigger
  event's full context — what did the ESP32 think the voltage was?
- Likely culprit: ADS1115 noise from CAN switching transients, or
  from the alternator switching loads when the car has just been
  driven. The divider's filter cap might be too small — increase
  the 10 µF tantalum (BOM row 15) to 22 µF or 47 µF.
- Other culprit: the voltage divider's tolerances changed (resistor
  drift with temperature). Recalibrate via the dashboard's calibration
  panel, comparing dashboard V against a multimeter on the OBD pigtail.

Any false trigger ≥ 1 per month is worth fixing. False triggers
that hit at high frequency (multiple per day) waste fuel, wear the
starter, and undermine the entire reason for using a sustain timer.

## Step 3 — MQTT message volume sanity check

```powershell
# from a host with mosquitto-clients installed, run for 24 h
mosquitto_sub -h <broker> -t "vroom/spectra/#" -v | tee mqtt-24h.log
```

Then count:

```powershell
(Get-Content mqtt-24h.log | Measure-Object -Line).Lines
```

Expected:
- 60 * 60 * 24 / `MQTT_PUBLISH_INTERVAL_S` state messages
- Plus a handful of event messages per actual trigger cycle
- For default `MQTT_PUBLISH_INTERVAL_S` = 30 s, that's ~2880 state
  + ~5-10 events per cycle

If the count is dramatically higher (10x+), something is publishing
in a loop. Common cause: a Pi-side bug where the daemon publishes on
every UART receive instead of every interval, and the ESP32 is
streaming faster than expected. Check `pi/app/comms/mqtt_publisher.py`
or wherever your publisher gating lives.

If the count is dramatically lower, MQTT isn't reaching the broker
reliably. Check WiFi signal strength at the parking spot — a phone
on the same WiFi is the easiest field test.

## Step 4 — Pi log rotation

By default Raspberry Pi OS rotates `/var/log/*.log` weekly via
logrotate, but **`/var/log/vroom/` is not in the default config**.
After ~3 weeks you'll have several hundred MB of accumulated log,
and after 6 months you'll fill a 32 GB SD card.

Confirm `/etc/logrotate.d/vroom` exists and includes:

```
/var/log/vroom/*.log {
    daily
    rotate 14
    compress
    delaycompress
    missingok
    notifempty
    copytruncate
}
```

`provision.sh` from step 10 installs this — but verify it's still in
place after any system update. The file gets stomped by accident
sometimes.

Test it manually:

```bash
sudo logrotate -d /etc/logrotate.d/vroom    # dry run
sudo logrotate -f /etc/logrotate.d/vroom    # force run
ls -lh /var/log/vroom/
```

You should see rotated `.gz` files and a fresh current log.

If `vroom.service` is using Python `logging.FileHandler` instead of
`RotatingFileHandler`, the `copytruncate` trick keeps it working —
the file handle keeps writing while logrotate copies + truncates.
Verify by triggering a manual rotation while the daemon is running
and watching that new entries continue to flow.

## Step 5 — ESP32 watchdog reboots

Some ESP32 reboots per month are normal. Sources:

- **Cosmic rays** — a SEU (single-event upset) hits a register, the
  WDT trips, the chip resets. Roughly one event per chip per month
  is expected at sea level (more in the mountains, less at higher
  latitudes — astrophysics). Not actionable.
- **Brownout** — battery dropped low enough that the buck couldn't
  hold 5 V. Visible as a brownout-detector reset in
  `machine.reset_cause()`. After a successful auto-trigger the
  alternator should pull the battery back up — but if the trigger
  itself fails before the engine catches, you can get a brownout
  reset.
- **Heat-induced glitch** — only in extreme summer. Above ~75 °C
  case temp the WROOM-32U starts to misbehave. If you see this,
  relocate the case or add ventilation per [14](14-case-mounting.md)
  step 6.

The ESP32 publishes `reset_cause` on every boot — log this in
InfluxDB / Grafana and watch the distribution. If you're seeing
>5 reboots per month or a sudden cluster of brownouts, that's a
real signal.

The Pi's WDT-style restarts (systemd restarting the daemon on
failure) should be ~0 per month. If you see `vroom.service` restart
events, dig into `journalctl -u vroom.service --since '1 week ago'`
for the underlying exception.

## Step 6 — Winter behavior

A car battery at -20 °C reads ~0.4-0.6 V lower than at +20 °C for
the same state-of-charge. This means a battery that would have
read 12.5 V on a mild day reads 12.0 V in deep cold — and that's
below your 12.2 V `LOW_V_TRIGGER`.

What you'll observe in winter:

- Auto-trigger frequency rises with falling outside temp (this is
  working as intended — the whole point of the build)
- Run durations may need to be longer than your `RUN_DURATION_S`
  default to actually warm the engine and put real charge back
  in the battery — consider raising to 1200 s (20 min) for winter
- Cold-soak voltage may trigger sustain false-positives if the
  threshold isn't temperature-compensated

Possible tweaks:

- **Seasonal threshold**: drop `LOW_V_TRIGGER` to 11.9 V for the
  winter months. The lower reading at the same charge state means
  you don't want to be triggering at "12.2 V cold" because the
  battery is actually fine — just chemically slowed.
- **Temperature-aware threshold**: more involved — read the ESP32's
  internal temp sensor (or, better, add a DS18B20 in the cabin),
  apply ~0.01 V/°C correction.

Document any seasonal tweaks in `logs/YYYY-MM-DD.md` so you remember
to undo them in spring.

## Step 7 — Summer behavior

Hot engine bay, hot under-dash, hot case. The Spectra's under-dash
peaks around 50 °C on a hot Toronto afternoon with the doors closed
and direct sun on the windshield. The ESP32 die can hit 70 °C
ambient + self-heat — still within spec but at the upper end.

What to watch for in summer:

- Case temperature trending up week over week — should plateau by
  August, not keep rising
- Increased brownouts or WDT resets, especially in early afternoon
- LM2596 thermal shutdown (visible as the 5 V rail dropping out and
  ESP32 resetting) — if you see this, add a small heatsink to the
  LM2596 or relocate the case
- Pi's `cpu_thermal` throttling (visible in `vcgencmd measure_temp`
  or via the Home Assistant Pi monitor) — should stay below 70 °C

If summer turns out to be the limiting factor, the recourse is more
ventilation, a different case material (aluminum dissipates much
better than ABS), or relocating off the firewall side.

## Step 8 — Schedule the periodic checkup

Put a recurring calendar event:

- **Weekly**: glance at the Home Assistant dashboard, confirm
  battery voltage trend looks sane, confirm any trigger events
  match what you remember
- **Monthly**: walk the car. Pop the under-dash panel. Confirm
  Velcro hasn't loosened, antennas haven't shifted, no rodent
  damage to cables, no visible corrosion at the OBD pigtail
- **Seasonally** (spring + fall): recheck parked-current draw
  (step 1), reassess `LOW_V_TRIGGER` (steps 6/7), rotate Pi logs
  manually to verify the cron-based rotation is still doing its
  job
- **Annually**: pull the case, inspect each solder joint, recap if
  the buck's electrolytics show bulging tops

## What you should have when done

- Parked-current draw measured at month 1 and recorded in
  `power-budget.md`
- A baseline value for "normal" auto-trigger frequency in your
  climate, recorded in `logs/`
- Verified logrotate is working — rotated `.gz` files visible in
  `/var/log/vroom/`
- A recurring weekly/monthly/seasonal/annual checkup schedule on
  your calendar
- Confidence (or a fix list) for moving on to optional features
  like cellular alerting

## Where artifacts go

- Monthly check-in notes in `logs/YYYY-MM-DD.md`
- Updated parked-current numbers in
  [`docs/power-budget.md`](power-budget.md) (separate edit, not
  this doc) once you have the measured value
- Photos of any physical drift (loose Velcro, shifted antennas,
  corrosion) to `logs/images/YYYY-MM-DD/`

## Troubleshooting

| Symptom | Fix |
|---|---|
| Parked current measures 30+ mA | LM2596 wasted-power problem. Swap for a TPS54331 or MP1584-based module — these idle at <2 mA. |
| Auto-trigger fires daily even in mild weather | Battery has aged out (sulfated, internal resistance up). Time for a new battery — vroom didn't kill it, but it's exposing the underlying problem. |
| MQTT volume 10x expected | Pi-side publisher publishing on every UART message instead of every interval. Bug fix in `pi/app/comms/mqtt_publisher.py`. |
| Log directory growing 100 MB/day | Daemon stuck in a tight error loop. `journalctl -u vroom.service -n 200` will usually show the same exception repeating. |
| ESP32 reboots 10x/month, all brownout | Battery health issue (every cycle dips voltage too far before the alternator catches), or buck input cap is undersized. Add a 100 µF electrolytic in parallel with the 10 µF tantalum. |
| Pi's SD card stops mounting after several months | SD card wear from log writes. Use a higher-endurance card (SanDisk High Endurance, Samsung PRO Endurance) — typical consumer SD cards are not rated for the write load. |
| Trigger frequency drops to zero in summer | Threshold is too low for hot weather — battery sits at 13+ V for hours after a drive. This is fine; the system just isn't being exercised. Run a manual auto-trigger test (step 16) monthly to verify the path still works. |

## Next

[19 — Cellular alerting (optional)](19-cellular-alerting.md) — sketch
of how to extend this build with a cellular modem for off-network
parking.
