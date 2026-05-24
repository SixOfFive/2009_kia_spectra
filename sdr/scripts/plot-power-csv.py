"""
Analyze an rtl_power CSV: report frequency peaks above the noise floor.

rtl_power CSV row format:
    date, time, hz_low, hz_high, hz_step, samples, dB_bin0, dB_bin1, ...

Each row is one sweep slice. The same frequency may appear in multiple rows
when the requested range exceeds the per-sweep window.

Usage:
    python plot-power-csv.py sdr/captures/fob-frequency-sweep.csv
    python plot-power-csv.py sdr/captures/band-baseline.csv --mode mean --top 5
"""
import argparse
import csv
import sys
from collections import defaultdict


def parse_rows(path):
    bins = defaultdict(list)
    with open(path) as f:
        for row in csv.reader(f):
            if not row or row[0].startswith("#"):
                continue
            try:
                hz_low = float(row[2])
                hz_step = float(row[4])
                powers = [float(p) for p in row[6:]]
            except (IndexError, ValueError):
                continue
            for i, p in enumerate(powers):
                freq = hz_low + i * hz_step
                bins[round(freq)].append(p)
    return bins


def main():
    ap = argparse.ArgumentParser(description="Find peaks in an rtl_power CSV.")
    ap.add_argument("csv", help="rtl_power CSV file")
    ap.add_argument("--top", type=int, default=10, help="show top N peaks")
    ap.add_argument(
        "--mode",
        choices=["mean", "max"],
        default="max",
        help="aggregate per bin (max finds transient FOB bursts; mean shows steady carriers)",
    )
    args = ap.parse_args()

    bins = parse_rows(args.csv)
    if not bins:
        sys.exit("No usable rows in CSV.")

    agg = {}
    for freq, samples in bins.items():
        agg[freq] = max(samples) if args.mode == "max" else sum(samples) / len(samples)

    sorted_powers = sorted(agg.values())
    floor = sorted_powers[len(sorted_powers) // 2]
    peaks = sorted(agg.items(), key=lambda kv: kv[1], reverse=True)[: args.top]

    print(f"Bins parsed: {len(agg)}")
    print(f"Noise floor (median): {floor:6.1f} dB")
    print(f"Top {args.top} peaks ({args.mode} per bin):")
    print(f"{'Frequency (MHz)':<18} {'Power (dB)':<12} {'Above floor':<12}")
    for freq, power in peaks:
        print(f"{freq/1e6:<18.4f} {power:<12.1f} {power - floor:<+12.1f}")


if __name__ == "__main__":
    main()
