# 02 — Verify the dongle works

## Goal

Confirm the SDR receives a real-world signal before going hunting for the FOB. Saves an hour of debugging "is it the antenna, the driver, the dongle, or the FOB?" later.

## Prerequisites

- [01 — software setup](01-software-setup.md) complete
- RTL-SDR plugged in with its stock antenna attached
- You're somewhere that gets at least one FM broadcast station

## Step 1 — Tuner self-test

```powershell
rtl_test -t
```

Expected output (your model may differ slightly):

```
Found 1 device(s):
  0:  Realtek, RTL2838UHIDIR, SN: 00000001

Using device 0: Generic RTL2832U OEM
Found Rafael Micro R820T2 tuner
Supported gain values (29): 0.0 0.9 ... 49.6
[R82XX] PLL not locked!
Sampling at 2048000 S/s.
No E4000 tuner found, aborting.
```

The `No E4000 tuner found` line is fine — we have R820T2. The PLL warning is also fine. What matters is the **first three lines** showing the dongle is detected with a known chipset.

## Step 2 — Receive an FM broadcast station

This is the equivalent of "I plugged in the speakers and heard music." If FM works, the receive chain is healthy end-to-end.

Pick a strong local FM station (Google "FM stations near me" if you don't know one). For example, Edmonton has CKUA on 94.9 MHz. Substitute your own.

```powershell
rtl_fm -f 94.9M -M wbfm -s 200000 -r 48000 - | ffplay -f s16le -ar 48000 -showmode 1 -
```

(Linux / macOS: same command works.)

If you don't have `ffplay` installed, use `sox` or any other s16le sink. Or just write to a `.raw` file and play it later:

```powershell
rtl_fm -f 94.9M -M wbfm -s 200000 -r 48000 fm-test.raw
# (let it record for 10 seconds, Ctrl-C)
# play fm-test.raw  # with sox installed
```

**Expected**: recognizable music or talk radio. Quality won't be great (no de-emphasis, mono) but it should be clearly audible.

If you hear silence or pure noise, **fix this before going further**. Try:

- A different FM station with a stronger signal
- Different antenna orientation (vertical usually helps)
- Different USB port (some USB 3.0 ports radiate noise that drowns out reception)
- A different gain setting: add `-g 40` for max gain

## Step 3 — Scan the 433 MHz ISM band background

Now look at the band where your FOB will transmit. Even with no FOB active, there's usually some background activity from neighbours' garage door openers, weather stations, tire pressure sensors, etc.

```powershell
rtl_power -f 433M:434M:1k -i 1 -g 40 -e 30 sdr/captures/band-baseline.csv
```

- `-f 433M:434M:1k` — sweep 433 to 434 MHz in 1 kHz bins
- `-i 1` — integrate for 1 second per sweep
- `-g 40` — high gain
- `-e 30` — run for 30 seconds total
- output: a CSV time-series of power per frequency

Open `band-baseline.csv` — even raw, you should see one column per frequency bin and one row per sweep. Power values in dB. If everything is `-100 dB` you have no antenna or wrong gain. Normal indoor background sits around `-60 to -80 dB`.

If you want to visualize: load the CSV in Excel/LibreOffice and chart the average per column, or use the included helper:

```powershell
python sdr/scripts/plot-power-csv.py sdr/captures/band-baseline.csv
```

This prints the top peaks and the estimated noise floor — see `sdr/scripts/README.md`.

## What you should have when done

- `rtl_test -t` reports your specific dongle model
- You successfully received an FM station with audio
- A baseline power sweep CSV in `sdr/captures/` showing realistic noise floor and possibly some background peaks

## Where artifacts go

- `sdr/captures/band-baseline.csv` — baseline power sweep (gitignored; keep locally for your own reference, no need to share)

## Troubleshooting

| Symptom | Fix |
|---|---|
| `rtl_test`: USB error | Re-run Zadig (Windows) or replug the dongle (Linux/macOS) |
| FM station: only noise | Try a stronger station; check antenna is screwed in firmly; try `-g 40` |
| FM station: chuffing / dropouts | Switch USB port; cheap USB hubs cause this |
| Power sweep: all -100 dB | Antenna not connected, or PLL never locked — replug the dongle |
| Power sweep: clipped at +5 dB | Gain too high — drop to `-g 20` |

## Next

[`03-frequency-confirmation.md`](03-frequency-confirmation.md) — confirm your FOB transmits on the frequency we think it does (probably 433.92 MHz but worth checking).
