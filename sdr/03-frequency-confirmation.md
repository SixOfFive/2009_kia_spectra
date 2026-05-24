# 03 — Confirm the FOB transmit frequency

## Goal

Verify the actual transmit frequency of your FOB before doing real captures. The FCC ID strongly implies 433.92 MHz for the Compustar 1WSHR-PRO (FCC ID `7087A-R762A433` — the trailing `433` is the manufacturer's frequency indicator), but always confirm before doing 10 minutes of captures at the wrong center frequency.

## Prerequisites

- [02 — hardware verification](02-hardware-verification.md) complete
- FOB battery is good (LED on FOB blinks bright when you press a button)
- Be in a relatively quiet RF environment — your kitchen at 11pm is better than a hackerspace during a workshop

## Step 1 — Wide sweep with the FOB held down

```powershell
rtl_power -f 433M:434M:1k -i 1 -g 40 -e 60 sdr/captures/fob-frequency-sweep.csv
```

While this is running (60 seconds), repeatedly press the FOB's Start button. Press for ~1 second, release for ~1 second, repeat. Make sure the FOB is close to the dongle (within 1m).

## Step 2 — Find the peak

Open `fob-frequency-sweep.csv` in any spreadsheet program, or use the helper script:

```powershell
python sdr/scripts/plot-power-csv.py sdr/captures/fob-frequency-sweep.csv
```

You should see a clear spike at one specific frequency — typically a sharp peak ~20-30 dB above the noise floor whenever you pressed the button. If you see multiple peaks, the strongest one (and the one that disappears when you stop pressing) is the FOB.

**Expected**: peak centered very close to 433.920 MHz. Most consumer 433 MHz devices land within ±100 kHz of that.

## Step 3 — Record the actual center frequency

In your `sdr/analysis/framing.md` file (create it if it doesn't exist), note:

```markdown
## FOB transmit frequency

- Measured center: 433.____ MHz
- Bandwidth: roughly ±____ kHz
- Source: rtl_power sweep on YYYY-MM-DD with `g 40 -i 1 -e 60`
- Capture file: sdr/captures/fob-frequency-sweep.csv
```

Use the **measured** frequency for all subsequent captures, not the nominal 433.92 MHz. A 30 kHz offset matters when you're decoding at high sample rates later.

## Step 4 — Live confirmation with GQRX (optional but recommended)

If you have GQRX installed (linux/mac: `sudo apt install gqrx-sdr` / `brew install gqrx`; Windows: download from [gqrx.dk](https://gqrx.dk)), open it:

1. **File → I/O Devices → Device string**: `rtl=0`
2. **Sample rate**: 2048000
3. Click **Start DSP** (the play triangle)
4. Tune to 433.920 MHz
5. Set demodulator to **Raw I/Q**
6. Press a FOB button — you should see a fat bright bar appear in the waterfall for the duration of the press
7. The bar's exact horizontal position confirms your frequency

GQRX is overkill for this step but gives you a visual gut-check that what `rtl_power` reported is real.

## What you should have when done

- A `sdr/captures/fob-frequency-sweep.csv` showing a clear FOB peak
- An entry in `sdr/analysis/framing.md` with the measured frequency
- Confidence that subsequent captures at that frequency will actually contain the FOB transmission

## Where artifacts go

- `sdr/captures/fob-frequency-sweep.csv` — raw sweep (gitignored)
- `sdr/analysis/framing.md` — committed record of findings (create now if not yet)

## Troubleshooting

| Symptom | Fix |
|---|---|
| No peak appears | FOB battery dead? Try a known-fresh CR2032. Or FOB is not 433.92 — try sweeping 313-316 MHz (some Compustar 1-way kits use 312.6 MHz) |
| Peak is faint and noisy | Move FOB closer to dongle; try `-g 49.6` (max gain) |
| Multiple identical peaks at harmonic spacing | That's normal — the OOK modulation produces sideband images. The fundamental is the strongest. |
| Peak at the edge of your sweep range | Re-sweep with a wider range, e.g. `-f 432M:435M:1k` |

## Notes specific to Compustar 1WSHR-PRO

- FCC ID `7087A-R762A433` — Firstech's part number convention puts the frequency at the end: `433` → 433.92 MHz
- 1-way FOB (Compustar's W-series and shorty remotes are 1-way; only the T-series 2-way uses 900 MHz)
- OOK (On-Off Keying) modulation, PWM bit encoding
- Transmits in short bursts (~80ms per packet, 3-5 repeats while button held)

## Next

[`04-recording-captures.md`](04-recording-captures.md) — now that we know the frequency, record clean per-button captures we can decode.
