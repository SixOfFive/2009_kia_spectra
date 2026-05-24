# SDR phase — reverse-engineering the Compustar FOB

This directory holds everything related to capturing, analyzing, and
ultimately replicating the 1WSHR-PRO FOB's rolling-code transmissions
so the ESP32 can play the role of the remote.

If you're working through this for the first time, follow the numbered
files **in order**. Each one is a self-contained step with prerequisites,
exact commands, expected outputs, and where to put the artifacts you
generate.

## Walkthrough

1. [`01-software-setup.md`](01-software-setup.md) — install rtl-sdr drivers, URH, supporting tools
2. [`02-hardware-verification.md`](02-hardware-verification.md) — confirm the dongle is detected and receives signal
3. [`03-frequency-confirmation.md`](03-frequency-confirmation.md) — verify the FOB transmits at 433.92 MHz
4. [`04-recording-captures.md`](04-recording-captures.md) — record clean 1-second IQ samples of each button press
5. [`05-urh-analysis.md`](05-urh-analysis.md) — open captures in Universal Radio Hacker, demodulate to bits
6. [`06-framing-extraction.md`](06-framing-extraction.md) — identify preamble, FOB serial, function codes, hopping code position
7. [`07-key-recovery.md`](07-key-recovery.md) — recover the FOB's KeeLoq device key so the ESP32 can synthesize new packets

## Directory layout

```
sdr/
├── README.md                       (this file)
├── 01-software-setup.md            ┐
├── 02-hardware-verification.md     │
├── 03-frequency-confirmation.md    │  step-by-step walkthrough
├── 04-recording-captures.md        │
├── 05-urh-analysis.md              │
├── 06-framing-extraction.md        │
├── 07-key-recovery.md              ┘
├── captures/        (gitignored — raw IQ binary files, can be huge)
├── scripts/         (helper Python — see scripts/README.md)
│   ├── plot-power-csv.py   (find frequency peaks in rtl_power output)
│   ├── diff-bits.py        (compare two .bits files)
│   ├── try-mfkeys.py       (brute-force a manufacturer-key database)
│   └── validate-key.py     (confirm a candidate device key against captures)
└── analysis/
    ├── framing.md              (committed — your final findings about the FOB protocol)
    ├── screenshots/            (gitignored by default — selectively commit ones worth keeping)
    └── press-logs/             (per-button capture session notes)
```

## What you need before starting

- An RTL-SDR dongle (RTL2832U + R820T2 chipset is standard; verified working with the Vomeko 100kHz–1.7GHz model used in this project)
- A 433 MHz-ish antenna (the stock telescoping whip that ships with most SDR dongles is fine for indoor captures)
- The aftermarket FOB you want to clone (1WSHR-PRO in our case)
- A computer to run the SDR software on (Windows 10/11, Linux, or macOS — instructions cover all three)
- The car within FOB radio range OR a Faraday-style enclosure to isolate captures from background RF (a metal cookie tin or microwave oven works in a pinch)

## Important: this is for your own car

The techniques in this walkthrough — extracting your FOB's device key,
synthesizing valid Keeloq packets — work because **you legitimately own
the FOB**. Doing this to a car you don't own or have permission to
operate is theft, fraud, and a variety of criminal offences depending
on jurisdiction. Don't.
