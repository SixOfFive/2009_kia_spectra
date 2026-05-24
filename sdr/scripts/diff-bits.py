"""
Compare two .bits files and report positions where they differ.

Used in step 06 (framing extraction) to identify which bits in a packet
are the rolling hopping code, which are the constant serial, and which
are the function code that changes between buttons.

Usage:
    python diff-bits.py A.bits B.bits

.bits files: one bit per character ('0' or '1'). Lines starting with '#'
or '[' are treated as comments (so URH labels like [gap] are ignored).
Whitespace within and between lines is ignored.
"""
import sys


def load_bits(path):
    bits = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or line.startswith("["):
                continue
            for ch in line:
                if ch in "01":
                    bits.append(int(ch))
    return bits


def runs_from_positions(positions):
    """Group sorted positions into contiguous runs as (start, end) inclusive."""
    if not positions:
        return []
    runs = []
    start = prev = positions[0]
    for p in positions[1:]:
        if p == prev + 1:
            prev = p
        else:
            runs.append((start, prev))
            start = prev = p
    runs.append((start, prev))
    return runs


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: diff-bits.py A.bits B.bits")

    a = load_bits(sys.argv[1])
    b = load_bits(sys.argv[2])
    if len(a) != len(b):
        sys.exit(f"length mismatch: {len(a)} vs {len(b)} — re-check URH demodulation")

    diffs = [i for i in range(len(a)) if a[i] != b[i]]
    print(f"{len(diffs)} of {len(a)} bits differ ({100 * len(diffs) / len(a):.1f}%)")

    if not diffs:
        print("Identical streams — same press captured twice, or counter not incrementing.")
        return

    runs = runs_from_positions(diffs)
    print(f"Differing positions: {diffs}")
    print(f"Contiguous runs:")
    for start, end in runs:
        width = end - start + 1
        print(f"  {start:3d}..{end:3d}  ({width} bit{'s' if width > 1 else ''})")


if __name__ == "__main__":
    main()
