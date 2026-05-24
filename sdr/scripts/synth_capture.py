"""
Generate a synthetic rtl_sdr cu8 capture file with a Compustar 1WG3R
fixed-code burst.

Used by ``sdr/tests/`` to round-trip the demodulator + the bench-day
tooling (``capture-to-secrets.py``, ``diff-captures.py``) without
needing real FOB captures on disk. Also handy standalone for sanity-
checking demodulator changes.

Wire format
-----------

The output is binary-compatible with ``rtl_sdr --sample-format=u8``:
interleaved unsigned-8-bit I and Q samples at the chosen sample rate,
DC-centered at 128. Magnitude after DC removal is what
``read_magnitude_envelope`` extracts in ``demod-ook.py``.

OFF state: I and Q jitter around 128 by NOISE_OFF (≈0 magnitude after
DC removal — looks like noise floor to the demodulator).

ON state: I is offset by ``on_amp`` (default 90) above 128 with the same
small jitter, Q stays around 128. Magnitude during ON is ~ on_amp, well
above the OFF floor.

The synthesized burst structure matches what we observed from the
genuine 1WSHR-PRO FOB via ``rtl_433 -A``: per packet,
``sync_count`` (default 3) sync pulses followed by the 35 data bits;
``repeats`` (default 8) packets per burst; pre/post silence padding so
``find_burst_regions`` sees a clean transition.

CLI
---

    python sdr/scripts/synth_capture.py out.bin --bits 010110...

Library
-------

    from synth_capture import synth_burst
    data = synth_burst("01101001011010010110100101101001011", repeats=8)
    open("out.bin", "wb").write(data)
"""

import argparse
import random
import sys


# Default pulse widths (microseconds) matching the rtl_433 -A measurements
# on a known-good 1WSHR-PRO FOB. See sdr/analysis/framing.md.
SHORT_HIGH_US = 732
SHORT_LOW_US = 1136
LONG_HIGH_US = 1100
LONG_LOW_US = 756
SYNC_HIGH_US = 1476
SYNC_LOW_US = 1500

# Synthesis amplitudes. on_amp is how far above DC the I channel is
# driven during HIGH; tuned so SNR is comfortably above the demodulator's
# 6 dB burst-detection threshold but not saturated.
ON_AMP_DEFAULT = 90
NOISE_OFF_DEFAULT = 3
NOISE_ON_DEFAULT = 5


def _emit(buf, sps, us, on, on_amp, noise_off, noise_on, rng):
    """Append ``us`` microseconds of either ON or OFF samples to ``buf``."""
    n_samples = int(round(us * sps))
    for _ in range(n_samples):
        if on:
            jitter = noise_on
            i = 128 + on_amp + rng.randint(-jitter, jitter)
            q = 128 + rng.randint(-jitter, jitter)
        else:
            jitter = noise_off
            i = 128 + rng.randint(-jitter, jitter)
            q = 128 + rng.randint(-jitter, jitter)
        if i < 0:
            i = 0
        elif i > 255:
            i = 255
        if q < 0:
            q = 0
        elif q > 255:
            q = 255
        buf.append(i)
        buf.append(q)


def synth_burst(bit_pattern,
                sample_rate=250_000,
                repeats=8,
                sync_count=3,
                short_high_us=SHORT_HIGH_US, short_low_us=SHORT_LOW_US,
                long_high_us=LONG_HIGH_US, long_low_us=LONG_LOW_US,
                sync_high_us=SYNC_HIGH_US, sync_low_us=SYNC_LOW_US,
                pre_silence_us=50_000, post_silence_us=100_000,
                on_amp=ON_AMP_DEFAULT,
                noise_off=NOISE_OFF_DEFAULT,
                noise_on=NOISE_ON_DEFAULT,
                seed=42):
    """
    Render a single Compustar 1WG3R-family burst as cu8 IQ bytes.

    :param bit_pattern: string of "0" and "1" characters (any length —
        defaults assume 35 but the renderer doesn't enforce that, so tests
        can synthesize odd lengths for edge cases).
    :param sample_rate: target rtl_sdr sample rate (Hz).
    :param repeats: how many packet repeats to include in the burst.
    :param sync_count: sync pulses per packet (default 3 for 1WSHR-PRO).
    :return: ``bytes`` ready to write to a .bin file.
    """
    rng = random.Random(seed)
    sps = sample_rate / 1_000_000  # samples per microsecond
    out = bytearray()

    _emit(out, sps, pre_silence_us, False, on_amp, noise_off, noise_on, rng)

    for _ in range(repeats):
        for _ in range(sync_count):
            _emit(out, sps, sync_high_us, True, on_amp, noise_off, noise_on, rng)
            _emit(out, sps, sync_low_us, False, on_amp, noise_off, noise_on, rng)
        for b in bit_pattern:
            if b == "1":
                _emit(out, sps, long_high_us, True, on_amp, noise_off, noise_on, rng)
                _emit(out, sps, long_low_us, False, on_amp, noise_off, noise_on, rng)
            elif b == "0":
                _emit(out, sps, short_high_us, True, on_amp, noise_off, noise_on, rng)
                _emit(out, sps, short_low_us, False, on_amp, noise_off, noise_on, rng)
            else:
                raise ValueError(
                    "bit pattern must be '0' or '1', got %r" % (b,)
                )

    _emit(out, sps, post_silence_us, False, on_amp, noise_off, noise_on, rng)
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(
        description="Synthesize a Compustar 1WG3R cu8 capture for testing.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("output_path")
    ap.add_argument("--bits", required=True,
                    help="bit pattern (35 chars of 0/1) to encode")
    ap.add_argument("--sample-rate", type=int, default=250_000)
    ap.add_argument("--repeats", type=int, default=8)
    ap.add_argument("--on-amp", type=int, default=ON_AMP_DEFAULT)
    ap.add_argument("--seed", type=int, default=42)
    args = ap.parse_args()

    if not all(c in "01" for c in args.bits):
        sys.exit("--bits must be a string of 0s and 1s")

    data = synth_burst(
        args.bits,
        sample_rate=args.sample_rate,
        repeats=args.repeats,
        on_amp=args.on_amp,
        seed=args.seed,
    )

    with open(args.output_path, "wb") as f:
        f.write(data)
    bytes_per_s = args.sample_rate * 2
    print(f"wrote {len(data)} bytes "
          f"({len(data) / bytes_per_s * 1000:.0f} ms) to {args.output_path}")


if __name__ == "__main__":
    main()
