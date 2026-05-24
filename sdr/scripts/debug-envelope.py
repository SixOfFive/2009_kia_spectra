"""
Diagnostic dump of an OOK capture: histogram of run lengths, first 50
runs, envelope statistics. Use this when demod-ook.py can't find packets
to understand what's actually in the signal.

Usage:
    python sdr/scripts/debug-envelope.py sdr/captures/fob-start-001-b1.bin
"""
import argparse
import os
import sys
from collections import Counter

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import importlib.util
_spec = importlib.util.spec_from_file_location("demod_ook", os.path.join(HERE, "demod-ook.py"))
demod_ook = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(demod_ook)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("path")
    ap.add_argument("--sample-rate", type=int, default=250_000)
    ap.add_argument("--downsample", type=int, default=2)
    ap.add_argument("--bucket-samples", type=int, default=10,
                    help="bucket size for run-length histogram")
    args = ap.parse_args()

    env = demod_ook.read_envelope(args.path, args.downsample)
    binary, threshold = demod_ook.threshold_binary(env)
    runs = demod_ook.run_lengths(binary)

    # Envelope stats
    lo = sorted(env)[len(env) // 20]
    hi = sorted(env)[len(env) * 19 // 20]
    print(f"Envelope stats (after {args.downsample}x downsample):")
    print(f"  blocks: {len(env)}")
    print(f"  5th percentile (noise floor): {lo:.2f}")
    print(f"  95th percentile (signal):     {hi:.2f}")
    print(f"  threshold used:               {threshold:.2f}")
    print(f"  total runs:                   {len(runs)}")

    # Run length histogram, separately for high and low
    bucket = args.bucket_samples
    hi_hist = Counter()
    lo_hist = Counter()
    for val, length in runs:
        b = (length // bucket) * bucket
        if val:
            hi_hist[b] += 1
        else:
            lo_hist[b] += 1

    def show_hist(name, hist, limit=20):
        print(f"\n{name} run lengths (envelope-samples, bucket={bucket}):")
        max_count = max(hist.values()) if hist else 1
        for length in sorted(hist):
            count = hist[length]
            bar = "#" * int(40 * count / max_count)
            us = length * args.downsample * 1_000_000 / args.sample_rate
            print(f"  {length:5d}-{length+bucket-1:<5d}  ({us:6.0f} us)  {count:4d}  {bar}")
            if length // bucket >= limit:
                rest = sum(c for l, c in hist.items() if l // bucket > limit)
                if rest:
                    print(f"  >{(limit+1)*bucket:5d}                       {rest:4d}  (truncated)")
                break

    show_hist("HIGH", hi_hist)
    show_hist("LOW", lo_hist)

    # First 30 runs verbatim
    print("\nFirst 30 runs:")
    sr = args.sample_rate
    ds = args.downsample
    for i, (val, length) in enumerate(runs[:30]):
        us = length * ds * 1_000_000 / sr
        marker = "HIGH" if val else "low "
        print(f"  {i:3d}  {marker}  length={length:5d}  ({us:7.1f} us)")


if __name__ == "__main__":
    main()
