# 05 — Demodulate captures to bit sequences

## Goal

Convert the raw IQ `.bin` files from step 04 into clean `.bits` text files
(one bit per character, MSB-first transmit order) that the framing analysis
in step 06 can diff against each other.

This step uses the project's own Python demodulator
(`sdr/scripts/demod-ook.py`). Earlier drafts of this walkthrough used
Universal Radio Hacker (URH), but URH has heavy install gotchas on Windows
(Python 3.13 only) and freezes on large multi-press captures. The Python
demodulator handles those scenarios out of the box and is what we now use.

URH remains a perfectly fine alternative if you want a GUI for visual
inspection — see the [URH alternative](#urh-alternative-optional) section
at the bottom.

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
   v  (demod-ook.py — produces one .bits file per packet)
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

Run the demodulator on each single-burst file:

```powershell
python sdr\scripts\demod-ook.py sdr\captures\fob-start-001-b1.bin
```

Expected output:

```
File: sdr/captures/fob-start-001-b1.bin
Bursts: 1, packets decoded: 1
  -> sdr/captures/fob-start-001-b1.bits: 0101011...1101
```

The `.bits` file contains:
- A few comment lines starting with `#` (source, sample rate, measured TE, etc.)
- One line of 66 `0`/`1` characters — your demodulated packet

If the demodulator fails to decode (`packets decoded: 0`), it will print
diagnostic hints. The two most common causes:

1. **AGC compression** — the capture was made at too-high gain, so the
   on-air OOK signal got squashed into <1% modulation depth and is
   buried in noise. Re-record with `-g 20` instead of `-g 40` (see
   step 04 troubleshooting).
2. **TE mismatch** — your specific FOB uses a non-default bit element
   time. Try `--te-us 200`, `--te-us 800`, or `--te-us 600` to find one
   that works.

Run with `--verbose` to see what's happening internally:

```powershell
python sdr\scripts\demod-ook.py sdr\captures\fob-start-001-b1.bin --verbose
```

## Step 4 — Demodulate all your bursts

You want at minimum 3 Start `.bits` files plus 1 each of Lock / Unlock /
Trunk for step 06 (framing extraction). With the trimmed-burst files
from step 2 you can pick any three Start bursts:

```powershell
python sdr\scripts\demod-ook.py sdr\captures\fob-start-001-b1.bin
python sdr\scripts\demod-ook.py sdr\captures\fob-start-002-b1.bin
python sdr\scripts\demod-ook.py sdr\captures\fob-start-003-b1.bin
python sdr\scripts\demod-ook.py sdr\captures\fob-lock-001-b1.bin
python sdr\scripts\demod-ook.py sdr\captures\fob-unlock-001-b1.bin
python sdr\scripts\demod-ook.py sdr\captures\fob-trunk-001-b1.bin
```

(All `.bits` files land next to the `.bin` they came from, in `sdr/captures/`.)

## Step 5 — Quick eyeball verification

Open one of the `.bits` files. The bit string after the `#` comment lines
is your packet. For three consecutive Start captures, the bits should
look mostly similar but with about 32 positions different — that's the
hopping code changing as the FOB counter increments.

```powershell
type sdr\captures\fob-start-001-b1.bits
type sdr\captures\fob-start-002-b1.bits
type sdr\captures\fob-start-003-b1.bits
```

That eyeball check is the gateway to step 06. If two consecutive Start
captures' bits are *identical*, the FOB counter didn't increment (rare,
might mean you accidentally analyzed the same burst twice). If 60+ bits
differ between them, the demodulator got confused — re-record at lower
gain or try a different `--te-us` value.

## What you should have when done

- One `.bits` file per packet you care about (3 Start + 1 each of
  Lock/Unlock/Trunk minimum)
- Quick eyeball confirmation that:
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
