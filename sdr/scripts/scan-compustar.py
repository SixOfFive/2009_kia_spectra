"""
Brute-force scan for a valid Compustar packet in a capture.

Strategy: convert the WHOLE capture into a continuous bit stream by
classifying every HIGH pulse as either "0" (short ≈ 708us), "1" (long ≈
1076us), or "X" (sync ≈ 1448us — treated as bit separators/markers).
Then scan every starting offset in the bit stream looking for a window
of 36 bits that satisfies the protocol checks:
  - bits[16..18] == 0b111
  - bits[35] == 0
  - bits[19..26] XOR bits[27..34] == 0xFF

When found, print the Remote ID + button code + bit offset.

Usage:
    python sdr/scripts/scan-compustar.py sdr/captures/fob-start-lg20-b1.bin
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import importlib.util
_spec = importlib.util.spec_from_file_location("demod_ook", os.path.join(HERE, "demod-ook.py"))
demod_ook = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(demod_ook)


def classify_high_pulses(runs, short_us, long_us, sync_us, eff_rate):
    """For every HIGH run, return its symbol: '0', '1', or 'X' (sync)."""
    short_samples = short_us * eff_rate / 1_000_000
    long_samples = long_us * eff_rate / 1_000_000
    sync_samples = sync_us * eff_rate / 1_000_000
    # Boundaries
    short_long_mid = (short_samples + long_samples) / 2
    long_sync_mid = (long_samples + sync_samples) / 2

    symbols = []
    for val, length in runs:
        if val != 1:
            continue
        if length < short_samples * 0.5:
            symbols.append('?')
        elif length < short_long_mid:
            symbols.append('0')
        elif length < long_sync_mid:
            symbols.append('1')
        elif length < sync_samples * 1.5:
            symbols.append('X')
        else:
            symbols.append('!')  # too long
    return symbols


def try_decode(bits, offset):
    """Try parsing 36 bits starting at offset. Return parsed dict or None."""
    if offset + 36 > len(bits):
        return None
    p = bits[offset:offset + 36]
    if any(b not in (0, 1) for b in p):
        return None
    # Unk1 check
    unk1 = (p[16] << 2) | (p[17] << 1) | p[18]
    if unk1 != 0b111:
        return None
    # Unk2 check
    if p[35] != 0:
        return None
    button_inv = 0
    for i in range(19, 27):
        button_inv = (button_inv << 1) | p[i]
    button = 0
    for i in range(27, 35):
        button = (button << 1) | p[i]
    if (button_inv ^ button) != 0xFF:
        return None
    remote_id = 0
    for i in range(16):
        remote_id = (remote_id << 1) | p[i]
    return {"offset": offset, "remote_id": remote_id, "button": button,
            "button_inverse": button_inv}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("path")
    ap.add_argument("--sample-rate", type=int, default=250_000)
    ap.add_argument("--short-us", type=int, default=708)
    ap.add_argument("--long-us", type=int, default=1076)
    ap.add_argument("--sync-us", type=int, default=1448)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.path):
        sys.exit(f"file not found: {args.path}")

    env, eff_rate = demod_ook.read_magnitude_envelope(
        args.path, args.sample_rate, 50_000
    )
    regions, _, _ = demod_ook.find_burst_regions(env, eff_rate)
    if not regions:
        sys.exit("no burst regions")

    print(f"File: {args.path}")
    print(f"Bursts: {len(regions)}")

    for r_idx, (start, end) in enumerate(regions):
        chunk = env[start:end]
        sorted_chunk = sorted(chunk)
        n = len(sorted_chunk)
        lo_mode = sorted_chunk[n * 10 // 100]
        hi_mode = sorted_chunk[n * 90 // 100]
        thr = (lo_mode + hi_mode) / 2
        binary = [1 if v > thr else 0 for v in chunk]
        runs = demod_ook.run_lengths(binary)

        symbols = classify_high_pulses(runs, args.short_us, args.long_us,
                                       args.sync_us, eff_rate)
        sym_str = "".join(symbols)
        if args.verbose:
            print(f"\nBurst {r_idx+1}: {len(symbols)} HIGH pulses")
            # Show in groups of 36
            for i in range(0, len(sym_str), 36):
                print(f"  [{i:4d}] {sym_str[i:i+36]}")

        # Try every offset, look for valid packet
        bits_only = [int(c) if c in '01' else -1 for c in symbols]
        valid_packets = []
        for offset in range(len(bits_only) - 36 + 1):
            p = try_decode(bits_only, offset)
            if p:
                valid_packets.append(p)

        if valid_packets:
            print(f"  Valid packets found at offsets: "
                  f"{[p['offset'] for p in valid_packets[:10]]}")
            # Report most-common values
            from collections import Counter
            ids = Counter(p["remote_id"] for p in valid_packets)
            buttons = Counter(p["button"] for p in valid_packets)
            print(f"  Most common Remote IDs: "
                  f"{[(f'0x{i:04X}', c) for i, c in ids.most_common(3)]}")
            print(f"  Most common button codes: "
                  f"{[(f'0x{b:02X}', c) for b, c in buttons.most_common(3)]}")
        else:
            print("  No valid packets at any offset")


if __name__ == "__main__":
    main()
