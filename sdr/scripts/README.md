# SDR helper scripts

Small pure-stdlib Python scripts that support the [SDR walkthrough](../README.md).
All scripts import the KeeLoq cipher from `esp32/src/lib/keeloq.py` so it
stays the single source of truth.

| Script | Used in step | Purpose |
|---|---|---|
| `plot-power-csv.py` | [02](../02-hardware-verification.md), [03](../03-frequency-confirmation.md) | Find frequency peaks in an `rtl_power` CSV |
| `diff-bits.py` | [06](../06-framing-extraction.md) | Compare two `.bits` files, report positions where bits flip |
| `try-mfkeys.py` | [07](../07-key-recovery.md) Path A | Brute-force a manufacturer-key database to derive a candidate device key |
| `validate-key.py` | [07](../07-key-recovery.md) | Decrypt captured hopping codes with a candidate device key, check counter increments by 1 |

Each script accepts `--help`. Example session for the key-recovery phase:

```powershell
# After URH gives you 28-bit serial and one 32-bit hopping code:
python sdr/scripts/try-mfkeys.py --serial 0xABCDEF1 --hopping 0xDEADBEEF --mfkeys keeloq_mfcodes.txt

# Take the device key from a HIT line and confirm with 3 consecutive captures:
python sdr/scripts/validate-key.py --device-key 0xAABBCC... --serial 0xABCDEF1 0xHOP1 0xHOP2 0xHOP3
```

The expected confirmation output: `All deltas = 1. Key is almost certainly correct.`
