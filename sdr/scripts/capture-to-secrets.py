"""
One-shot helper: read an rtl_sdr unsigned-8-bit IQ capture (.bin), run it
through the existing demod-compustar.py pipeline, and print the EXACT
line(s) you should paste into ``esp32/src/secrets.py``.

This wraps demod-compustar.py's decoder so you don't have to manually
copy a 35-bit string out of verbose output and remember which button it
belongs to. Run it once per button capture and you'll have the entire
``COMPUSTAR_PACKETS`` dict + ``COMPUSTAR_REMOTE_ID`` ready to paste.

Usage:

    python sdr/scripts/capture-to-secrets.py --button START sdr/captures/fob-start-001.bin
    python sdr/scripts/capture-to-secrets.py --button LOCK   sdr/captures/fob-lock-001.bin
    python sdr/scripts/capture-to-secrets.py --button UNLOCK sdr/captures/fob-unlock-001.bin
    python sdr/scripts/capture-to-secrets.py --button TRUNK  sdr/captures/fob-trunk-001.bin

Output (one line per invocation), for example:

    "START":  "01101001011010010110100101101001011",  # from fob-start-001.bin, Remote ID 0xABCD
    # COMPUSTAR_REMOTE_ID = 0xABCD

Exit codes:
    0  success
    1  decode failure (no valid packets, or capture file missing)
    2  inconsistent packets within the capture (signals demod issue
       or mixed-button recording)
"""
import argparse
import importlib.util
import os
import sys
from collections import Counter


HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


def _load_demod():
    """Load demod-compustar.py as a module (it has a hyphen in the name)."""
    spec = importlib.util.spec_from_file_location(
        "demod_compustar", os.path.join(HERE, "demod-compustar.py")
    )
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


BUTTONS = ("START", "LOCK", "UNLOCK", "TRUNK")
PATTERN_LEN = 35  # firmware stores 35-bit patterns per button


def decode_capture(path, sample_rate, decimate_to=50_000):
    """
    Run the demod pipeline on ``path`` and return a list of decoded
    35-bit pattern strings (one per successfully-decoded burst).

    Re-implements the burst loop from demod-compustar.main() so we can
    grab the raw bit list rather than the parsed dict (parse_packet
    requires 36 bits; the firmware's pattern is the first 35).
    """
    dc = _load_demod()
    demod_ook = dc.demod_ook

    env, eff_rate = demod_ook.read_magnitude_envelope(
        path, sample_rate, decimate_to
    )
    short_samples = dc.SHORT_US_DEFAULT * eff_rate / 1_000_000
    long_samples = dc.LONG_US_DEFAULT * eff_rate / 1_000_000
    sync_samples = dc.SYNC_US_DEFAULT * eff_rate / 1_000_000

    regions, _, _ = demod_ook.find_burst_regions(env, eff_rate)
    if not regions:
        return []

    patterns = []
    for start, end in regions:
        chunk = env[start:end]
        sorted_chunk = sorted(chunk)
        n = len(sorted_chunk)
        if n < 10:
            continue
        lo_mode = sorted_chunk[n * 10 // 100]
        hi_mode = sorted_chunk[n * 90 // 100]
        thr = (lo_mode + hi_mode) / 2
        binary = [1 if v > thr else 0 for v in chunk]
        runs = demod_ook.run_lengths(binary)

        run_offsets = [0]
        for _, length in runs:
            run_offsets.append(run_offsets[-1] + length)

        m_short, m_long, m_sync = dc.estimate_pulse_widths(
            runs, short_samples, long_samples, sync_samples
        )
        sync_points = dc.find_sync_then_data(
            runs, run_offsets, m_sync, m_short, m_long
        )

        # The 1WSHR-PRO transmits THREE sync pulses per burst followed by
        # 35 data bits. find_sync_then_data flags all three sync runs as
        # candidate starts; only the LAST (= the one immediately followed
        # by data-width HIGH pulses, not another sync-width HIGH) gives
        # the correct pattern. Filter accordingly.
        sync_min = m_long * 1.15
        data_sync_points = []
        for sync_run_idx, ds, dg in sync_points:
            # Look at the next HIGH run after sync_run_idx + 1 (the
            # post-sync LOW gap). If it's sync-width, this isn't the
            # final sync — skip.
            j = sync_run_idx + 2
            while j < len(runs) and runs[j][0] != 1:
                j += 1
            if j >= len(runs):
                continue
            next_high_len = runs[j][1]
            if next_high_len >= sync_min:
                # Next HIGH is itself a sync — we're not on the final sync yet.
                continue
            data_sync_points.append((sync_run_idx, ds, dg))

        for sync_run_idx, _, _ in data_sync_points:
            bits, err = dc.decode_packet_from_high_pulses(
                runs, sync_run_idx + 2, m_short, m_long,
                n_bits=PATTERN_LEN,
            )
            if err:
                continue
            if len(bits) < PATTERN_LEN:
                continue
            patterns.append("".join(str(b) for b in bits[:PATTERN_LEN]))
    return patterns


def remote_id_from_pattern(pattern):
    """First 16 bits MSB-first are the Compustar Remote ID."""
    v = 0
    for ch in pattern[:16]:
        v = (v << 1) | (1 if ch == "1" else 0)
    return v


def consensus(patterns):
    """
    Return (pattern, dissenting_patterns) where ``pattern`` is the
    most-common decoded pattern across all bursts. If a non-majority
    pattern was also seen we surface it so the caller can warn.
    """
    counts = Counter(patterns)
    most = counts.most_common()
    winner, _ = most[0]
    dissent = [p for p in counts if p != winner]
    return winner, dissent


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("path", help="rtl_sdr unsigned-8-bit IQ capture (.bin)")
    ap.add_argument(
        "--button",
        required=True,
        type=str.upper,
        choices=BUTTONS,
        help="Which Compustar button this capture is for",
    )
    ap.add_argument("--sample-rate", type=int, default=250_000)
    ap.add_argument("--decimate-to", type=int, default=50_000)
    args = ap.parse_args()

    if not os.path.exists(args.path):
        print(f"ERROR file not found: {args.path}", file=sys.stderr)
        return 1

    patterns = decode_capture(args.path, args.sample_rate, args.decimate_to)
    if not patterns:
        print(
            f"ERROR no packets decoded from {args.path} — "
            "check capture quality / sample rate",
            file=sys.stderr,
        )
        return 1

    winner, dissent = consensus(patterns)
    basename = os.path.basename(args.path)
    rid = remote_id_from_pattern(winner)

    if dissent:
        print(
            f"WARN {len(patterns)} bursts decoded but {len(dissent)} "
            "produced different patterns:",
            file=sys.stderr,
        )
        for p in dissent:
            print(f"      dissenting: {p}", file=sys.stderr)
        print(
            "      proceeding with the majority pattern — verify this "
            "isn't a mixed-button capture or a demod alignment issue",
            file=sys.stderr,
        )

    # The line the user should paste into COMPUSTAR_PACKETS:
    print(f'    "{args.button}":  "{winner}",  # from {basename}, Remote ID 0x{rid:04X}')
    # Cosmetic Remote ID line; safe to print every time, the user only
    # needs to keep one copy.
    print(f"# COMPUSTAR_REMOTE_ID = 0x{rid:04X}")

    if dissent:
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
