# SDR capture + Keeloq analysis

Tools and notes for reverse-engineering the Compustar FOB transmission so the ESP32 can synthesize valid packets.

## Hardware

- **RTL-SDR:** Vomeko 100 kHz – 1.7 GHz with HF upconverter (R820T2 + RTL2832U)
- 433 MHz antenna (the stock telescoping whip works fine for inside-the-house captures)

## Software dependencies

- `rtl-sdr` — drivers and command-line tools (`rtl_sdr`, `rtl_power`, `rtl_test`)
- **URH (Universal Radio Hacker)** — pip-installable GUI for signal analysis
- Optional: GQRX for live spectrum viewing, `inspectrum` for offline inspection

```bash
# Linux
sudo apt install rtl-sdr librtlsdr-dev
pip install urh

# Windows
# Use Zadig to install the RTL-SDR USB driver
# Download URH installer from github.com/jopohl/urh/releases
```

## Capture workflow

### Step 1 — Confirm frequency

```bash
rtl_power -f 433M:434M:1k -i 1 -g 40 -e 60 fob_sweep.csv
```

Run while pressing the FOB Start button. The transmission should peak at 433.92 MHz.

### Step 2 — Capture clean IQ samples

```bash
rtl_sdr -f 433920000 -s 2000000 -g 40 captures/fob_start_001.bin
```

Press Start once, release, Ctrl-C the capture after ~1 second. Repeat 10× into separate files. Repeat for each button on the FOB.

**`captures/` is in `.gitignore` — these files are huge.** Keep the raw bins locally only.

### Step 3 — Demodulate in URH

1. Open URH → File → Import → Complex Signal → choose your .bin
2. Set sample rate to 2 MS/s
3. Switch modulation to ASK
4. URH auto-detects bit length
5. Look at the demodulated bitstream — expect:
   - ~12-cycle preamble
   - ~10×TE header gap
   - 66 bits of data payload

### Step 4 — Identify framing

Compare captures of:
- Multiple Start presses → 32 bits change (hopping code), 28 stay same (serial), function code identifies Start
- Different buttons → 4-bit function code differs

Document findings in `framing.md` (to be created).

## Notes (to be filled in as we learn)

- [ ] Confirmed frequency: _____
- [ ] TE (bit period): _____
- [ ] FOB serial (28-bit, hex): _____
- [ ] Function code Start: _____
- [ ] Function code Lock: _____
- [ ] Function code Unlock: _____
- [ ] Encoder chip identified: _____
