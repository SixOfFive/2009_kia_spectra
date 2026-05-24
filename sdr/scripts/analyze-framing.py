"""
Identify the HOPPING vs FUNCTION-code vs SERIAL+STATUS regions of a HCS-style
packet given multiple captures across multiple buttons.

Inputs: glob patterns of .bits files for each button. The script:

1. Computes a per-burst consensus by majority-voting .pN.bits within each
   unique base file (e.g. all fob-start-002-b1.p*.bits vote for one
   consensus, all fob-start-002-b2.p*.bits vote for another, etc.).
2. Groups consensus strings by button.
3. For each bit position 0..N-1, classifies it:

   - HOP (hopping code) — varies between bursts of the SAME button.
     These are the ~32 positions encrypted by KeeLoq.
   - FN  (function code candidate) — stable within each button, value
     differs between buttons. These are the ~4 positions that encode
     which button was pressed.
   - SER (serial / status) — stable within each button AND constant value
     across all buttons. The ~28-bit serial + 2 status bits.
   - ?   — too noisy or insufficient data to classify.

Usage:
    python sdr/scripts/analyze-framing.py \\
        --button start  "sdr/captures/fob-start-*-b*.p*.bits" \\
        --button lock   "sdr/captures/fob-lock-*-b*.p*.bits" \\
        --button unlock "sdr/captures/fob-unlock-*-b*.p*.bits" \\
        --button trunk  "sdr/captures/fob-trunk-*-b*.p*.bits"

Output: a per-position table showing per-button consensus value plus the
final classification. Designed to be eyeballed; downstream you'd record
the conclusions in framing.local.md (gitignored).
"""
import argparse
import glob
import os
import re
import sys
from collections import defaultdict


def load_bits(path):
    bits = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or line.startswith("["):
                continue
            for c in line:
                if c in "01":
                    bits.append(int(c))
    return bits


def majority_vote(bit_lists, threshold=0.7):
    """Return list of bits with '?' where confidence < threshold."""
    if not bit_lists:
        return []
    n = len(bit_lists[0])
    if not all(len(b) == n for b in bit_lists):
        # Reject voters with non-matching lengths
        most_common = max(set(len(b) for b in bit_lists),
                          key=[len(b) for b in bit_lists].count)
        bit_lists = [b for b in bit_lists if len(b) == most_common]
        n = most_common
    if not bit_lists:
        return []

    voters = len(bit_lists)
    out = []
    for i in range(n):
        ones = sum(b[i] for b in bit_lists)
        zeros = voters - ones
        conf = max(ones, zeros) / voters
        if conf < threshold:
            out.append(None)
        else:
            out.append(1 if ones > zeros else 0)
    return out


def group_by_burst(paths):
    """
    Group .bits files by their base burst — i.e. strip the .pN.bits suffix
    and group files sharing the same base (those were demodulated repeats
    of the same press).
    """
    groups = defaultdict(list)
    for p in paths:
        base = re.sub(r"\.p\d+\.bits$", ".bits", p)
        base = re.sub(r"\.bits$", "", base)
        groups[base].append(p)
    return groups


def stable_value(consensuses):
    """
    Given a list of per-burst consensus strings (each a list of 0/1/None),
    return a per-position summary: same length list where each entry is
    either 0, 1, None (unstable across bursts), or 'X' (insufficient data).
    """
    if not consensuses:
        return []
    n = max(len(c) for c in consensuses)
    out = []
    for i in range(n):
        values = [c[i] for c in consensuses if i < len(c) and c[i] is not None]
        if not values:
            out.append("X")
            continue
        if all(v == values[0] for v in values):
            out.append(values[0])
        else:
            out.append(None)
    return out


def classify_position(per_button_stable):
    """
    Given a dict {button: stable_value_at_this_position}, classify:
    - HOP if any button has None (unstable across that button's bursts)
    - SER if all buttons have the SAME stable value
    - FN if all buttons have stable values that DIFFER
    - X if any button has insufficient data
    """
    vals = list(per_button_stable.values())
    if any(v == "X" for v in vals):
        return "X"
    if any(v is None for v in vals):
        return "HOP"
    if len(set(vals)) == 1:
        return "SER"
    return "FN"


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--button", action="append", nargs=2,
                    metavar=("LABEL", "GLOB"), required=True,
                    help="button label + glob pattern; repeat per button")
    ap.add_argument("--vote-threshold", type=float, default=0.6,
                    help="per-burst majority-vote confidence threshold")
    args = ap.parse_args()

    button_consensus = {}   # label -> list of per-burst consensus strings
    for label, pattern in args.button:
        paths = sorted(glob.glob(pattern))
        if not paths:
            print(f"WARN button {label!r}: no files matched {pattern!r}")
            continue
        bursts = group_by_burst(paths)
        print(f"Button {label!r}: {len(paths)} packet files in {len(bursts)} bursts")
        per_burst = []
        for base, ps in sorted(bursts.items()):
            bit_lists = [load_bits(p) for p in ps]
            bit_lists = [b for b in bit_lists if b]
            if not bit_lists:
                continue
            c = majority_vote(bit_lists, threshold=args.vote_threshold)
            per_burst.append(c)
            n_def = sum(1 for b in c if b is not None)
            print(f"   {base}: {len(ps)} voters, consensus has {n_def}/{len(c)} confident bits")
        button_consensus[label] = per_burst
    print()

    if not button_consensus:
        sys.exit("No data loaded.")

    # All consensus strings should be same length
    bit_length = max(
        len(c) for cs in button_consensus.values() for c in cs
    ) if any(button_consensus.values()) else 0
    if not bit_length:
        sys.exit("No bits in any consensus.")

    # For each button, find positions stable within that button's bursts
    per_button_stable = {}
    for label, cs in button_consensus.items():
        per_button_stable[label] = stable_value(cs)
        n_stable = sum(1 for v in per_button_stable[label] if v in (0, 1))
        n_unstable = sum(1 for v in per_button_stable[label] if v is None)
        n_unknown = sum(1 for v in per_button_stable[label] if v == "X")
        print(f"Button {label!r}: {n_stable} stable / {n_unstable} unstable / "
              f"{n_unknown} no-data positions across its bursts")
    print()

    # Per-position classification
    print(f"Per-position classification (length = {bit_length}):")
    header = "pos | " + " | ".join(f"{lbl:>6}" for lbl in per_button_stable) + " | class"
    print(header)
    print("-" * len(header))

    classes_count = defaultdict(int)
    classification = []
    for i in range(bit_length):
        per_button_val = {
            lbl: (per_button_stable[lbl][i] if i < len(per_button_stable[lbl]) else "X")
            for lbl in per_button_stable
        }
        cls = classify_position(per_button_val)
        classes_count[cls] += 1
        classification.append(cls)
        row = f"{i:3d} | " + " | ".join(
            f"{'?' if v is None else 'X' if v == 'X' else str(v):>6}"
            for v in per_button_val.values()
        ) + f" | {cls}"
        print(row)

    print()
    print("Summary:")
    for k, v in sorted(classes_count.items()):
        print(f"  {k}: {v}")

    # Print the classification map as a single string for easy paste
    label_map = {"SER": "S", "FN": "F", "HOP": "H", "X": "."}
    print()
    print("Map: S=serial, F=function-code, H=hopping, .=unknown")
    print("    " + "".join(label_map[c] for c in classification))

    # If we have FN positions, show the per-button bits there (= function code)
    fn_positions = [i for i, c in enumerate(classification) if c == "FN"]
    if fn_positions:
        print()
        print(f"Function code positions ({len(fn_positions)} bits): {fn_positions}")
        for lbl in per_button_stable:
            bits = "".join(
                str(per_button_stable[lbl][i]) for i in fn_positions
            )
            val = int(bits, 2) if bits else 0
            print(f"  {lbl:>8}: bits {bits} = 0x{val:X}")

    # Same for SER positions
    ser_positions = [i for i, c in enumerate(classification) if c == "SER"]
    if ser_positions:
        # Just print the SER value from any button (they all agree)
        first_btn = next(iter(per_button_stable))
        bits = "".join(str(per_button_stable[first_btn][i]) for i in ser_positions)
        print()
        print(f"Serial+status positions ({len(ser_positions)} bits): bits {bits}")
        if len(ser_positions) >= 28:
            # First 28 = serial in HCS standard layout (if our 0-based positions align)
            serial_bits = bits[:28]
            print(f"  If layout = [serial(28) | function(4) | status(2)]: serial = 0x{int(serial_bits,2):07X}")


if __name__ == "__main__":
    main()
