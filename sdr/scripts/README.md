# SDR helper scripts

Pure-stdlib Python scripts that support the [SDR walkthrough](../README.md).
No third-party dependencies — every script runs on any Python 3.x install.
All scripts import the KeeLoq cipher from `esp32/src/lib/keeloq.py` so it
stays the single source of truth.

| Script | Used in step | Purpose |
|---|---|---|
| `plot-power-csv.py` | [02](../02-hardware-verification.md), [03](../03-frequency-confirmation.md) | Find frequency peaks in an `rtl_power` CSV |
| `inspect-capture.py` | [04](../04-recording-captures.md) | Pre-demod sanity check on a `.bin` capture — finds bursts above the noise floor, reports timing + peak/floor ratios |
| `trim-burst.py` | [04](../04-recording-captures.md), [05](../05-demodulation.md) | Extract each strong burst from a long capture as its own small `.bin` |
| `demod-ook.py` | [05](../05-demodulation.md) | Headless OOK demodulator — turns an IQ capture into a 66-bit `.bits` packet. Replaces URH for our purposes. |
| `debug-envelope.py` | troubleshooting | Diagnostic dump of envelope statistics + run-length histograms when `demod-ook.py` can't find packets |
| `consensus-bits.py` | [06](../06-framing-extraction.md) | Majority-vote across .bits files (multiple decodes of same press) to produce a clean bit string |
| `analyze-framing.py` | [06](../06-framing-extraction.md) | Classify each bit position as HOPPING / FUNCTION-code / SERIAL given multiple captures across multiple buttons |
| `diff-bits.py` | [06](../06-framing-extraction.md) | Compare two `.bits` files, report positions where bits flip |
| `try-mfkeys.py` | [07](../07-key-recovery.md) Path A | Brute-force a manufacturer-key database to derive a candidate device key |
| `validate-key.py` | [07](../07-key-recovery.md) | Decrypt captured hopping codes with a candidate device key, check counter increments by 1 |

Each script accepts `--help`. Example end-to-end:

```powershell
# Step 03: find the FOB carrier frequency
python sdr/scripts/plot-power-csv.py sdr/captures/fob-frequency-sweep.csv

# Step 04: confirm the captures are real bursts (after rtl_sdr recording)
python sdr/scripts/inspect-capture.py sdr/captures/fob-start-001.bin

# Step 05: trim multi-press captures, then demodulate single bursts
python sdr/scripts/trim-burst.py sdr/captures/fob-start-001.bin
python sdr/scripts/demod-ook.py sdr/captures/fob-start-001-b1.bin

# Step 06: diff packets to find the hopping-code region
python sdr/scripts/diff-bits.py sdr/captures/fob-start-001-b1.bits \
    sdr/captures/fob-start-002-b1.bits

# Step 07: brute-force manufacturer keys, then validate
python sdr/scripts/try-mfkeys.py --serial 0xABCDEF1 --hopping 0xDEADBEEF \
    --mfkeys keeloq_mfcodes.txt
python sdr/scripts/validate-key.py --device-key 0xAABBCC... \
    --serial 0xABCDEF1 0xHOP1 0xHOP2 0xHOP3
```

The expected key-recovery confirmation: `All deltas = 1. Key is almost
certainly correct.`
