"""
Try each manufacturer key from a Flipper Zero-style keystore to derive a
candidate KeeLoq device key for your FOB. For each (mfr_key, learning_type)
candidate, derive the device key from your serial, decrypt a captured
hopping code, and check the plaintext for plausibility.

A plaintext is "plausible" when:
  - discrimination nibble == low 4 bits of serial (HCS3xx convention)
  - function nibble is nonzero
  - counter < 50000 (a fresh FOB is well below this)

Usage:
    python try-mfkeys.py \\
        --serial 0xABCDEF1 \\
        --hopping 0xDEADBEEF \\
        --mfkeys path/to/keeloq_mfcodes.txt

Keystore file format (one entry per line, whitespace-tolerant):
    KEY_HEX  TYPE  NAME
where TYPE is one of: simple, normal
(Flipper colon-separated form `KEY:TYPE_NUM:NAME` is also accepted, with
TYPE_NUM 1=simple, 2=normal, 3=secure. Secure variants are skipped — they
need a seed value we don't extract from over-the-air captures alone.)

Get the Flipper Unleashed mfcodes file from:
    https://github.com/DarkFlippers/unleashed-firmware
        applications/main/subghz/assets/keeloq_mfcodes
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "esp32", "src"))
from lib import keeloq  # noqa: E402


def derive_simple(serial32, mfr_key):
    """device_key = (serial XOR mfr_high) << 32 | (serial XOR mfr_low)"""
    mfr_high = (mfr_key >> 32) & 0xFFFFFFFF
    mfr_low = mfr_key & 0xFFFFFFFF
    return ((serial32 ^ mfr_high) << 32) | (serial32 ^ mfr_low)


def derive_normal(serial32, mfr_key):
    """
    Microchip AN642 'normal' learning:
        device_key_low  = decrypt(serial | 0x20000000, mfr_key)
        device_key_high = decrypt(serial | 0x60000000, mfr_key)
    """
    low = keeloq.decrypt(serial32 | 0x20000000, mfr_key)
    high = keeloq.decrypt(serial32 | 0x60000000, mfr_key)
    return (high << 32) | low


DERIVATIONS = {
    "simple": derive_simple,
    "normal": derive_normal,
}

TYPE_NUM_MAP = {"1": "simple", "2": "normal", "3": "secure"}


def parse_keystore(path):
    entries = []
    skipped_secure = 0
    with open(path) as f:
        for raw in f:
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            lower = line.lower()
            if lower.startswith(("filetype", "version", "encryption")):
                continue

            if line.count(":") >= 2:
                parts = line.split(":", 2)
                key_hex, type_str, name = parts[0].strip(), parts[1].strip(), parts[2].strip()
                type_name = TYPE_NUM_MAP.get(type_str, type_str.lower())
            else:
                parts = line.split(None, 2)
                if len(parts) < 2:
                    continue
                key_hex = parts[0]
                type_name = parts[1].lower()
                name = parts[2] if len(parts) > 2 else ""

            try:
                key = int(key_hex, 16)
            except ValueError:
                continue

            if type_name == "secure":
                skipped_secure += 1
                continue
            if type_name in DERIVATIONS:
                entries.append((key, type_name, name))
    return entries, skipped_secure


def plausible(plaintext, serial32):
    counter = plaintext & 0xFFFF
    discrim = (plaintext >> 16) & 0xF
    function = (plaintext >> 20) & 0xF
    return (
        discrim == (serial32 & 0xF)
        and function != 0
        and counter < 50000
    )


def main():
    ap = argparse.ArgumentParser(description="Brute-force KeeLoq manufacturer keys against a capture.")
    ap.add_argument("--serial", required=True, help="28-bit FOB serial in hex (0x prefix ok)")
    ap.add_argument("--hopping", required=True, help="32-bit captured hopping code in hex")
    ap.add_argument("--mfkeys", required=True, help="path to keystore file")
    ap.add_argument("--show-all", action="store_true", help="print every candidate, not just hits")
    args = ap.parse_args()

    serial = int(args.serial, 16) & 0xFFFFFFFF
    hopping = int(args.hopping, 16) & 0xFFFFFFFF

    entries, skipped_secure = parse_keystore(args.mfkeys)
    print(f"Loaded {len(entries)} (key, type) pairs from {args.mfkeys}")
    if skipped_secure:
        print(f"Skipped {skipped_secure} 'secure' entries — need seed value not in OTA captures")
    print(f"Serial:  0x{serial:08X}  (low nibble = {serial & 0xF:X})")
    print(f"Hopping: 0x{hopping:08X}")
    print()

    hits = 0
    for mfr_key, type_name, name in entries:
        device_key = DERIVATIONS[type_name](serial, mfr_key)
        plaintext = keeloq.decrypt(hopping, device_key)
        counter = plaintext & 0xFFFF
        discrim = (plaintext >> 16) & 0xF
        function = (plaintext >> 20) & 0xF
        is_hit = plausible(plaintext, serial)
        if is_hit:
            hits += 1
        if is_hit or args.show_all:
            marker = " HIT " if is_hit else "     "
            print(
                f"{marker} {type_name:6} {name[:30]:30}  "
                f"dev=0x{device_key:016X}  plain=0x{plaintext:08X}  "
                f"cnt={counter:5}  discrim={discrim:X}  func={function:X}"
            )

    print()
    print(f"{hits} plausible hit(s) of {len(entries)} candidates.")
    if hits == 0:
        print("No matches. Confirm serial/hopping inputs, then fall back to Path B (PICkit).")
    elif hits > 5:
        print("Many hits — re-run validate-key.py with multiple captures to disambiguate.")


if __name__ == "__main__":
    main()
