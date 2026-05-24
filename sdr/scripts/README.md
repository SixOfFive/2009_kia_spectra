# SDR helper scripts

Pure-stdlib Python scripts that support the [SDR walkthrough](../README.md).
No third-party dependencies — every script runs on any Python 3.x install.
The HCS-KeeLoq helper scripts (`try-mfkeys.py`, `validate-key.py`)
import the KeeLoq cipher from `esp32/src/lib/keeloq.py` to keep it
as the single source of truth; none of the other scripts pull from
the ESP32 firmware tree.

| Script | Used in step | Purpose |
|---|---|---|
| `plot-power-csv.py` | [02](../02-hardware-verification.md), [03](../03-frequency-confirmation.md) | Find frequency peaks in an `rtl_power` CSV |
| `inspect-capture.py` | [04](../04-recording-captures.md) | Pre-demod sanity check on a `.bin` capture — finds bursts above the noise floor, reports timing + peak/floor ratios |
| `trim-burst.py` | [04](../04-recording-captures.md), [05](../05-demodulation.md) | Extract each strong burst from a long capture as its own small `.bin` |
| `demod-compustar.py` | [05](../05-demodulation.md) | **Compustar 1WG3R-family decoder.** Symmetric-PWM, 3-sync triplet + 35-bit data. Use this one for 1WSHR-PRO / 1WG3R-SH / 1WAMR-1900 FOBs. |
| `demod-ook.py` | [05](../05-demodulation.md) | Generic OOK / HCS-PWM demodulator. Use for non-Compustar HCS-family FOBs. |
| `scan-compustar.py` | [05](../05-demodulation.md) troubleshooting | Brute-force packet alignment finder — useful when `demod-compustar.py` can't find sync |
| `debug-envelope.py` | troubleshooting | Diagnostic dump of envelope statistics + run-length histograms when no packets decode |
| `consensus-bits.py` | [06](../06-framing-extraction.md) (HCS-KeeLoq path) | Majority-vote across .bits files to clean up a noisy multi-decode |
| `analyze-framing.py` | [06](../06-framing-extraction.md) (HCS-KeeLoq path) | Classify each bit position as HOPPING / FUNCTION-code / SERIAL across captures |
| `diff-bits.py` | [06](../06-framing-extraction.md) (HCS-KeeLoq path) | Compare two `.bits` files, report positions where bits flip |
| `try-mfkeys.py` | [07](../07-key-recovery.md) (HCS-KeeLoq only) | Brute-force a manufacturer-key database to derive a candidate device key |
| `validate-key.py` | [07](../07-key-recovery.md) (HCS-KeeLoq only) | Decrypt captured hopping codes with a candidate device key, check counter increments by 1 |

Each script accepts `--help`. Example end-to-end (Compustar 1WG3R-family path):

```powershell
# Step 03: find the FOB carrier frequency
python sdr/scripts/plot-power-csv.py sdr/captures/fob-frequency-sweep.csv

# Step 04: confirm the captures are real bursts (after rtl_sdr recording)
python sdr/scripts/inspect-capture.py sdr/captures/fob-start-001.bin

# Step 05: trim multi-press captures, then demodulate single bursts
python sdr/scripts/trim-burst.py sdr/captures/fob-start-001.bin
python sdr/scripts/demod-compustar.py sdr/captures/fob-start-001-b1.bin --verbose
```

Output of `demod-compustar.py --verbose` prints the Remote ID and the
35-bit pattern; copy those into `sdr/analysis/framing.local.md`
(gitignored) and you're done with the SDR phase.

Example end-to-end (HCS-KeeLoq path — only if your FOB has rolling code):

```powershell
python sdr/scripts/demod-ook.py sdr/captures/fob-start-001-b1.bin
python sdr/scripts/diff-bits.py sdr/captures/fob-start-001-b1.bits \
    sdr/captures/fob-start-002-b1.bits
python sdr/scripts/try-mfkeys.py --serial 0xABCDEF1 --hopping 0xDEADBEEF \
    --mfkeys keeloq_mfcodes.txt
python sdr/scripts/validate-key.py --device-key 0xAABBCC... \
    --serial 0xABCDEF1 0xHOP1 0xHOP2 0xHOP3
```

The expected key-recovery confirmation: `All deltas = 1. Key is almost
certainly correct.`
