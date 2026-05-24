"""
Compustar 1-way FOB protocol decoder.

Per the rtl_433 project's compustar_1wg3r device file (which decodes
1WG3R-SH, 1WAMR-1900, and apparently 1WSHR-PRO too), the protocol is:

  Modulation: OOK_PULSE_PWM
  short_width: 708 us       (= "0" bit's high duration)
  long_width:  1076 us      (= "1" bit's high duration)
  reset_limit: 1532 us      (inter-packet gap)
  sync_width:  1448 us      (sync pulse at packet start)

  Bit encoding: each bit is a (HIGH, LOW) pulse pair where
    "0" = HIGH(708) + LOW(1076)
    "1" = HIGH(1076) + LOW(708)
  Total bit period = 1784 us = 89.2 samples at 50 kSps envelope rate.

  Packet payload (36 bits, MSB first):
    bits  0..15:  16-bit Remote ID (constant per FOB)
    bits 16..18:   3 bits "unknown, always 111"
    bits 19..26:   8-bit inverted button code (= ~button)
    bits 27..34:   8-bit button code
    bit  35:       1 bit "unknown, always 0"

  Validity check: bits[19..26] XOR bits[27..34] should equal 0xFF.

This is FIXED CODE — there is NO rolling code, NO KeeLoq encryption,
NO device key, NO counter. Same press = identical packet. Different
button = different button code value.

This module supersedes the HCS300-PWM assumption that demod-ook.py
makes. demod-ook.py is kept around for HCS-family FOBs that use the
standard 1:2 PWM ratio (most non-Compustar HCS variants).

Usage:
    python sdr/scripts/demod-compustar.py sdr/captures/fob-start-004-b1.bin

    # Verbose:
    python sdr/scripts/demod-compustar.py sdr/captures/fob-start-004-b1.bin --verbose

    # Override timing if your FOB is slightly different:
    python sdr/scripts/demod-compustar.py path.bin --short-us 708 --long-us 1076
"""
import argparse
import os
import sys

# Reuse envelope + filtering primitives
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import importlib.util
_spec = importlib.util.spec_from_file_location("demod_ook", os.path.join(HERE, "demod-ook.py"))
demod_ook = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(demod_ook)


# Protocol constants (from rtl_433 compustar_1wg3r.c)
SHORT_US_DEFAULT = 708
LONG_US_DEFAULT = 1076
SYNC_US_DEFAULT = 1448
RESET_US_DEFAULT = 1532
N_BITS = 36


def decode_packet_from_high_pulses(runs, start_run_idx, short_samples,
                                    long_samples, n_bits=N_BITS):
    """
    rtl_433's OOK_PULSE_PWM decodes by looking at HIGH-pulse widths only:
      - HIGH pulse near short_width → bit "0"
      - HIGH pulse near long_width → bit "1"
    Gaps (LOW runs) are variable and ignored for bit decoding.

    Starting at run index `start_run_idx`, scan forward through runs;
    every HIGH run is one bit. Need n_bits HIGH pulses.
    """
    bits = []
    midpoint = (short_samples + long_samples) / 2
    i = start_run_idx
    while i < len(runs) and len(bits) < n_bits:
        val, length = runs[i]
        if val == 1:   # HIGH run = one bit
            bits.append(0 if length < midpoint else 1)
        i += 1
    if len(bits) < n_bits:
        return bits, f"only got {len(bits)} of {n_bits} HIGH pulses"
    return bits, None


def find_sync_then_data(runs, run_offsets, sync_samples, short_samples,
                       long_samples):
    """
    Find packet starts by looking for a sync-width HIGH pulse.

    A real sync must be STRICTLY wider than a long-bit pulse to avoid
    false positives: sync_min = long_samples * 1.15 (= ~62 at 50kHz),
    sync_max = sync_samples * 1.3 (covers measurement broadening).

    Returns list of (sync_run_idx, data_start_sample, post_sync_gap_samples).
    """
    sync_min = long_samples * 1.15
    sync_max = sync_samples * 1.35

    starts = []
    for i, (val, length) in enumerate(runs):
        if val != 1 or not (sync_min <= length <= sync_max):
            continue
        if i + 1 >= len(runs):
            continue
        low_val, low_len = runs[i + 1]
        if low_val != 0:
            continue
        # Skip the sync HIGH + its LOW partner. First data bit starts next.
        if i + 2 >= len(run_offsets):
            continue
        data_start_sample = run_offsets[i + 2]
        starts.append((i, data_start_sample, low_len))
    return starts


def estimate_pulse_widths(runs, expected_short, expected_long, expected_sync):
    """
    Empirically refine short / long / sync widths from observed runs.
    Bins runs by length proximity to each expected width and returns the
    median of each bin. Falls back to expected if a bin is empty.
    """
    short_bin, long_bin, sync_bin = [], [], []
    for val, length in runs:
        # Only consider HIGH runs for sync; both polarities for short/long
        if abs(length - expected_short) < expected_short * 0.3:
            short_bin.append(length)
        elif abs(length - expected_long) < expected_long * 0.2:
            long_bin.append(length)
        elif val == 1 and abs(length - expected_sync) < expected_sync * 0.3:
            sync_bin.append(length)

    def median(xs, default):
        if not xs:
            return default
        return sorted(xs)[len(xs) // 2]

    return (median(short_bin, expected_short),
            median(long_bin, expected_long),
            median(sync_bin, expected_sync))


def parse_packet(bits):
    """
    Parse a 36-bit Compustar packet into its fields.
    Returns dict with: remote_id, unk1, button_inverse, button, unk2, valid.
    """
    if len(bits) < N_BITS:
        return None

    def bits_to_int(bs, start, n, msb_first=True):
        v = 0
        for i in range(n):
            v = (v << 1) | bs[start + i]
        return v

    remote_id = bits_to_int(bits, 0, 16)
    unk1 = bits_to_int(bits, 16, 3)
    button_inv = bits_to_int(bits, 19, 8)
    button = bits_to_int(bits, 27, 8)
    unk2 = bits[35]

    valid = (
        unk1 == 0b111
        and unk2 == 0
        and (button_inv ^ button) == 0xFF
    )
    return {
        "remote_id": remote_id,
        "unk1": unk1,
        "button_inverse": button_inv,
        "button": button,
        "unk2": unk2,
        "valid": valid,
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path")
    ap.add_argument("--sample-rate", type=int, default=250_000)
    ap.add_argument("--short-us", type=int, default=SHORT_US_DEFAULT)
    ap.add_argument("--long-us", type=int, default=LONG_US_DEFAULT)
    ap.add_argument("--sync-us", type=int, default=SYNC_US_DEFAULT)
    ap.add_argument("--decimate-to", type=int, default=50_000)
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.path):
        sys.exit(f"file not found: {args.path}")

    env, eff_rate = demod_ook.read_magnitude_envelope(
        args.path, args.sample_rate, args.decimate_to
    )
    short_samples = args.short_us * eff_rate / 1_000_000
    long_samples = args.long_us * eff_rate / 1_000_000
    sync_samples = args.sync_us * eff_rate / 1_000_000

    if args.verbose:
        print(f"File: {args.path}")
        print(f"Envelope: {len(env)} samples at {eff_rate:.0f} Hz")
        print(f"short = {args.short_us} us = {short_samples:.1f} samples")
        print(f"long  = {args.long_us} us = {long_samples:.1f} samples")
        print(f"sync  = {args.sync_us} us = {sync_samples:.1f} samples")

    regions, _, _ = demod_ook.find_burst_regions(env, eff_rate)
    if not regions:
        sys.exit("No bursts found")

    base, _ = os.path.splitext(args.path)
    decoded_packets = []
    for r_idx, (start, end) in enumerate(regions):
        chunk = env[start:end]
        # Direct envelope thresholding at -g 20 works cleanly (no AC-coupling needed).
        # Threshold = midpoint of the bimodal in-burst distribution.
        sorted_chunk = sorted(chunk)
        n = len(sorted_chunk)
        lo_mode = sorted_chunk[n * 10 // 100]
        hi_mode = sorted_chunk[n * 90 // 100]
        thr = (lo_mode + hi_mode) / 2
        binary = [1 if v > thr else 0 for v in chunk]
        runs = demod_ook.run_lengths(binary)
        if args.verbose:
            print(f"\nBurst {r_idx+1} ({start/eff_rate:.3f}-{end/eff_rate:.3f}s):")
            print(f"  lo_mode={lo_mode:.0f}, hi_mode={hi_mode:.0f}, threshold={thr:.0f}")

        run_offsets = [0]
        for _, length in runs:
            run_offsets.append(run_offsets[-1] + length)

        # Refine pulse widths from observed data
        m_short, m_long, m_sync = estimate_pulse_widths(
            runs, short_samples, long_samples, sync_samples
        )
        if args.verbose:
            print(f"  measured: short={m_short:.1f} ({m_short*1e6/eff_rate:.0f}us), "
                  f"long={m_long:.1f} ({m_long*1e6/eff_rate:.0f}us), "
                  f"sync={m_sync:.1f} ({m_sync*1e6/eff_rate:.0f}us)")

        sync_points = find_sync_then_data(runs, run_offsets, m_sync,
                                           m_short, m_long)
        if args.verbose:
            print(f"  runs={len(runs)}, sync points={len(sync_points)}")

        for sync_run_idx, _, _ in sync_points:
            # Data starts right after the sync HIGH + LOW pair.
            # sync_run_idx is the HIGH sync; +1 is the LOW gap; +2 is bit 1's HIGH.
            bits, err = decode_packet_from_high_pulses(
                runs, sync_run_idx + 2, m_short, m_long, n_bits=N_BITS,
            )
            if err:
                if args.verbose:
                    print(f"  sync at run {sync_run_idx}: {err}")
                continue
            parsed = parse_packet(bits)
            if parsed is None:
                continue
            decoded_packets.append(parsed)
            if args.verbose:
                v = "VALID" if parsed["valid"] else "invalid"
                print(f"  sync at run {sync_run_idx}: bits = {''.join(str(b) for b in bits)}")
                print(f"    parsed: ID=0x{parsed['remote_id']:04X} "
                      f"button=0x{parsed['button']:02X} "
                      f"~button=0x{parsed['button_inverse']:02X} "
                      f"unk1={parsed['unk1']:03b} unk2={parsed['unk2']} [{v}]")

    print()
    print(f"File: {args.path}")
    print(f"Burst regions: {len(regions)}, packets decoded: {len(decoded_packets)}")
    valid_packets = [p for p in decoded_packets if p["valid"]]
    print(f"Valid packets (integrity check passed): {len(valid_packets)}")
    if valid_packets:
        # Most-common values
        from collections import Counter
        ids = Counter(p["remote_id"] for p in valid_packets)
        buttons = Counter(p["button"] for p in valid_packets)
        print(f"Remote IDs seen: {[(f'0x{i:04X}', c) for i, c in ids.most_common(5)]}")
        print(f"Button codes seen: {[(f'0x{b:02X}', c) for b, c in buttons.most_common(5)]}")
    elif decoded_packets:
        print("WARN no valid packets — possible decoder issue. First few raw:")
        for p in decoded_packets[:3]:
            print(f"  ID=0x{p['remote_id']:04X} button=0x{p['button']:02X} "
                  f"~button=0x{p['button_inverse']:02X} unk1={p['unk1']:03b} unk2={p['unk2']}")


if __name__ == "__main__":
    main()
