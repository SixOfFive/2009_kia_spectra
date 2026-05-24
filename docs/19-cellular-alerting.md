# 19 — Cellular alerting (optional, sketch only)

## Goal

Sketch how to extend the build with a cellular modem so trigger
events and battery status reach you even when the car is parked
out of WiFi range — long-term airport parking, a friend's driveway,
a campground.

> **Status: not yet applicable to this project.**
>
> The Spectra build described in steps 01-18 lives within WiFi range
> of the user's house. There is no cellular hardware installed, no
> SIM, no carrier account. This doc is included as a sketch for
> readers reproducing the build who want to extend it for off-network
> use, and as a reference for the author if circumstances change.
>
> Nothing in this doc has been bench-tested for this specific
> deployment. Treat the parts list and wiring as a starting point,
> not a known-good configuration.

## When this makes sense

| Scenario | Cellular worth it? |
|---|---|
| Car parked at home most of the time, occasional WiFi-free outings | No — see "Alternative: rely on home WiFi" below |
| Car parked at an airport for a week+ at a time | Yes |
| Car parked at a vacation property out of WiFi range | Yes if the property doesn't have its own internet to hop on |
| Car is a daily driver but garaged in a basement WiFi dead zone | Maybe — try a directional WiFi antenna first |
| You want the system to text you if something goes wrong | Yes if it doesn't already do this via Home Assistant |

The break-even is roughly: a cellular IoT data plan in Canada runs
~$10-15/month for a single-device plan with 100-500 MB/month — and
this build sends <1 MB/month. If you're spending $120-180/year on
data to monitor a remote-start that triggers ~5 times per winter,
that's a fairly expensive feature. Make sure the use case justifies
the cost.

## Hardware options

| Module | Approx. CAD | Notes |
|---|---|---|
| SIM7600G-H (4G LTE Cat-4, global bands) | $80-120 | The default starting point. USB or UART to the Pi. Bands cover most carriers worldwide. |
| EC25-A (4G LTE Cat-4, NA-specific) | $90-110 | More compact, mini-PCIe form factor — needs a mini-PCIe-to-USB carrier board. Common in industrial IoT. |
| SIM7080G (NB-IoT / LTE-M) | $40-60 | Much lower-power than Cat-4 and much cheaper data plans, but slower and not all carriers support LTE-M for IoT in Canada. Best fit if you're sure your carrier supports it. |
| Particle Boron (LTE Cat-M1, prebuilt cloud) | $90-100 | Includes a managed cloud service with first MB/month free. Easiest path if you don't want to manage the modem at the AT-command level. |

For a no-frills first cellular extension on this build, **SIM7600G-H
over USB to the Pi** is the path of least resistance. Linux sees it
as `wwan0` after `ModemManager` (already in Raspberry Pi OS) detects
it; you `nmcli connection add` with the APN and it Just Works.

## Mounting

Cellular modems pull peak ~2 A at TX during cell registration and
need 4G antennas with reasonable line-of-sight to the cell tower.
That has implications for placement:

- **Inside the existing under-dash case**: tight fit; works if the
  case has internal volume to spare. Antenna goes outside the case
  via SMA pigtail, ideally up near the rear-view mirror (same place
  factory telematics mount their cellular antennas).
- **Separate case bolted near the cellular antenna**: cleaner RF but
  more wiring to route. UART or USB to the main case under the
  dash.

The 2 A peak rules out powering the modem off the same 5 V buck
output as the Pi and display — too close to the 5 A rail's ceiling
during simultaneous Pi-boot + modem-registration. **Add a second
buck** dedicated to the modem, sized 2.5 A or higher, fed off the
same fused +12 V as the main buck.

The 4G antenna placement matters more than the 433 MHz antenna in
step 14. Get it as high in the cabin as you reasonably can, away
from metal, ideally with two ports (LTE Cat-4 uses 2x2 MIMO and a
single-antenna installation drops you to single-stream throughput).

## Software integration: MQTT bridge to cellular

The cleanest approach is to **keep the existing MQTT broker** on
your home network and have the Pi bridge to it via the cellular
link when WiFi is unreachable. Two ways:

1. **Two MQTT clients on the Pi** — one targets `broker.local` over
   WiFi (priority), one targets `broker.public.example.com` over
   cellular (fallback). The Pi-side publisher picks whichever is
   connected. Pro: simple. Con: your broker needs to be reachable
   from the internet (port-forward + auth + TLS).
2. **A managed MQTT cloud** — HiveMQ Cloud (free tier 100 messages
   per second, way more than vroom uses), AWS IoT Core, Adafruit IO.
   The Pi always publishes there; Home Assistant subscribes to the
   same cloud broker. Pro: no port-forwarding from home. Con: an
   external dependency in the loop.

For most readers reproducing this build, **option 2 with HiveMQ
Cloud's free tier** is the right starting point. Same MQTT topics,
just a different broker hostname; the existing Pi-side publisher
code in `pi/app/comms/mqtt_publisher.py` doesn't change. Home
Assistant's MQTT integration handles the broker change without
changing any of the entity declarations from
[17](17-home-assistant-integration.md).

## Cost considerations

Rough annual cost for adding cellular to this build (Canada, 2026):

| Line item | One-time | Monthly |
|---|---|---|
| SIM7600G-H module + antennas | $130 | — |
| Second buck converter + fuse + TVS | $15 | — |
| Carrier SIM activation | $5-15 | — |
| Data plan (IoT, 100 MB) | — | $10 |
| HiveMQ Cloud broker | — | $0 (free tier) |
| **Total year 1** | **$155** | **$120** |
| **Total subsequent years** | — | **$120** |

For a build that triggers 5-20 times a winter, **$120 + $0/year**
is hard to justify unless you have a specific need (airport, remote
property, regulatory documentation, etc.).

Lower-cost alternatives below.

## Alternative: rely on home WiFi + daily summary email

If you don't strictly need real-time alerting when the car is away,
have the Pi send a daily summary email when it *is* on home WiFi.
Catches "anything weird in the last 24 hours" without any cellular
hardware.

Simple cron job on the Pi running a Python script with `smtplib` or
the SendGrid free tier. Sample script:

```python
# /opt/vroom/pi/scripts/daily_summary.py
import json, smtplib, datetime, pathlib
from email.message import EmailMessage

events_today = []
log_path = pathlib.Path("/var/log/vroom/events.log")
today = datetime.date.today().isoformat()
for line in log_path.read_text().splitlines():
    try:
        ev = json.loads(line)
        if ev["ts"].startswith(today):
            events_today.append(ev)
    except Exception:
        continue

msg = EmailMessage()
msg["Subject"] = f"Spectra daily summary {today}"
msg["From"] = "spectra@your.domain"
msg["To"] = "you@example.com"
msg.set_content(f"Events today: {len(events_today)}\n\n" +
                "\n".join(json.dumps(e) for e in events_today))

with smtplib.SMTP("smtp.example.com", 587) as s:
    s.starttls()
    s.login("user", "pass")
    s.send_message(msg)
```

Schedule via crontab:

```bash
sudo -u pi crontab -e
# add:
30 23 * * * /usr/bin/python3 /opt/vroom/pi/scripts/daily_summary.py
```

Failure mode: if the car is out of WiFi range, no email goes out
that day. After 2 missed days you'll probably notice on your own.
For the cost (zero hardware, zero monthly), this is the right
default for most readers.

## Alternative: SMS via a USB cellular dongle, low-rate

A used 3G/4G USB stick (Huawei E3372 / E8372 etc.) on a prepaid SIM
gives you a $0/month "send 1 SMS per event" channel — most prepaid
SIMs in Canada keep their number active for 90 days after the last
activity, and a single MQTT publish per week counts as activity.
Total cost: ~$30 USB stick + ~$50/year prepaid top-up.

Not as polished as the full SIM7600 + IoT plan path but it's a
real option for the "I just want to know if anything weird
happened" use case.

## What this doc deliberately doesn't cover

- **Carrier-specific APN settings** — every carrier has different
  APN/auth/protocol requirements; consult the carrier's IoT
  onboarding docs
- **GPS tracking** — the SIM7600G-H has GPS but adding live-tracking
  to a remote-start system has different threat-model implications
  (theft tracking, surveillance) and warrants its own design
  discussion
- **Two-way SMS for emergency stop** — possible (modem SMS → MQTT →
  controller) but the COMMAND whitelist on the Pi side already
  handles this via MQTT; SMS is just another transport
- **Backup cellular for the Home Assistant side** rather than the
  vehicle side — different problem, different doc

## What you should have when done

- (If you went with the SIM7600 path) Cellular link confirmed
  registering on the carrier, MQTT publishes appearing on the
  broker over cellular, fallback logic verified by toggling WiFi
  off and watching the publisher route to cellular
- (If you went with daily-summary email) Daily summary arriving in
  your inbox at the scheduled time, including event records for
  the day
- (If you chose neither) An explicit decision documented in
  `logs/YYYY-MM-DD.md` that you've considered cellular and decided
  the home-WiFi setup is sufficient for your use case

## Where artifacts go

- Cellular modem build photos to
  `logs/images/YYYY-MM-DD/cellular-install-*.jpg`
- APN configuration, SIM IMEI/ICCID, broker URL → **not** in any
  committed file; treat them the same as `secrets.py`. Local file
  in your `.gitignore` only.
- Updated BOM if you went with the cellular path — add to a new
  optional section in [docs/BOM.md](BOM.md) so other readers can
  see the upgrade option

## Troubleshooting

| Symptom | Fix |
|---|---|
| Modem registers but no data | APN wrong, or carrier requires PDP context activation beyond the default. Check `mmcli -m 0 --simple-status` for `state: connected` vs `registered`. |
| MQTT works on WiFi, fails on cellular | The cellular carrier blocks port 1883. Use TLS-encrypted MQTT on port 8883, which is universally allowed. |
| Modem hot-swaps fine on the bench but doesn't reconnect after engine cycle | Cellular registration can take 30-60 s after a cold boot. Add a startup delay to the publisher or let it queue messages until the link is ready. |
| Data bill spikes | A bug somewhere is publishing way more than expected, or the modem firmware is running periodic large keepalives. Set a carrier-side data cap as a safety net. |
| Modem draws too much during registration and resets the ESP32 | Second buck is undersized or shared too closely with the main 5 V rail. The whole point of the separate buck in this design is to keep cellular's brownout from crashing the rest of the system. |

## Next

There is no step 20 in this build. If you've made it through 19,
the system is installed, monitored, and either cellular-extended or
explicitly scoped to home-WiFi-only.

Future work — note as separate projects rather than additional
reproduction steps:

- GPS tracking + theft alerting
- Two-way OBD command (e.g. remote DTC read) for diagnostic
  support across a distance
- Companion mobile app instead of the Home Assistant frontend
- Multi-vehicle support — same controller, multiple FOB pattern
  sets, one MQTT topic per vehicle
