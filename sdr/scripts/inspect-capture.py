"""
Quick visualizer for an rtl_sdr unsigned-8-bit IQ capture: finds bursts
of activity above the noise floor and prints their timing.

Used as a pre-URH sanity check — confirms the file actually contains
FOB packet bursts before you invest time loading it in the GUI.

Usage:
    python sdr/scripts/inspect-capture.py sdr/captures/fob-start-001.bin
    python sdr/scripts/inspect-capture.py sdr/captures/fob-start-001.bin --sample-rate 250000
"""
import argparse
import os
import sys


def power_envelope(path, sample_rate, block_size):
    """
    Yield (time_s, avg_power) tuples by reading the file in chunks of
    `block_size` samples and computing mean envelope amplitude.

    rtl_sdr writes interleaved I, Q as unsigned 8-bit (offset 128 = 0).
    Power proxy: |I-128| + |Q-128|, averaged over the block.
    """
    with open(path, "rb") as f:
        block_idx = 0
        bytes_per_block = block_size * 2  # 2 bytes per IQ sample
        while True:
            buf = f.read(bytes_per_block)
            n = len(buf) // 2
            if n == 0:
                return
            total = 0
            for i in range(0, n * 2, 2):
                # |I-128| + |Q-128| is cheap and correlates with envelope
                total += abs(buf[i] - 128) + abs(buf[i + 1] - 128)
            avg = total / n
            yield block_idx * block_size / sample_rate, avg
            block_idx += 1


def find_bursts(envelope, threshold_db_over_floor=3.0, min_burst_s=0.01):
    """
    From a list of (time, power) samples, return [(start_s, end_s, peak)] for
    each contiguous run of power > floor + threshold.

    Uses a robust noise-floor estimate (median of lower 60%).
    """
    powers = sorted(p for _, p in envelope)
    lower = powers[: int(len(powers) * 0.6)]
    floor = lower[len(lower) // 2] if lower else 1.0
    # Convert dB-style threshold to linear ratio (3 dB ≈ 1.41×, 6 dB ≈ 2×)
    threshold = floor * (10 ** (threshold_db_over_floor / 20))

    bursts = []
    start = None
    peak = 0
    last_t = 0
    for t, p in envelope:
        last_t = t
        if p > threshold:
            if start is None:
                start = t
                peak = p
            else:
                peak = max(peak, p)
        else:
            if start is not None:
                if t - start >= min_burst_s:
                    bursts.append((start, t, peak / floor))
                start = None
                peak = 0
    if start is not None and last_t - start >= min_burst_s:
        bursts.append((start, last_t, peak / floor))
    return bursts, floor


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("path")
    ap.add_argument("--sample-rate", type=int, default=250_000)
    ap.add_argument("--block-ms", type=float, default=2.0,
                    help="envelope block size in ms (smaller = finer resolution)")
    ap.add_argument("--threshold-db", type=float, default=3.0,
                    help="how many dB above noise floor counts as 'burst'")
    args = ap.parse_args()

    if not os.path.exists(args.path):
        sys.exit(f"file not found: {args.path}")

    size = os.path.getsize(args.path)
    samples = size // 2
    duration = samples / args.sample_rate
    print(f"File:     {args.path}")
    print(f"Size:     {size:,} bytes  ({samples:,} I/Q samples)")
    print(f"Duration: {duration:.2f} s at {args.sample_rate} sps")
    print()

    block = max(1, int(args.sample_rate * args.block_ms / 1000))
    env = list(power_envelope(args.path, args.sample_rate, block))
    print(f"Envelope blocks: {len(env)}  ({args.block_ms} ms each)")

    bursts, floor = find_bursts(env, args.threshold_db)
    print(f"Noise-floor envelope amplitude: {floor:.2f}")
    print(f"Bursts found above +{args.threshold_db:.1f} dB: {len(bursts)}")
    print()
    if bursts:
        print(f"{'#':>3} {'start (s)':>10} {'end (s)':>10} {'len (ms)':>10} {'peak/floor':>10}")
        for i, (s, e, ratio) in enumerate(bursts, 1):
            print(f"{i:>3} {s:>10.3f} {e:>10.3f} {(e-s)*1000:>10.1f} {ratio:>10.2f}x")
    else:
        print("No bursts detected. Either:")
        print("  - FOB wasn't pressed during the recording")
        print("  - The recording is all noise (gain too low / FOB too far)")
        print("  - Threshold is too aggressive; try --threshold-db 1.5")


if __name__ == "__main__":
    main()
