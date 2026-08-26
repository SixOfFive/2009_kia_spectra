# Monitoring voltage with an ESP32-S3 (12 V battery monitor)

Measure a DC voltage (a car/12 V battery, a bench supply, anything 0–18 V) with an
ESP32-S3-N16R8, and read it from your PC/phone over WiFi as JSON.

- **Firmware:** [`voltage_monitor/voltage_monitor.ino`](../voltage_monitor/voltage_monitor.ino)
- **Python client:** [`python/voltage_client.py`](../python/voltage_client.py)
- **Board:** ESP32-S3-N16R8 (CH343 UART port). See `[[ESP32-S3-N16R8 dev board reference]]` in the vault.
- **Canonical build/flash instructions:** [`esp32-s3/README.md`](../README.md) — the commands below reflect the original sibling-project layout.

---

## 1. How it works

The ESP32 ADC only reads **0–3.3 V**. A resistor **voltage divider** scales the
higher source voltage down into that window; firmware multiplies it back up.

```
V_adc = V_source x R2 / (R1 + R2)
V_source = V_adc x (R1 + R2) / R2 = V_adc x DIVIDER
```

With **R1 = 1 MΩ, R2 = 220 kΩ**:

```
DIVIDER = (1,000,000 + 220,000) / 220,000 = 5.545
max readable = 3.3 V x 5.545 = ~18.3 V   (covers a 12 V system incl. ~14.8 V charging)
divider current @ 14 V = 14 / 1,220,000 = ~11 uA   (negligible drain)
```

The high resistance is deliberate: low current draw, **and** it limits fault current
(a 40 V spike pushes only 40 µA into the pin). The **100 nF cap** across R2 gives the
ADC a low-impedance source to sample and filters electrical noise — with big resistors
it is required, not optional.

---

## 2. Parts

| Part | Value | Role |
|------|-------|------|
| R1 | 1 MΩ | divider top (source → node) |
| R2 | 220 kΩ | divider bottom (node → GND) |
| C1 | 100 nF ceramic | across R2 (node → GND), filtering |
| ESP32-S3-N16R8 | — | the brain |
| Buck converter | set to **5.0 V** | powers the board from 12 V (car install only) |
| Inline fuse | 100–500 mA | **car install only** — fire safety on the 12 V tap |

Optional automotive hardening: a TVS diode across the 12 V input (load-dump), a
1N4148 in series for reverse-polarity protection. Not needed for the bench test.

---

## 3. Wiring — what connects where

`node` = the junction between R1, R2, C1, and the ADC pin.

### A) Bench test first (no car, no buck) — recommended

Feed the divider from the board's **own 5 V pin** so you have a known voltage to
calibrate against. Everything shares the board's ground.

```
  ESP32 5V pin ──[ R1 = 1 MΩ ]──┬───────────────► ESP32 GPIO1
                                │
                          [ R2 = 220 kΩ ]   ║ C1 = 100 nF   (R2 ∥ C1)
                                │           ║
  ESP32 GND ────────────────────┴───────────╨──► ESP32 GND
```

Expected: 5 V → node ≈ 0.90 V → firmware reports ≈ **5.0 V**. (Feeding 3V3 instead
reads ≈ 3.3 V.) This proves the math and lets you calibrate (section 6).

### B) Car / 12 V install (after the bench test passes)

```
  12V (+) ──[FUSE]──┬──[ R1 = 1 MΩ ]──┬───────────► ESP32 GPIO1
                    │                 │
                    │           [ R2 = 220 kΩ ] ║ C1 = 100 nF
                    │                 │          ║
                    ├── buck IN+      │          ║
                    │                 │          ║
  12V (−)/chassis ──┴── buck IN- ─────┴──────────╨──► ESP32 GND
                         buck OUT 5V ───────────────► ESP32 5V/VIN pin
                         buck OUT GND ──────────────► ESP32 GND
```

**Critical rules**
1. **Common ground** — vehicle chassis/(−), buck (−), divider R2, and ESP32 GND must
   all be the same node, or the reading is meaningless.
2. **Set the buck to 5.0 V *before* connecting it to the board** — power it from 12 V,
   turn the trim pot while measuring the output with a multimeter, *then* hook it to
   the ESP32's 5V/VIN pin. Sending 12 V into the 5V pin destroys the board.
3. **Fuse the 12 V tap.** Always, when connecting to a battery.

---

## 4. Flash the firmware

```powershell
# Prereq: arduino-cli installed with the esp32 core + libraries (see esp32-s3/README.md).
# Run from the repo root; the sketch is esp32-s3/voltage_monitor.
$dir = ".\esp32-s3\voltage_monitor"
arduino-cli compile --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=custom" -u -p COM6 $dir
# (If arduino-cli uses a non-default core/library location, add: --config-file <your-arduino-cli.yaml>)
```

On boot the serial monitor (115200) prints the assigned IP, e.g. `WiFi OK. IP = 192.168.x.x`.

> **Partition scheme:** use `PartitionScheme=custom` — it reads `partitions.csv` in the sketch folder: **two 3 MB OTA app slots + ~9.9 MB LittleFS**. This first flash must be over **USB**; every update after that goes over WiFi (next section).
>
> ⚠️ **Do not "fix" this to `app3M_fat9M_16MB`.** The two schemes have identical
> app geometry, so a build made with either one will OTA fine — which makes the
> difference invisible until it isn't. But `partitions.csv` names the data
> partition **`spiffs`**, which is the label `LittleFS.begin()` mounts, while
> `app3M_fat9M_16MB` names it `ffat` (subtype `fat`). Flashing that one over
> **USB** rewrites the partition table and LittleFS then fails to mount on the
> next boot — losing the event log, start history and drain buckets, with no
> error on the dashboard. (OTA never rewrites the table, so it cannot cause this.)

WiFi credentials live in `voltage_monitor/secrets.h` (gitignored — copy
`secrets.h.example` and fill in):
```cpp
#define SECRET_WIFI_SSID "your-2.4ghz-ssid"
#define SECRET_WIFI_PASS "your-password"
```
> The repo only ships `secrets.h.example`; the real `secrets.h` is
> gitignored so the password never reaches the public remote. ESP32 is
> **2.4 GHz only** — the SSID must be a 2.4 GHz network.

---

## 4b. Updating firmware over WiFi (OTA)

After the one USB bootstrap flash, **all future updates go over the network — no cable:**

- **Browser:** open `http://esp32-volt.local/update` (or the IP), pick the compiled
  `.bin`, click flash. The board writes it to the spare app slot and reboots into it.
  (There's also an **update** link in the dashboard footer.)
- **Command line:** compile to a `.bin`, then POST it:
  ```powershell
  & $cli compile --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=custom" --output-dir build\ota esp32-s3\voltage_monitor --config-file $cfg
  curl.exe -F "firmware=@build\ota\voltage_monitor.ino.bin" http://esp32-volt.local/update
  ```

Bump `FW_VERSION` in the sketch to confirm an update actually landed — it shows in
`/json` and the dashboard footer.

> **Security:** `/update` is open — anyone on the network can flash the board. That's
> acceptable on an isolated IoT VLAN, but add HTTP auth or a token before exposing it
> more widely.

## 5. Read the values over WiFi (Python)

The board serves a live **dashboard** at `http://<host>/`, machine-readable current JSON at `http://<host>/json`, and the **full 24 h recorded history as CSV** at `http://<host>/history`:
```json
{"vbatt":12.34,"temp_c":38.0,"adc_mv":901,"divider":5.545,"cal":1.0,"rssi":-52,"uptime_s":123,
 "heap_free":258352,"heap_total":356160,"psram_free":8299392,"psram_total":8388608,
 "disk_used":32768,"disk_total":10235904,"mode":"sta","ip":"esp32-volt.local",
 "interval_s":60,"samples":38,"led":"green","fw":"1.1"}
```

Poll it from your PC (stdlib only, no pip install):
```powershell
python esp32-s3/python/voltage_client.py esp32-volt.local
# or use the IP printed on serial:
python ...\voltage_client.py 192.168.x.x
# five readings then stop:
python ...\voltage_client.py 192.168.x.x 5
```

`esp32-volt.local` works via mDNS if your PC resolves it; otherwise use the IP (find it
on the serial monitor or in your router's client list). One-liner test:
```powershell
python -c "import urllib.request;print(urllib.request.urlopen('http://192.168.x.x/json').read().decode())"
```

---

## 5b. Dashboard + offline Access-Point fallback

Open `http://esp32-volt.local/` (or the IP) in a browser for a live dashboard: big
**voltage** and **chip-temperature** readouts, **seven 24-hour graphs** (voltage,
temperature, free memory, disk used, network in, network out, WiFi RSSI), and status cards.
Live values poll `/json` every 2 s; the **graphs are served by the ESP32** (`GET
/history`, CSV) — so a refresh or an AP-mode visit shows the real recorded history,
not a graph that restarts in the browser.

History is a **24 h ring buffer (1440 samples @ 60 s) in PSRAM**, snapshotted to the
filesystem every 10 min and reloaded on boot — it survives reboots and power cycles.

**If the board can't join `IoT`** (e.g. the vehicle is away from home) — at boot after
the ~20 s join attempt, **or after 5 continuous minutes of a dropped connection** — it
falls back to **Access-Point mode**, creating its own WiFi network as the router + DHCP
server:

- SSID **`ESP32-Volt`**, password **`esp32volt`** (change `AP_PASS` in the sketch; `""` = open).
- Connect a phone to it, then browse **http://192.168.4.1/**. Captive-portal DNS means
  almost any address you type lands on the dashboard.
- The footer shows the current mode (`STA` on home WiFi, `AP` in fallback) and the IP.
- It stays in AP mode until rebooted; power-cycle near home WiFi to rejoin. (Auto
  retry-and-rejoin is a small future addition.)

## 6. Calibration (do this once)

Real resistors are ±1–5 %, so trim the reading in software:
1. Bench-wire the divider fed from the 5 V pin (section 3A).
2. Measure that 5 V pin with a multimeter — note the true value, e.g. `5.02 V`.
3. Run the Python client; note what the board reports, e.g. `4.88 V`.
4. Set `CAL = true / reported = 5.02 / 4.88 = 1.0287` in the sketch, re-flash.
5. Re-check: it should now match the multimeter. Done.

---

## 7. Battery-life expectations (small ~45 Ah car battery)

Drain is dominated by the **board + buck**, not the divider (~11 µA, negligible).
Self-discharge of the battery itself is ~3 mA-equivalent (~5 %/month).

| Mode | Board draw | Battery drain | To 50 % (won't crank) | To flat |
|------|-----------|---------------|----------------------|---------|
| Always-on + WiFi | ~120 mA | ~59 mA | **~16 days** | ~32 days |
| Always-on, no radio | ~45 mA | ~25 mA | ~37 days | ~75 days |
| Deep-sleep duty-cycled | ~8 mA | ~9 mA | **~3.4 months** | ~7 months |

The dev board's always-on AMS1117 regulator + LEDs set a ~8 mA floor that limits the
deep-sleep mode; a bare module would sleep at µA, at which point battery self-discharge
dominates anyway (~months). For a parked car, deep-sleep duty cycling is the move;
always-on WiFi will flatten a small battery in ~2 weeks.

> **Measured on the installed car, August 2026.** The table above is bench
> estimation for the board alone. In the vehicle the *total* parked draw —
> car plus board — measures **~280 mA**, i.e. **12.5 % of the battery per
> day**, flat in roughly 8 days. The board is a small part of that; the car
> has an unlocated parasitic fault. Two independent fits agree (24 h
> least-squares **-6.3 mV/h**, long-term anchored **-5.5 mV/h**).
> Full measurements, the healthy-charging profile, and why percent-per-day
> is a better number than mV/h: [docs/power-budget.md](../../docs/power-budget.md)
> section 7.

### How the firmware samples — needed to read the graphs correctly

Three different clocks, and the graph shows the fastest one at the slowest rate:

| What | Rate |
|---|---|
| ADC read (64 conversions averaged) | every **250 ms** |
| Auto-start / engine-edge evaluation | every **1 s** |
| History sample written to the graph | every **60 s** |

`recordSample()` stores **the latest single 250 ms reading, not a per-minute
average** — so the voltage graph is a **1-in-240 snapshot**. A lone spike on it
is a real reading, but it is *not* evidence of a sustained condition, and the
control logic deliberately ignores excursions the graph faithfully records.
Engine on/off is detected at **≥13.2 V held 5 s** and **<13.10 V held 120 s**,
both timestamped at the moment the edge first appeared rather than when it was
confirmed.

---

## 7b. The run log — every engine start and stop

Separate from the event log, and kept in its own section at the bottom of the
**Logs** tab. It answers the question the event log cannot: *how long does this
car actually sit between runs, and which of those runs did the board fire?*
Events are 16 bytes each in two rotating generations of 2000, so it holds
roughly 4000 events — years at the handful a day this car produces.

Raw CSV is at **`/runs?n=<count>`**:

```
ts,kind,src,flags,v,dur_s
1787163535,1,2,0,14.13,0
1787165896,2,2,64,12.88,2361
```

| Field | Values |
|---|---|
| `kind` | 0 = start command sent, 1 = **engine ON**, 2 = **engine OFF**, 3 = start drew no charge |
| `src` | 0 = board (auto), 1 = board (manual), 2 = key or FOB |
| `flags` | `0x01` RF burst accepted · `0x40` recovered · `0x80` reconstructed |
| `dur_s` | on an OFF, how long the engine ran |

**`src` is the useful column.** Engine on/off is detected from alternator
voltage, so it catches runs the board had nothing to do with — the log is a
complete account of the car, not just of this project.

### Detection, and why it needs two kinds of hysteresis

On at **≥13.2 V held 5 s**, off at **<13.10 V held 120 s**, and each edge is
timestamped **when it first appeared**, not when it was confirmed, so durations
and the gaps between runs stay exact.

Voltage alone is not enough. A real alternator dips below any sensible
threshold at idle and under load — one continuous 96-minute drive was once
logged as **fourteen separate runs**, some lasting 1–2 seconds. Only the time
requirement separates a dip from a shutdown.

The off threshold is not just "a bit below on", either. A battery that has just
been driven holds **surface charge** and can sit at 12.9–13.0 V for over half an
hour, so a threshold at 12.90 V never fires at all. See
[power-budget.md](../../docs/power-budget.md) section 7.

### The two badges — never trust a number that carries one

| Badge | Means | Trust the duration? |
|---|---|---|
| *(none)* | recorded live, both edges measured | yes |
| **reconstructed** | hand-derived from other evidence when the log was first created | it is the best available account, not a measurement |
| **recovered** | the board rebooted while this run was open; the end time was rebuilt from the voltage history | approximately — good to about a minute in the normal case |

### What happens if the board reboots mid-drive

Nothing is lost. On boot the firmware looks for a run left open:

- **Still charging** → it adopts the open run, so you get one correct run rather
  than two fragments.
- **Clearly stopped** → it closes it, and *recovers* the end time from the
  sample ring, which is restored from flash and still holds the samples from
  before the reset. Marked **recovered**.
- **In between** → it waits rather than guessing.

The recovery deliberately requires **three consecutive** samples above the
threshold before it believes the engine was running at that moment. One sample
is not enough: the graph is a 1-in-240 snapshot (section 7), so it catches brief
excursions that were never the alternator. On a real case, the naive rule dated
a shutdown 7 minutes late and the three-sample rule got it right to within a
minute.

An open run older than **12 hours** is never adopted — that is a leftover of an
older firmware bug, not a drive still in progress.

---

## 7c. Debug tab &mdash; the BLE scanner

`/debug` scans for nearby Bluetooth LE devices. **The radio is off in normal
operation and this is the only thing that turns it on**: it brings the stack up,
listens, and puts it back down. Nothing is stored, connected to, or paired.

Per device you get name, address, public/random, RSSI and advertised service
UUIDs &mdash; enough to recognise a specific dongle.

### It is BLE only, and that changes how to read a result

**The ESP32-S3 has no Bluetooth Classic radio at all.** A Classic (SPP) device
cannot appear here no matter what, so **an empty result does not mean nothing is
there.**

This matters if you are using it to identify an OBD-II dongle. Most cheap ELM327
clones are Classic; the BLE ones exist because iOS will not do arbitrary SPP,
which makes &ldquo;works with iPhone&rdquo; a reliable tell. The scanner can
prove a device *is* BLE. It can never prove one is not.

### Scanning repeatedly &mdash; tick "keep the radio up"

**Bringing the BLE stack up and down is what fragments the heap. Scanning is much
cheaper.** That distinction is the whole trick to using this page.

Each up/down cycle costs contiguity and does not give it back. Over three cycles
the largest free block fell **131&nbsp;KB &rarr; 65&nbsp;KB &rarr; 55&nbsp;KB**
while *total* free heap barely moved &mdash; free heap is not the binding
constraint, contiguity is. NimBLE needs roughly 70&nbsp;KB placed contiguously,
so below a 60&nbsp;KB largest block it cannot be placed at all, and you get
**about two scans per boot**.

Ticking **keep the radio up** places the stack once and reuses it. Six scans back
to back, all completing:

| Scan | Devices | Largest block |
|---|---|---|
| 1 | 12 | 61428 |
| 2 | 12 | 57332 |
| 3 | 13 | 40948 |
| 4 | 11 | 40948 |
| 5 | 11 | 36852 |
| 6 | 13 | 36852 |
| *Radio off* | &mdash; | *65524 recovered* |

**An improvement, not a cure.** The block still steps down as you scan, so a
session gets longer rather than unlimited &mdash; but six scans beats two.

The box is **ticked by default**, and the radio **drops itself after 5 minutes
with no scan**, so it cannot be left on by forgetting. **Radio off** ends it
immediately. That matters because a radio left up holds ~70&nbsp;KB of heap and
burns real current on a board whose whole purpose is not draining the battery.

### The fragmentation does not recover on its own

Worth being blunt about, because it is the failure you are most likely to hit.
A board left alone **overnight** was still refusing scans **16 hours later** &mdash;
184&nbsp;KB free, largest block 55&nbsp;KB. Two scans the previous afternoon had
taken it from 147&nbsp;KB to 57&nbsp;KB and it simply stayed there. Normal
operation neither worsens nor heals it.

So once you are below the limit, **only a reboot helps** &mdash; waiting will not.

Watch it coming: `/json` carries `heap_block` (largest contiguous block) and
`bt_up`, and the page prints the block on every result. When it does run out a
**Reboot board** button appears; a reboot defragments fully, at the cost of
~20&nbsp;s of sampling and a 15-minute park-confirm re-arm before auto-start
protection is live again.

### Why a scan is refused rather than attempted

Earlier firmware checked only *total* free heap, tried anyway, and `init()`
**panicked the board instead of returning false.** The guard now tests
`ESP.getMaxAllocHeap()` and names the actual figure in the refusal.

Those guards apply **only when the stack has to be brought up**. Once it is
placed they are meaningless &mdash; the largest block is small precisely
*because* the stack is holding it &mdash; and an early version that checked them
unconditionally refused every scan while the radio was up.

### Behaviour worth knowing

- The scan runs on its own task, so the dashboard stays responsive; a 5&nbsp;s
  scan takes about 8&nbsp;s door to door.
- The page pauses its own 2&nbsp;s `/json` poll for the duration. The WebServer
  takes one connection at a time and two pollers on a weak link is enough to
  start resetting connections.
- WiFi and BLE share one radio and one antenna, so expect the link to feel
  slightly slower while a scan runs.
- Advertised names are treated as hostile input &mdash; stripped to printable
  ASCII and escaped &mdash; because they are arbitrary bytes from an
  unauthenticated stranger in radio range. Real neighbours were already
  advertising non-UTF-8 bytes on day one.

### API

| Call | Does |
|---|---|
| `GET /btscan?s=<2..15>` | start a scan, returns `202` at once |
| `GET /btscan?s=<n>&keep=1` | ...and leave the radio up afterwards |
| `GET /btscan` | poll; returns the result **once**, then resets to idle |
| `GET /btscan?off=1` | put the radio down now |

The start call answers *before* the radio comes up. It did the reverse once, and
`BLEDevice::init()` starved the reply on the same core &mdash; a `202` that
should be instant took **2.76&nbsp;s**, and when it outlasted the browser the
page reported "could not start" for a scan that was running perfectly.

Because a poll consumes the result, a second poll still in flight comes back
`idle`; don't let a straggler overwrite a good display.

---

## 8. Roadmap / extensions

- **Deep-sleep version:** wake every N minutes, read, push the value (HTTP POST or MQTT),
  sleep. Trades live-polling for months of runtime. (Polling won't work while asleep.)
- **MQTT** to Home Assistant on the IoT VLAN instead of HTTP polling.
- **Logging/alerts** in the Python client (CSV, or alert when V < 12.0 / cranking dips).
- **Hardening** for permanent install: TVS + reverse-polarity diode (section 2).

## File map
```
esp32/
  sketches/voltage_monitor/voltage_monitor.ino   firmware
  python/voltage_client.py                        PC-side poller
  docs/voltage-monitor.md                         this file
```
