# 12 — KeeLoq packet synthesis: bench + first car test

## Goal

End-to-end validation that the ESP32 + CC1101 can produce a transmission
the real Compustar receiver accepts — first verified visually with an
SDR (matches the FOB's pattern), then by triggering a Lock cycle in the
car. Lock first because it's non-destructive; Start comes only after
Lock works reliably.

This is the moment of truth for the whole project. Take your time.

## Prerequisites

- [07 — KeeLoq device key recovered](../sdr/07-key-recovery.md), validated
  with `validate-key.py` showing counter increments by 1
- [11 — UART link working](11-uart-link.md), dashboard streaming live data
- All values populated in `esp32/src/secrets.py` (device key, serial,
  counter — set initial counter to current+10 for safety margin)
- All values populated in `sdr/analysis/framing.md` (function codes for
  each button, measured TE)

## Step 1 — Update config + secrets

Open `esp32/src/secrets.py` (gitignored — your local-only file) and
populate:

```python
WIFI_SSID = "your-wifi"
WIFI_PASSWORD = "..."

MQTT_BROKER = "192.168.1.10"   # or None if you don't have one
MQTT_PORT = 1883
MQTT_USERNAME = None
MQTT_PASSWORD = None
MQTT_TOPIC_PREFIX = "vroom"

# These three are the critical Compustar values
COMPUSTAR_DEVICE_KEY = 0xABCDEF0123456789       # from sdr/07
COMPUSTAR_SERIAL = 0x0ABCDE1                    # from sdr/06
COMPUSTAR_COUNTER = 8234                        # current FOB counter + 10
```

Open `esp32/src/lib/compustar.py` and update the `Function` codes from
your `sdr/analysis/framing.md` if they differ from the placeholders:

```python
class Function:
    START = 0x_   # measured value
    LOCK = 0x_
    UNLOCK = 0x_
    TRUNK = 0x_
```

If the SDR walkthrough measured a different TE timing, update
`esp32/src/config.py`:

```python
RF_TE_US = 400       # or your measured value
```

Copy the updated files to the ESP32 via Thonny. Verify with the REPL:

```python
>>> from config import COMPUSTAR_DEVICE_KEY, COMPUSTAR_SERIAL, COMPUSTAR_COUNTER
>>> hex(COMPUSTAR_DEVICE_KEY)
'0xabcdef0123456789'
>>> import config
>>> config.secrets_ready()
True
```

## Step 2 — Generate a packet and verify against your captures

Bench-only — does not transmit. Just confirms the synthesized packet
matches what the FOB would produce at this counter value.

In Thonny REPL:

```python
>>> from lib import compustar
>>> import config
>>> packet = compustar.build_packet(
...     serial=config.COMPUSTAR_SERIAL,
...     function_code=compustar.Function.LOCK,
...     counter=config.COMPUSTAR_COUNTER,
...     device_key=config.COMPUSTAR_DEVICE_KEY,
... )
>>> hex(packet["hopping_code"])
>>> hex(packet["fixed_code"])
>>> packet["bits"]
```

Compare the structure to one of your captured Lock packets in
`sdr/analysis/fob-lock-001.bits`. The fixed_code should match
exactly (serial bits + Lock function code). The hopping code WILL
differ because you're using a future counter value — that's expected.

## Step 3 — Synthesize + capture + decrypt round-trip

Confirm the packet your ESP32 will transmit is one your captured key
correctly decrypts:

```python
>>> # On the ESP32, generate the hopping code for counter+10:
>>> from lib import compustar, keeloq
>>> import config
>>> plaintext = (
...     (config.COMPUSTAR_COUNTER & 0xFFFF)
...     | ((config.COMPUSTAR_SERIAL & 0xF) << 16)
...     | ((compustar.Function.LOCK & 0xF) << 20)
... )
>>> hop = keeloq.encrypt(plaintext, config.COMPUSTAR_DEVICE_KEY)
>>> hex(hop)
```

Then on your dev machine, decrypt:

```powershell
python sdr/scripts/validate-key.py `
    --device-key 0xABCDEF0123456789 `
    --serial 0x0ABCDE1 `
    0x<hop_from_above>
```

Expected: discrimination matches serial low nibble, function = your
LOCK code, counter matches what you set in secrets.

## Step 4 — Live transmit, RTL-SDR sniff

Set up the RTL-SDR to record at 433.92 MHz (per the SDR walkthrough).
Start recording:

```powershell
rtl_sdr -f 433920000 -s 2000000 -g 40 sdr/captures/synth-lock-001.bin
```

In Thonny on the ESP32, transmit one Lock packet:

```python
>>> from lib.cc1101 import CC1101
>>> from lib import compustar
>>> import config
>>> radio = CC1101(spi_id=2, cs_pin=5, gdo0_pin=22)
>>> radio.init_433mhz_ook()
>>> packet = compustar.build_packet(
...     serial=config.COMPUSTAR_SERIAL,
...     function_code=compustar.Function.LOCK,
...     counter=config.COMPUSTAR_COUNTER,
...     device_key=config.COMPUSTAR_DEVICE_KEY,
... )
>>> pulses = compustar.packet_to_pulses(packet["bits"], te_us=config.RF_TE_US)
>>> radio.transmit_burst(pulses, repeats=4, guard_ms=39)
```

Stop the recording (Ctrl-C). Open the resulting `.bin` in URH:

- Should look identical in structure to your real Lock captures from
  step 04 (same preamble, gap, packet length)
- Demodulated bits should match the synthesized `packet["bits"]` exactly

If the on-air pattern differs structurally from real Lock captures,
**do not proceed to the car**. Re-check:
- TE timing in `config.RF_TE_US`
- Preamble length in `compustar.PREAMBLE_HALF_BITS`
- Header gap in `compustar.HEADER_GAP_TE`

## Step 5 — Car test: Lock cycle

**Safety setup first:**
- Park in a safe location, hood OPEN
- Locate the Compustar valet switch (small black switch usually
  zip-tied under the dash) — flip it to disable the system OR have
  your hand near it
- Have your real FOB on you, in case you need to manually unlock
- Doors UNLOCKED to start (so we can observe locking)

Trigger the synthesized Lock packet from Thonny exactly as in step 4.

**Expected**: doors lock immediately, parking lights flash once (typical
Compustar Lock acknowledgement). If this happens, **you've successfully
replicated the FOB**. The hardest part of the project is done.

**If nothing happens**:
- Counter is probably wrong — too far ahead means receiver rejects,
  too far behind means receiver requires re-sync (two consecutive valid
  codes). Try transmitting twice in quick succession (the
  `transmit_burst` with repeats handles this naturally).
- Function code is wrong — re-verify Lock = 0x2 (or whatever you
  measured) by capturing your FOB pressing Lock and comparing to
  what the ESP32 actually emitted.
- TE timing is off — receivers tolerate ±20% on TE; outside that they
  reject. Measure preamble cycle width in URH precisely.

**If something else cycles** (e.g. trunk opens, hazards flash):
- Function code mapping is wrong — swap the values per your captures

Don't try Start yet, regardless of how good Lock looked. Start first,
Stop second:

## Step 6 — Car test: Unlock cycle

Same procedure but with `Function.UNLOCK`. Confirms the function-code
table is solid across multiple values.

## Step 7 — Car test: Start cycle

Only after Lock + Unlock work consistently across 5+ trials each.

Same procedure but with `Function.START`. The engine should crank and
catch. Be ready to flip the valet switch if it runs unexpectedly long
or anything looks wrong.

Run for 30 seconds, then re-transmit the Start function — Compustar
treats a second Start press as a Stop. Engine should shut off.

## Step 8 — Wire into the controller's auto path

Once all three manual transmits work, you can trust the controller's
auto path. Drop the ADS1115 input voltage below `LOW_V_TRIGGER` (e.g.
with a benchtop supply at 12.0V) and wait `LOW_V_SUSTAIN_S` — the
controller should trigger the same sequence automatically:

1. Power on the Pi
2. Transmit Start RF
3. Run for `RUN_DURATION_S`
4. Transmit Stop RF
5. Send `shutdown_pi` command, wait grace, cut MOSFET
6. Enter COOLDOWN

Watch the dashboard the whole time — every step should show up in the
events feed.

## What you should have when done

- Synthesized Lock + Unlock + Start packets all accepted by the car
- The full controller auto path validated from low-V trigger to
  cooldown
- Confidence to move to the in-car install

## Where artifacts go

- `sdr/captures/synth-*.bin` — your synthesized transmissions for
  comparison; commit if small + clean
- `sdr/analysis/synthesis-validation.md` — narrative notes from this
  bench session (commit)
- Updated `compustar.Function` values if any changed from placeholders

## Notes on the counter

After every successful transmit the controller persists `counter+1`
to flash (`compustar_counter.json` on the ESP32). After bench testing,
your real FOB is N presses behind the controller's counter. That's
fine for the receiver — it advances to whatever value you transmit
that's "near future" of what it had — but it means your FOB will fail
to start the car for the next few presses (because its counter is
"behind"). The FOB will re-sync automatically after a couple of
presses (most Compustar receivers re-sync within a window of ~16).

If you want the FOB to keep working immediately after bench testing,
finish testing on the bench (not at the car) and let the receiver only
see live values when you're actually doing in-car tests.

## Next

After this, you're past the prove-it-works phase. Remaining steps are
in-car install:

- 13 — wire to OBD-II (Y-splitter + pigtail)
- 14 — mount case + run display ribbon to dash
- 15 — first live trigger in-car
- 16 — first voltage-triggered auto-start

These are mechanical / install steps and we'll write them as they're done.
