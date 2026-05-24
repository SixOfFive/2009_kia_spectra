"""
Headless OOK demodulator for HCS-family (Compustar) rolling-code packets.

Reads an unsigned-8-bit IQ capture (rtl_sdr native format), recovers the
OOK envelope (with AC-coupling + low-pass filter to handle AGC-compressed
captures where the modulation depth is < 1%), detects packet structure
(preamble + header gap + 66-bit PWM-encoded data) and writes the
demodulated bits to a `.bits` file matching the format used by
diff-bits.py.

This replaces URH for the demodulation step. Use it when:
  - URH won't open / freezes on Windows
  - The capture has AGC-compressed contrast (cheap RTL-SDRs at high gain)
  - You want batch processing across many capture files
  - You want reproducibility — URH's center-line and bit-length settings
    are GUI-tuned and hard to script

PWM bit encoding (HCS300/301):
    binary 0 = short high (TE) + long low (2*TE)
    binary 1 = long high (2*TE) + short low (TE)
Each bit therefore takes 3*TE wall-clock time. Default TE is 400us.

Pipeline:
    raw IQ -> magnitude envelope -> LP filter -> AC-couple -> threshold
    -> run-length detect -> find preamble -> find header gap -> decode bits

Usage:
    python sdr/scripts/demod-ook.py sdr/captures/fob-start-001-b1.bin
    python sdr/scripts/demod-ook.py sdr/captures/fob-start-001-b1.bin \\
        --sample-rate 250000 --te-us 400 --verbose

Output: `<base>.bits` file plus a one-line per-packet summary to stdout.
"""
import argparse
import math
import os
import sys


# ---------- Reading + envelope extraction ----------

def read_magnitude_envelope(path, sample_rate, decimate_to_hz=50_000):
    """
    Read interleaved unsigned-8-bit IQ. Return (envelope, eff_sample_rate)
    where envelope is a list of magnitude samples at `decimate_to_hz` Hz.

    Magnitude = sqrt((I-128)^2 + (Q-128)^2). Block-averaged during
    decimation to suppress high-frequency artifacts (IQ-imbalance beats).
    """
    decim = max(1, sample_rate // decimate_to_hz)
    eff_rate = sample_rate / decim

    env = []
    with open(path, "rb") as f:
        accum_sq = 0.0
        count = 0
        target_count = decim
        while True:
            chunk = f.read(8192 * 2)
            if not chunk:
                break
            for i in range(0, len(chunk), 2):
                di = chunk[i] - 128
                dq = chunk[i + 1] - 128
                accum_sq += di * di + dq * dq
                count += 1
                if count >= target_count:
                    env.append(math.sqrt(accum_sq / count))
                    accum_sq = 0.0
                    count = 0
        if count:
            env.append(math.sqrt(accum_sq / count))
    return env, eff_rate


# ---------- Filtering ----------

def low_pass(env, window):
    """Boxcar moving-average low-pass. window in samples."""
    if window <= 1:
        return list(env)
    out = []
    acc = sum(env[:window])
    out.append(acc / window)
    for i in range(window, len(env)):
        acc += env[i] - env[i - window]
        out.append(acc / window)
    # Pad to keep length consistent (replicate first value)
    return [out[0]] * (window - 1) + out


def ac_couple(env, baseline_window):
    """
    Subtract a longer moving-average baseline from each sample. The
    baseline tracks slow envelope drift (AGC settling, fade); subtracting
    it isolates the faster OOK modulation.
    """
    baseline = low_pass(env, baseline_window)
    return [e - b for e, b in zip(env, baseline)]


# ---------- Burst detection ----------

def find_burst_regions(envelope, eff_rate, min_burst_ms=50, min_db_above=6):
    """
    Find time windows where the raw envelope is well above noise floor.
    Returns list of (start_idx, end_idx) into envelope.

    Uses 5th percentile for noise floor (rather than 30th) so trimmed
    single-burst files — where 90%+ of samples ARE the burst — still
    detect the silence correctly.
    """
    sorted_env = sorted(envelope)
    n = len(sorted_env)
    noise_floor = sorted_env[max(1, n // 20)]   # 5th percentile
    # If signal floor and noise floor are too close (very compressed
    # capture), use the bimodal valley between low and high modes.
    high_value = sorted_env[n * 95 // 100]
    if high_value < noise_floor * 2:
        # Compressed signal — use midpoint as threshold; bursts are
        # whatever's slightly above the median.
        noise_floor = sorted_env[n // 2]
        threshold = noise_floor * 1.02
    else:
        threshold = noise_floor * (10 ** (min_db_above / 20))

    min_burst_samples = int(eff_rate * min_burst_ms / 1000)
    regions = []
    in_burst = False
    start = 0
    above_count = 0
    below_count = 0
    for i, v in enumerate(envelope):
        if v > threshold:
            if not in_burst:
                start = i
                in_burst = True
            above_count += 1
            below_count = 0
        else:
            if in_burst:
                below_count += 1
                # Allow brief dips (up to 5ms) before declaring burst end
                if below_count > int(eff_rate * 0.005):
                    if i - start >= min_burst_samples:
                        regions.append((start, i - below_count))
                    in_burst = False
                    above_count = 0
                    below_count = 0
    if in_burst and len(envelope) - start >= min_burst_samples:
        regions.append((start, len(envelope)))
    return regions, noise_floor, threshold


# ---------- Run-length analysis ----------

def to_binary(signal, threshold):
    return [1 if s > threshold else 0 for s in signal]


def schmitt_trigger(signal, hi_threshold, lo_threshold):
    """
    Hysteresis thresholder: goes HIGH when signal > hi_threshold, goes LOW
    when signal < lo_threshold, stays put in between. Eliminates spurious
    transitions from noise hovering near a single threshold.
    """
    out = []
    state = 0
    for s in signal:
        if state == 0:
            if s > hi_threshold:
                state = 1
        else:
            if s < lo_threshold:
                state = 0
        out.append(state)
    return out


def run_lengths(binary):
    if not binary:
        return []
    runs = []
    cur = binary[0]
    cnt = 1
    for b in binary[1:]:
        if b == cur:
            cnt += 1
        else:
            runs.append((cur, cnt))
            cur = b
            cnt = 1
    runs.append((cur, cnt))
    return runs


# ---------- Preamble + packet structure ----------

def find_packets_in_runs(runs, te_samples, n_bits=66, verbose=False):
    """
    Scan run list for packet starts. A packet starts with a preamble
    (>=8 consecutive short runs ~TE long), then either:
      - A long LOW (>= 3.5*TE) acting as header gap (standard HCS), OR
      - Direct transition into data (some Compustar variants omit the gap
        or use a sync-burst of 2*TE pulses instead)

    Returns list of (preamble_start, data_start, header_gap_len_samples).
    header_gap_len_samples is 0 if no gap was present.
    """
    short_lo = te_samples * 0.5
    short_hi = te_samples * 1.6
    long_gap_min = te_samples * 3.5

    packets = []
    i = 0
    while i < len(runs):
        preamble_runs = 0
        j = i
        while j < len(runs):
            _, length = runs[j]
            if short_lo <= length <= short_hi:
                preamble_runs += 1
                j += 1
            else:
                break
        if preamble_runs >= 8 and j < len(runs):
            val, length = runs[j]
            gap_len = 0
            data_start = j
            if val == 0 and length >= long_gap_min:
                gap_len = length
                data_start = j + 1
                if verbose:
                    print(f"    candidate packet: preamble runs {i}..{j-1} "
                          f"({preamble_runs} short alternations), header gap "
                          f"{length} samples, data starts at run {data_start}")
            else:
                if verbose:
                    print(f"    candidate packet: preamble runs {i}..{j-1} "
                          f"({preamble_runs} short alternations), NO header gap "
                          f"(next run is val={val} len={length}), "
                          f"data starts at run {data_start}")
            packets.append((i, data_start, gap_len))
            # Skip past the data we'll consume so we don't re-detect inside it
            i = min(data_start + n_bits * 2, len(runs))
            continue
        i += 1
    return packets


def decode_bits_from_runs(runs, start_idx, n_bits=66):
    """
    Starting at run index `start_idx`, decode `n_bits` PWM bits.
    Each bit is a (high, low) pair: short+long = 0, long+short = 1.
    """
    bits = []
    i = start_idx
    while i + 1 < len(runs) and len(bits) < n_bits:
        hi_val, hi_len = runs[i]
        lo_val, lo_len = runs[i + 1]
        if hi_val != 1 or lo_val != 0:
            return bits, f"pattern break at run {i} (val {hi_val},{lo_val})"
        if hi_len < lo_len:
            bits.append(0)
        else:
            bits.append(1)
        i += 2
    if len(bits) < n_bits:
        return bits, f"short: got {len(bits)} of {n_bits}"
    return bits, None


# ---------- TE estimation ----------

def estimate_te(runs, expected_te):
    """
    Refine TE estimate from observed run lengths. Looks at the shortest
    mode (those are the TE-long pulses) and returns its median.
    """
    lengths = sorted(r for _, r in runs if r > 0)
    if not lengths:
        return expected_te
    # Take the shortest 25% — these should mostly be TE-long pulses
    short = lengths[: max(20, len(lengths) // 4)]
    measured = sorted(short)[len(short) // 2]
    ratio = measured / expected_te
    if 0.5 < ratio < 2.0:
        return measured
    return expected_te


# ---------- Top-level pipeline ----------

def demodulate(path, sample_rate=250_000, te_us=400, decimate_to=50_000,
               lp_window_us=200, baseline_window_us=2000,
               n_bits=66, verbose=False):
    """
    Full demodulation pipeline. Returns list of (bits, info_dict) packets.
    """
    info = {}

    env, eff_rate = read_magnitude_envelope(path, sample_rate, decimate_to)
    info["eff_rate"] = eff_rate
    info["env_samples"] = len(env)
    info["duration_s"] = len(env) / eff_rate
    if verbose:
        print(f"Envelope: {len(env)} samples at {eff_rate:.0f} Hz "
              f"({info['duration_s']:.2f} s total)")

    # Per-TE samples in envelope domain
    te_samples = eff_rate * te_us / 1_000_000
    info["te_envelope_samples"] = te_samples
    if verbose:
        print(f"TE = {te_us}us = {te_samples:.1f} envelope samples")

    # Find bursts (gross level)
    regions, noise_floor, burst_thresh = find_burst_regions(env, eff_rate)
    info["noise_floor"] = noise_floor
    info["burst_threshold"] = burst_thresh
    info["bursts"] = regions
    if verbose:
        print(f"Noise floor: {noise_floor:.2f}, burst threshold: {burst_thresh:.2f}")
        print(f"Bursts detected: {len(regions)}")
        for s, e in regions:
            print(f"  region {s/eff_rate:.3f}-{e/eff_rate:.3f}s ({(e-s)*1000/eff_rate:.0f}ms)")

    if not regions:
        return [], info

    # Process each burst separately — adaptive threshold per burst
    packets = []
    lp_w = max(1, int(eff_rate * lp_window_us / 1_000_000))
    baseline_w = max(2, int(eff_rate * baseline_window_us / 1_000_000))
    if verbose:
        print(f"LP window: {lp_w} samples, baseline window: {baseline_w} samples")

    for r_idx, (start, end) in enumerate(regions):
        if verbose:
            print(f"\nBurst {r_idx + 1} ({start/eff_rate:.3f}-{end/eff_rate:.3f}s):")
        chunk = env[start:end]

        # Low-pass then AC-couple
        smoothed = low_pass(chunk, lp_w)
        ac = ac_couple(smoothed, baseline_w)

        # Adaptive Schmitt trigger: hi/lo thresholds at ±20% of the
        # 80th-percentile-absolute peak. Hysteresis eliminates noise
        # spikes near zero crossing.
        abs_ac = sorted(abs(v) for v in ac if v != 0)
        if not abs_ac:
            if verbose:
                print("  (no AC content — skipping)")
            continue
        peak80 = abs_ac[len(abs_ac) * 8 // 10]
        hi_thresh = peak80 * 0.25
        lo_thresh = -peak80 * 0.25
        if verbose:
            ac_min, ac_max = min(ac), max(ac)
            print(f"  AC range: {ac_min:.2f} to {ac_max:.2f}, "
                  f"peak80={peak80:.2f}, schmitt hi/lo: ±{peak80*0.25:.2f}")

        binary = schmitt_trigger(ac, hi_thresh, lo_thresh)
        runs = run_lengths(binary)
        if verbose:
            print(f"  Runs: {len(runs)}")
        if len(runs) < 30:
            if verbose:
                print("  (too few runs — burst probably not a clean packet)")
            continue

        measured_te = estimate_te(runs, te_samples)
        if verbose:
            measured_us = measured_te * 1_000_000 / eff_rate
            print(f"  Measured TE: {measured_te:.1f} envelope samples "
                  f"({measured_us:.0f} us)")

        candidates = find_packets_in_runs(runs, measured_te, n_bits=n_bits,
                                          verbose=verbose)
        if verbose:
            print(f"  Packet candidates: {len(candidates)}")

        for pre_idx, data_idx, gap_len in candidates:
            bits, err = decode_bits_from_runs(runs, data_idx, n_bits=n_bits)
            if err and len(bits) < n_bits:
                if verbose:
                    print(f"    decode error: {err}")
                continue
            packets.append((bits, {
                "burst_index": r_idx + 1,
                "header_gap_samples": gap_len,
                "header_gap_te_units": gap_len / measured_te,
                "measured_te_us": measured_te * 1_000_000 / eff_rate,
            }))

    return packets, info


# ---------- CLI ----------

def write_bits_file(out_path, bits, header_lines):
    with open(out_path, "w") as f:
        for line in header_lines:
            f.write(f"# {line}\n")
        f.write("".join(str(b) for b in bits) + "\n")


def main():
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("path", help="rtl_sdr unsigned-8-bit IQ capture (.bin)")
    ap.add_argument("--sample-rate", type=int, default=250_000)
    ap.add_argument("--te-us", type=int, default=400,
                    help="HCS bit element time in microseconds (default 400)")
    ap.add_argument("--decimate-to", type=int, default=50_000,
                    help="downsample envelope to this rate (default 50 kHz, "
                         "= 8us resolution, plenty for TE=400us)")
    ap.add_argument("--lp-us", type=int, default=200,
                    help="low-pass window in us (smaller = sharper edges, "
                         "but more noise; default 200)")
    ap.add_argument("--baseline-us", type=int, default=2000,
                    help="baseline-tracker window in us (longer = better AC "
                         "coupling, but might miss long bits; default 2000)")
    ap.add_argument("--n-bits", type=int, default=66,
                    help="packet length in bits (HCS66 default 66)")
    ap.add_argument("--max-packets", type=int, default=10,
                    help="stop after decoding this many packets (default 10). "
                         "Files contain multiple repeats; emit each as .pN.bits "
                         "so step 06 can majority-vote across them.")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    if not os.path.exists(args.path):
        sys.exit(f"file not found: {args.path}")

    packets, info = demodulate(
        args.path,
        sample_rate=args.sample_rate,
        te_us=args.te_us,
        decimate_to=args.decimate_to,
        lp_window_us=args.lp_us,
        baseline_window_us=args.baseline_us,
        n_bits=args.n_bits,
        verbose=args.verbose,
    )

    print()
    print(f"File: {args.path}")
    print(f"Bursts: {len(info.get('bursts', []))}, "
          f"packets decoded: {len(packets)}")
    if not packets:
        print()
        print("No packets cleanly decoded. Possible causes:")
        print("  1. Capture has AGC-compressed contrast (try recording with")
        print("     -g 20 instead of -g 40, OR --baseline-us 5000)")
        print("  2. TE mismatch (try --te-us 200 or --te-us 800)")
        print("  3. Bad capture (re-run inspect-capture.py to check)")
        print("  Use --verbose for diagnostic output.")
        sys.exit(2)

    base, _ = os.path.splitext(args.path)
    written = 0
    for i, (bits, meta) in enumerate(packets[: args.max_packets]):
        suffix = "" if args.max_packets == 1 else f".p{i + 1}"
        out_path = f"{base}{suffix}.bits"
        write_bits_file(out_path, bits, [
            f"Source: {args.path}",
            f"Sample rate: {args.sample_rate} Hz",
            f"TE: {args.te_us} us nominal, {meta['measured_te_us']:.0f} us measured",
            f"Header gap: {meta['header_gap_samples']} envelope samples "
            f"(~{meta['header_gap_te_units']:.1f} TE)",
            f"From burst #{meta['burst_index']}",
            f"Bits: {len(bits)} demodulated, MSB-first transmit order",
            "Layout (HCS66): 32-bit hopping | 28-bit serial | 4-bit function | 1 V_LOW | 1 repeat",
        ])
        bit_str = "".join(str(b) for b in bits)
        print(f"  -> {out_path}: {bit_str}")
        written += 1


if __name__ == "__main__":
    main()
