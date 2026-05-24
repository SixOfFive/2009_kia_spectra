"""
Diff two rtl_sdr captures by their demodulated 35-bit Compustar patterns.

Used in ``docs/12-bench-validation.md`` step 3 to confirm that the
synthesized CC1101 transmission produces the same on-air bit pattern
as the genuine FOB:

    python sdr/scripts/diff-captures.py \\
        sdr/captures/fob-lock-001.bin \\
        sdr/captures/synth-lock-001.bin

Output is a side-by-side comparison of the two patterns plus a
``PASS`` / ``FAIL`` summary. On FAIL the position of the first
differing bit is highlighted.

Exit code:
    0  patterns match (PASS)
    1  patterns differ, or one/both captures failed to decode (FAIL)
"""
import argparse
import importlib.util
import os
import sys


HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def _load_helper(module_name, filename):
    """Load a hyphenated-filename Python module from sdr/scripts/."""
    spec = importlib.util.spec_from_file_location(
        module_name, os.path.join(HERE, filename)
    )
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def decode_one(path, sample_rate, decimate_to):
    """
    Decode ``path`` to a single 35-bit pattern string (the majority
    pattern across bursts). Returns (pattern, n_bursts_decoded,
    n_dissenting). On total failure returns (None, 0, 0).
    """
    c2s = _load_helper("capture_to_secrets", "capture-to-secrets.py")
    patterns = c2s.decode_capture(path, sample_rate, decimate_to)
    if not patterns:
        return None, 0, 0
    winner, dissent = c2s.consensus(patterns)
    return winner, len(patterns), len(dissent)


def render_diff(a, b):
    """
    Print a side-by-side comparison of two equal-length bit strings.
    Returns position (0-indexed) of first mismatch, or -1 if equal.
    """
    width = max(len(a), len(b))
    a = a.ljust(width, " ")
    b = b.ljust(width, " ")
    marker = "".join("^" if a[i] != b[i] else " " for i in range(width))
    first_diff = -1
    for i in range(width):
        if a[i] != b[i]:
            first_diff = i
            break
    print(f"  A: {a}")
    print(f"  B: {b}")
    print(f"     {marker}")
    return first_diff


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("capture_a", help="First capture .bin (e.g., genuine FOB)")
    ap.add_argument("capture_b", help="Second capture .bin (e.g., synthesized CC1101 transmit)")
    ap.add_argument("--sample-rate", type=int, default=250_000)
    ap.add_argument("--decimate-to", type=int, default=50_000)
    args = ap.parse_args()

    for p in (args.capture_a, args.capture_b):
        if not os.path.exists(p):
            print(f"ERROR file not found: {p}", file=sys.stderr)
            return 1

    name_a = os.path.basename(args.capture_a)
    name_b = os.path.basename(args.capture_b)

    print(f"Decoding A: {name_a}")
    pat_a, n_a, d_a = decode_one(args.capture_a, args.sample_rate, args.decimate_to)
    if pat_a is None:
        print(f"  FAIL: no packets decoded from {name_a}", file=sys.stderr)
    else:
        print(f"  {n_a} burst pattern(s) decoded ({d_a} dissenting)")

    print(f"Decoding B: {name_b}")
    pat_b, n_b, d_b = decode_one(args.capture_b, args.sample_rate, args.decimate_to)
    if pat_b is None:
        print(f"  FAIL: no packets decoded from {name_b}", file=sys.stderr)
    else:
        print(f"  {n_b} burst pattern(s) decoded ({d_b} dissenting)")

    if pat_a is None or pat_b is None:
        print()
        print("FAIL  one or both captures could not be decoded")
        return 1

    print()
    print("Comparison (35-bit patterns, MSB first):")
    first_diff = render_diff(pat_a, pat_b)

    print()
    if pat_a == pat_b:
        print(f"PASS  captures match — 35 bits identical")
        return 0
    else:
        n_diff = sum(1 for x, y in zip(pat_a, pat_b) if x != y)
        print(f"FAIL  patterns differ — first mismatch at bit {first_diff}, "
              f"{n_diff} of {max(len(pat_a), len(pat_b))} bits differ")
        return 1


if __name__ == "__main__":
    sys.exit(main())
