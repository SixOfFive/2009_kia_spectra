# 04 — Record clean per-button captures

## Goal

Save high-quality IQ samples of the FOB transmitting each button. We need at least:

- **10 captures of Start** (so we can see which bits change between presses → that's the hopping code; which stay the same → serial + function code)
- **3 captures of Lock** (so we can identify the 4-bit function code by comparing Start vs Lock)
- **3 captures of Unlock** (same)
- **3 captures of Trunk** if your FOB has a trunk button (same)

These captures are the raw material for everything that follows.

## Prerequisites

- [03 — frequency confirmation](03-frequency-confirmation.md) complete
- Know your FOB's actual center frequency (probably 433.920 MHz)
- Captures directory exists: `sdr/captures/` — should already be there with gitignored contents

## File naming convention

Use this exact naming so the analysis scripts in step 05 can group them automatically:

```
sdr/captures/fob-<button>-<NNN>.bin
```

Examples:
- `sdr/captures/fob-start-001.bin`
- `sdr/captures/fob-start-002.bin`
- `sdr/captures/fob-lock-001.bin`
- `sdr/captures/fob-unlock-001.bin`
- `sdr/captures/fob-trunk-001.bin`

Three-digit zero-padded sequence numbers so file listings sort correctly.

## Step 1 — Confirm capture parameters

Standard capture settings for this project:

| Parameter | Value | Why |
|---|---|---|
| Center frequency | Your measured frequency (~433.92 MHz) | From step 03 |
| Sample rate | 2,000,000 Hz (2 MS/s) | Plenty of bandwidth for OOK; clean edges in URH |
| Gain | 40 dB | High enough for clear capture without clipping when FOB is close |
| Duration | 1 second per capture | One button press = ~80ms packet × 3-5 repeats ≈ 250-400ms of activity. 1 second comfortably contains a full burst with headroom. |
| Format | unsigned 8-bit I/Q (rtl_sdr default) | URH and inspectrum both read this natively |

## Step 2 — Record Start button × 10

For each of the 10 captures:

1. Hold the FOB ~30cm from the dongle's antenna
2. In one terminal, start recording:
   ```powershell
   rtl_sdr -f 433920000 -s 2000000 -g 40 sdr/captures/fob-start-001.bin
   ```
   (substitute your measured frequency from step 03)
3. Within 200ms of starting, press the FOB Start button once and release
4. Wait until the dongle has been recording for ~1.5 seconds
5. **Ctrl-C** to stop
6. Repeat with `-002.bin`, `-003.bin`, etc. through `-010.bin`

The pacing matters — pressing too late means the recording stops before the FOB finishes the repeat burst. Aim for: press button → count to 1 → Ctrl-C.

### File size sanity check

Each 1-second capture at 2 MS/s of 8-bit IQ = exactly 4,000,000 bytes = 4.0 MB. If yours are wildly different sizes, the capture command isn't getting the parameters you think.

```powershell
ls sdr/captures/fob-start-*.bin
```

## Step 3 — Record Lock × 3, Unlock × 3, Trunk × 3

Same procedure, different button:

```powershell
rtl_sdr -f 433920000 -s 2000000 -g 40 sdr/captures/fob-lock-001.bin
rtl_sdr -f 433920000 -s 2000000 -g 40 sdr/captures/fob-lock-002.bin
rtl_sdr -f 433920000 -s 2000000 -g 40 sdr/captures/fob-lock-003.bin

rtl_sdr -f 433920000 -s 2000000 -g 40 sdr/captures/fob-unlock-001.bin
# ... etc

rtl_sdr -f 433920000 -s 2000000 -g 40 sdr/captures/fob-trunk-001.bin
# ... etc
```

## Step 4 — One "no-press" baseline

Record one capture where you don't press anything — useful as a noise-floor reference when you compare in URH.

```powershell
rtl_sdr -f 433920000 -s 2000000 -g 40 sdr/captures/fob-baseline-noaction.bin
```

## Step 5 — Document the session

Create `sdr/analysis/press-logs/2026-MM-DD-session.md` (use today's date) with notes on:

- FOB battery freshness
- Distance to dongle
- Any RF interference noticed (microwave running, WiFi nearby, etc.)
- Which captures, if any, you suspect were bad (button bounced, missed the window, etc.)

This file gets committed (it's small markdown) — it's a permanent record of the session conditions for future-you wondering why one capture is noisier than another.

Template:

```markdown
# Capture session 2026-MM-DD

- FOB: Compustar 1WSHR-PRO, FCC ID 7087A-R762A433
- Dongle: Vomeko RTL-SDR (R820T2 + RTL2832U)
- Center frequency: 433.____ MHz (measured per step 03)
- Sample rate: 2 MS/s, gain 40 dB
- Distance: ~30 cm
- Environment: <your room, what else was running>

## Captures recorded

| File | Button | Notes |
|------|--------|-------|
| fob-start-001.bin | Start | clean |
| fob-start-002.bin | Start | clean |
| ... | ... | ... |
```

## What you should have when done

- ≥10 Start captures, ≥3 each of Lock/Unlock (and Trunk if applicable), 1 baseline
- All ~4MB each
- A session log in `sdr/analysis/press-logs/`

## Where artifacts go

- `sdr/captures/fob-*.bin` — raw IQ files (gitignored — too large)
- `sdr/analysis/press-logs/YYYY-MM-DD-session.md` — committed, small markdown

## Troubleshooting

| Symptom | Fix |
|---|---|
| File is 4 MB but only contains noise | You missed the press window — re-record with better timing |
| File is much larger than 4 MB | You let it run too long — Ctrl-C sooner (size = sample_rate × bytes_per_sample × seconds) |
| All captures look identical (same hopping code bits) | FOB is broken (counter not incrementing) — replace battery, try a different FOB or hardware-reset the encoder |
| URH in step 05 won't open the file | Verify file size: should be exact multiple of 2 (I + Q bytes per sample). Truncation can happen if disk fills. |

## Why the specific quantities?

The 10 Start captures aren't arbitrary:
- We need at least 2 captures to see which bits *change* between presses (= the encrypted hopping code)
- More captures = higher confidence we've correctly identified hopping vs. fixed bits, and lets us see the counter actually incrementing by 1 each press

The 3 each of Lock/Unlock/Trunk lets us isolate the 4-bit function code by XORing the fixed-code portions of different button captures — only those 4 bits should differ.

## Next

[`05-urh-analysis.md`](05-urh-analysis.md) — open these captures in Universal Radio Hacker and extract the bit patterns.
