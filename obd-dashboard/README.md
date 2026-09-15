# OBD-II log dashboard

A page on the projects VM, **http://192.168.15.3/vroom/**, that graphs the car's OBD-II
log. A puller on the VM checks the board in the car every minute and keeps every row the
board has served. It also keeps the values the log has no column for, such as the VIN and
fault codes. The page reads those files and builds its graphs in the browser, straight
from the CSV.

```
board in the car (esp32-s3/voltage_monitor)
   |  GET /obdjson every minute; /obdlog.csv when the log grew; VIN and codes while running
   v
projects VM: vroom-obd-pull.timer -> /opt/vroom-obd/vroom_obd_pull.py
   |  writes /var/www/html/vroom/data/
   v
Apache (default vhost, /vroom/) -> the page: index.html, chart.js, app.js
```

The page never talks to the car. Opening it, or leaving it open anywhere, costs the board
nothing.

## The page

- **Range:** Last drive, 24 h, 7 days, 30 days, All, or any single drive from the
  *Drive* list. A drive is a run of rows with no silence longer than 5 minutes.
- **Hide time between drives** (on by default) puts the drives in range side by side,
  each as wide as it is long, with a hairline between them. Off, the axis is real time.
- **Hover** a graph for the value and the reading's date and time. The crosshair moves on
  every graph at once. **Drag** across a graph to zoom all of them, and **double-click**
  to zoom back out. A focused graph steps through readings with the arrow keys
  (Shift for 10, Home and End).
- **Graphs** come from whatever numeric columns the CSV has. Values that share a unit and
  a question share an axis: temperatures, fuel trims, and supply voltages. Everything
  else gets its own graph. A column the page does not know still gets a graph, under
  *Other columns*.
- **Latest values** show the newest reading in the range.
- **Vehicle** lists the single values below. **Fault codes & readiness** shows the
  check-engine light from the log, the stored, pending and permanent codes, and the
  readiness monitors.
- **Text values in the log** lists state changes of text columns (the fuel system state)
  in the range.
- **Board & pull** shows whether the board answers, the engine and link state, the last
  download, the log's size on the board and on the VM, and the firmware and uptime.
  From fw 4.80 it also shows the row interval and how long the last sweep took.
- **Table** shows the rows in range, newest first, with every value the graphs draw.
- **Links** can name a view: `#range=7d`, `#range=all&gaps=0`, `#range=drive:3`.
- The page re-reads `data/` every 30 s and redraws only when a file changed.

## Files in `data/`

| File | What |
|---|---|
| `obdlog.csv` | Every row the board has served, merged and de-duplicated, oldest first. It only grows; rows the board has since dropped stay here. One header, in the firmware's column order; columns with no value in any row are left out. |
| `obdlog-board.csv` | The last download, byte for byte. |
| `vehicle.json` | The single values, each with when it was last seen and since when it has been the same. |
| `vehicle-info.txt` | The same values as a plain-text table, for download. |
| `status.json` | The puller's last check: board reachable, engine and link state, log size, download result, archive size, board facts. |

## What the puller asks the board, and when

One run is one check, started by `vroom-obd-pull.timer` every minute.

| Request | When |
|---|---|
| `GET /obdjson` | Every check. It is small, and it never opens the car link. `log.bytes` says whether the log changed. |
| `GET /obdlog.csv` | When `log.bytes` differs from the last download. While the engine runs, at most every 2 minutes. After a failure, backs off to at most every 15 minutes. |
| `GET /obdjson?cat=vehicle`, then `?cat=codes`, then `GET /obdjson` | While the engine runs and the link has been live for 20 s, every 30 minutes. |
| `GET /json` | Every 10 minutes, for firmware, uptime, WiFi signal and auto-start state. |

**Why the category requests look like that.** The board reads VIN, calibration ID, ECU
name and fault codes only while a page asks for that category. Its OBD task polls
whichever category was asked for last, until something asks for another. So the puller
asks the way the Vehicle and Fault codes pages do, waits up to 20 s for each read, and
then asks for the Overview. That hands the task back to its default, even when a request
failed. The log's own 30-second sweep carries on regardless, so no row is lost. The
fault-code read is the same read-only request the board's Fault codes page makes.

**What is kept from `/json`:** only firmware, build, uptime, WiFi signal, battery
reading, clock state, auto-start state and the last run. That answer also carries the
WiFi network's name and the access point's address, and `data/` is served to the LAN,
so those are never written.

### The single values

These are the values the log has no column for: VIN, calibration ID, ECU name, OBD
standard, fuel type, protocol, reader firmware and address, stored, pending and permanent
fault codes, and each readiness monitor. The board forgets them at every reboot. The VM
keeps **the last non-empty value of each, read while the engine was running**:

- An empty or unread value never replaces a kept one.
- A code list the car answered with no codes is a value, `none`. A list it did not
  answer is not.
- *Same value since* resets when a value changes, so a new fault code shows when it
  first appeared.

## Deploy

```bash
obd-dashboard/deploy/deploy.sh            # default target node@192.168.15.3
```

It installs the site to `/var/www/html/vroom/`. `index.html` names each asset with a hash
of its content, so browsers drop stale copies. It also installs:

- the puller to `/opt/vroom-obd/`
- `vroom-obd-pull.service` and `.timer`
- `apache-vroom.conf` as `conf-available/vroom.conf`, which turns off directory listings
  and compresses CSV and JSON

It enables the timer, reloads Apache and runs one check. It never deletes or overwrites
`data/`. The target account needs passwordless sudo.

Settings are environment variables, see `Config` in the puller. For example, if the
board's address changes, put `VROOM_BOARD_URL=http://<address>` in
`/etc/default/vroom-obd`.

## Operating

```bash
systemctl list-timers vroom-obd-pull.timer
sudo journalctl -u vroom-obd-pull.service -n 50      # one line per new rows, failure or capture
cat /var/www/html/vroom/data/status.json
```

A check that finds nothing new logs nothing, so a quiet journal is normal while the car is
away.

## Tests

```bash
python3 obd-dashboard/tests/test_pull.py
```

No board and no network are needed. The fixtures follow the board's real formats: the
30-column header of fw 4.75–4.77, the 26 columns fw 4.78 writes for this car, and the
`/obdjson` answers of fw 4.79. CI runs these tests with the rest.

## To undo

```bash
sudo systemctl disable --now vroom-obd-pull.timer
sudo a2disconf vroom && sudo systemctl reload apache2
sudo rm /etc/systemd/system/vroom-obd-pull.service /etc/systemd/system/vroom-obd-pull.timer \
        /etc/apache2/conf-available/vroom.conf
sudo systemctl daemon-reload
sudo rm -rf /opt/vroom-obd
# /var/www/html/vroom/data holds the only copy of rows the board has since dropped;
# keep it, or copy it off before: sudo rm -rf /var/www/html/vroom
```

The projects VM's landing page gained a *vroom — Kia OBD-II log* card under *Static
sites*. The page before that edit is `/var/www/html/index.html.bak-pre-vroom`.

## Deliberately not done

- **No login.** The page is open to the LAN like the other static sites on the VM. It
  shows the log, the VIN and fault codes. Nothing on it controls anything.
- **The whole CSV is read in the browser.** Since fw 4.80 writes a row every 10 s, that
  is about 50 KB per hour of driving, roughly 20 MB for a year of an hour a day. That
  still reads in about a second. Splitting the archive by month would be the next step
  if it ever does not.
- **Rows without a valid clock are not drawn.** These are rows dated before 2020, written
  before the board had the time. They stay in the CSV, and *Board & pull* counts them.
- **Values from *Read codes & VIN now* with the engine off are not captured.** Single
  values are written only while the engine runs.
