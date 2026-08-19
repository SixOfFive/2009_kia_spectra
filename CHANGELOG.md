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

## 2026-08-19 — fw 4.57: engine-off edge 12.90 V -> 13.10 V (surface charge straddled it)

### Fixed — a run that ended at 12:57 had not closed 37 minutes later

The off edge was `AS_ALT_V - 0.3` = **12.90 V**. That is inside the band a
freshly-charged battery sits in. After two drives today the resting voltage
plateaued at **12.91–12.99 V** — surface charge, not depletion — so the detector
never saw the engine stop:

```
12:57       engine actually stops
12:58       13.11 V   -> detector: still running
13:06       12.98 V   -> still running
13:20       12.94 V   -> still running
13:34       12.90 V   -> still running, 37 min after shutdown
```

Left alone it self-clears once surface charge decays, but the OFF is
timestamped **when the voltage first dropped**, so the run would have been
recorded as ~76 minutes instead of ~39, and the long-term drain anchor — taken
12 h after engine-off — would start from the wrong moment.

The off edge is now an explicit constant rather than an offset that no longer
means anything:

```cpp
const float AS_ENG_OFF_V = 13.10f;   // below this, held AS_RUN_OFF_S, a run ends
```

Chosen from the measured separation on 2026-08-19:

| | Range |
|---|---|
| Engine actually running | **13.58 – 14.27 V** (14.2 at start, tapering to 13.6) |
| After shutdown, settling | **12.91 – 13.11 V** (decaying) |

`AS_RUN_OFF_S = 120` is what makes a threshold this close to the running floor
safe: every dip that caused the 2026-08-17 fourteen-runs bug was under 60 s, so
the 2-minute hold still rejects them.

### Verified — the 4.51 debounce itself works

Today's 08:26 → 09:01 drive logged as **exactly one ON/OFF pair, 2099 s**. That
was the outstanding verification from 4.51, where one continuous drive had been
recorded as fourteen separate runs.

### Known — one dangling ON in the run log

`engRunning` is a plain function-static, so it zeroes on boot. Flashing this
while the 12:18:55 run was still open means **no OFF will ever be written for
it**. Deliberate: the alternative was waiting for the old firmware to close it
at a ~45-minute-late timestamp, recording a confidently wrong 76-minute run.
A visibly missing OFF is better than a plausible wrong duration.

The general case is unhandled too — a reboot mid-drive produces a second ON with
no intervening OFF. A boot-time reconciliation (close a dangling run when
voltage is clearly below `AS_ENG_OFF_V`) would fix both; not done here.

### Notes

- Charging system confirmed healthy while investigating: **14.27 V** peak,
  tapering to 13.6 V as the battery stopped accepting current.
- Unexplained: a single sample at **13.65 V at 13:04**, seven minutes after
  shutdown, with neighbours near 12.98.
- Asset tags `?v=456` -> `?v=457`; the Last-charge tooltip quoted the old
  threshold.

---

## 2026-08-18 — fw 4.56: %/day gets its own line instead of wrapping mid-phrase

### Fixed — the inline span broke across lines

4.53 hung the `%/day` off the end of the value as an inline span, so the
Long-term rate card rendered:

```
LONG-TERM RATE
-5.49 mV/h · 12.7
%/day
```

Two causes, both in `.card .v`. It carries **`word-break:break-word`**, and the
grid track is **`minmax(158px,1fr)`** — a 165 px card cannot hold a 19 px value
plus a 14 px suffix on one line, so the number and its unit were split across
the break.

Now a real second line, `.card .sv`, rather than a span squeezed onto the first:

```
.card .sv{color:var(--mut);font-size:13px;font-weight:600;
          margin-top:4px;line-height:1.2;white-space:nowrap}
```

The interpunct separator went with it — it existed only to divide two things
sharing a line. Both cards read as a clean three-row stack:

```
LONG-TERM RATE          BATTERY DRAIN RATE      PROJECTED TO 11.8 V
-5.49 mV/h              -6.3 mV/h               4.9 days
12.5 %/day              14.2 %/day
```

### Verified — measured, because the obvious check was wrong

Confirmed against the live board at 1280 px and at 375 px mobile: three rows at
y=490/515/543, all grid cards uniform at 165 px, no horizontal page scroll.

Worth recording how that check went, because the **first method was unsound**.
Testing overflow with `scrollWidth > clientWidth` reported `false` for a
deliberately absurd 40-character string — which should obviously have
overflowed. It had not: `clientWidth` itself grew 89 px -> 331 px. With
`white-space:nowrap` **inside an `auto-fit` grid track, long text widens the
card rather than clipping it**, so the two measurements move together and the
comparison can never fire.

Re-measured the *card* instead of the span. Width is 165 px with the live value
and still 165 px at the worst realistic string (`-100.0 %/day`), so the nowrap
never actually stretches anything. The latent behaviour is real but out of
reach at any value this card can produce.

### Notes

- Asset cache tags `?v=455` -> `?v=456`. CSS changed this time, not just JS.
- Hierarchy is deliberate: mV/h stays the primary value since it is what the
  card measures and what its title names; %/day is the derived gloss beneath.

---

## 2026-08-18 — fw 4.55: %/day on the short-term card too, and a corrected caption

### Added — the same `%/day` on the 24 h "Battery drain rate" card

4.53 put `%/day` beside the long-term rate only. It now appears on the
short-term card as well, from the same `socPct()` fit:

```
BATTERY DRAIN RATE           PROJECTED TO 11.8 V
-6.3 mV/h  ·  14.3 %/day     4.9 days
fit r^2=0.80 (usable) | 24.0 h window | 1440 samples | temp-comp 7.1 mV/degC
```

The figure **dims to `#6e7681` when r² < 0.6**, so a number the fit cannot
support visibly recedes instead of reading as confidently as a good one.

Useful as a cross-check: the two cards are independent fits over different
spans and currently read **14.3 %/day (24 h least-squares)** against
**12.5 %/day (long-term anchor)**. Agreement at that level is a much stronger
statement about the ~280 mA parasitic drain than either card alone.

### Fixed — 4.54's caption described the wrong window

4.54 shipped this with the caption *"%/day extrapolates this short window"*, on
the belief that the short-term fit spans ~30 minutes. **It does not.**
`DRAIN_MIN_N = 30` is the minimum sample count before any slope is reported —
not the window length. The fit actually spans the **whole 24 h ring**:
`drain_win_s` reads 86438 s over 1440 samples.

So a daily figure is a fair read of that fit, not an extrapolation from a short
lever, and the caption contradicted the `24.0 h window` printed immediately
beside it. The real weakness of this card is a different one, and is now what
the caption says:

> %/day follows this window, which restarts on every reboot and carries the
> day/night thermal swing — the long-term card is the steadier daily figure

That is also precisely why the long-term card exists at all, so the caption now
points at the right distinction instead of an invented one.

4.54 was flashed and superseded within the hour; it is not a release worth
keeping.

### Notes

- Asset cache tags `?v=453` -> `?v=455`.
- No new `/json` fields — still computed browser-side from `vbatt` and
  `drain_mvph`, so this remains free on the wire.

---

## 2026-08-18 — fw 4.53: the drain rate also reads as % of battery per day

### Added — `%/day` beside the long-term mV/h

The Long-term rate card showed `-5.57 mV/h` and nothing else. That number is
only meaningful to someone who already knows the conversion; nobody looks at
millivolts per hour and feels how fast the battery is emptying. It now reads:

```
LONG-TERM RATE
-5.57 mV/h  ·  12.8 %/day
```

The conversion runs entirely in the browser from `vbatt` and `lt_mvph`, both of
which `/json` already carried. **No new JSON fields, no extra bytes on the
wire** — the link is poor enough that this mattered more than the convenience of
computing it on the board.

The card gained a tooltip that shows its own working, because 12.8 %/day is an
alarming number and it should be auditable rather than taken on faith:

> -5.57 mV/h is -134 mV/day; stepped down the rested lead-acid curve from
> 12.59 V that is **13.0 %/day**, so a full battery would reach flat in about
> 7.7 days of sitting. Charge now reads about 86 %. Cross-check with the
> power-budget rule I(mA) = |mV/h| x C(Ah): 279 mA at 50 Ah.

That cross-check is the point of quoting it: `docs/power-budget.md` §6 predicts
**-1.2 to -1.9 mV/h for car + board as designed**, and the board is measuring
-5.57. The two independent routes to a current — the SoC curve and the
power-budget rule — agree at ~280 mA, which is squarely the parasitic fault the
docs describe, not instrumentation error.

### Fixed — do not interpolate the published SoC table directly

First cut interpolated the standard rested-voltage table (12.73 V = 100 %,
12.20 V = 50 %, 11.31 V = 0 %). It shipped as 4.52 and read **14.2 %/day**,
which is wrong in a way worth recording.

The table is rounded to 10 % and 10 mV, so its **segment slopes jitter between
0.063 and 0.125 %/mV** — the 12.42-12.50 step reads *twice as steep* as
11.90-12.06. That is rounding, not chemistry. Slope is precisely what this card
consumes, so the artifact came straight through: at a perfectly constant drain,
the display swung

```
12.60 V -> 12.5 %/day     12.50 V -> 15.4 %/day     12.40 V -> 12.5 %/day
```

A ±20 % wobble driven by nothing but where the voltage happened to sit between
two rounded table rows. On a card whose entire job is to be the *stable*,
days-to-weeks number, that would read as the drain changing when it had not.

4.53 replaces the interpolation with a least-squares quadratic through the
10-100 % rows: `socPct(v) = 2732.89 - 520.682 v + 24.6611 v²`. It tracks every
table point within 2.3 points (mostly under 1.2), and its slope now rises
smoothly and monotonically — 0.066 %/mV at 11.9 V to 0.111 %/mV at 12.8 V —
which is the real shape of the curve rather than quantisation noise. Cubic was
tried and rejected: max residual 2.26 vs 2.34, no meaningful gain for the extra
term. The parabola's vertex is at 10.56 V, so it is monotonic across every
voltage a car battery can present.

Same sweep, smoothed:

```
12.80 V -> 14.3 %/day   12.60 V -> 13.0   12.50 V -> 12.4   12.20 V -> 10.4
```

Still voltage-dependent, but now for a physical reason: the curve genuinely
flattens as the battery drains, so a *constant current* shows up as a
*steepening* mV/h. Reading %/day instead of mV/h removes that distortion, which
is the second reason the conversion earns its place.

`socPct()` is deliberately **unclamped**. Clamping it at 100 % would zero the
slope above 12.73 V and report a real drain as `0 %/day` on a nearly-full
battery — the exact case this system exists to catch. Only the absolute charge
figure in the tooltip is clamped to 0-100; the difference never is.

### Notes

- `hr_ok` is now **true** with 103 hourly buckets, so the long-term fit is
  finally reporting on a settled regression rather than the two-point anchor.
- Caveat carried in the tooltip: the SoC curve assumes a rested, healthy
  battery near 25 °C, so it reads high while surface charge is still
  dissipating, and usable reserve is well short of the full 100 %.
- Asset cache tags `?v=449` -> `?v=453` (7 CSS + 5 JS references) because the
  shared `app.js` changed. 4.52 was flashed but superseded within minutes; it is
  not a release worth keeping.

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

## 2026-08-17 — fw 4.51: one drive was logged as fourteen runs

### Verified — the run history's live path works
The 2026-08-17 drive produced 32 events with `flags=0` (no reconstructed marker)
and `src=2` (key/FOB), confirming `runLog()` → 8-slot queue → `flushRunsToFlash()`
from the safety task. That was the last unproven piece of 4.46.

### Fixed — but the data it produced was wrong
One continuous drive, **13:21:24 → 14:57:30, was recorded as fourteen separate
runs**, several lasting 1–2 seconds:

```
13:22:01  OFF  12.77 V  ran 37s
13:22:09  ON   14.16 V     (+8s)
13:22:26  OFF  12.66 V  ran 17s
13:22:28  ON   13.48 V     (+2s)
...
14:13:11  ON   13.29 V
14:13:12  OFF  12.52 V  ran 1s
14:13:13  ON   13.27 V     (+1s)
```

The engine-state detector had **voltage hysteresis but no time hysteresis**. The
off edge fires at `AS_ALT_V - 0.3` = 12.9 V, and this alternator dips below that
transiently at idle and under load. The data proves the signal really is crossing
the line rather than measurement noise: every spurious `OFF` read **12.52–12.90 V**
while every `ON` read **13.2–14.2 V**. Voltage alone cannot separate a dip from a
shutdown.

Time can. An edge must now persist before it counts — `AS_RUN_OFF_S = 120`,
`AS_RUN_ON_S = 5`. Chosen against this dataset: every artefact was under 60 s,
and the shortest gap that looked like a genuine stop was 164 s.

**The edge is timestamped when it first appeared, not when it was confirmed**, so
run durations and the gaps between runs stay exact rather than being shifted by
the debounce. `runLogAt()` carries that timestamp; `g_lastRunTs`, the run
duration and the 12 h drain baseline all use it.

This mattered beyond cosmetics: `g_lastRunTs` moves on every ENGINE ON, so each
false edge also reset the drain baseline and discarded the in-progress hourly
bucket. Fourteen times in ninety minutes.

### Added — `POST /runs?reset=1`
Discards the recorded run history and rebuilds it from the reconstruction table,
keeping the six backfilled entries and dropping the artefacts this bug wrote. Not
invoked automatically — the junk entries are real observations of voltage
crossings, just wrong about what they mean, and that is the owner's call.

### Worth watching, unrelated to the logging
The alternator reading **12.52 V while driving** is low. It may be nothing more
than idle plus load on a tired battery, but it is the kind of thing this archive
now records hourly, and the year view will show whether it drifts.

---

## 2026-08-17 — fw 4.50: OTA was resetting the board, and boot lines were six hours out

### Fixed — OTA tripped the *interrupt* watchdog, not the task watchdog
A dozen consecutive uploads failed, stopping at wildly varying byte counts
(66 KB, 97 KB, 597 KB, 809 KB, sometimes the full 1.28 MB with no reply). The
cause was in the log all along, on the boot line after one of them:

```
2026-08-17 12:36:55  boot: fw 4.48, CPU 80 MHz, reset=interrupt-watchdog
```

Not `task-watchdog` — the **interrupt** watchdog, which fires when interrupts
stay disabled too long. `Update.write()` erases 4 KB sectors with the cache off,
and on the S3 that stalls **both** cores. Two things were making it worse:

- **Core 0 was still touching the filesystem mid-OTA.** `recordSample()` calls
  `LittleFS.usedBytes()` for `disk_kb` every 60 s, and the loop runs five
  `flush*ToFlash()` helpers. A second flash user during an erase is exactly how
  that watchdog fires. A `g_otaActive` flag now suspends all of it; `disk_kb`
  reuses its last value for the duration.
- **No yield between chunks.** Back-to-back erases never let interrupts run. A
  1 ms yield every 32 KB costs ~40 ms across a 1.3 MB image and stops it dead.

Diagnosis was slower than it should have been because **`/update` reported
nothing**. Failures went to `Serial`, which is unattached in a parked car, so a
dozen attempts left no trace. Flagged as a nice-to-have days ago; it actively
blocked this. Now `Update.errorString()` is returned in the response body *and*
written to the event log, along with accept/abort:

```
2026-08-17 12:46:58  OTA accepted: 1279136 bytes, rebooting
```

**What was claimed as verification does not hold.** 4.50 was re-flashed with no
rate limit — 24.4 s, `HTTP 200 OK`, clean reboot — and that was presented as the
in-firmware change working. The laptop's Wi-Fi had already been fixed by then, so
the result says nothing about these changes. See the retraction below.

### Fixed — early boot lines were stamped in UTC
`setenv("TZ")/tzset()` ran *after* the archive restores, so those lines were six
hours ahead of the boot line printed in the same second:

```
2026-08-17 18:36:55  hourly archive restored: 72 hours from flash
2026-08-17 12:36:55  boot: fw 4.49, CPU 80 MHz, reset=software
```

Cosmetic, but it reads as a clock fault in a log meant to be trusted. The
timezone is now applied as the first thing `setup()` does, before anything can
call `logLine()`. All three lines now agree:

```
2026-08-17 12:45:31  hourly archive restored: 72 hours from flash
2026-08-17 12:45:31  daily archive restored: 1 days from flash
2026-08-17 12:45:31  boot: fw 4.50, CPU 80 MHz, reset=software
```

### Retracted: the "correction" to the 4.49 entry was itself wrong
This entry originally claimed the 4.49 RF diagnosis was a mistake. **It was not.
The link really was degraded — on the laptop, not the car.** The owner switched
MOBILE off 5 GHz and the problem went away; measured afterwards on a healthy
link, loss to the gateway is **0%** at 540 Mbit/s, against 46.7% during the
failures.

Two errors produced the bad retraction, both worth keeping:

- **The inference was backwards.** Identical loss to the board, the gateway *and*
  a wired host was read as "the measurement must be broken". Those three paths
  share a first hop — the laptop's own Wi-Fi — so identical loss across all of
  them is the strongest possible evidence that **the shared hop is the fault**.
  It pointed straight at the answer and was used to argue the opposite.
- **The supporting throughput number was meaningless.** "TCP to the NAS at
  16.2 MB/s" was a `dd` of a file the compiler had written minutes earlier: page
  cache, not network. It never touched the wire.

### Consequently, the OTA fix below is NOT verified
The 4.50 changes were declared proven because a full-speed flash succeeded in
24.4 s without a rate limit. **That test is confounded** — the laptop's link had
been repaired by then, which alone explains it. The changes remain defensible on
their own terms (a second core must not touch the filesystem while
`Update.write()` erases with the cache off, and `/update` must report failures
somewhere that survives), but there is **no evidence yet that they fixed
anything**, and the `interrupt-watchdog` reset may well have been a *consequence*
of a stalling upload rather than an independent fault.

Treat the OTA fix as untested hardening until a flash fails again, or does not.

---

## 2026-08-17 — fw 4.49: countdown flapping collapses to two lines per 30 minutes

### Fixed — the flap consolidation could never collapse the countdown
The 4.35 mechanism keys on the **full line text including voltages**, so
`12.19 V` and `12.20 V` are different strings and the repeat count never
accumulates. A battery dithering across the trigger therefore filled the
1000-line ring with near-identical pairs and buried everything worth reading:

```
11:47:22 LOW-V countdown STARTED: 12.19 V below 12.20 V, need 60s  [repeated 2 times]
11:47:07 LOW-V countdown RESET after 1s of 60s (voltage recovered) at 12.22 V
11:47:06 LOW-V countdown STARTED: 12.20 V below 12.20 V, need 60s
...
```

The countdown no longer logs per event at all. Events accumulate into a
**30-minute window** which emits exactly **two lines**, one per kind, with the
voltages as a range so different values fold together instead of splitting:

```
countdown: 10 starts/30m, 12.18-12.20 V (trigger 12.20, need 60s)
countdown: 15 resets/30m, 12.19-12.22 V, max 30s/60s -- voltage recovered x14, park-confirm x1
```

Reset **reasons are preserved with counts**, so the nine distinct guards that
4.34 made visible are not lost to the batching — a window that is mostly
`voltage recovered` but contains one `park-confirm` still says so.

A window is also **closed early**, before an auto-start fires or an engine edge
is logged, so a summary never lands after the event it led up to.

Lines are deliberately terse: `LOG_LEN` is 108 including a 20-character
timestamp, leaving 88 for the message, and the reason list is budget-capped so it
truncates rather than pushing the counts off the end. Worst case measured at 103.

Verified by simulating the aggregator against the reported burst before
flashing: 20 log lines become 2, mixed reasons survive with counts, and a lone
countdown still produces a correctly-singular summary.

### Flash blocked — the link, not the firmware
Could not be OTA'd when built. The board is healthy and responsive, but:

```
ping:  15 sent, 8 received, 46.7% loss, RTT to 212 ms
rssi:  -58/-59 earlier today -> -61 to -65
OTA:   809,697 of 1,278,839 bytes before the connection dropped
```

Small single round-trip requests still succeed (11 of 12), which is why the
dashboard stays usable while a 1.28 MB sustained transfer cannot finish. Nothing
in this release is implicated; it is the same RF-degradation class as the
original `IoT` AP problem, now on `Password is Taco` ch10.

---

## 2026-08-16 — fw 4.48: the detail popup's tooltip was painting behind it

### Fixed — a z-order bug that read as "the tooltip shows the wrong content"
Reported after 4.47: hovering inside the detail popup produced tooltip content
belonging to the page underneath, no matter where the pointer moved.

`#tip` is `z-index:50`; the popup overlay is `z-index:60`. **The tooltip was
rendering behind the backdrop.** What showed through the 82%-opaque overlay was
the layer beneath, which is exactly what it looked like. `#tip` is now `80`, with
the ordering written down where the popup is defined so it cannot drift again:

```
page  <  #gmod (60)  <  #tip (80)
```

Second, smaller cause: the delegated `data-tip` handlers fire on every document
`mousemove`/`mouseover`, including over the popup. A card tip from the page below
could overwrite the point being hovered. Both handlers now bail out while the
popup is open — the popup owns `#tip` for as long as it is up.

### Verified — including the WiFi page specifically
```
charts wired:  g_rssi, g_link, g_nin, g_nout
popup opens:   "WiFi RSSI dBm", 248 of 288 buckets
z-index:       tip 80 > modal 60          (was 50 < 60)
tip class:     ""  -> the graph tooltip, not the "rich" card tooltip
after a stray document mousemove: the popup tooltip SURVIVES
```

That last check is the regression itself: before this, a background card tooltip
could clobber the popup's on any pointer movement.

---

## 2026-08-16 — fw 4.47: click any graph for a full detail view, and a year of history

### Added — a daily tier, so "year" means something
The hourly archive covers ~125 days across two generations, and a year view wants
daily resolution anyway (365 points). So there is now a daily bucket alongside the
hourly one: `/daily.bin` + `.old`, 400 records per generation, **~2 years**.

Deliberately reuses the existing `HourAgg` record rather than introducing a
second type — no new on-disk format, and therefore **no third format migration**.
The first two both shipped with bugs (4.38, 4.39); not spending that risk again
for a struct that would have been identical anyway.

Unlike the hourly bucket, the daily one is **not** discarded when the engine
starts. A day the car ran is real data, and the stored min/max make the 14 V
excursion visible instead of burying it in the mean.

### Added — `/agg?span=year` and `&full=1`
`year` = 365 buckets × 1 day from the daily archive. `full=1` adds each bucket's
min and max next to its mean — roughly triple the payload, which is why the
always-on inline charts never ask for it and the click-through popup always does.
One series, user-initiated, so it is affordable there.

### Added — the detail popup
Clicking **any** graph opens it larger with its own range selector
(24 h / 7 d / 30 d / 1 y) and considerably more information:

- **Min/max envelope** drawn as a band — the spread inside each bucket, which a
  line of means hides entirely.
- **Dashed window extremes, labelled in-graph** (`max 12.37 V`), scaled so they
  are always on-canvas.
- **Stat row**: latest, mean, minimum, maximum, range, coverage.
- **Rich tooltip** per point: value, the bucket's own low/high and spread, where
  it sits in the window as a percentage, the bucket's time span, and how long ago.

```
12.31 V | Sun, 15 Feb 2026 | mean over 1 day
low 12.19 V · high 12.40 V | spread 0.21 V | 75% of the way up this range | 181d 12h ago
```

Built in JS and appended to the DOM at load, so all four chart pages get it
without touching any page template.

### Fixed during verification
- **A day-wide bucket rendered as `02:10:26 – 02:10:26`.** The end of a 1-day
  bucket is the same clock time as its start, so `toLocaleTimeString()` printed
  both ends identically. Day-sized buckets now show the date alone.
- **DRAM went 24% → 36%** when the per-bucket extreme buffers were added as
  static arrays (~56 KB for 365 × 11 × 3). Moved to PSRAM, allocated once at
  boot: DRAM is now **19%**, *better* than 4.46, because the old 288-sized
  buffers moved with them. `/agg` returns 503 rather than misbehaving if that
  allocation ever fails. Free heap on the board went 193 KB → 219 KB.

### Verified
Against a mock serving the real HTML/CSS/JS, mirroring the live board's shape —
day and year populated, week/month nearly empty, `drain` absent entirely:

```
modal opens on click, no JS errors
year   365 of 365 buckets · 1 day each, envelope present, 123k lit px
week   2 of 168 buckets      (sparse path)
drain  0 of 168 buckets -> "not recorded for this range yet"
```

Then on hardware across all four spans, inline and `full=1`.

### Expect it to fill in
The year view is empty until the **first midnight rollover** writes a daily
bucket, then gains one point per day. Same shape as every other tier here: the
data cannot be reconstructed, only accumulated.

---

## 2026-08-15 — fw 4.46: a dedicated run history, kept for years

### Added — engine starts, stops and failed attempts in their own log
The event log answers "what happened recently" and rotates at 48 KB × 2, which
on this board is under a fortnight. "How long does this car sit between runs, and
how often does a start not take?" needs months. Those are different retention
problems, so they now have different stores.

`/runs.bin` + `/runs.old`, **16 B per event**, two generations of 2000 — roughly
4000 events, or years at the handful per day this car produces. Recorded:

| kind | when |
|---|---|
| `Start sent` | a burst was transmitted (flag records whether the CC1101 accepted it) |
| `Engine ON` | alternator came up, attributed **auto / manual / key-FOB** |
| `Engine OFF` | charging ended, with the run length |
| `No start` | fired, no charging inside the verification window |

Attribution is what makes "time between manual and auto starts" answerable later:
a start being verified is ours, anything else is the key or the FOB.

Engine edges are detected on the safety task (core 0), which must never touch the
filesystem, so `runLog()` queues into an 8-slot ring under its own `portMUX` and
the loop drains it — the same split already used for the drain buckets and the
sample journal. A full queue drops rather than blocks.

`GET /runs?n=N` returns newest-last CSV, default 200, capped at 1000, streamed
from the file tail so a months-deep history is never a megabyte transfer.

### Added — the run history section on the Logs tab
A table below the event log: when, event, source, volts, detail — plus an
explicit **"sat 6d 17h between runs"** row inserted between a shutdown and the
next start, which is the number the whole feature exists to surface.

### Added — one-time backfill of what could be recovered
The history would otherwise start empty. Six events were reconstructed and are
written **flagged `RUN_F_BACKFILL`**, shown as *reconstructed* in the table, so
they can never be mistaken for live records:

```
08/08 16:52:51  Engine ON   key / FOB      --      <- last_run in NVS
15/08 09:10:39  Start sent  auto       12.14 V  RF sent ok
15/08 09:13:39  No start    auto       12.16 V  no charge after 3m
15/08 09:45:27  Start sent  auto       12.16 V  RF sent ok
15/08 09:45:46  Engine ON   auto       13.29 V
15/08 10:11:28  Engine OFF  auto       12.89 V  ran 25m 41s
```

Provenance is recorded in source beside the table. The 08-08 entry is the value
of `last_run` read off `/json` before the 08-15 run overwrote it — auto-start had
never fired by then, so the source was the key or the FOB, and no end time was
ever stored. The 08-15 timestamps were derived independently and **cross-check
exactly** against the `/starts` ring (`1786806639`, `1786808727`) and `last_run`
(`1786808746`).

**Nothing older is recoverable.** The 1000-line log ring had already rolled back
only as far as 08:21 that morning, the sample ring holds 24 h, and the hourly
archive 22 h. The board never stored engine events durably — which is precisely
the gap this release closes.

### Verified
`/runs` returns all six with flags `128`/`129`; the table renders them newest
first with the reconstructed marker; boot logged `run history seeded with 6
reconstructed events (marked as such)`, and the seed is a no-op once a run file
exists.

**Not yet exercised:** the live path. The seed writes the file directly, so
`runLog()` → queue → `flushRunsToFlash()` only runs on a real engine event. Same
position the drain buckets were in after 4.36 — it will prove itself on the next
start, and the first live row is the one to check.

---

## 2026-08-15 — fw 4.45: a separate, adjustable retry gap for starts that drew no charge

### Changed — the cooldown was governing two different risks
`as_cool` applied equally to "the engine ran, don't fire again yet" and "the
engine never started, try again". Those are opposite situations:

- After a **verified** start the engine is running. Firing again would *toggle it
  off* — the 1WG3R start is a toggle — and waste fuel. A long gap is correct.
- After an **unverified** start there is positive evidence the engine is not
  running: no charging within `AS_VERIFY_S`. A further burst cannot toggle
  anything off, and waiting a full cooldown just drains the battery.

`POST /autostart?retry=N` (60–86400 s, NVS `as_retry`, default **300 s**), with a
**Retry after no-start** field on the dashboard. `/json` reports both `as_retry`
and `as_gap` — the gap actually in force right now.

Selection is on `g_asFails > 0`, which is non-zero exactly when the last
automatic start produced no charging, and is reset the moment one verifies.

### Fixed — the retry gap alone would not have worked
Investigating the 2026-08-15 failure properly showed the cooldown was **never the
governing limit**. After a failed start `g_needRearm` stays set, and re-arming
requires the battery to climb to `trigger + 0.15 V` and hold it for 600 s — a bar
an *uncharged* battery never reaches, because nothing charged it. So the retry
was actually released by the `AS_REARM_MAX_COOLDOWNS` escape hatch at
**2 × 900 s = 30 min**, which matches the observed ~35 minute gap far better than
the 900 s cooldown does.

Shipping a retry gap without addressing that would have changed a number nobody
was waiting on. So a start that fails verification now also clears the re-arm
requirement:

```cpp
g_needRearm = false; g_rearmS = 0;
```

Safe by the same evidence: no charging within 180 s means the engine is not
running, so a further burst cannot toggle a running engine off. Park-confirm, the
fail streak, the lockout and the 24 h cap all still apply, and the re-arm
hysteresis is untouched for starts that *did* work — which is the case it was
written for (a battery sitting just above the trigger re-firing every cooldown).

### Verified live
`as_retry 300` / `as_cool 900` / `as_gap 900` with `as_fails 0` — the verified
path selected correctly. `retry=30` rejected with
`{"ok":false,"detail":"retry must be 60-86400 s"}`; `retry=600` accepted and
`as_gap` correctly stayed at 900 because the last start verified.

The cooldown tooltip now shows both gaps, which one is in force, and why they
differ.

---

## 2026-08-15 — fw 4.44: adjustable lockout limit, after the first real auto-start

### The first auto-start ever fired — and the first attempt missed
`/starts` and the log, both of which only exist because 4.43 landed the night
before:

```
09:10:39  AUTO-START FIRED at 12.14 V -- RF transmit ok, waiting 180s for charge
09:13:39  auto-start FAILED: no charging after 180s -- fail 1 of 2
09:45:27  AUTO-START FIRED at 12.16 V -- RF transmit ok
09:45:46  ENGINE ON: alternator charging at 13.29 V          <- 19 seconds
09:45:46  start VERIFIED: engine running, charging at 13.29 V
10:11:28  ENGINE OFF: charging ended at 12.89 V after 25m 41s
```

**The starter never engaged on the first attempt.** Raw samples either side of it:

```
09:09:42  12.14 V
09:10:39  <- fired
09:10:42  12.17 V     three seconds later, HIGHER than before
09:11:43  12.16 V
```

Cranking this engine pulls 150–250 A and would drag the battery to roughly 10 V.
There is no dip at all, so the Compustar never acted on the command. That rules
out the starter, fuel and the battery; `tx=ok` rules out the CC1101 and SPI. The
burst was transmitted and not honoured.

Identical conditions 35 minutes later worked in 19 s. That signature — one miss,
then immediate success, nothing changed — is a **probabilistic RF link**, most
likely the wake-up carrier being marginal against the receiver's duty-cycled
listen window. Each burst has some chance of landing.

Also confirmed by this run: the 25m 41s duration is measured between the two
alternator edges, not commanded. The board sends one burst and never transmits
again, so run length is entirely the Compustar's own timer. And
`hourly bucket in progress discarded (engine started); 21 archived hours kept`
shows the 4.37 archive-retention change behaving as designed across a real run.

### Changed — the lockout limit is now configurable
`AS_MAX_FAILS` was a compile-time `2`. On a link that misses bursts
occasionally, two unlucky attempts latch the lockout on a car that starts
perfectly well — which is precisely what nearly happened here.

`POST /autostart?maxfail=N`, NVS-backed (`as_maxf`), sitting alongside `cool` and
`max24`, with a matching **Fails to lock out** field on the dashboard.

- **`0` disables latching entirely.** Both the tooltip and the help text state
  plainly that this removes the only guard against cranking a car that will never
  start.
- **Changing the limit re-evaluates an existing lockout.** Raising it from 2 to 4
  while latched at a streak of 2 clears the latch rather than leaving the board
  locked out by a rule that no longer applies. Lowering it never latches
  retroactively.
- Tooltips quote the live limit instead of a hardcoded `2`, and the lockout
  advice now separates the two cases: a dead starter repeats every time, a missed
  burst does not.

Verified live: `maxfail=4` → `as_maxfails 4`; back to `2`; `maxfail=999` →
`{"ok":false,"detail":"maxfail must be 0-255 (0 = never latch)"}`.

### Not done — worth considering separately
`as_cool` currently governs **both** the gap after a successful start and the
retry after a failed one, and those are not the same risk. A failed start is
positive evidence the engine is not running, so an early retry cannot toggle a
running engine off. A separate short retry gap for unverified attempts, keeping
the full cooldown after a verified start, fits this failure mode better than
raising the fail limit. Left alone because it changes firing behaviour on a real
vehicle.

---

## 2026-08-14 — fw 4.43: the auto-start fire path had no record that survives

### Fixed — the one-shot event was about to go unrecorded
Auto-start has never fired in this vehicle. Checking the fire path ahead of an
expected first trigger, **every outcome went to `Serial.printf` and nowhere
else** — and the board sits in a car with nothing attached to serial:

```
*** AUTO-START FIRED at %.2f V (tx %s) ***          Serial only
start verified: engine running (charging seen)       Serial only
auto-start: no charging after 180 s -- fail N of 2    Serial only
*** auto-start LOCKED OUT ***                        Serial only
```

`/logtext` would have shown `LOW-V countdown STARTED` and then nothing at all
about the fire or whether the engine caught. The `/starts` row would still record
`src`, `ok` and `ver`, but the sequence — transmit result, the 180 s verification
window, the fail count, the lockout latch and its reason — existed only on a
serial port nobody is reading.

All five are now mirrored to `logLine()`, so they reach the RAM ring, `/logtext`,
and flash via the loop's flush. Calling `logLine` from the safety task on core 0
is safe and already precedented (4.34's `clearLow`): it writes the ring under
`g_logMux` and never touches the filesystem.

The fire line also now states what happens next — `waiting 180s for charge` — so
a log read after the fact shows the expected next event rather than requiring the
reader to know `AS_VERIFY_S`.

---

## 2026-08-14 — fw 4.42: hovering a sparse graph now finds the nearest reading

### Fixed
Reported after flashing 4.41: only the 24 h range appeared to have per-point
mouseover. The tooltip was working; there was simply almost nothing to hover.
The week view holds **2 populated buckets out of 168**, so an exact-bucket hit
was required across a graph that is 99 % empty, and `showTip` correctly hid
itself everywhere else. Correct behaviour, unusable result.

- **Hover snaps to the nearest populated bucket** (`nearestIdx`). The snap is
  unbounded on purpose: the crosshair and dot move to whatever it landed on, so
  which reading is shown is never ambiguous. Hovering the far left of a week
  graph now reports the reading at bucket 166 rather than nothing.
- **A series with nothing recorded says so.** `drain` has no hourly history at
  all — the two surviving buckets came from the legacy conversion, which only
  ever held voltage and temperature — so its canvas was simply blank, which reads
  as broken. It now draws "not recorded for this range yet", and does not respond
  to hover.
- **A lone reading draws as a dot.** A polyline of one point renders nothing at
  all, so a range containing exactly one bucket was invisible.

Verified against a mock reproducing the live board exactly (2 trailing buckets of
168, `drain` absent): far-left hover → index 166 with the tooltip shown, empty
series → 807 lit pixels of placeholder text and `hoverIdx -1`, no exceptions.

---

## 2026-08-14 — fw 4.40/4.41: week and month chart ranges, aggregated on the board

### Added — 24 h / 7 d / 30 d on every graph
A range selector on all four chart pages (WiFi, Voltage, CPU, Mem/Disk), with the
choice persisted in `localStorage` so it survives navigation. Dashed min/max
lines already existed on the 24 h charts; they now apply to every span.

### Added — `/agg?span=day|week|month[&cols=]`, so the ESP32 does the reduction
A month of raw samples is ~44,600 rows and about 1.8 MB. That is not
transmittable over this link, and `/history` at 24 h was already the largest
transfer on the device at ~36 KB — **refetched every 30 s**, which is ~4.3 MB/h
with a chart page open, and the original cause of the `/history` watchdog stalls.

The window is divided into a fixed number of buckets and only the **mean** of
each is sent:

| span | window | buckets | source | payload (3 series, full ring) |
|---|---|---|---|---|
| day | 24 h | 288 × 5 min | raw sample ring | ~4.5 KB |
| week | 7 d | 168 × 1 h | hourly archive | ~2.3 KB |
| month | 30 d | 180 × 4 h | hourly archive | ~3.2 KB |

Every span costs about the same regardless of how much time it covers, and all
three are far smaller than the 24 h fetch they replace. Refresh cadence now
scales with the span too — 30 s / 5 min / 30 min — because a month view does not
change meaningfully between polls.

Three deliberate choices:

- **Window min/max ride in the header, not per row.** The dashed lines are
  window-wide, so per-row extremes would be pure overhead. Computing them on the
  board is also strictly *more accurate* than the client could manage: they come
  from the per-hour min/max, so they are the real extremes. **The maximum of a
  set of daily averages is not the maximum the battery reached.**
- **Empty buckets are omitted, not padded.** The row index carries the x
  position, so a gap in the record costs nothing to transmit. The chart lifts the
  pen across gaps rather than drawing a line through them, which would invent a
  reading that was never taken.
- **The client takes column order from the response header**, not from its own
  `PAGE.cols`. The board emits in its fixed series order, which only
  coincidentally matches on all four pages today.

### Changed — the hourly archive carries every graphed series
It stored `{ts, v, t}` in 12 bytes, so there was no long-term record of RSSI,
link rate, network, CPU or heap at all. Now one `HourAgg` per hour with
mean/min/max for all eleven series, 136 B (~3.3 KB/day, ~1.2 MB/year).

Floats rather than scaled ints: the drain fit needs the precision on `vbatt`, and
at this volume packing saves nothing that matters on a 10 MB filesystem — flash
wear is not a constraint (see 4.37).

**Migration written the way 4.39 taught me:** the format is versioned by a
`HAG1` magic, and a leading epoch can never collide with it, so the *absence* of
a header unambiguously identifies the old records. Legacy rows are converted with
the unrecorded series marked `NaN` rather than zero — a zero RSSI would be a lie
on a graph — and the rewrite seeds a new file first, removing the source only on
success. Verified live: `hourly archive: converted 2 legacy hours to the wide
format`.

### Fixed (4.41) — numeric padding wasted about a fifth of the payload
`String(float, decimals)` formats via `dtostrf` with width `decimals + 2`, which
**left-pads short values with spaces**: `drain=-2/ 0`, and ` 0` in every row.
Harmless to parse, but across 288 rows × several integer-valued series it was
close to a fifth of the transfer, which defeats the point of aggregating.
Replaced with `snprintf("%.*f")`.

Also fixed on the way: `String(float, uint8_t)` is genuinely ambiguous on this
core — `String(float, unsigned int)` and `String(long long, unsigned char)` each
win on one argument — and `agFromSample()` became the first function definition
in the sketch, which is where the `.ino` auto-prototype block is inserted, so
every type used in any function signature had to move above it.

### Verified
Against a mock serving the real HTML/CSS/JS before flashing (the fw 4.20
workflow), with a deliberate 40-bucket hole and true extremes wider than the
plotted means — the case that puts dashed lines off-canvas if the scaling is
wrong:

```
day    288 pts, 40 gaps, mn=12.13 mx=12.37, lo=12.09 hi=12.41   <- extremes enclosed
week   168 pts, step 3600, labels "7 d", tooltip "mean of 1 h"
month  180 pts, step 14400, "180 of 180 buckets"
hover over a gap -> tooltip hidden, no exception
```

Then on hardware: legacy conversion logged, `/agg` correct for all four pages ×
three spans, padding confirmed gone. Costs 19 KB of DRAM for the bucket
accumulator (19% → 24%), static rather than heap because a failed allocation in
the HTTP path is a worse outcome than the memory.

### Known limitation
**Week and month views start nearly empty and fill over time.** RSSI, link,
network, CPU and heap were never recorded hourly before 4.40, so that history
does not exist and cannot be reconstructed. Voltage and temperature carry over
only the two hours that survived. The `rgnote` coverage indicator ("N of M
buckets") exists to make this obvious rather than looking like a bug.

---

## 2026-08-14 — fw 4.39: the 4.37 migration never reached flash — 24 h of history lost

### Fixed — data loss, not a cosmetic bug
4.37's `/history.bin` migration read the old snapshot into the **RAM ring** and
then deleted the file, without ever writing those samples to the journal. The
`Serial` line even claimed `history: migrated /history.bin -> append-only
journal`; it had not. Only samples taken *after* boot were journaled.

So the 1440 migrated samples existed solely in volatile PSRAM. They survived for
as long as 4.37 ran and died at the next reboot — which was the 4.38 flash.
**~24 h of voltage history was destroyed and is not recoverable**; LittleFS has
no undelete and the source file was already gone.

Caught immediately after the 4.38 boot:

```
samples     8         <- not 1440; only the RTC batch survived
fs_wr_b     264       <- 8 x 32 B + 8 B header: the journal's FIRST ever write
hr_buckets  1         <- the 4.38 /drain.bin migration did work
```

`seedJournalFromRing()` now writes the entire ring into a fresh journal, oldest
sample first, and `/history.bin` is deleted **only if that write succeeds** — a
failed seed keeps the source so the next boot can retry instead of losing
everything. Any future path that puts samples in the ring from outside the
journal must call it before discarding its source.

### Why this got through
Two migration defects in three releases, from the same root cause: **treating
"the data is in the ring" as equivalent to "the data is safe."** It is not, and
the entire point of 4.37 was to separate those two ideas. Worse, the check that
was supposed to verify the migration — `samples` still reading 1440 — could only
ever confirm the RAM half, and I reported it as verification of the whole thing.

The honest lesson for persistence changes here: **verify across a reboot, not
within one.** A count that looks right on the boot that performed a migration
proves nothing about what is on flash.

### Ongoing risk: none
The steady-state path was never affected. On boot the ring is rebuilt from
`.old` + `.jrn` + RTC, all of which are on flash or in reset-surviving memory, so
no further loss is possible. The defect was confined to the one-time migration,
which has already run on this board.

---

## 2026-08-14 — fw 4.38: migrate the file 4.37 renamed without a migration

### Fixed
4.37 renamed the hourly bucket file `/drain.bin` → `/hourly.bin` and **shipped no
migration for it**, so the upgrade orphaned the old file and silently dropped the
buckets it held. Caught on the live board immediately after flashing:
`hr_buckets` went **1 → 0**, and with no file-management endpoint on the device
the stray `/drain.bin` could not be removed remotely.

`loadDrainFromFlash()` now adopts a pre-4.38 `/drain.bin` if `/hourly.bin` does
not exist yet, and otherwise deletes it. Either way the stray goes.

The cost here was one bucket and ~12 bytes, because the archive had barely
started accumulating. **The lesson is worth more than the damage:** the same
release contained a carefully-written migration for `/history.bin` — read once,
replayed, removed — and the second rename in the same commit got none. Renaming a
persisted file is a data migration whether or not it feels like one.

### Verified on hardware (fw 4.37 flash, 2026-08-14 13:37)
The parts that could only be checked by flashing:
- ~~**`/history.bin` migration works.**~~ `samples` stayed at **1440** across the
  upgrade and `disk_used` fell 151,552 → 102,400, i.e. exactly 49,152 bytes
  (12 × 4096) — the old snapshot read and removed. **This conclusion was wrong,
  see fw 4.39.** The samples reached the RAM ring, not flash; reading a healthy
  `samples` count one boot later does not prove persistence, and calling it
  "verified" on that basis was premature.
- **New counters live:** `fs_wr_b 0`, `fs_wr_n 0`, `sb_n 1` seven seconds after
  boot — one sample buffered in RTC RAM, nothing written to flash yet, which is
  the intended behaviour (first flush at 30 samples).
- Clean OTA: `boot: fw 4.37, CPU 80 MHz, reset=software`, 29.5 s upload,
  reassociated at −59 dBm.

---

## 2026-08-14 — fw 4.37: tiered sample storage (RTC RAM → journal → hourly archive)

### Changed — the history ring is no longer rewritten whole, every 10 minutes
`saveHistory()` wrote the **entire** 1440-sample ring — 46,092 bytes — to
`/history.bin` every `SAVE_MS` (600 s). Measured cost:

| | before | after |
|---|---|---|
| bytes/day | **6.64 MB** | **46 KB** (144× less) |
| bytes/year | 2.42 GB | 16.8 MB |
| filesystem commits/day | 144 | 48 |
| block erases/day | ~1,728 | ~24 (72× less) |
| history lost per reboot | **up to 10 min** | **nothing** |

Being straight about the motivation: **flash wear was never the failure mode.**
Spread over the partition's 2,528 blocks, 1,728 erases/day is 0.68 per block per
day — about **400 years** to a 100k-cycle endurance limit. The board, the car and
the battery all lose that race. The rewrite was worth killing for the last row of
that table: this board has taken a lot of watchdog reboots, and each one silently
discarded up to ten minutes of history.

Three tiers, each with different survival properties:

- **Tier 1 — RTC slow RAM** (`g_sb[30]`, 960 B). Every sample lands here first
  and touches no flash. `RTC_NOINIT` survives a watchdog reset, a software reset
  and an OTA reboot — every reset this board has actually taken — so batching 30
  samples costs no data. Only disconnecting the battery loses the tail, and that
  stops the car anyway. Replayed into the ring at boot.
- **Tier 2 — `/hist.jrn`**, append-only, two generations (`.jrn` + `.old`,
  ~46 KB each), rotated and never rewritten in place. Same shape as the event
  log, for the same reason. Written by the loop core only; the safety task sets a
  flag and never touches the filesystem.
- **Tier 3 — `/hourly.bin`**, one 12 B record per hour (~105 KB/year), rotated at
  62 days per generation.

The batch is also flushed on both engine edges, so an ENGINE ON/OFF transition is
never left stranded in RAM.

### Changed — the hourly archive survives an engine start
`resetDrainBuckets()` used to `LittleFS.remove()` the whole bucket file on engine
start. That was unnecessary: `computeHourlyDrain()` already skips everything
before `g_lastRunTs + DR_SETTLE_S`, so old buckets cannot influence the estimate.
Deleting them only destroyed the one long-term record the board keeps. Now just
the part-built hour is dropped — it straddles the run and is meaningless — and
`/drain.bin` becomes `/hourly.bin`, an archive that accumulates across parks.

### Added
- One-time migration: `/history.bin` is read once on first 4.37 boot, replayed
  into the ring, then removed, so upgrading does not throw away the last 24 h.
- `fs_wr_b` / `fs_wr_n` / `sb_n` in `/json` — bytes written, commits, and samples
  currently held in RTC RAM. Wear is now measured rather than assumed.

### Concurrency note
`g_sb` is written by the safety task on core 0 and drained by the loop on core 1,
so the count is guarded by its own `portMUX` (`g_sbMux`) exactly like `g_logMux`.
The flush consumes precisely the `n` records it wrote and shifts any sample taken
*during* the write down to the front, rather than zeroing the count — zeroing
would drop that sample from flash while leaving it in the RAM ring, and the two
would disagree after the next reboot.

### Build environment note (2026-08-14) — no change made
A **full core rebuild is slow, and that is expected.** Changing the FQBN (here,
the partition scheme) invalidates the warm build path and forces all ~200 core
objects to recompile. The share answers a directory listing in ~7.5 s and is
mounted `actimeo=1`, so each of those objects re-stats all five `-I` include
paths over SMB; a full rebuild runs 40+ minutes. Incremental builds hide this
entirely, because only the sketch recompiles when `~/.cache/vroom-build` is warm.

Locations are deliberate and stay as they are — libraries on the share (shared
with the Windows setup), core toolchain on local disk (CIFS `nounix` cannot hold
the symlinks it contains). Budget the time instead of moving things:

```
arduino-cli --config-file .../arduino-cli-linux.yaml compile \
  --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=custom" \
  --build-path ~/.cache/vroom-build \
  --output-dir esp32-s3/voltage_monitor/build-out \
  esp32-s3/voltage_monitor
```

---

## 2026-08-14 — correction: the partition scheme in the docs was right all along

**Retracting the note under fw 4.24 below**, which said
`esp32-s3/docs/voltage-monitor.md` was wrong to specify `PartitionScheme=custom`
because the real build used `app3M_fat9M_16MB`. The docs are correct. Both
schemes work *for OTA*, which is why the discrepancy went unnoticed for a dozen
releases — but they are **not** interchangeable for a USB flash, and the note as
written pointed at the one that destroys the filesystem.

Verified against the installed board:

```
disk_total 10354688 = 0x9E0000        # matches voltage_monitor/partitions.csv
LittleFS.begin(true)                  # default partition label: "spiffs"
```

- The sketch's own `partitions.csv` names that partition **`spiffs`**. Stock
  `app3M_fat9M_16MB` names it **`ffat`**, subtype `fat`. `LittleFS.begin()` looks
  up the label `spiffs` and would find nothing — no event log, no start history,
  no drain buckets.
- The two schemes have **identical app geometry** (`app0` @ `0x10000`, `app1` @
  `0x310000`, `0x300000` each). That is the whole reason `app3M_fat9M_16MB`
  builds have been OTA-ing successfully since 4.24.
- **OTA never rewrites the partition table** — it writes one app slot and flips
  `otadata`. The table on the device is still the one the original USB bootstrap
  flash laid down, i.e. the custom one. So the compile-time scheme has been
  irrelevant to every update actually performed.

The trap is a **USB** flash with `app3M_fat9M_16MB`: it *would* rewrite the table,
replace `spiffs` with `ffat`, and LittleFS would silently fail to mount on the
next boot. The 4.24 note recommended exactly that, and both the CHANGELOG build
command and `braindump.md` carried it forward.

Resolution: `PartitionScheme=custom` is now the single documented FQBN everywhere
— docs, CHANGELOG and braindump — since it is correct for both paths. No firmware
change; `voltage_monitor.ino` is untouched.

---

## 2026-08-14 — fw 4.36: build stamp, and hourly drain buckets spanning the whole park

### Added — compile stamp in the footer
`fw 4.36 · built Aug 14 2026 12:12:11`, from `__DATE__`/`__TIME__`, also in
`/json` as `build`. Two flashes can carry the same version during development, so
the stamp is what actually identifies what is running on a board in the field.

### Changed — the drain estimate now regresses hourly buckets, not two points
The 24 h ring cannot see a multi-day drain; the fw 4.29 two-point anchor could
span days but was hostage to noise at either end. Now: **one bucket per hour for
the whole park**, each the mean of ~60 samples, regressed across all of them.

- **Persisted to LittleFS** (`/drain.bin`, 12 B per hour, ~62 days at `DR_MAX`),
  restored at boot. A reboot — watchdog, brownout or OTA — no longer resets the
  measurement, which is the failure that has repeatedly destroyed it.
- **Cleared on engine start**, since averaging across a recharge is meaningless.
- **Reports r²**, and returns *no* ETA below 0.5 rather than a number nobody
  should act on.
- Flash I/O stays on the loop core via a pending-bucket flag — the safety task
  must never touch LittleFS.

Both Main and Voltage read from it, since `autoStartEtaS()` already prefers the
long-term path. The two-point anchor survives only as a fallback for the first
hours of a park, before six buckets exist.

**Two deliberate deviations from the request:**

*The 12 h exclusion was kept.* Buckets are stored from engine-off but the fit
starts at +12 h. The first half-day is surface charge dissipating, not parasitic
drain, and including it would overstate the slope — the owner's original instinct
was right. The settling data stays visible.

*Temperature is stored per bucket and used as a second regressor.* Averaging
alone does not fix thermal bias: this battery moves ~5 mV/°C and the diurnal
swing is several times the daily trend, so a time-only fit over a partial last
day reflects the weather. `V ~ time + temp` removes it, which is why the existing
24 h fit already did this.

### Note
`DrainHour` and `HourFit` are declared beside `DrainFit` near the top for the
documented reason: the `.ino` auto-prototype pass emits
`HourFit computeHourlyDrain();` above the definitions, so the types must exist by
then.

---

## 2026-08-14 — fw 4.35: tooltips everywhere, log flap consolidation, UI spacing

### Added — an explanation on every visible value
Hovering any card, hero metric or badge now shows what it means, **what it says
right now**, and what would change it. Tips are rebuilt on every poll so they
quote live values rather than static blurbs, and anything carrying one gets a
dotted underline on its label so it is discoverable. They also fire on tap for
phones. 17 elements on the Main tab.

The prompt for this was `Lockout`, which previously showed two words and nothing
else. It now reads: *"the runaway guard. It latches ON after 2 consecutive
automatic starts that drew no charge... Right now: no (fail streak 1 of 2). It
would trip if the next 2 automatic starts both failed to bring the alternator
up."*

### Added — repeating log lines collapse instead of filling the ring
A value dithering at a threshold emits the same one- or two-line pattern
endlessly. `logLine()` now recognises a repeating 1- or 2-line cycle and rewrites
a `[repeated N times]` suffix on the lines already in the ring instead of
appending.

**Keyed on the full message text including voltages**, so `12.22 V` repeating
increments the count while a change to `12.23 V` starts a fresh line — that is
what keeps a collapsed line honest rather than hiding a moving value behind a
count.

### Changed — spacing and visual polish
Grid gap 12 → 16 px, minimum card width 120 → 158 px so cards stop cramming,
padding 12 → 16 px, radius 10 → 14 px, section labels given real vertical rhythm,
subtle gradient and shadow on cards and hero metrics with a slight lift on hover.

### Fixed
The graph hover reuses the same `#tip` node and never reset its class, so after
hovering a card the graph tooltip inherited the wrapping style and was then
hidden mid-hover by the card handler. `showTip()` now clears it.

### Considered and rejected — a deadband on the countdown reset
The log showed `STARTED`/`RESET` pairs one second apart, and a hysteresis margin
on the reset was drafted. **Withdrawn on the owner's correction:** the battery is
crossing the threshold on its way down, and once it sits solidly below, the hold
completes normally. A deadband would let a countdown continue while voltage sat
*above* the trigger, which breaks the meaning of "sustained below". The strict
comparison stays; the consolidation above is what makes the crossing phase
readable.

### Known gap
Auto-start config changes are written to NVS and Serial but **not to the flash
log**, so a threshold or hold change leaves no durable trace.

Surfaced by a real case: the hold read 300 s before this flash and 60 s after,
with every other NVS value intact. **That turned out to be a deliberate change by
the owner — there was no fault, and NVS persistence is working correctly.** But
the device could not say so, and half an hour went into ruling out a persistence
bug that never existed. A logged line naming the setting, its old value and its
new one would have answered it instantly. Worth adding.

---

## 2026-08-14 — fw 4.34: live sustain countdown on Main, and every reset explained

### Added — live countdown, Main tab
When voltage drops below the threshold a red banner appears immediately, showing
remaining time in large type, a progress bar, and a subline reading
*"55 s of 300 s held below 12.20 V — now 12.18 V. Recovering above the threshold
resets it to zero."*

**It ticks every second without polling the board every second.** The dashboard
polls `/json` every 2 s and that was deliberately left alone — weak link, small
socket pool, and a day already lost to socket-related failures. The seconds are
interpolated locally and **resynced to the board's authoritative `as_low_s` on
every poll**, so the display can never drift from the device by more than one
poll.

On recovery the banner clears and a persistent amber note explains what happened,
distinguishing *"Countdown reset at 12.24 V (threshold 12.20 V)"* from
*"Countdown completed — auto-start fired."*

### Added — every countdown abandonment is logged with its reason
There were **nine** code paths that could silently zero the sustain counter
(recovery, park-confirm, lockout, invalid reading, 24 h cap, re-arm wait,
verification in progress, boot grace, RF not ready). All of them just assigned
`g_lowSince = 0` with no trace. A `clearLow()` macro now logs the abandonment,
how far it had got, and which guard tripped:

```
LOW-V countdown STARTED: 12.18 V below 12.20 V, need 300s
LOW-V countdown RESET after 47s of 300s (voltage recovered) at 12.21 V
```

This matters more with a longer hold: a 5-minute window is a bigger target for
interruption than 60 s, and previously a countdown could restart repeatedly with
nothing recorded anywhere.

### Validated
Against a mock that counts and then recovers: countdown measured ticking
`4m 10s → 4m 05s` over four seconds with the bar advancing 16.7 % → 18.3 %,
subline correct, and the reset note appearing with the right voltages on
recovery. No JavaScript errors. Cache-bust `?v=434`.

**Field state:** flashed first try (`HTTP=200`, 44 s, 27.8 KB/s), board back on
its `.94` reservation, RSSI −58 (best recorded), hold already configured to
**300 s**, battery 12.24 V against a 12.20 V threshold.

---

## 2026-08-12 — fw 4.33: recommended trigger 12.4 V → 12.2 V

### Changed
`AS_DEF_VOLTS` and the dashboard help text now say **12.2 V**. The board has been
running 12.2 V from NVS all along; the firmware kept recommending 12.4 V, which
made every status report flag a discrepancy that was never going to be actioned.

**This is a measurement-led change, not a relaxation.** The cold-weather argument
for 12.4 V is sound in general and is kept in the source comment — near −20 °C the
engine needs roughly double the cranking torque while the battery delivers about
half its power, and 12.2 V rested (~SG 1.19) slushes around −26 °C where 12.4 V
(~SG 1.23) is good to about −37 °C.

It is the wrong number **for this car**. The battery settles at about **12.30 V**
after days parked, so a 12.4 V trigger sits *above* its resting voltage: the board
would fire immediately, then need 12.55 V (threshold + `AS_REARM_MARGIN`) held for
`AS_REARM_S` to re-arm — a level this pack never reaches without a long drive. It
would fall through the `AS_REARM_MAX_COOLDOWNS` escape hatch and start the engine
every cooldown, indefinitely. **A threshold above resting voltage is not a safety
margin, it is a loop.**

The honest reading of a 12.30 V rested battery is ~60 % SoC on a pack that no
longer takes a full charge — consistent with the two batteries this car has
already killed. Raising the trigger cannot fix a tired battery; it only runs the
starter more.

**Revisit when** the battery is replaced or the parasitic drain is located: a
healthy pack resting at 12.6–12.7 V carries a 12.4 V trigger comfortably, and in
deep cold it should. The help text now explains the dependency rather than naming
a bare number, so the reasoning travels with the setting.

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
>
> **Correction, 2026-08-14: this had it backwards — the docs were right.** Both
> schemes share the same app geometry, so either OTAs fine, but only `custom`
> creates the `spiffs` partition `LittleFS.begin()` mounts. See the 2026-08-14
> correction entry at the top of this file.

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
  --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=custom" \
  --build-path ~/.cache/vroom-build esp32-s3/voltage_monitor
```

> This originally read `PartitionScheme=app3M_fat9M_16MB`; corrected 2026-08-14.
> `custom` reads the sketch's `partitions.csv` and is the only scheme that is safe
> over **both** USB and OTA.

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
