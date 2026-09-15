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

### The badges — two say "less certain", one says "more"

| Badge | Means | Trust the duration? |
|---|---|---|
| *(none)* | recorded live, both edges measured from battery voltage | yes |
| **OBD** | an edge was timed by the engine computer over the OBD link (fw 4.75, flags bit 5): RPM reaching zero, or the ECU's own run-time counter for a start — since 4.78 also a start that voltage confirmed late and the counter moved earlier | yes — the most precise edge the board records |
| **reconstructed** | hand-derived from other evidence when the log was first created | it is the best available account, not a measurement |
| **recovered** | the board rebooted while this run was open; the end time was rebuilt from the voltage history | approximately — good to about a minute in the normal case |

An **OBD** run is not a different kind of run, only a better-timed one. It appears
when the OBD link was live at the edge — see section 7d — and a run's other edge may
still come from voltage.

### Who started it

The **Source** column says what started or stopped a run: *auto*, *manual* (the
dashboard) or *key / FOB*. Before fw&nbsp;4.78, runs the board itself started were
shown as *key / FOB*. The start check passes on the first charging sample, while the
run's ON edge confirms 5&nbsp;s later and had forgotten the start by then, so a
phantom external start was also recorded. Runs from before 4.78 keep that label.

A dashboard **Start** sent while the engine runs is a **stop** — a Compustar start to a
running engine switches it off. Since 4.78 the log shows it as *Stop sent* (flags bit
4) with no start record and nothing to verify, and the run's end reads *stopped from
the dashboard*. In the seconds before voltage confirms a start, a press still counts as
a start: the board cannot tell a stop from a retry then.

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

`/debug` scans for nearby Bluetooth LE devices, and tests whether the board can
actually talk to one. **Since fw&nbsp;4.76 the Bluetooth stack is placed once at boot
and stays up** (see *Why the stack stays up* below); the radio itself only works
while something scans or holds a link. Nothing is stored or paired, and nothing is
connected to unless you press **Test** on a result (see *Connection test* below) or
the OBD link is up.

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

### Why the stack stays up

**Bringing the BLE stack up and down is what fragments the heap &mdash; not scanning,
and not connecting.** Each up/down cycle cost contiguity and never gave it back.
Over three cycles the largest free block fell **131&nbsp;KB &rarr; 65&nbsp;KB &rarr;
55&nbsp;KB** while *total* free heap barely moved: free heap is not the binding
constraint, contiguity is. NimBLE needs roughly 70&nbsp;KB, including a ~60&nbsp;KB
contiguous placement, and below that it could not be placed at all. Waiting never
helped &mdash; a board left alone overnight was still refusing **16 hours later**
(184&nbsp;KB free, largest block 55&nbsp;KB). Only a reboot did.

Firmware 4.71&ndash;4.75 managed that rather than removing it. A *keep the radio up*
box reused one bring-up across scans (six scans instead of about two), and the radio
dropped itself after 5&nbsp;minutes idle to hand its heap back &mdash; which meant a
fresh bring-up the next time anything needed Bluetooth. On 2026-09-14 an OBD page
opened after such a drop was refused with *memory is too fragmented to start
Bluetooth*; after a reboot, half an hour and one link session later, 4.75's largest
block was already down to 86&nbsp;KB with 186&nbsp;KB free (a fresh boot measures
about 147&nbsp;KB).

**fw&nbsp;4.76 removes the cause.**

- The stack is placed **once, at boot** &mdash; after WiFi connects, before the web
  server takes traffic and before the safety task exists &mdash; and is never taken
  down, so there is no later bring-up left to refuse. The *keep the radio up* box and
  **Radio off** are gone; `GET /btscan?off=1` answers 409 and says why.
- **Not done: moving general allocations into the 8&nbsp;MB of PSRAM.** Lowering the
  core's 4&nbsp;KB "`malloc()` prefers internal RAM up to here" threshold to 128&nbsp;bytes
  bought about 25&nbsp;KB of headroom, but lwIP in this core allocates with plain
  `malloc` (`CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` is off), so its packet buffers moved
  into PSRAM too &mdash; a setup the core does not support. It was reverted before
  release. (An OTA under it was followed by a `reset=PANIC` boot. That panic comes from
  the boot-time Bluetooth bring-up described below, not from the threshold.)
- **An OTA upload is the one exception.** It ends any OBD link and takes the stack down
  as soon as nothing is using it; a good image reboots anyway. That keeps the stack's
  heap out of the upload's way, and it is the one remaining way to take the stack down
  remotely &mdash; the board lives in the car, so OTA must never depend on Bluetooth.
  **Known issue (2026-09-14):** 2 of the 7 boots that placed the stack at boot panicked,
  both of them the first boot after an OTA. The fw&nbsp;4.79 core dump shows why. The
  controller's interrupt is allocated on core&nbsp;0's IPC task, whose stack is only
  1&nbsp;KB in this core (`CONFIG_ESP_IPC_TASK_STACK_SIZE=1024`), and it overflowed into
  the end-of-stack watchpoint. Each time, the board rebooted and the next boot came up
  cleanly. The OTA teardown is not involved.

The idle stack's cost is mainly heap: nothing scans, advertises or connects unless
asked, and with WiFi power-save off the shared radio is powered for WiFi anyway. Its
current draw has not been measured. `/json` carries `bt_up`, `heap_free`,
`heap_block` and, since 4.76, `wifi_ps_drv` &mdash; the WiFi driver's *actual*
power-save mode (0 = off; `wifi_ps` is only the requested setting), which shows the
stack did not quietly turn power-save back on.

If the boot bring-up ever fails, the log says so, and a scan, the connect test or the
OBD link each still attempt their own bring-up behind the old heap checks. A refusal
that advises a reboot comes with a **Reboot board** button beside it &mdash; on the OBD
page it reboots at once, with no confirm; on this page it still asks first. A reboot
costs ~20&nbsp;s of sampling and a 15-minute park-confirm re-arm before auto-start
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

### Connection test &mdash; can the board talk to it?

A scan proves a device is advertising. It does not prove the board can connect to
it, find its serial service and get an answer back &mdash; and those are what
decide whether a BLE OBD-II dongle is usable from here. **Test** on a scan row
answers that (fw 4.73). One test is one short connection:

1. connect to the address, as public or random exactly as the scan reported it;
2. discover every service and characteristic;
3. pick the serial pair &mdash; a notify characteristic for replies and a write
   characteristic for commands. The usual ELM327 layouts (`FFF0`, `FFE0`, `18F0`
   and the vLinker-style 128-bit service) win ties, but any vendor service with a
   notify + write pair is tried. Where a service has both, the write
   characteristic that is *not* also the notify one is used;
4. subscribe, then send `ATZ`, `ATE0`, `ATI`, `ATRV` and collect each reply up to
   the ELM327 `>` prompt. A reply arrives split across notifications, so the first
   packet is not the answer;
5. disconnect, and wait for the link to actually drop.

The verdict says how far it got:

| Verdict | Read it as |
|---|---|
| `no connection` | not advertising, out of range, or already held by a phone |
| `connected, but no serial (notify + write) service found` | not an ELM327-style dongle, or it hides the service until paired |
| `connected, but subscribing to its replies failed` | the dongle refused the subscription &mdash; often a sign it wants pairing |
| `connected and subscribed, but the dongle never answered` | commands went to the wrong characteristic, or the dongle is wedged |
| `connected and answering, but nothing identified an ELM327` | it talks, but not as an ELM327 |
| `PASS: connected and the dongle answered as an ELM327` | usable from the board |

**Measured on the car's Veepeak**, ignition off: connected in 8.3&nbsp;s at
&minus;58&nbsp;dBm, serial pair `FFF1` (notify) / `FFF2` (write), `ATZ` and `ATI`
answered `ELM327 v1.5`, `ATRV` answered `11.9V` &mdash; PASS.

Worth knowing before you press it:

- **Close the OBD app on your phone first.** A BLE dongle normally takes one
  connection at a time and stops advertising while something holds it, so a
  connected phone makes a working dongle look absent.
- **`ATRV` is not a battery reference.** It is the dongle measuring its own supply
  on OBD pin&nbsp;16, which is why it answers with the car off &mdash; but the
  Veepeak read 11.9&nbsp;V while the board's calibrated divider read 12.89&nbsp;V.
- **Query the car** adds `ATSP0`, `0100` and `010C`, which need the ignition ON;
  with it off, `UNABLE TO CONNECT` or `NO DATA` is the normal answer from a working
  dongle. Each OBD request waits up to 6&nbsp;s, and a first request that has to
  search protocols can take longer &mdash; if `0100` comes back as `SEARCHING...`
  with no prompt, run the test again.
- **A device that is not there takes about 30&nbsp;s to report.** This core's
  NimBLE `BLEClient::connect()` ignores its timeout argument and always waits its
  own 30&nbsp;s.
- **A connection is expensive.** Free heap fell 178&nbsp;KB &rarr; 91&nbsp;KB across
  the Veepeak test with the radio left up, and the largest block 131&nbsp;KB &rarr;
  47&nbsp;KB. Anything that holds a link open carries that for as long as it lasts.

#### Only read-only commands

The endpoint is unauthenticated on the LAN and the far side of the dongle is the
car's diagnostic bus, so commands pass an **allow list**: `ATZ ATWS ATI AT@1 ATRV
ATDP ATDPN ATE0 ATL0 ATS0 ATH0 ATH1 ATSP0`, plus OBD-II requests in the read-only
modes `01 02 03 07 09 0A`. Mode `04` &mdash; clear codes, which also wipes the
readiness monitors &mdash; cannot be reached, and neither can AT commands that
rewrite the dongle's saved settings. `ATSP0` is the one save allowed, because it
only restores the factory default, automatic protocol search.

#### One client, reused &mdash; because the library deletes it

`BLEDevice` keeps its own pointer to the last client it created and deletes it
inside `deinit()`. Deleting the firmware's copy as well would be a double free, and
creating a client per test would leak all but the last. So the firmware creates one
client per radio session, reuses it, and forgets it at every teardown &mdash; which
is why every teardown path now goes through one function.

### Core dumps (fw 4.79)

Every panic leaves an ELF core dump in the `coredump` flash partition: the core is built
with `CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH`. The board lives in the car with no serial
cable, so 4.79 makes the dump readable over WiFi:

- **After a panic reset**, the event log records the crashing task, program counter,
  fault cause and address, the first characters of the crashing firmware's ELF SHA-256,
  and the backtrace (`  ^ panic in task ...` and `  ^ backtrace: ...`).
- **`GET /coredump?info=1`** returns the same as JSON, plus the core's own panic reason.
- **`GET /coredump`** downloads the raw dump for `esp-coredump`.
- **`POST /coredump?erase=1`** clears it. A new panic overwrites the previous dump anyway.

To find the crash, run the backtrace through `addr2line` with the ELF of the firmware
that crashed, the one whose `sha256sum` starts with `app_sha`:

```
xtensa-esp32s3-elf-addr2line -pfiaC -e voltage_monitor.ino.elf 0x42xxxxxx 0x40xxxxxx ...
```

The ELF sits in the build path as `voltage_monitor.ino.elf`, and the next build
overwrites it. Keep a copy of every flashed build's ELF.

### API

| Call | Does |
|---|---|
| `GET /btscan?s=<2..15>` | start a scan, returns `202` at once |
| `GET /btscan?s=<n>&keep=1` | `keep` is ignored since 4.76 &mdash; the stack always stays up |
| `GET /btscan` | poll; returns the result **once**, then resets to idle |
| `GET /btscan?off=1` | retired in 4.76: answers 409 &mdash; the stack stays up from boot |
| `GET /btconnect?addr=<mac>&t=<public\|random>` | start a connect test, returns `202` at once |
| `GET /btconnect?...&cmds=ATZ,ATI,...` | replace the default `ATZ,ATE0,ATI,ATRV` (allow list, at most 8) |
| `GET /btconnect?...&keep=1` | `keep` is ignored since 4.76 &mdash; the stack always stays up |
| `GET /btconnect` | poll; returns the result **once**, then resets to idle |
| `GET /coredump?info=1` | the last panic's core dump: task, PC, cause, fault address, app SHA, backtrace, reason (fw 4.79) |
| `GET /coredump` | download the raw core dump (404 if there is none) |
| `POST /coredump?erase=1` | erase it |

The start call answers *before* the radio comes up. It did the reverse once, and
`BLEDevice::init()` starved the reply on the same core &mdash; a `202` that
should be instant took **2.76&nbsp;s**, and when it outlasted the browser the
page reported "could not start" for a scan that was running perfectly.

Because a poll consumes the result, a second poll still in flight comes back
`idle`; don't let a straggler overwrite a good display.

---

## 7d. OBD tab &mdash; live data through a BLE OBD-II reader

`/obd` shows what the car's engine computer reports, through a **BLE ELM327
dongle** plugged into the OBD port (the car's is a Veepeak, proven with the
*Connection test* in 7c). It is read-only: nothing here clears codes or changes
anything in the car.

### Pages

An **Overview** &mdash; the reader's status, engine speed, road speed, coolant,
ECU supply voltage, the check-engine light with its code count, and a card per
category &mdash; plus one page per category:

| Page | Shows |
|---|---|
| Engine | RPM, speed, coolant, load, throttle, intake air, timing advance, airflow, manifold pressure, oil temperature |
| Fuel &amp; air | fuel system state, short- and long-term trims, O2 sensors, commanded lambda, fuel pressure and level, barometric pressure |
| Electrical | ECU supply voltage beside the dongle's `ATRV` and the board's own battery reading |
| Distance &amp; time | run time, distance and warm-ups since codes were cleared, distance with the light on, ambient temperature |
| Fault codes | stored, pending and permanent codes; readiness monitors |
| Vehicle | VIN, calibration ID, ECU name, OBD standard, fuel type, protocol, the reader |

**Only values the car says it supports are read.** A connection first asks the car
for its supported-PID maps (`0100`, `0120` ...); everything else shows as *not
supported by this car* rather than as an error.

### Setting up or changing the reader

The reader is chosen on the page, not in the firmware. **Scan for readers** lists
nearby BLE devices with likely OBD readers first, **Use** saves the choice to NVS,
and **Change reader** forgets it and scans again &mdash; swapping dongles needs no
firmware change. Close any phone app connected to the dongle first.

### When the board holds the link

**Only while the engine runs.** Since fw&nbsp;4.77 the board connects by itself as soon
as it sees the engine running (charging voltage, confirmed in 5&nbsp;s) &mdash; no page
needed, logging on or off &mdash; holds the link for the drive, and disconnects when the
engine stops. With the engine off it leaves the reader alone: opening an OBD page shows
*engine off &mdash; the board connects to the reader when the engine starts* and changes
nothing, so nothing talks to the car's bus while it is parked and the dongle is free to
sleep. The one exception is **Read codes &amp; VIN now** (below). A live BLE connection
costs about 88&nbsp;KB of heap on a board whose real job is the auto-start &mdash;
accepted while driving, when the auto-start has nothing to do. The Bluetooth stack
itself stays up from boot, so a new link never needs a bring-up.

**Before 4.77 an open page held the link,** and a page left open anywhere kept it up:
during testing one on another device held it for 25&nbsp;minutes, because a browser
still runs a background tab's timers roughly once a minute &mdash; often enough to keep a
60-second timeout from running out. Pages no longer open or hold the link; a hidden tab
still stops polling, to spare the board's one-connection web server. The log names what
opened each link: `OBD: link opened for the running engine`, or for a Read-now request
and the device that sent it.

While the link is held, the Debug page's scanner and connect test refuse, saying
why &mdash; there is one radio. While the engine runs, that is the whole drive. An OTA
upload ends the link (see *Why the stack stays up* in &sect;7c).

On connecting, the board resets the dongle (`ATZ`), sets the reply format the parser
expects (`ATE0 ATL0 ATS1 ATH0`), selects automatic protocol search (`ATSP0`, the only
one of these the dongle saves), then asks the car what it supports.

### Engine off: Read codes &amp; VIN now

The dongle stays powered &mdash; OBD pin&nbsp;16 is always live &mdash; but the board
does not connect to it while the engine is off. To read fault codes or vehicle details
without starting the engine, turn the key to ON and press **Read codes &amp; VIN now**
on any OBD page (`POST /obdnow`). The board connects once, waits up to 30&nbsp;s for the
car to answer, reads the check-engine status, the stored, pending and permanent codes,
and the VIN, calibration ID and ECU name, then disconnects until the engine starts. The
results stay on the Fault codes and Vehicle pages. With the key off it reports *the car
did not answer* and disconnects; a reader it cannot reach gets one attempt, not a retry
loop. If the engine starts mid-read, the link simply carries on as the drive's link.

### How the data is read

While the engine runs, one task owns the dongle and polls only what the open page
shows: the four overview values on the Overview, one category's values on its own page
&mdash; plus RPM on every pass and the log's sweep every 30&nbsp;s. Fault codes are read
when the codes page opens, again every minute while it stays open, or on **Read
again**; vehicle details once per connection.

The reply parsing lives in `obd_elm.cpp`, which has no Arduino dependencies, so it is
tested on a PC against the reply shapes in the ELM327 datasheet and SAE J1979 &mdash;
CAN and the older protocols, multi-frame VINs, several ECUs answering at once:

```
g++ -std=c++17 -Wall -Wextra -I esp32-s3/voltage_monitor \
    esp32-s3/voltage_monitor/obd_elm.cpp esp32-s3/tests/test_obd_elm.cpp \
    -o /tmp/test_obd_elm && /tmp/test_obd_elm
```

**Adding a value** is one row in `OBD_PIDS`, plus a formula case in `obdDecode()` if
the formula is new. **Adding a page** is one row in `OBD_CATS`; its tab and its card
on the Overview follow from the table.

### The OBD log

With **logging on**, which is the default, the board writes a row to `/obdlog.csv`
every 30&nbsp;s while the car answers. The first column is local date and time.
Then comes one column per value **the car reports**, with the unit in the name
(`coolant_C`, `speed_kmh`, `rpm`), then the dongle's supply, the board's battery
reading, the check-engine light and the stored-code count. Since fw&nbsp;4.78 the car's
own PID map decides the columns: this Spectra has no manifold pressure, oil
temperature, upstream O2 voltage or fuel pressure, so those have none (26 columns, not
30). A supported value that did not answer in time is an empty cell, so the columns
never shift within a file; if the set changes, a new file generation starts with its
own header. **Nothing is written while the car is off.**

A log needs the link while you drive, and the board holds it then regardless: **it
connects to the reader by itself when the engine starts**, logging on or off, and keeps
the link for the drive (see *When the board holds the link*). Logging only decides whether rows are written. The
connection follows the engine-start voltage edge, which itself takes 5&nbsp;s to
confirm.

The Overview's **OBD log** card turns logging on and off (remembered in NVS),
downloads the whole log as one CSV, and clears it. Two generations of about
512&nbsp;KB each are kept; when the newer fills, the older is dropped. Rows are built
by the Bluetooth task in RAM and written by the loop core along with every other file
write, never while an OTA upload is streaming.

### Engine start and stop from the ECU

While the link has a fresh reading, **the engine computer times the run log's
edges** instead of battery voltage. RPM falling to zero ends a run at once, where
voltage has to stay below 13.10&nbsp;V for 120&nbsp;s to see past surface charge.
A voltage dip also cannot end a run the ECU says is still going. When the ECU is the
first to see a start, its own run-time counter (PID&nbsp;1F) dates it. Such runs carry
an **OBD** badge in the run log (flags bit&nbsp;5).

**Since fw&nbsp;4.78 the counter also corrects a start voltage saw first.** On this car
voltage confirmed a start about 42&nbsp;s late &mdash; the battery took that long to
climb from 12.65&nbsp;V to the 13.2&nbsp;V charging threshold &mdash; and the OBD link
only opens once it has. When PID&nbsp;1F then puts the start earlier, the board moves
the run's start there: the run length, `last_run`, and the ON record on flash
(fixed-size records, so that one is rewritten in place), which gains the **OBD** badge.
Once per run, at most 30&nbsp;minutes back, never before the previous stop. The log
says `ENGINE ON: the ECU dates this start N s earlier than voltage did`.

Two guards keep it honest:

- An OBD "stopped" is only accepted once voltage agrees the alternator has stopped
  charging. A link that loses the ECU mid-drive cannot end a run early.
- Silence from the ECU only counts as a stop if it had been reporting the engine
  running. A car that never answered says nothing about its engine.

**None of this touches the auto-start decision.** That stays on voltage alone
(`g_parkS`). A Start sent to a running Compustar engine switches it off, and nothing
on the fire path reads the run state that OBD now helps decide.

### API

| Call | Does |
|---|---|
| `GET /obdjson` | Overview data, plus `engine` (`running` / `off`); never opens or holds the link (4.77) |
| `GET /obdjson?cat=<key>` | one category: `engine`, `fuel`, `electrical`, `trip`, `codes`, `vehicle` |
| `GET /obdjson?cat=codes&refresh=1` | ...and re-read the fault codes now |
| `POST /obdcfg?addr=<mac>&t=<public\|random>` | remember the reader |
| `POST /obdcfg?forget=1` | forget it; an open link closes |
| `POST /obdnow` | Read codes &amp; VIN now: with the engine off, connect once, read codes and vehicle details, disconnect (409 with a reason if it cannot) |
| `GET /obdstate` | diagnostics: link state, idle time, poll count, last poller, task heartbeat and current step &mdash; **does not** keep the link alive |

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
