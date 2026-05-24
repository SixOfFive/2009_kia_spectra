# 05 — Demodulate in Universal Radio Hacker

## Goal

Turn the raw IQ binary files from step 04 into clean bit sequences. URH does the demodulation, signal extraction, and bit slicing in a GUI you can verify by eye.

## Prerequisites

- [04 — recording captures](04-recording-captures.md) complete
- URH installed (`pip install urh`, see [01 — software setup](01-software-setup.md))
- At least one good Start capture (`sdr/captures/fob-start-001.bin`)

## Step 1 — Launch URH and import

```powershell
urh
```

In the URH main window:

1. **File → Import → Complex Signal**
2. Select `sdr/captures/fob-start-001.bin`
3. URH will ask about format:
   - **Sample rate**: 2000000 (or 250000 if you fell back to that in step 04 — match whatever `rtl_sdr` was actually running at)
   - **Bandwidth**: leave default
   - **Data type**: select **"unsigned 8 bit"** (this is what `rtl_sdr` writes)
   - Carrier frequency: 433920000 (or your measured value)
4. Click **OK**

A waveform appears. The horizontal axis is time (in seconds), the vertical is signal amplitude.

You should see:
- Mostly flat noise floor
- A clear burst (or 3-5 closely-spaced bursts) of activity somewhere in the first ~500ms of the file

## Step 2 — Zoom in on a single packet

1. Use the scroll/zoom controls to find one isolated packet
2. The OOK transmission shows up as alternating high-amplitude (1) and low-amplitude (0) blocks
3. Each packet is preceded by a **preamble**: ~12 cycles of fast alternation
4. Then a **header gap**: ~4ms of low amplitude
5. Then the **data payload**: ~26ms of PWM-encoded bits

A clean packet at 2 MS/s should look something like this (the high portions are wider/narrower than the low portions depending on the bit value):

```
preamble       gap        data bits
||||||||||||  ____   _-__-_--__--___...
```

If you can't see this structure, the capture is too noisy or you missed the packet — go back to step 04 and re-capture.

## Step 3 — Set the demodulator

In the **Interpretation** tab (right side):

1. **Modulation**: **ASK** (Amplitude Shift Keying — what OOK is called in URH's terminology)
2. URH usually auto-detects the **Bit Length**. For HCS chips at 433 MHz this typically lands around **800 samples** at 2 MS/s sample rate (= 400 µs per bit, the TE value from the datasheet)
3. **Center**: drag the green horizontal line to bisect the high/low amplitude — should auto-snap correctly
4. **Tolerance**: leave default

The demodulated bits appear at the bottom of the URH window as a series of `0`s and `1`s.

## Step 4 — Verify the framing matches HCS expectations

A clean HCS packet should demodulate to roughly:

- ~12 preamble half-bit cycles (depending on TE configuration; the preamble in URH may show as `010101010101...` or similar)
- A long gap (URH may render this as a stretch of `0`s)
- 66 data bits of payload

If the demodulator is correct, you should be able to identify each section by eye.

Use URH's **Mark / Label** feature:

1. Select a range of bits with click-drag
2. Right-click → Add Label → name it "preamble" / "header_gap" / "hopping_code" / "fixed_code" / "status"
3. URH highlights the section so you can visually confirm the structure

## Step 5 — Export the bit sequence

Once you're happy with the demodulation:

1. Right-click in the bits area → **Copy bits to clipboard** (or use the menu equivalent)
2. Paste into a text file: `sdr/analysis/fob-start-001.bits`

Format the file as one packet per line, like:

```
# Capture: sdr/captures/fob-start-001.bin
# Bits as extracted by URH on 2026-MM-DD
# Layout: preamble | gap | hopping(32) | fixed(32) | status(2)
0101010101010101010101010
[gap]
10110100110...  (32 bit hopping)
00001010111... (32 bit fixed: 28-bit serial + 4-bit function)
01             (2 bit status: V_LOW | repeat)
```

Repeat for at least 3 Start captures, 1 Lock capture, 1 Unlock capture so step 06 has enough to work with.

## Step 6 — Save the URH project

**File → Save Project As** → `sdr/analysis/urh-project.urh`

This commits to git (small XML file) and means anyone reproducing your work can open the project and see the exact demodulator settings + labels you used.

## What you should have when done

- A clear visual understanding of where preamble / gap / data are in the signal
- Demodulated bit sequences for at least 3 Start, 1 Lock, 1 Unlock captures
- `.bits` text files in `sdr/analysis/`
- A `urh-project.urh` file committed to git

## Where artifacts go

- `sdr/analysis/fob-*-NNN.bits` — committed, text files (small)
- `sdr/analysis/urh-project.urh` — committed, URH project XML
- `sdr/analysis/screenshots/` — selectively committed screenshots of well-labeled packets (helpful for the README)

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Demodulated bits look like noise | Bit length is wrong — try doubling or halving it, or let URH auto-detect again |
| Preamble + gap visible but data bits are garbage | Center line is in the wrong place — drag it to true midpoint between high/low amplitude |
| Packet is cut off mid-data | Capture file was too short — re-record with longer duration |
| URH consumes 100% CPU and freezes | The capture file may be too long. Crop in URH: Edit → Crop to selection. |
| Bits come out in groups of 8 with leading/trailing zeros | URH's "Symbol" decoder is interfering — set it to NRZ raw |

## Helpful URH features

- **View → Analysis** tab: shows multiple captures side by side so you can see which bits change
- **Compute → XOR Selection**: pick two captures, XOR their bit sequences, see exactly which bits flipped (= hopping code locations)
- **Decode → Decoding**: leave at "NRZ" (raw bits); other settings are for higher-level protocols we don't use

## Next

[`06-framing-extraction.md`](06-framing-extraction.md) — use the bit sequences to identify the FOB serial, function codes, and structure of the encrypted hopping code.
