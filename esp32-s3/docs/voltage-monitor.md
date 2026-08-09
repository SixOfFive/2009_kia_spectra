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
