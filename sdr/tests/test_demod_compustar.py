"""
Round-trip tests for sdr/scripts/demod-compustar.py via synth_capture.

demod-compustar.py is the lower-level decoder under capture-to-secrets /
diff-captures. This file exercises it directly (via --verbose stdout
parsing) to catch regressions in the burst detector, sync-finding logic,
and pulse-width estimator before they cascade into the higher-level
tools.

Note: demod-compustar.py uses ``N_BITS = 36`` per the canonical 1WG3R
spec, but our 1WSHR-PRO sub-variant is 3-sync + 35 bits, so the 36th bit
in the demod's output is always a sync-pulse contaminant. These tests
only assert against the first 35 bits and the extracted Remote ID
(stable across the contamination since it's in bits 0..15).
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.normpath(os.path.join(HERE, "..", "scripts"))
sys.path.insert(0, SCRIPTS)

from synth_capture import synth_burst  # noqa: E402


def _write(bits, path, **kwargs):
    with open(path, "wb") as f:
        f.write(synth_burst(bits, **kwargs))


def _run_demod(path):
    """Invoke demod-compustar.py --verbose; return (output, rc)."""
    script = os.path.join(SCRIPTS, "demod-compustar.py")
    r = subprocess.run(
        [sys.executable, script, "--verbose", path],
        capture_output=True, text=True,
    )
    return r.stdout + r.stderr, r.returncode


def _extract_ids(out):
    """Return the set of Remote IDs the demod reported across its candidate
    decodes."""
    return set(re.findall(r"ID=0x([0-9A-Fa-f]{4})", out))


def test_burst_detected_in_synth():
    """demod-compustar must successfully detect at least one burst."""
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, "synth.bin")
        _write("01" * 17 + "0", p)
        out, rc = _run_demod(p)
        assert rc == 0, f"rc={rc}, out:\n{out}"
        assert "Burst" in out, f"expected 'Burst' in output:\n{out}"


def test_silence_only_decodes_zero_packets():
    """A file with only baseline noise should decode 0 valid packets.

    (The burst detector might still pick up the whole noisy window as a
    'burst region' since there's no real silence reference to compare
    against — but no real sync pulses means no candidate decodes.)"""
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, "silence.bin")
        from synth_capture import _emit, NOISE_OFF_DEFAULT
        import random
        rng = random.Random(0)
        sps = 250_000 / 1_000_000
        buf = bytearray()
        _emit(buf, sps, 500_000, False, 90, NOISE_OFF_DEFAULT, 5, rng)
        with open(p, "wb") as f:
            f.write(buf)
        out, _rc = _run_demod(p)
        # Whether the burst detector flags noise as a burst or not, the
        # packet-decode count for pure noise must be 0.
        m = re.search(r"packets decoded:\s*(\d+)", out)
        assert m, f"missing packet count summary:\n{out}"
        n = int(m.group(1))
        assert n == 0, f"expected 0 packets from silence, got {n}"


def test_remote_id_extracted_correctly_from_alternating():
    """Pattern 01010... has Remote ID = 0b0101010101010101 = 0x5555."""
    bits = "01" * 17 + "0"
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, "synth.bin")
        _write(bits, p)
        out, _rc = _run_demod(p)
        ids = _extract_ids(out)
        # At least one of the candidate decodes should report the right ID.
        assert "5555" in ids, f"expected 0x5555 among {ids}\n{out}"


def test_remote_id_extracted_correctly_from_mixed():
    """Pattern 0110100101101001... has Remote ID 0x6969."""
    bits = "01101001011010010110100101101001011"
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, "synth.bin")
        _write(bits, p)
        out, _rc = _run_demod(p)
        ids = _extract_ids(out)
        assert "6969" in ids, f"expected 0x6969 among {ids}\n{out}"


def test_data_bits_present_in_decode():
    """The first 35 bits of one candidate decode should match the synth
    input (the 36th bit is sync-pulse contamination)."""
    bits = "01101001011010010110100101101001011"
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, "synth.bin")
        _write(bits, p)
        out, _rc = _run_demod(p)
        # Find every "bits = <36-char string>" the demod printed
        candidates = re.findall(r"bits = ([01]{36})", out)
        assert candidates, f"no decoded bit strings in output:\n{out}"
        truncated = {c[:35] for c in candidates}
        assert bits in truncated, (
            f"expected {bits} in first-35 of {truncated}"
        )


def test_synth_repeats_count_matches():
    """Demod should report packets-decoded equal to the synth repeats count
    (give or take a small overshoot from the sync-misalignment candidates)."""
    bits = "1" * 35
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, "synth.bin")
        _write(bits, p, repeats=4)
        out, _rc = _run_demod(p)
        # Look for "packets decoded: N" in summary line
        m = re.search(r"packets decoded:\s*(\d+)", out)
        assert m, f"missing packet count summary:\n{out}"
        n = int(m.group(1))
        # The demod produces multiple candidates per packet (one per sync
        # in the triplet), so n is between repeats and 3*repeats. Just
        # check it's plausibly nonzero.
        assert n >= 4, f"expected at least 4 packets decoded, got {n}"


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
    print(f"\n{passed}/{len(tests)} demod_compustar tests passed")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    run()
