"""
Majority-vote across multiple decoded .bits files to produce a consensus
bit pattern. Useful when demod-ook.py is noisy at certain positions
(typical when the FOB's encoding has pulses with near-equal HIGH/LOW
durations — the bit decision becomes noisy at those positions).

Usage:
    # Consensus across all .pN.bits files in a directory matching a glob:
    python sdr/scripts/consensus-bits.py "sdr/captures/fob-start-*-b*.p*.bits"

    # Verbose mode shows per-position vote breakdown:
    python sdr/scripts/consensus-bits.py "sdr/captures/fob-start-*.bits" --verbose

Output:
    Per-position consensus, plus a confidence indicator (% of voters
    agreeing at each position). Bits where confidence is below 75% are
    flagged with '?' for follow-up investigation.
"""
import argparse
import glob
import sys


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


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("pattern", help="glob pattern matching .bits files")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--confidence-threshold", type=float, default=0.75,
                    help="positions below this confidence are flagged with '?'")
    args = ap.parse_args()

    paths = sorted(glob.glob(args.pattern))
    if not paths:
        sys.exit(f"no files matched pattern: {args.pattern}")

    all_bits = []
    for p in paths:
        b = load_bits(p)
        if not b:
            print(f"WARN {p}: empty")
            continue
        all_bits.append((p, b))

    if not all_bits:
        sys.exit("No bits loaded from any file.")

    # Find common length (some packets may be truncated)
    lengths = [len(b) for _, b in all_bits]
    common_len = max(set(lengths), key=lengths.count)
    voters = [(p, b) for p, b in all_bits if len(b) == common_len]
    rejected = [(p, b) for p, b in all_bits if len(b) != common_len]

    print(f"Files matched: {len(paths)}")
    print(f"Bit lengths seen: {sorted(set(lengths))}")
    print(f"Using {len(voters)} voters at length {common_len} ({len(rejected)} rejected)")
    print()

    if not voters:
        sys.exit("No usable voters.")

    n = len(voters)
    consensus = []
    confidences = []
    flagged = []
    for i in range(common_len):
        ones = sum(b[i] for _, b in voters)
        zeros = n - ones
        bit = 1 if ones > zeros else 0
        conf = max(ones, zeros) / n
        consensus.append(bit)
        confidences.append(conf)
        if conf < args.confidence_threshold:
            flagged.append((i, ones, zeros))

    out = []
    for i, b in enumerate(consensus):
        out.append("?" if confidences[i] < args.confidence_threshold else str(b))

    print("Consensus:")
    print("".join(out))
    print()
    print(f"Confident positions: {common_len - len(flagged)}/{common_len}")
    print(f"Low-confidence positions ('?'): {len(flagged)}")

    if args.verbose and flagged:
        print()
        print("Low-confidence breakdown:")
        for i, ones, zeros in flagged:
            print(f"  pos {i:3d}: ones={ones}/{n}, zeros={zeros}/{n} (conf {max(ones,zeros)/n*100:.0f}%)")

    if args.verbose:
        print()
        print("Per-voter bits (compared to consensus):")
        for p, b in voters:
            diffs = sum(1 for i in range(common_len) if b[i] != consensus[i])
            print(f"  {p}: {diffs} bits differ from consensus")


if __name__ == "__main__":
    main()
