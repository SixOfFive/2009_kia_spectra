"""
Sanity tests for synth_capture.py — the synthetic cu8 IQ-file generator
used by the rest of sdr/tests/.

These tests verify the synth itself (byte structure, file size, ON/OFF
amplitude separation, error handling) without involving the demodulator.
The other test files in this directory exercise the full
synth -> demod -> assert round trip.
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.normpath(os.path.join(HERE, "..", "scripts"))
sys.path.insert(0, SCRIPTS)

from synth_capture import synth_burst  # noqa: E402


# ----- File structure -----

def test_output_is_bytes():
    data = synth_burst("0" * 35, repeats=1)
    assert isinstance(data, bytes)
    # Each sample is 2 bytes (I, Q) — total must be even
    assert len(data) % 2 == 0


def test_file_size_matches_expected_duration():
    """Length should match (pre + repeats * packet + post) microseconds at
    the chosen sample rate."""
    bits = "0" * 35
    sample_rate = 250_000
    pre_us = 50_000
    post_us = 100_000
    # 1 repeat, 3 sync pulses + 35 zero bits
    # sync pair = 1476+1500 = 2976 us, x3 = 8928 us
    # zero bit = 732+1136 = 1868 us, x35 = 65380 us
    expected_us = pre_us + post_us + 8928 + 65380
    expected_samples = int(round(expected_us * sample_rate / 1_000_000))
    expected_bytes = expected_samples * 2
    data = synth_burst(bits, sample_rate=sample_rate, repeats=1,
                       pre_silence_us=pre_us, post_silence_us=post_us)
    # Allow 0.5% rounding slop from int(round(...)) per emission
    rel = abs(len(data) - expected_bytes) / expected_bytes
    assert rel < 0.005, (
        f"size mismatch: got {len(data)} bytes, expected ~{expected_bytes} "
        f"(rel diff {rel:.3%})"
    )


def test_repeats_scales_size_linearly():
    bits = "0" * 35
    one = synth_burst(bits, repeats=1, pre_silence_us=0, post_silence_us=0)
    eight = synth_burst(bits, repeats=8, pre_silence_us=0, post_silence_us=0)
    ratio = len(eight) / len(one)
    assert abs(ratio - 8) < 0.01, f"expected 8x size, got {ratio:.3f}x"


# ----- Amplitude / DC structure -----

def _magnitude_of_sample(data, idx):
    """Magnitude of sample at index `idx` (in samples, not bytes)."""
    i = data[2 * idx] - 128
    q = data[2 * idx + 1] - 128
    return math.sqrt(i * i + q * q)


def test_off_state_near_dc_center():
    """First pre-silence samples should have low magnitude (close to DC)."""
    data = synth_burst("0" * 35, pre_silence_us=10_000)
    # Look at samples deep inside the pre-silence (skip the first few to
    # avoid edge artifacts from the rng warm-up)
    mags = [_magnitude_of_sample(data, i) for i in range(100, 500)]
    avg = sum(mags) / len(mags)
    assert avg < 10, f"expected ~0 magnitude during silence, got avg {avg:.2f}"


def test_on_state_well_above_noise():
    """Samples deep inside an ON pulse should have magnitude near on_amp."""
    # Use only 1 repeat with no pre-silence so the first ON pulse starts at offset 0
    data = synth_burst("0" * 35, repeats=1, pre_silence_us=0, post_silence_us=0,
                       on_amp=90)
    # The first ON pulse is the leading sync (1476 us at 250 ksps = 369 samples)
    # Sample roughly the middle of it
    mid_idx = 50  # ~200 us into the first sync
    mag = _magnitude_of_sample(data, mid_idx)
    assert mag > 50, f"expected ON magnitude > 50, got {mag:.1f}"


def test_seed_deterministic():
    """Same bits + same seed -> byte-identical output."""
    a = synth_burst("01" * 17 + "0", seed=123)
    b = synth_burst("01" * 17 + "0", seed=123)
    assert a == b


def test_different_seed_changes_noise_not_envelope():
    """Different seeds give different bytes (due to noise) but same length."""
    a = synth_burst("01" * 17 + "0", seed=1)
    b = synth_burst("01" * 17 + "0", seed=2)
    assert len(a) == len(b)
    assert a != b


# ----- Input validation -----

def test_invalid_bit_char_raises():
    try:
        synth_burst("0" * 17 + "X" + "0" * 17)
        assert False, "should have raised ValueError on non-bit char"
    except ValueError as e:
        assert "0" in str(e) or "1" in str(e)


def test_empty_pattern_produces_just_silence_and_sync():
    """An empty bit pattern still emits sync triplets per repeat — the
    renderer doesn't enforce a fixed length, so tests can synthesize odd
    cases."""
    data = synth_burst("", repeats=2)
    # Should be roughly: pre + 2*(3*sync) + post bytes
    assert len(data) > 0


def run():
    tests = [v for k, v in globals().items()
             if k.startswith("test_") and callable(v)]
    passed = 0
    failed = 0
    for t in tests:
        try:
            t()
            passed += 1
            print(f"PASS {t.__name__}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
        except Exception as e:
            failed += 1
            print(f"ERROR {t.__name__}: {type(e).__name__}: {e}")
    print(f"\n{passed}/{len(tests)} synth_capture tests passed")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    run()
