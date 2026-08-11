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

## 2026-08-10

### Added
- `.gitattributes` pinning all text to LF (`* text=auto eol=lf`), with binary
  assets marked explicitly.

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
