"""
Sanity-check a candidate KeeLoq device key against captured hopping codes.

If the key is correct AND the captures are consecutive FOB presses, the
decrypted counter should increment by exactly 1 between them. The
discrimination nibble should match the low 4 bits of your serial, and the
function nibble should match the on-wire function code.

Usage:
    python validate-key.py \\
        --device-key 0xAABBCCDDEEFF0011 \\
        --serial 0xABCDEF1 \\
        0xHOPPING1 0xHOPPING2 0xHOPPING3
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "esp32", "src"))
from lib import keeloq  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--device-key", required=True, help="64-bit device key (hex)")
    ap.add_argument("--serial", required=True, help="28-bit serial (hex)")
    ap.add_argument("hopping", nargs="+", help="one or more 32-bit hopping codes (hex)")
    args = ap.parse_args()

    key = int(args.device_key, 16) & 0xFFFFFFFFFFFFFFFF
    serial = int(args.serial, 16) & 0xFFFFFFFF
    serial_low = serial & 0xF

    print(f"Device key:  0x{key:016X}")
    print(f"Serial:      0x{serial:08X}  (low nibble = {serial_low:X})")
    print()
    print(f"{'#':<3} {'hopping':<12} {'plaintext':<12} {'counter':>8} {'discrim':>8} {'func':>5}  ok?")
    print("-" * 64)

    counters = []
    all_ok = True
    for i, hop_hex in enumerate(args.hopping):
        hop = int(hop_hex, 16) & 0xFFFFFFFF
        plain = keeloq.decrypt(hop, key)
        counter = plain & 0xFFFF
        discrim = (plain >> 16) & 0xF
        function = (plain >> 20) & 0xF
        ok = discrim == serial_low and function != 0 and counter < 50000
        all_ok = all_ok and ok
        print(
            f"{i+1:<3} 0x{hop:08X}  0x{plain:08X}  "
            f"{counter:>8} {discrim:>8X} {function:>5X}  {'yes' if ok else 'NO'}"
        )
        counters.append(counter)

    if len(counters) >= 2:
        deltas = [counters[i + 1] - counters[i] for i in range(len(counters) - 1)]
        print()
        print(f"Counter deltas: {deltas}")
        if all_ok and all(d == 1 for d in deltas):
            print("All deltas = 1. Key is almost certainly correct.")
        elif all_ok and all(0 < d < 20 for d in deltas):
            print("Deltas look like real consecutive presses. Likely correct.")
        else:
            print("Deltas look wrong — key may be incorrect, or captures aren't consecutive presses.")


if __name__ == "__main__":
    main()
