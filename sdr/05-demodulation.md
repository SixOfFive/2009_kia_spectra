# 05 — Demodulate captures to bit sequences

## Goal

Convert the raw IQ `.bin` files from step 04 into clean `.bits` text files
(one bit per character, MSB-first transmit order) that the framing analysis
in step 06 can diff against each other.

This step uses the project's own Python demodulators. There are TWO
choices, depending on which FOB family you're working with:

- `sdr/scripts/demod-compustar.py` — Compustar 1WG3R-family FOBs
  (1WSHR-PRO, 1WG3R-SH, 1WAMR-1900). PWM pulse widths 708/1076/1448 µs.
  This is the project's actual FOB. **Use this one first.**
- `sdr/scripts/demod-ook.py` — generic 1:2-ratio PWM (standard HCS
  configuration, used by many other rolling-code FOBs). Kept around as
  a fallback for non-Compustar HCS variants. Don't use this one for
  Compustar FOBs — its 1:2 ratio assumption misclassifies 1WG3R's
  ~1:1.5 pulses.

Earlier drafts of this walkthrough used Universal Radio Hacker (URH),
but URH has heavy install gotchas on Windows (Python 3.13 only) and
freezes on large multi-press captures. The Python demodulators handle
those scenarios out of the box and are what we now use.

URH remains a perfectly fine alternative if you want a GUI for visual
inspection — see the [URH alternative](#urh-alternative-optional)
section at the bottom.

You can also cross-check our demod output against `rtl_433`'s reference
decoder — see the [rtl_433 cross-check](#rtl_433-cross-check-optional)
section at the bottom.

## Prerequisites

- [04 — recording captures](04-recording-captures.md) complete
- One or more `.bin` capture files in `sdr/captures/`
- Python 3.x on your machine (any version — the demodulator uses only stdlib)

## Pipeline

```
fob-start-001.bin
   |
   v  (inspect-capture.py — confirms bursts above noise floor)
   v  (trim-burst.py — slices each strong burst into its own small .bin)
   v
fob-start-001-b1.bin  fob-start-001-b2.bin  fob-start-001-b3.bin
   |
   v  (demod-compustar.py for 1WG3R-family FOBs; or demod-ook.py for HCS)
   v
fob-start-001-b1.bits  fob-start-001-b2.bits  fob-start-001-b3.bits
```

## Step 1 — Sanity-check the capture has real bursts

Before spending time demodulating, confirm `inspect-capture.py` sees
your packet bursts:

```powershell
python sdr\scripts\inspect-capture.py sdr\captures\fob-start-001.bin
```

You should see ≥3 bursts with `peak/floor` ratio above 15×. If everything
is below 5× or you see zero bursts, re-record with the FOB closer / at
lower gain (see step 04 troubleshooting).

## Step 2 — Trim multi-press captures into single bursts

If you used the "long capture, press 3 times" pattern from step 04, each
file contains multiple packet bursts. URH/demod work better on
single-burst files:

```powershell
python sdr\scripts\trim-burst.py sdr\captures\fob-start-001.bin
```

This emits `fob-start-001-b1.bin`, `fob-start-001-b2.bin`, etc. — one per
strong burst, ~1 second each. Repeat for every long capture you have:

```powershell
python sdr\scripts\trim-burst.py sdr\captures\fob-start-002.bin
python sdr\scripts\trim-burst.py sdr\captures\fob-start-003.bin
python sdr\scripts\trim-burst.py sdr\captures\fob-lock-001.bin
python sdr\scripts\trim-burst.py sdr\captures\fob-unlock-001.bin
python sdr\scripts\trim-burst.py sdr\captures\fob-trunk-001.bin
```

Skip this step if you recorded as one-press-per-file in step 04.

## Step 3 — Demodulate to bits

For a **Compustar 1WG3R-family FOB** (1WSHR-PRO, 1WG3R-SH, 1WAMR-1900),
run the Compustar-specific demodulator:

```powershell
python sdr\scripts\demod-compustar.py sdr\captures\fob-start-001-b1.bin
```

Expected output:

```
File: sdr/captures/fob-start-001-b1.bin
Burst regions: 1, packets decoded: 8
Valid packets (integrity check passed): 0
WARN no valid packets — possible decoder issue. First few raw:
  ID=0x<your-id> button=0x<bc> ~button=0x<~bc> unk1=000 unk2=0
```

(Don't worry about "Valid packets: 0" — the integrity check is for
the canonical 1WG3R layout; our 1WSHR-PRO is a structural sub-variant
and the `unk1` field is `000` instead of `111`. The Remote ID and
per-button bit patterns still extract correctly, which is what we
need.)

For a **standard HCS-PWM FOB**, run the generic demodulator instead:

```powershell
python sdr\scripts\demod-ook.py sdr\captures\fob-start-001-b1.bin
```

Both demodulators emit `.bits` files in `sdr/captures/`:
- A few comment lines starting with `#` (source, sample rate, measured
  pulse widths, etc.)
- One line of `0`/`1` characters — your demodulated packet.

For Compustar 1WG3R-family FOBs the bit line is **35 characters** long
(between sync triplets); for HCS-PWM FOBs it's **66 characters**.

If `demod-compustar.py` reports `Burst regions: 0`, the burst detector
missed the packet — typically because the capture was made at too-high
gain (`-g 40`) so AGC compression killed the modulation depth. Re-record
with `-g 20` per step 04's troubleshooting and try again.

Run with `--verbose` to see what's happening internally:

```powershell
python sdr\scripts\demod-compustar.py sdr\captures\fob-start-001-b1.bin --verbose
```

## Step 4 — Demodulate all your bursts

For a Compustar 1WG3R-family FOB, you need at minimum **1 burst per
button** (4 buttons = 4 files) since the protocol is fixed-code and all
repeats of a given button are identical. Multiple Start captures are
useful to confirm that — they should produce byte-identical 35-bit
patterns.

For an HCS-KeeLoq FOB you need 3+ Start captures plus 1 each of
Lock/Unlock/Trunk so that step 06 can diff them and identify the
hopping-code position.

```powershell
python sdr\scripts\demod-compustar.py sdr\captures\fob-start-001-b1.bin
python sdr\scripts\demod-compustar.py sdr\captures\fob-start-002-b1.bin
python sdr\scripts\demod-compustar.py sdr\captures\fob-start-003-b1.bin
python sdr\scripts\demod-compustar.py sdr\captures\fob-lock-001-b1.bin
python sdr\scripts\demod-compustar.py sdr\captures\fob-unlock-001-b1.bin
python sdr\scripts\demod-compustar.py sdr\captures\fob-trunk-001-b1.bin
```

(All `.bits` files land next to the `.bin` they came from, in
`sdr/captures/`.)

## Step 5 — Quick eyeball verification

Open one of the `.bits` files. The bit string after the `#` comment
lines is your packet.

**For a Compustar 1WG3R-family FOB**, three consecutive Start
captures should produce **identical** 35-bit patterns — the protocol is
fixed-code, no rolling counter. That's the project's actual case.

```powershell
type sdr\captures\fob-start-001-b1.bits
type sdr\captures\fob-start-002-b1.bits
type sdr\captures\fob-start-003-b1.bits
```

If they aren't identical, either the demodulator missed sync alignment
(re-run with `--verbose`) or you accidentally captured a different
button.

**For an HCS-KeeLoq FOB**, three Start captures should differ in
about 32 positions — the hopping code changes as the FOB counter
increments. If two consecutive Start captures' bits are identical
on an HCS FOB, the FOB counter didn't increment (rare, might mean
you analyzed the same burst twice). If 60+ bits differ, the demod
got confused — re-record at lower gain or try a different `--te-us`.

## What you should have when done

For a Compustar 1WG3R-family FOB:
- 4 `.bits` files (one per button) with stable 35-bit patterns
- Optional: multiple `.bits` per button confirming patterns are
  byte-identical across presses

For an HCS-KeeLoq FOB:
- 3+ Start `.bits` files (varying hopping code)
- 1 `.bits` each for Lock/Unlock/Trunk
- Eyeball confirmation that:
  - Start vs Start: ~32 bits different (the hopping code)
  - Start vs Lock: ~36 bits different (hopping + 4-bit function code)

## Where artifacts go

- `sdr/captures/*.bits` — gitignored (per `sdr/captures/*` rule). The
  bits ARE FOB-identifying so they should stay local. Step 06 records
  the *positions* of constant-vs-changing bits in `framing.local.md`
  (also gitignored).

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `Bursts: 0, packets decoded: 0` | Either the burst-detector missed your packet (try `--verbose` for noise-floor diagnostics) or there are no bursts (re-run `inspect-capture.py`) |
| `Bursts: 1, packets decoded: 0` | Burst detected but no packet structure inside. Most common: AGC compression — re-record at `-g 20`. Less common: TE mismatch — try `--te-us 200/600/800`. |
| 60+ bits differ between two Start captures | Demod is misaligning bits, probably wrong TE. Try a few `--te-us` values; pick the one where Start-vs-Start diffs are ~32 bits. |
| `pattern break at run N` | The demodulator found preamble + header gap, started decoding bits, then hit a non-PWM section. Usually means the capture had RF interference mid-packet — pick a different burst's `-b2.bin` or `-b3.bin` instead. |
| `decode error: short read` | Capture got truncated before 66 bits were received. Use one of the other bursts (the `-b2.bin` or `-b3.bin` files) which should have full packets. |

## rtl_433 cross-check (optional)

If you want a second opinion on the on-air pulse structure, install
[`rtl_433`](https://github.com/merbanan/rtl_433/releases) (use the
**nightly** build on Windows — release 25.12 doesn't have the
`compustar_1wg3r` decoder yet). Then:

```powershell
rtl_433.exe -r cu8:sdr\captures\fob-start-001-b1.bin -s 250000 -f 433968000 -A
```

`-A` runs the pulse-width analyzer (independent of any specific
decoder). You should see three pulse-width clusters near 732 / 1100 /
1476 µs for a Compustar 1WG3R-family FOB. The histogram counts also
let you verify the burst structure: for ours, 142 short + 144 long +
24 sync pulses = 310 total pulses, which is 8 packets × (3 sync + 35
data) + ~6-pulse preamble.

The full 1WG3R decoder runs with `-R 302`:

```powershell
rtl_433.exe -r cu8:sdr\captures\fob-start-001-b1.bin -s 250000 -f 433968000 -R 302
```

For 1WG3R-SH and 1WAMR-1900 FOBs this prints `{"model":"Compustar-1WG3R","id":...,"button_str":...}`
JSON per packet. For 1WSHR-PRO it produces no output — our sub-variant
has 3 sync pulses + 35 data bits instead of 1 sync + 36 bits, and
rtl_433's strict structural check rejects the framing. That's fine —
the project's own `demod-compustar.py` handles the sub-variant, and
the `-A` output above gives ground-truth on pulse widths.

## URH alternative (optional)

If you'd rather use URH for visual inspection — for example to confirm
the demodulator's bit length matches what's on-air — it remains a fine
tool. You just need Python 3.13 specifically (see [01 — software setup](01-software-setup.md)
for the install gotcha), and you'll want to use the trimmed
single-burst files since URH stalls on multi-press captures.

In URH:

1. Open `fob-start-001-b1.bin`
2. Format dialog: Sample rate = your capture rate (250000 typical),
   Data type = Unsigned 8 bit
3. In the Interpretation tab: Modulation = ASK
4. URH auto-detects bit length; should land near `samples_per_bit = TE_us * sample_rate / 1_000_000`. For TE=400µs at 250 kSps, that's 100 samples.
5. Right-click bit area → Copy bits to clipboard → paste into a `.bits` file

URH's main advantage over `demod-ook.py` is the visual waveform — useful
the first time you're confirming a new FOB's modulation. After that the
Python tool's reproducibility usually wins.

## Next

[`06-framing-extraction.md`](06-framing-extraction.md) — diff the `.bits`
files to identify the FOB serial, function codes, and hopping code
position within the packet.
