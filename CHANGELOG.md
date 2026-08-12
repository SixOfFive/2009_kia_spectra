# Changelog

All notable changes to **vroom** (voltage-triggered remote start, 2009 Kia
Spectra). Newest first, grouped by change type.

The shipped build is the ESP32-S3 firmware in `esp32-s3/voltage_monitor/`;
firmware versions below are the `FW_VERSION` string reported by `/json`. The
`pi/` telematics half is archived and unmaintained — changes to it are not
tracked here.

This file was backfilled on 2026-08-10 from git history and `logs/*.md`,
covering the 4.x arc (the firmware that made remote start actually work). For
anything earlier, see `logs/` and `git log`.

---

## 2026-08-11 — field diagnosis (no firmware change)

Live telemetry pulled off the installed board (fw 4.23). No release; recorded
because it changes the priority of several open items.

### Known issues
- **Auto-start can be prevented from ever arming by watchdog reboots.**
  `AS_PARK_NEED` is 900 s of park-confirm, and **a reboot resets the counter to
  zero**. Observed watchdog reboots on 2026-08-10 were 42, 39 and **17 minutes**
  apart — the 17-minute gap only just cleared the 900 s window. **If resets land
  under ~15 minutes apart, the low-voltage protection never arms at all**, and
  nothing surfaces that. This is the same class of silent-protection-loss the fw
  4.9 hardening was written to eliminate, arriving by a different route. Caught
  live at `as_state: park-wait, as_park_s: 642/900`, which then rebooted before
  arming.
- **Watchdog resets are not "isolated".** Five recorded in the log ring, four of
  them inside 97 minutes on 2026-08-10. The earlier "leave it for now" assessment
  was made against much sparser evidence.
- **Every WDT breadcrumb reads identically:**
  `^ WDT stall: loop was 'http', safety was 'idle'` — the safety task on core 0 is
  healthy in every case; the loop task always hangs in the HTTP path. This is
  direct confirmation that fw 4.24 targets the right code, and that the core-0
  split is doing its job.
- **Reproduced on demand:** a `GET /history?cols=vbatt,rssi` (~36 KB) over the
  in-car link hung for 90 s and rebooted the board. On 4.23 that chunked send
  never feeds the watchdog — exactly the defect 4.24 fixes.
- **The deployed threshold is 12.2 V, not 12.4 V.** `as_volts` reads **12.2**
  live. `AS_DEF_VOLTS` is `12.4f` in source, but the live value comes from NVS,
  so the cold-weather recommendation has never reached the installed unit. The
  dashboard's own help text on that page recommends 12.4 V.

### Root cause identified — Wi-Fi instability is the AP, not the board
The single AP `E8:9C:25:B2:23:E8` (SSID `IoT`) was observed on channels
**5 → 4 → 3 → 7 → 8 → 3 → 4** in under 48 hours. Auto-channel-selection is running
continuously and each hop deauthenticates the client. Disconnect reasons match:
`auth-expire`, `assoc-expire`, `auth-leave`, `4way-handshake-timeout`,
`auth-fail`, `assoc-fail`. A 4-way handshake failing at −66 dBm is not a range
problem.

**Pinning `IoT` to a fixed channel (1, 6 or 11) and disabling auto-channel is the
highest-leverage fix available** — likely worth more than any firmware change,
since it should reduce the disconnects, the AP-fallback episodes, and the HTTP
stalls that trigger the reboots.

**The Wi-Fi antenna is not the problem.** RSSI is a consistent **−64 to −70 dBm**
across dozens of associations over three days, TX power is maxed at 19.5 dBm, and
the board negotiates 11n-HT20 and gets an IP in 2–3 s whenever the AP cooperates.
An unseated U.FL costs ~20–25 dB and would put it at −85 to −95, where it would
not associate cleanly at all.

### Measured drain — the parasitic fault is present and dominant
The live least-squares fit is untrustworthy and correctly rejected
(`drain_mvph +3.2`, `r² 0.223`, `as_eta_s -1`) — the 4.23 plausibility cap doing
its job. A better measurement comes from the 67.4 h park itself: last engine run
2026-08-08 16:52, battery now **12.33 V** (rested, ≈63 % SoC). From a
post-alternator ~12.6–12.7 V that is **−4.0 to −5.5 mV/h**, i.e. **200–275 mA at
50 Ah** or **140–190 mA at a degraded 35 Ah**, against the ~91 mA the power budget
predicts for car + board. `docs/power-budget.md` §6: "worse than −2.5 mV/h → the
fault is present and dominates everything in this document." Roughly double that.

Treat the figure as indicative — the starting voltage is inferred, not measured —
but it points the same way as everything else. Board's own share confirmed live at
State C (`cpu_mhz 80`, `wifi_ps false`): ~61 mA ≈ 0.77 W.

Also: `/starts` returns `[]` — **auto-start has never fired.**

---

## 2026-08-12 — fw 4.32: smoothed long-term fit, task watchdog 30 s → 5 min

### Fixed — the projection "bounced around like crazy"
Both ends of the long-term fit were **single ADC samples**. The rate is
`(vNow − refV) / hours`; with the anchor only ~30 mV from the present reading, a
one-LSB wobble (~5.5 mV at the battery) is a large fraction of the signal — and
that rate is the **denominator** of the ETA, so the error compounds. Measured on
4.31, consecutive polls:

```
v=12.30  ->  -0.840 mV/h  ->  Mon 17 Aug  ->  43.2%
v=12.29  ->  -1.260 mV/h  ->  Sat 15 Aug  ->  56.2%
v=12.29  ->  -1.050 mV/h  ->  Sun 16 Aug  ->  50.1%
```

**10 mV of ADC quantisation halved the rate and slid the projected date four
days.** Both ends now use a 120-sample (2 h) mean via `smoothedVoltsRecent()`,
and the anchor averages 120 samples forward instead of trusting one. NVS schema
bumped to 3 to force re-seeding, so the noisy single-sample anchor cannot
persist. The safety path is deliberately untouched — `autoStartEtaS()` still uses
the instantaneous voltage for the "already below threshold" check.

### Changed — task watchdog 30 s → 300 s
On the evidence of four days of logs: **nine watchdog firings, every one network
I/O** (`'http'`, `'/history'`, `'ota'`), and every one recording
`safety was 'idle'` — the core-0 task the watchdog exists to protect was healthy
in all nine. **Not one real hang was ever caught.** Each firing cost a reboot
*and* 15 minutes of disarmed auto-start, since a reboot resets the 900 s
park-confirm.

So a longer timeout makes the safety function **more** available, not less. The
cost: a genuine safety-task hang now self-recovers in up to 5 min instead of 30 s
— immaterial when the sampler runs at 1 Hz, the trigger needs a 60 s sustain, and
the battery moves ~1 mV/h. Note `esp_task_wdt_config_t` carries one timeout for
all watched tasks, so this cannot be set per-task without a separate software
watchdog.

This is compensation, not the repair: bounding the I/O (`waitWritable`, 4.28)
remains the correct fix and stays. The wider timeout stops *legitimate* slowness
from looking like a stall.

### Why it was needed: OTA was rebooting the board mid-upload
Five consecutive 4.32 uploads failed with `HTTP=000` at varying byte counts. The
cause was not the network — the breadcrumb named it:

```
2026-08-12 10:04:26   ^ WDT stall: loop was 'ota', safety was 'idle'
2026-08-12 10:04:26 boot: fw 4.31, CPU 240 MHz, reset=TASK-WATCHDOG
```

**The OTA itself was tripping the watchdog**, rebooting the board and dropping
the connection. At ~18 KB/s a 1.22 MB image needs ~68 s against a 30 s watchdog;
the handler feeds the WDT only when data arrives, so any link stall past 30 s in
that window rebooted it. Throughput had fallen from 55 KB/s to 18 KB/s after
moving to channel 10, where the main router's three radios contend for airtime —
the +5 dB of signal was paid for in throughput. Raising the CPU to 240 MHz did
not help (18.1 vs 16.8 KB/s), confirming the bottleneck was the channel, not the
board.

**Delivered as a binary for manual upload**, since the defect blocks its own fix
over the air: 1,223,584 bytes, md5 `7b988ef05086924a82ce882e2741e782`.

---

## 2026-08-12 — fw 4.31: real dates, cycle progress % and bar

### Added — Voltage tab
- **Next auto-start** and **Last auto-start** as **actual calendar dates**
  (`Wed 19 Aug 06:21`), not countdowns. 4.30 had shipped every forward-looking
  value as a countdown; "date" means a date.
- **Last run → next auto-start** now shows the **duration *and* both endpoints**
  (`Sat 8 Aug 16:52 → Wed 19 Aug 06:21`).
- **Percent elapsed through the cycle, with a progress bar** — green under 60 %,
  amber to 85 %, red beyond. Validated against an independent calculation:
  card read `50% elapsed`, bar 50.3 % wide, computed 50.3 %.

`fmtDate()` helper added; all date fields use it for a consistent short format.

### Note on stability of the projection
The estimate refines as the baseline grows, and the percentage moves with it —
between two readings minutes apart the projection went from *16 Aug / 7.3 days /
50 %* to *19 Aug / 10.6 days / 35 %*. Nothing about the battery changed; the
long-term rate is still settling as its window extends. **Treat the bar as a
trend indicator, not a precise gauge**, until the baseline spans several days.

### Confirmed — reboot before OTA
The `Update`-wedge workaround from 4.30 held: a `POST /reboot` immediately before
the upload, and 4.31 flashed first try (`HTTP=200`, 54.7 s) where 4.30 had needed
four attempts. This now looks like the reliable procedure after repeated OTA
cycles in one session, not a one-off.

---

## 2026-08-12 — fw 4.30: cycle + last-auto-start cards, and every ETA now uses the anchor

### Added — two cards on the Voltage tab
- **Last run → next auto-start** — the whole projected cycle: time already elapsed
  since the vehicle last ran, plus the remaining estimate. Verified independently
  against `/json` (3.7 days elapsed + 4.8 remaining = 8.5 total).
- **Last auto-start** — `as_last`, which is written *only* by the auto-start fire
  path, so it can never be confused with a dashboard button press or a key/FOB
  start. Currently reads "never".

### Changed — `autoStartEtaS()` prefers the long-term anchor
It was still computing from the 24 h least-squares fit, which fed the Main tab's
ETA card, the dashboard status line **and SNMP OID `.44`**. That fit is a short
window, it restarts on every reboot, and on this car the diurnal thermal swing
(~130 mV) is several times the daily trend (~20 mV) — so it is structurally
incapable of the answer, and it was driving the Cacti feed too. It now prefers
the anchored long-term estimate wherever one exists, falling back to the short
fit only when no anchor is available yet.

Immediately visible after flashing: `as_eta_s` went from **−1** on 4.29 to
**3.6 days**, matching `lt_eta_s` (the 60 s difference is the sustain hold).

### Operational finding — a wedged `Update` state rejects OTAs until a reboot
Three consecutive 4.30 OTAs failed. The verbose trace was decisive: the third
sent all 1,221,751 bytes, curl logged `upload completely sent off`, and the board
answered **`HTTP/1.1 200 OK`** with a 6-byte body — **`FAILED`**. So the image
arrived intact and `Update.hasError()` rejected it. The image was ruled out
independently: magic byte `e9`, MD5 identical across the CIFS copy, 1.22 MB into
3 MB slots.

**A `POST /reboot` immediately before the OTA cleared it, and the same image then
flashed first try** (`HTTP=200`, 81 s). Worth remembering after several rapid OTA
cycles in one session.

**Gap this exposed:** the specific reason only goes to `Serial`
(`Update.printError(Serial)`), and there is no USB on a board behind a dash — the
one piece of information that would identify the failure is written where nobody
can read it. `/update` should return `Update.errorString()` in the response and
the flash log instead of a bare `FAILED`, for the same reason the WDT breadcrumbs
exist: a failure you cannot see is a failure you cannot fix remotely. **Not yet
implemented.**

---

## 2026-08-12 — fw 4.29: anchor the long-term baseline to the VEHICLE, not the boot

### Fixed
4.28's bootstrap path anchored the long-term reference at **the moment the board
booted**, which on a device that reboots means the baseline restarts every time —
defeating the entire purpose of the feature, which exists precisely to escape
that. The measurement must be anchored to the **car's last run**.

Now the target is `last_run + LT_SETTLE_S` (12 h after the engine stopped, past
the fast fluctuating settle). If that moment has already passed, it does **not**
wait for a new cycle: the 24 h history ring is written through to flash by
`saveHistory()` and **survives reboots**, so the voltage at that time is usually
still on record. `seedLongTermFromHistory()` takes the earliest stored sample at
or after the target and uses its real recorded timestamp and voltage.

A one-time NVS schema bump (`lt_schema` → 2) discards any anchor written by the
4.28 scheme, so a wrong boot-time value cannot persist and quietly poison the
number.

**Verified in the field immediately after flashing:**
```
last_run 08-08 16:52   anchor 08-11 07:39 @ 12.32 V   baseline 25.8 h
rate -0.86 mV/h        ETA 4.8 days to the 12.20 V trigger
```
`drain baseline back-dated to 12.32 V ... (62.8 h after the last run, 25.7 h of
baseline)` — a usable number the moment the board came up, rather than six hours
after a reboot.

**Known limit:** the ring holds 24 h, so when the last run is older than that the
anchor lands at the oldest stored sample rather than exactly `last_run + 12 h`.
That is the longest baseline the stored data supports, and it is real measured
voltage rather than an assumption. From the next engine run onward the 12 h mark
is recorded as it happens and the baseline extends for as long as the car sits.

### What this says about the drain
**−0.86 mV/h over a 25.8 h baseline ≈ 43 mA at 50 Ah** — close to the board's own
consumption and nowhere near the "150–250 mA parasitic fault" claimed on 08-11.
That earlier figure came from an uncompensated fit over a window whose thermal
swing (130 mV) was six times its trend (20 mV); it read a day/night cycle as
depletion. The long-term anchor is immune to that by construction, because it
compares two points at the same phase of the settling curve rather than fitting
through a diurnal cycle.

---

## 2026-08-12 — fw 4.28: the stall fix that actually works, long-term drain ETA

### Fixed — `/history` watchdog stalls, third and correct attempt
4.27 did not fix it either: three more `WDT stall: loop was '/history'` overnight
(04:32, 04:34, 08:52). The mechanism, finally understood:

`NetworkClient::write()` retries up to 10 times, each with a **hardcoded 1 s
`select()`** (`WIFI_CLIENT_SELECT_TIMEOUT_US`) that no setting can change — so a
single write has a **~10 s floor regardless of `SO_SNDTIMEO`**, and
`sendContent()` issues several writes per chunk. That is why both earlier
attempts failed: **4.24** fed the watchdog *after* a call that never returned,
and **4.27** shrank a timeout that was never the binding constraint.

**The fix is to never enter a blocking write.** `waitWritable()` polls the socket
with `select()` in 50 ms slices, resetting the WDT each slice because *we* own
the wait, and only calls `sendContent()` once the socket is genuinely writable. A
client that never drains has its response aborted after 4 s instead of hanging
the loop. Applied to `/history`, `/logtext`, `/logpage`, `/starts`.

**The watchdog is not weakened.** It stays armed at 30 s throughout; a genuine
hang anywhere, including inside these handlers, still panics and reboots exactly
as before. Only legitimate network waiting stops *looking* like a hang. (An
earlier proposal to `esp_task_wdt_delete()` during streaming was rejected for
exactly this reason — it would have traded a stall for a lockup.)

### Added — long-term drain estimate (Voltage tab)
The 24 h RAM ring cannot see a drain that plays out over days, **and every reboot
restarts it** — which is precisely what the stalls above were doing to the
measurement. So the long-term view anchors a single reference point instead:
**12 h after the engine last stops**, record time and voltage, skipping the fast
fluctuating settling phase while surface charge dissipates. The rate is the slope
from that anchor to now.

**Both ends live in NVS, so it survives reboots** and keeps extending for as long
as the car sits — immune to the failure mode that has been destroying the 24 h
fit. Reports `lt_ref_ts`, `lt_ref_v`, `lt_mvph`, `lt_eta_s` in `/json`, with four
cards and an explanation on the Voltage tab. Holds off until 6 h of baseline
exist; re-arms on the next engine run; bootstraps immediately if the last run is
already older than 12 h (so a fresh flash does not wait a whole cycle).

### Added — external (key/FOB) starts are now recorded
Previously `/starts` held only board-fired starts, so a key or FOB start appeared
as an `ENGINE ON` log line and nothing else — the start history was not a
complete account of the car's runs. A run detected without the board asking for
it now gets a start-history entry with a new **`external`** source (purple pill),
marked confirmed by definition since the alternator is demonstrably up. Guarded
on `g_verifying` so a start the board *did* fire is not double-counted.

### Changed
`ENGINE OFF` now records run duration: `charging ended at 12.94 V after 20m 14s`.
That gives the Compustar's real runtime as measured rather than remembered, and
pairs with the charging voltage during the run — the number that shows whether
HVAC load was consuming the alternator's output instead of charging the battery.

Compile-verified 1,219,951 bytes (38 %), globals 61,696 (18 %). Browser-validated
against mocks: long-term cards render (`~4.8 days`, rate, anchor time/voltage),
the three-way source pill maps `auto`/`external`/`manual` correctly, zero console
errors. Cache-bust bumped to `?v=428`.

**Field state after flashing:** RSSI **−61 dBm** (best recorded; it was −74 on
`IoT`), baseline anchored at 12.29 V by the bootstrap path as designed.

---

## 2026-08-11 — fw 4.27: bound per-write stalls (4.24 was insufficient)

### Fixed
- **`/history` was still rebooting the board on 4.26 — fw 4.24 did not fix it.**
  Caught in the field by the 4.24 breadcrumb, which named the endpoint:
  `^ WDT stall: loop was '/history', safety was 'idle'` (on 4.23 the same line
  read a generic `'http'`, so the per-endpoint naming is what localised it).

  **Why 4.24 failed.** It fed the watchdog *between* chunks. The stall is inside
  a *single write*: `NetworkClient::write()` retries up to
  `WIFI_CLIENT_MAX_WRITE_RETRY` (10) times, each a 1 s `select()` plus a `send()`
  bounded by `SO_SNDTIMEO` — which `WebServer` sets from `HTTP_MAX_SEND_WAIT`,
  **5000 ms**. So one write against a stalled client blocks up to
  **10 × (1 + 5) = 60 s, twice the 30 s watchdog**, and the
  `esp_task_wdt_reset()` sits *after* that write, so it is never reached.
  Feeding between chunks helps a merely-slow client and does nothing for a stuck
  one — which is what a flaky link actually produces.

  **The fix bounds the socket instead of the loop:** `boundSendStall()` calls
  `server.client().setTimeout(1000)` at the top of every streaming handler
  (`/history`, `/logtext`, `/logpage`, `/starts`, `/scan`), capping one write at
  ~10 × (1 + 1) = 20 s — inside the watchdog, after which the per-chunk feed
  works as intended. Each send loop also breaks on `!client().connected()` rather
  than writing into a dead socket.

  **Verified:** `GET /history?cols=vbatt,rssi` returned 29,232 bytes / 1,441 rows
  in **3.7 s** with uptime still climbing. The same request hung 90 s and
  rebooted 4.23. Caveat: the link had also improved to −67 dBm by then, so this
  does not prove the fix at −77; the mechanism analysis is what carries it.

  **Lesson worth keeping:** when a watchdog fires inside I/O, bound the I/O.
  Feeding the dog around a blocking call only works if the call itself is bounded.

### Measured drain — the parasitic fault is large and confirmed
With `/history` finally retrievable, a least-squares fit over **25.7 h / 1,440
samples**: **−6.91 mV/h at r² 0.741** (12.47 → 12.34 V). Converting with
`I = |mV/h| × C`: **345 mA at 50 Ah, 242 mA at 35 Ah.** `docs/power-budget.md` §6
predicts −1.82 mV/h for car + board and −7.26 mV/h for "car + board + 300 mA
fault" — so the measurement lands essentially on the 300 mA-fault row. Net of the
board (~61 mA) and a healthy car (~30 mA), **the unlocated fault is roughly
150–250 mA**, dominating everything else in that document, exactly as it warned.

At that rate the 150 mV of headroom from 12.35 V to the 12.2 V trigger is about
**21 hours** — so if the car is not driven, auto-start should fire for the first
time within a day.

### Also changed (no flash — via the new 4.26 config endpoint)
`boot_s` 20 → **45 s** and `ap_after_s` 300 → **180 s**. The 4.26 log showed a
boot-connect window expiring **three seconds** before association completed,
which cost ten minutes in AP fallback. First real payoff of runtime config.

---

## 2026-08-11 — fw 4.26: runtime-configurable WiFi

### Added
- **Full WiFi configuration at runtime, NVS-backed, in a new section on the WiFi
  tab.** `secrets.h` now only *seeds* the config on first boot; it no longer
  dictates it, so changing networks no longer means a reflash.

  | Group | Settings |
  |---|---|
  | Home network | SSID (with scan-assisted picker), password, minimum accepted security (any / WPA / WPA2 / WPA3) |
  | Fallback AP | SSID, password, security (Open / WPA2 / WPA+WPA2 / WPA2+WPA3), channel, hidden |
  | Timers | raise-AP-after, retry-home-every, per-retry wait, boot connect window |
  | Radio | TX power (12 steps, −1 to 19.5 dBm), protocol (b/g/n, b/g, b-only, +LR), hostname |

- `GET /wificfg` returns current settings; `POST /wificfg` validates and applies.

### The safety design, which is the point
This board is bolted behind a dash and can start a car. A mistyped SSID or
password would strand it on its fallback AP — and until now there was nothing you
could *do* from that AP, so recovery meant a USB reflash of an inaccessible
board. So credential changes are **applied, verified, and automatically
reverted**: `applyPendingWifi()` runs from `loop()` (never the HTTP handler, so
the reply escapes before the radio drops), tries the new network for the boot
connect window, and on failure restores the previous credentials and reconnects.
A typo now costs one connection cycle instead of a dashboard disassembly.

Supporting choices:
- **Passwords travel in the POST body, never the query string.** `trackReq()`
  stamps `server.uri()` into the RTC breadcrumb, which is written to the
  persistent flash log on a watchdog reset — a password in a URL would end up in
  that log. (`_currentUri` is set after the query is stripped, so this is belt
  and braces, but the log is durable and worth being careful about.)
- **Passwords are never returned by `GET /wificfg`** — only `*_pass_set`
  booleans, so a stored password cannot be read back off the device. A blank
  password field means "leave unchanged".
- **A secured AP with a password under 8 characters is refused**, because
  `softAP()` would silently fail to start and that AP is the only recovery path.
- Every out-of-range or corrupt NVS value falls back to something that still
  connects, never to something that strands the board.
- `WiFi.setMinSecurity()` is now always called explicitly. The Arduino default is
  already `WPA2_PSK`, so the config default is WPA2 to preserve exactly today's
  behaviour rather than silently loosening it.
- `esp_wifi_set_protocol` persists to NVS in the driver — the fw 4.18 lesson — so
  the protocol is always written explicitly rather than assumed.

### Validated in a browser before flashing
Per the 4.20 precedent, the real `WIFI_HTML` / `APP_CSS` / `APP_JS` were extracted
from the sketch and served against mock endpoints. Confirmed: all 20 controls
present, every field populated from `/wificfg`, the timer hint computing
("raise the AP after 5 min, then retry home every 10 min"), the scan populating
the SSID picker while correctly excluding hidden networks, the POST going out as
`application/x-www-form-urlencoded` with **the password in the body and not the
URL**, and the password field self-clearing after save. Zero console errors.

**Also caught pre-flash:** `app.css` and `app.js` changed, but every page still
requested them as `?v=420` — browsers would have served stale cached assets
against the new firmware. Bumped to `?v=426` across all 12 references.

Compile-verified: 1,216,928 bytes (38 % of the app slot), globals 61,672 (18 %).

---

## 2026-08-11 — fw 4.25 flashed; antenna CLEARED, weak AP identified

4.25 went out by OTA at −77 dBm: 1,199,367 bytes in 21.5 s (55.6 KB/s), clean
`reset=software` reboot, back on the LAN in ~12 s. `/scan` then answered the
question the whole Wi-Fi investigation had been circling.

### The antenna is fine
The board sees **14 APs spanning −64 to −96 dBm**. A disconnected, pinched or
metal-buried antenna costs 20–30 dB, which would push everything below the noise
floor — such a board sees two or three APs and nothing under about −80. Hearing
−96 dBm proves the receive path is healthy and sensitive. **The earlier suspicion
that the 10-foot/−74 dBm combination indicted the antenna was wrong.**

### What is actually weak: the `IoT` AP itself
Measured by the same receiver at the same instant, from the car:

| AP | RSSI from the car |
|---|---|
| `Password is Taco` / `TacoForYou` / hidden (ch 10, one router) | **−64, −64, −65** |
| `IoT` (ch 6) | **−75** |

**An 11 dB gap between two of the user's own APs, same receiver, same moment.**
A receiver that hears −64 from one AP is not deaf; `IoT` is simply farther,
lower-powered, or more obstructed. This reframes months of "weak signal in the
car" — it was never the board.

**Highest-leverage fix: move the board onto the ch 10 network for ~11 dB
instantly**, which is more than any channel change or antenna rework can offer.
The counter-argument is real though: `IoT` is presumably a segregated SSID, and
this device can start the car, so keeping it off the main network is defensible.
The alternative that preserves segregation is to move the `IoT` AP closer to the
driveway or raise its transmit power.

### Channel 6 is the worst realistic choice — now provable from the car
Interference summed in linear power from the board's own scan position, expressed
as dBm-equivalent (lower is better):

```
ch5  -74.3    ch3  -72.2  <- clear, and the empirical winner
ch4  -73.1    ch6  -66.1  <- current, ~6 dB worse than ch3
ch1  -70.8    ch10 -59.5  <- worst
```

Channel 6 carries **`TELUS1365` at −76, essentially equal to `IoT`'s own −75** —
co-channel contention with a network we do not control. **Channel 3 has no
occupant at all** from the car's viewpoint, which independently explains why it
produced the only associations that never died of a Wi-Fi failure.

**A laptop scan from indoors missed this entirely** — it saw `TELUS1365` only on
5 GHz and never registered its 2.4 GHz radio on ch 6. The device's own scan, from
where the device actually lives, is the better instrument. That is the argument
for `/scan` existing.

### Added
- **`GET /scan` — WiFi survey, intended as an antenna health check.** Returns
  every visible AP as `{ssid, bssid, rssi, ch}` plus the board's own `self_rssi`,
  hidden SSIDs included.

  **Why a scan rather than reading a single RSSI:** one RSSI number only means
  something if you already know the distance and the AP's transmit power. A scan
  compares *this* receiver against many transmitters at once, so it can be held
  against a phone or laptop standing in the same spot. A healthy front end sees
  roughly the same AP list at roughly the same levels; a disconnected, pinched or
  metal-buried antenna shows **far fewer APs and a uniform ~20–30 dB deficit
  across all of them**. That pattern cannot be explained away by distance or AP
  power, which is what makes it conclusive — and it is the closest thing to the
  "can I measure the antenna?" question, since the ESP32 exposes no VSWR, no
  reflected power and no antenna-detect pin, and DC resistance is meaningless on
  an antenna that is open or near-short by design.

  **Costs, deliberately accepted:** `scanNetworks()` blocks for a few seconds
  because it visits every channel, and it briefly takes the radio off the home
  channel, so an already marginal STA link may drop and re-associate. On-demand
  only, never periodic. The WDT is fed either side (the 4.24 mechanism), and the
  safety task on core 0 is untouched, so auto-start is unaffected throughout.
  Registered explicitly as `HTTP_GET` per the 4.22 route-shadowing lesson.

Compile-verified: 1,198,907 bytes (38 % of the 3 MB app slot), globals 61,544
(18 % DRAM) — +1,499 bytes of flash and +32 bytes of RAM over 4.24. Not flashed.

---

## 2026-08-10 — fw 4.24

### Fixed
- **fw 4.24** **A slow client could trip the task watchdog.** The chunked-send
  loops in `/history`, `/logtext`, `/logpage` and `/starts` can span many seconds
  against the car's ~−70 dBm link, and none of them fed the watchdog while
  sending. A legitimately slow send was therefore indistinguishable from a stall,
  and the ~30 s WDT would panic-reboot the board mid-response — a plausible
  contributor to the isolated TASK-WATCHDOG resets logged against 4.19/4.20,
  which correlated with Wi-Fi disruption. All four now call
  `esp_task_wdt_reset()` between chunks, matching what the OTA handler already
  did for the same reason. Safe because these run on the loop task, which is
  subscribed to the WDT; the safety task on core 0 is untouched and keeps its own
  independent deadline.

### Changed
- **fw 4.24** `trackReq()` stamps the **requested endpoint** into the WDT
  breadcrumb instead of a generic `"http"`, so if a stall does happen inside a
  handler the next boot names which one. Builds on the 4.21 breadcrumbs.
  `loopMark()` `strncpy`s into a fixed `RTC_NOINIT` buffer, so passing the
  temporary `String`'s `c_str()` is safe.

> **Compile-verified 2026-08-11, not yet flashed.** Builds clean against esp32
> core 3.3.10 — the same core that produced 4.23, so the two differ only by the
> source change. 1,197,408 bytes, 38 % of the 3 MB app slot; globals 61,512 bytes
> (18 % DRAM). `4.24` confirmed present in the output binary. The board is still
> running 4.23; flash from close range.
>
> The FQBN was recovered from the 4.23 build's own `build.options.json` rather
> than from the docs, which disagree —
> `esp32-s3/docs/voltage-monitor.md` says `PartitionScheme=custom` but the real
> build used **`PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB`**.
> Worth correcting in the docs: a wrong partition scheme on a 16 MB OTA board is
> exactly the mistake that bricks an update.

### Added
- `.gitattributes` pinning all text to LF (`* text=auto eol=lf`), with binary
  assets marked explicitly.

### Build environment (2026-08-11)
The Arduino toolchain was rebuilt for Linux after the Windows→Debian reinstall.
It had survived at `/mnt/datacifs/SixOfFive/claude/esp32/tools/` but as Windows
binaries (`PE32+ executable`), unusable here.

**The durable gotcha: `/mnt/datacifs` is CIFS mounted `nounix` and cannot create
symlinks** — `ln -s` returns `Input/output error`. The Linux GCC toolchain
contains them (`xtensa-esp-elf/lib64/libcc1.so → libcc1.so.0.0.0`), so installing
the esp32 core onto that share fails partway through extraction. The Windows
toolchain lived there only because Windows toolchains have no symlinks. **Test
`ln -s` on that share before installing anything that might need one** — this
applies equally to `node_modules`, Python venvs, and any other Linux toolchain.

Resolution: `arduino-cli` (Linux), both configs, and the libraries stay on the
share; only the compiler tree moved to local disk (`~/.arduino15`), which is also
much faster to build against than CIFS. `arduino-cli-linux.yaml` documents the
reason inline. Build command:

```
arduino-cli --config-file .../arduino-cli-linux.yaml compile \
  --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB" \
  --build-path ~/.cache/vroom-build esp32-s3/voltage_monitor
```

Note that `arduino-cli core install` is a **no-op when the version matches** — it
checks only that the platform is present, not that the binaries match the host
OS, so it reported "already installed" over a tree of Windows `.exe` files. A
toolchain moved between operating systems will not self-correct; uninstall first.

### Fixed
- 29 tracked files had been rewritten to CRLF by a Windows-side editor reaching
  this tree over the CIFS share, producing a ~9,400-line whitespace-only diff
  that buried a real uncommitted firmware change. Files converted back to LF and
  the repo immunized against a recurrence. **To undo:** delete `.gitattributes`
  and run `git add --renormalize .`.

---

## 2026-08-09

### Changed
- Docs no longer hardcode paths to the Arduino toolchain folder, which had moved.
  Build instructions now describe the toolchain generically (`8377886`).

---

## 2026-08-08 — fw 4.10 → 4.23

### Added
- **fw 4.10** `POST /powerup`: one-shot, parameterless endpoint that sets Wi-Fi
  power-save **off** and CPU **240 MHz**, both persisted to NVS. Fireable
  automatically like `/reboot`; also a footer link on the dashboard.
- **fw 4.11** "Since last start" tile (elapsed time) and a rolling event log with
  a viewer at `/logs` (auto-refresh at a settable, per-browser interval) plus raw
  `/logtext`.
- **fw 4.13** Verbose diagnostics into the event log, aimed at the drop-out hunt:
  reset reason at boot via `esp_reset_reason()` (power-on / software / brownout /
  **TASK-WATCHDOG** / panic), Wi-Fi associate / got-IP / **disconnect with reason
  code and name** / lost-IP, NTP sync callback, and a latched low-heap warning
  below 40 KB. SNMP polls and per-request logging deliberately excluded — they
  would flood the ring.
- **fw 4.15** Voltage-derived **engine on/off** ("Last charge (ran)"), logging
  `ENGINE ON/OFF` from the alternator-charging threshold (≥13.2 V, 0.3 V
  hysteresis) and persisting `last_run`. Catches key- and FOB-initiated starts,
  not just board-fired ones — so a start command with no charge means it did not
  crank.
- **fw 4.17** Event log **persisted to flash**. The PSRAM ring was wiped on every
  reboot, which made it useless for "what happened right before it rebooted".
  Now mirrored to a rolling LittleFS file: append to `/log.txt`, rotate to
  `/log.old` at 48 KB (one generation, ~96 KB disk cap), and replay the tail of
  both into the ring on boot so `/logtext` shows cross-reboot history.
- **fw 4.19** Wi-Fi telemetry in `/json`: `ssid`, `bssid`, `ch`, `phy`,
  `txpwr_dbm`, `proto`. Revealed the car is on the `IoT` SSID.
- **fw 4.20** Dashboard split into **lazy-loaded tabbed pages** (Main / WiFi-Net /
  Voltage / CPU / Mem-Disk / Log / Update) over shared, versioned `/app.css` and
  `/app.js?v=`, plus `/history?cols=` so each page pulls only its own columns.
  Added a Wi-Fi **link-rate (Mbps) graph** from the negotiated PHY. Motivation:
  the single page was too heavy for the car's weak link.
- **fw 4.21** `/logpage?p=N` — server-side log pagination returning the total plus
  one 25-line page, newest-first (~1.5 KB per view instead of the whole log).
- **fw 4.21** **WDT-culprit breadcrumbs**: both watched tasks stamp a "what am I
  doing" marker into `RTC_NOINIT` memory, which survives a reset. On the next
  boot, if the reset was `TASK_WDT`, the persistent log records
  `^ WDT stall: loop was 'X', safety was 'Y'`. Diagnostic only — the fix was
  deferred.

### Changed
- **fw 4.11** `computeDrain()` is now a **two-variable least-squares fit**
  (V ~ time + temp). Chip temperature explains ~55% of parked voltage wobble
  (+5.1 mV/°C, r = 0.74 over 26 h), so the plain V-vs-time slope was noisy.
  Immediate payoff: the raw fit read ~−24 mV/h, temperature-compensated reads
  **~−4 mV/h** — most of the apparent drain was thermal, not depletion. Falls
  back to the plain fit when temperature is flat. The **trigger still runs on raw
  voltage** (a cold dip is a valid reason to start).
- **fw 4.12** Event log lines carry full `YYYY-MM-DD HH:MM:SS` (was time-only),
  with `TZ` set early so even the boot line is local time.
- **fw 4.14** Disconnect logging debounced: out of range the driver retries every
  few seconds and each retry fired an event, which would have dumped 50+ identical
  `no-AP-found` lines and blown the ring. Now logs the first, suppresses identical
  repeats for 2 minutes, and reports `[+N more suppressed]`.
- **fw 4.15** Event-log ring moved to **PSRAM at 1000 lines** (was 120 in DRAM);
  DRAM use fell from 22% to 18%.
- **fw 4.16** Min/max dashed reference lines and labels on **all** graphs (was a
  subset), with exact extremes even for flat metrics.
- **fw 4.20** Log tab rebuilt newest-first, paginated at 25/page. History format
  magic bumped `VOL5` → `VOL6` for the stored link-rate column.

### Fixed
- **fw 4.22** **CPU button regression from the 4.20 tab split.** The CPU *page*
  was registered as `server.on("/cpu", handleCpuPage)` with no method, so it
  matched GET *and* POST and shadowed the `HTTP_POST` handler — `POST /cpu`
  returned page HTML and the button reported "CPU request error". All page routes
  are now explicitly `HTTP_GET`. Every route was audited; `/cpu` was the only
  shadowed one.
- **fw 4.22 → 4.23** **Spurious drain-rate spikes and bogus ETAs.** A
  just-settled or reboot-spanning least-squares fit throws a steep false slope —
  one read **−112 mV/h → ETA ~4 h** when true parked drain is a few mV/h
  (days to weeks). 4.22 gated on r² ≥ 0.6 and a ≥1 h window, but a
  reboot-spanning fit (8810 s window against 56 min uptime) passed both. 4.23
  adds a physical-plausibility cap: **|rate| > 40 mV/h is not real parked drain**
  (it is a reboot-spanning fit, a CPU-clock voltage step, or post-drive
  surface-charge settling), so `drainTrusted()` returns false, the graph reads 0
  and the ETA reads −1 ("settling"). Safe because the **trigger is
  voltage-threshold based, not rate based** — capping the projection can never
  change whether it fires.

### Known issues
- Isolated **TASK-WATCHDOG resets** correlated with Wi-Fi loss (fw 4.19 and 4.20).
  The board self-recovers in ~30 s, so this was deferred. The tight 85 s reboot
  loop seen earlier was a separate problem, caused by 4.18 and fixed by 4.19
  (`476cefd`).
- OTA over the car's ~−70 dBm link is unreliable; the ~1.2 MB upload frequently
  drops mid-transfer. Flash from close range.
- A **rogue DHCP server** on the LAN periodically overrides the board's
  `192.168.15.94` reservation. Reach the board at `esp32-volt.local` — mDNS
  follows the lease.

---

## 2026-08-08 — fw 4.18 / 4.19, reverted experiment

### Changed
- **fw 4.18** forced `esp_wifi_set_protocol(11B)` plus max TX power, trading
  throughput for range at the car's weak spot. Against this AP it **backfired**:
  latency rose to ~1.7 s with 20% loss, and a watchdog-fed task starved into a
  **TASK-WATCHDOG reboot loop every ~85–95 s** (heap and CPU were fine, so a
  stall rather than exhaustion). The disconnect log showed the client fighting
  the AP with auth-expire, no-AP-found, and 4-way-handshake-timeout.
- **fw 4.19 reverted** to default b/g/n and the loop stopped; the board re-linked
  at 11n-HT40 with 0% loss. **Gotcha worth keeping:**
  `esp_wifi_set_protocol` **persists to NVS**, so reverting required *explicitly*
  setting b/g/n to overwrite the stored b-only value — simply removing the 4.18
  call would not have undone it. Max TX power was kept (harmless).

---

## 2026-08-07 — fw 4.7 → 4.9

### Added
- **fw 4.7** Voltage min/max reference lines; uptime and combined CPU-load panels
  in the dashboard hero.
- **fw 4.8** Min/max reference lines extended to temperature, RSSI, both CPU
  cores, and drain.
- **fw 4.9** Dashboard **reboot** button, for a manual kick when the board is
  installed behind the dash and cannot be power-cycled by hand.

### Changed
- **fw 4.9 — reliability hardening for deployment.** A deployed safety device
  must not have its protection silently disabled by a flaky network. After the
  board went HTTP-unresponsive post-flash at the weak in-car signal, the
  realization: a hung `loop()` also stops `evalAutoStart()`, so low-voltage
  protection would stay offline until a manual power-cycle.
  - Sampling and the auto-start decision moved to a **dedicated FreeRTOS task on
    core 0**, independent of the `loop()` on core 1 that runs Wi-Fi/HTTP/SNMP.
  - The safety task **owns the ADC and temperature sensor**; everything else
    reads cached `g_lastV` / `g_lastTemp`, so there is no cross-core peripheral
    contention.
  - An **RF mutex** serializes an auto-fire against a manual `/transmit`, so the
    CC1101 SPI and the start log are never touched concurrently (no double-fire).
  - A **task watchdog (~30 s, panic-reboot)** watches both tasks, turning "hung
    until someone notices" into "self-heals in seconds".

  Trade-off accepted: a watchdog reboot resets the in-RAM sustain countdown, so a
  low battery restarts its 60 s hold after the ~5 s boot. Reboots are rare in
  normal operation.

---

## 2026-08-06 — fw 4.0 → 4.6, **remote start works**

### Fixed
- **fw 4.0** Added the **~1.44 s wake-up carrier** and 8× data repetition. The
  Compustar receiver is duty-cycled, and the FOB precedes its data with a long
  carrier to wake it. This is why nothing — Lock included — had ever worked.
- **fw 4.1** Matched the FOB's packet timing: inter-packet gap **1 ms** (was
  39 ms, 35× too wide, letting the receiver lose bit-clock lock between packets)
  and a **0.525 s trailing carrier** that was missing entirely.
- **fw 4.2** **Swapped data-bit gap timing — the defect that hid for months.** A
  1WG3R bit is a pulse *plus a matching gap*: `0` = short-HIGH + **short**-LOW
  (~1493 µs period), `1` = long-HIGH + **long**-LOW (~2243 µs). The firmware had
  the LOW values swapped, making both periods ~1850 µs and indistinguishable to a
  receiver decoding by period. It stayed hidden because an rtl_433-style decoder
  reads only the HIGH pulse, so the signal always *looked* bit-perfect.
  **Lesson: measure the LOW/gap, not just the HIGH.**
- **fw 4.3** **`RF_START_BURSTS` 3 → 1 — this is the change that cranked the
  engine.** The FOB sends exactly one burst per press; a second Start ~2.5 s later
  reads as a re-press and *cancels* the start. Three bursts were self-cancelling.

  Verified via the OBD-II battery tap, which doubles as a start detector: a
  board-fired Start reproduces the FOB's crank-dip → ~14.3 V alternator-charging
  signature, and a second Start stops the engine.

### Added
- **fw 4.4** **ETA to the next auto-start**, projecting the parked drain rate down
  to the trigger voltage plus hold. Dashboard card, `as_eta_s` in `/json`, and
  SNMP OID `.44`. Auto-start **armed** on the installed unit.
- **fw 4.5** Battery **drain-rate graph** over time — the actual parasitic-leak
  hunting tool.

### Changed
- **fw 4.6** The drain tile reports honest partial progress while its window
  fills, instead of a bare "0 of 30".
- Docs finalized on the **ESP32-only** scope decision: one board does battery
  sensing, dashboard, SNMP, and the remote-start trigger. The Pi telematics half
  is kept in-repo as revivable reference but is not part of the shipped system.
