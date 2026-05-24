# 12 — Compustar packet replay: bench + first car test

## Goal

End-to-end validation that the ESP32 + CC1101 can produce a transmission
the real Compustar receiver accepts — first verified visually with an
SDR (matches the FOB's pattern), then by triggering a Lock cycle in the
car. Lock first because it's non-destructive; Start comes only after
Lock works reliably.

This is the moment of truth for the whole project. Take your time.

## Background — why this got simpler

Earlier drafts of this doc assumed the FOB used KeeLoq rolling-code
encryption and described synthesizing fresh hopping codes from a
recovered device key. **That assumption was wrong for this FOB.** SDR
analysis (verified by `rtl_433`'s pulse analyzer) confirmed the
Compustar 1WSHR-PRO is a **fixed-code** member of the
[Compustar 1WG3R family](https://github.com/merbanan/rtl_433/blob/master/src/devices/compustar_1wg3r.c).
Every press of a given button transmits an identical 35-bit pattern
forever — no rolling counter, no encryption, no device key.

So this doc no longer covers key recovery and counter management. The
validation steps are:

1. Copy the four captured 35-bit bit patterns into `secrets.py`.
2. Bench-transmit a packet via CC1101 and confirm it shows up on an
   SDR with the same pulse shape as the genuine FOB.
3. Trigger a Lock cycle in the car.
4. Trigger Unlock.
5. Only then, trigger Start.
6. Verify the controller's auto-trigger path works against a low
   voltage simulated on the ADC.

If you're working with a different Compustar FOB that DOES use KeeLoq
rolling-code (some older Compustar models do), then `keeloq.py` is
still in tree and [`sdr/07-key-recovery.md`](../sdr/07-key-recovery.md)
walks the original path. This doc covers the 1WG3R-family fixed-code
case.

## Prerequisites

- All four button bit-patterns captured per
  [`sdr/05-demodulation.md`](../sdr/05-demodulation.md) and
  [`sdr/06-framing-extraction.md`](../sdr/06-framing-extraction.md);
  values present in `sdr/analysis/framing.local.md` (gitignored).
- [11 — UART link working](11-uart-link.md), dashboard streaming live data.
- Smoke tests from [09](09-bench-smoke-tests.md) passed — CC1101 part-ID
  reads as `0x14` / `0x04`, GDO0 toggles drive the antenna, OOK burst
  visible on the SDR.

## Step 1 — Populate `secrets.py`

Open `esp32/src/secrets.py` (gitignored — your local-only file). Copy
the values from `sdr/analysis/framing.local.md` (also gitignored):

```python
WIFI_SSID = "your-wifi"
WIFI_PASSWORD = "..."

MQTT_BROKER = "192.168.1.10"   # or None if you don't have one
MQTT_PORT = 1883
MQTT_USERNAME = None
MQTT_PASSWORD = None
MQTT_TOPIC_PREFIX = "vroom/spectra"

# Compustar 1WSHR-PRO captured 35-bit packets (FIXED CODE)
COMPUSTAR_REMOTE_ID = 0x____   # 16-bit ID, cosmetic but please fill in

COMPUSTAR_PACKETS = {
    "START":  "00000000000000000000000000000000000",  # 35 chars
    "LOCK":   "00000000000000000000000000000000000",
    "UNLOCK": "00000000000000000000000000000000000",
    "TRUNK":  "00000000000000000000000000000000000",
}
```

Whitespace, `_`, and `-` in the patterns are ignored, so you can
write them as e.g. `"0010 1101_11010 1100001000 010011111011"` for
readability.

Copy the file to the ESP32 via Thonny (or
`mpremote cp esp32/src/secrets.py :secrets.py`). Verify with the REPL:

```python
>>> import config
>>> config.secrets_ready()
True
>>> hex(config.COMPUSTAR_REMOTE_ID)
'0x<your-fob-id>'
>>> from lib import compustar
>>> compustar.validate_packets(config.COMPUSTAR_PACKETS)
[]
```

Empty list = all four patterns are well-formed 35-bit strings. If
anything is wrong, `validate_packets()` returns a list of error
strings naming the offending button.

Then run the preflight check from your dev machine to spot common
issues:

```powershell
python tools\preflight.py --side esp32
```

Should report `secrets.COMPUSTAR_PACKETS has all 4 buttons (35 bits each)`
and not flag any placeholder.

## Step 2 — Render a packet on the ESP32 and inspect

Bench-only — does not transmit yet. Confirms the renderer is producing
sensible pulse pairs for the captured bits.

```python
>>> from lib import compustar
>>> import config
>>> pulses = compustar.build_pulses_for_button(
...     compustar.Button.LOCK, config.COMPUSTAR_PACKETS)
>>> len(pulses)
38                          # 3 sync pulses + 35 data pulses
>>> pulses[0]
(1476, 1500)                # first sync HIGH/LOW pair
>>> pulses[3]
(732, 1136) or (1100, 756)  # first data bit, depending on Lock's bit 0
```

If the first three pulses aren't all `(1476, 1500)` sync pairs, the
constants in `compustar.py` were edited — restore them to the
rtl_433-derived defaults (`SYNC_HIGH_US`, `SYNC_LOW_US` etc.) or pass
overrides into `packet_to_pulses()`.

## Step 3 — Live transmit, SDR sniff

Set up the RTL-SDR to record at your measured frequency (433.968 MHz
or whatever your `sdr/03` step found). Start a 5-second recording:

```powershell
rtl_sdr -f 433968000 -s 250000 -g 20 sdr\captures\synth-lock-001.bin
```

In Thonny on the ESP32, while the rtl_sdr capture is running, transmit
one Lock packet:

```python
>>> from lib.cc1101 import CC1101
>>> from lib import compustar
>>> import config
>>> radio = CC1101(spi_id=2, cs_pin=5, gdo0_pin=22)
>>> radio.init_433mhz_ook()
>>> pulses = compustar.build_pulses_for_button(
...     compustar.Button.LOCK, config.COMPUSTAR_PACKETS)
>>> radio.transmit_burst(pulses, repeats=config.RF_BURST_REPEATS,
...                      guard_ms=config.RF_GUARD_MS)
```

Stop the rtl_sdr recording (Ctrl-C). Inspect + trim + demodulate the
synthesized burst:

```powershell
python sdr\scripts\inspect-capture.py sdr\captures\synth-lock-001.bin
python sdr\scripts\trim-burst.py sdr\captures\synth-lock-001.bin
python sdr\scripts\demod-compustar.py sdr\captures\synth-lock-001-b1.bin --verbose
```

Cross-check:

- The demodulated bit pattern should be **identical** to
  `COMPUSTAR_PACKETS["LOCK"]` from your secrets.py.
- `inspect-capture.py` should report 1 strong burst with peak/floor
  ratio comparable to your original FOB captures.
- `demod-compustar.py --verbose` should report 8 packet repeats per
  burst (or whatever `RF_BURST_REPEATS` is set to) and the same Remote
  ID as your FOB.

If the demodulated bits don't match the stored pattern, **do not
proceed to the car**. Likely causes:

- Pulse-width drift — verify `compustar.SHORT_HIGH_US` / `LONG_HIGH_US`
  / `SYNC_HIGH_US` haven't been edited. The defaults match the
  rtl_433 reference and a known-good FOB.
- CC1101 `init_433mhz_ook` didn't actually enter async TX mode —
  check that `gdo0` is set up as OUTPUT and `IOCFG0=0x2D` (async
  serial data input).
- Sample rate mismatch in your capture — the rtl_sdr command above
  uses `-s 250000` to match the FOB captures from step 04.

Optionally cross-check pulse widths with rtl_433:

```powershell
rtl_433.exe -r cu8:sdr\captures\synth-lock-001-b1.bin -s 250000 -f 433968000 -A
```

The pulse-width histogram should show the same three clusters (short,
long, sync) as the rtl_433 trace recorded in `sdr/analysis/framing.md`.

## Step 4 — Car test: Lock cycle

**Safety setup first:**

- Park in a safe location, hood OPEN.
- Locate the Compustar valet switch (small black switch usually
  zip-tied under the dash) — flip it to disable the remote-start
  system, OR have your hand near it ready to flip.
- Have your real FOB on you, in case you need to manually unlock.
- Doors UNLOCKED to start, so you can observe the lock event.

Trigger the synthesized Lock packet from Thonny exactly as in step 3
(without the rtl_sdr running — that was only for verification).

**Expected**: doors lock immediately, parking lights flash once
(typical Compustar Lock acknowledgement). If this happens, **you've
successfully replicated the FOB**.

**If nothing happens**:

- The genuine FOB transmits ~8 packet repeats per press; the controller
  defaults to `RF_BURST_REPEATS=8` to match. If the receiver was
  ignoring single packets, the burst should still trigger it. Try
  setting `repeats=16` for one more margin attempt.
- Verify you're transmitting at the correct frequency. Run
  `inspect-capture.py` against your synth capture — peak should be
  within ~50 kHz of where your original FOB captures peaked.
- Try with the antenna closer to the car (within 1 m). Receiver
  sensitivity is generous but verify it's not a range issue first.

**If something else cycles** (trunk pops, hazards flash):

- You labelled a button's bit pattern incorrectly in `secrets.py`.
  Cross-check against `framing.local.md` — every entry there should
  exactly match a FOB-captured pattern, and the dict key (`START` /
  `LOCK` / `UNLOCK` / `TRUNK`) should match the button you pressed
  during capture.

## Step 5 — Car test: Unlock cycle

Same procedure but with `compustar.Button.UNLOCK`. Confirms the button
mapping is solid across multiple buttons.

## Step 6 — Car test: Start cycle

Only after Lock + Unlock work consistently across 5+ trials each.

```python
>>> radio.transmit_burst(
...     compustar.build_pulses_for_button(
...         compustar.Button.START, config.COMPUSTAR_PACKETS),
...     repeats=config.RF_BURST_REPEATS,
...     guard_ms=config.RF_GUARD_MS,
... )
```

The Compustar 1-way protocol uses the **same Start button** to crank
the engine and to stop it again. Engine should crank and catch. Be
ready to flip the valet switch if it runs unexpectedly long or
anything looks wrong.

Let it idle 30 seconds, then re-transmit the Start packet — the
Compustar brain treats the second press as Stop. Engine should shut
off.

## Step 7 — Validate the controller's auto path

Once all three manual transmits work, you can trust the controller's
auto path. Drop the ADS1115 input voltage below `LOW_V_TRIGGER` (e.g.
with a benchtop supply at 12.0V or a divider giving ~3.0V at the ADC
input) and wait `LOW_V_SUSTAIN_S` — the controller should trigger the
same sequence automatically:

1. Power on the Pi (P-MOSFET on)
2. Transmit Start RF
3. Run for `RUN_DURATION_S`
4. Transmit Start RF again (stops the engine)
5. Send `shutdown_pi` command, wait grace, cut MOSFET
6. Enter COOLDOWN

Watch the dashboard the whole time — every step should show up in the
events feed as `low_voltage_trigger` -> `compustar_tx` ->
`engine_started` -> `engine_stopped`.

## What you should have when done

- All four captured bit patterns transmitted from the ESP32 and
  observed on the SDR matching the genuine FOB's pulse shape.
- Lock + Unlock + Start all accepted by the car.
- The full controller auto path validated from low-V trigger to
  cooldown.
- Confidence to move to the in-car install.

## Where artifacts go

- `sdr/captures/synth-*.bin` — synthesized transmissions; gitignored
  per the existing `sdr/captures/*` rule. They're as FOB-identifying
  as the real captures.
- Notes from this session into `sdr/analysis/bench-validation.local.md`
  (gitignored — keep local).

## What about counter desync after testing?

Pre-rewrite this doc warned about post-bench-test FOB counter desync.
That's a non-issue here: the protocol is fixed-code, so there's no
counter on either side. Your genuine FOB and the ESP32 transmit the
same bits every time. Bench-testing as many times as you want has
zero effect on the FOB.

## Next

After this, you're past the prove-it-works phase. Remaining steps are
in-car install:

- 13 — wire to OBD-II (Y-splitter + pigtail)
- 14 — mount case + run display ribbon to dash
- 15 — first live trigger in-car
- 16 — first voltage-triggered auto-start

These are mechanical / install steps and we'll write them as they're
done.
