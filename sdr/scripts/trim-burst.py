"""
Extract a single packet burst from a long rtl_sdr capture as its own
smaller .bin file. Useful when URH stalls trying to render a 15-second
multi-press recording — give it a 0.5-1 second single-burst file instead.

Internally reuses the burst detector from inspect-capture.py.

Usage:
    # Trim ALL detected bursts in start-001.bin into start-001-b1.bin,
    # start-001-b2.bin, ... with 100ms of padding on each side:
    python sdr/scripts/trim-burst.py sdr/captures/fob-start-001.bin

    # Just one specific burst (1-indexed, matches inspect-capture.py output):
    python sdr/scripts/trim-burst.py sdr/captures/fob-start-001.bin --burst 3

    # Override defaults:
    python sdr/scripts/trim-burst.py sdr/captures/fob-start-001.bin --pad-ms 200 --sample-rate 250000
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

# Reuse the burst finder
import importlib.util
_spec = importlib.util.spec_from_file_location(
    "inspect_capture", os.path.join(HERE, "inspect-capture.py")
)
inspect_capture = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(inspect_capture)


def extract_range(path, sample_rate, start_s, end_s, out_path):
    start_byte = int(start_s * sample_rate) * 2   # 2 bytes per IQ sample
    end_byte = int(end_s * sample_rate) * 2
    with open(path, "rb") as src, open(out_path, "wb") as dst:
        src.seek(start_byte)
        remaining = end_byte - start_byte
        while remaining > 0:
            chunk = src.read(min(65536, remaining))
            if not chunk:
                break
            dst.write(chunk)
            remaining -= len(chunk)
    return os.path.getsize(out_path)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("path")
    ap.add_argument("--sample-rate", type=int, default=250_000)
    ap.add_argument("--block-ms", type=float, default=2.0)
    ap.add_argument("--threshold-db", type=float, default=3.0)
    ap.add_argument("--pad-ms", type=float, default=100,
                    help="padding before/after each burst (ms)")
    ap.add_argument("--burst", type=int, default=None,
                    help="extract only this burst index (1-based)")
    ap.add_argument("--min-peak-ratio", type=float, default=15.0,
                    help="skip bursts with peak/floor below this (filters weak chatter)")
    args = ap.parse_args()

    if not os.path.exists(args.path):
        sys.exit(f"file not found: {args.path}")

    duration = os.path.getsize(args.path) / 2 / args.sample_rate
    block = max(1, int(args.sample_rate * args.block_ms / 1000))
    env = list(inspect_capture.power_envelope(args.path, args.sample_rate, block))
    bursts, floor = inspect_capture.find_bursts(env, args.threshold_db)

    # Filter weak chatter
    strong = [(s, e, r) for s, e, r in bursts if r >= args.min_peak_ratio]
    print(f"Detected {len(bursts)} bursts total, {len(strong)} above {args.min_peak_ratio}x floor")
    if not strong:
        sys.exit("No strong bursts found — re-record or lower --min-peak-ratio")

    pad_s = args.pad_ms / 1000.0
    base, ext = os.path.splitext(args.path)

    selected = []
    if args.burst is not None:
        if args.burst < 1 or args.burst > len(strong):
            sys.exit(f"burst {args.burst} out of range 1..{len(strong)}")
        selected = [(args.burst, strong[args.burst - 1])]
    else:
        selected = list(enumerate(strong, 1))

    print()
    for i, (start_s, end_s, ratio) in selected:
        clip_start = max(0.0, start_s - pad_s)
        clip_end = min(duration, end_s + pad_s)
        out_path = f"{base}-b{i}{ext}"
        size = extract_range(args.path, args.sample_rate, clip_start, clip_end, out_path)
        print(f"  burst {i}: {clip_start:.3f}-{clip_end:.3f}s "
              f"({(clip_end - clip_start) * 1000:.0f} ms, peak {ratio:.1f}x floor) "
              f"-> {out_path} ({size:,} bytes)")


if __name__ == "__main__":
    main()
