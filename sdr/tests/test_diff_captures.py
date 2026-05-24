"""
Round-trip tests for sdr/scripts/diff-captures.py.

Synthesize pairs of cu8 captures — matched (same bits, different seeds)
and mismatched (single-bit flip) — and confirm the diff script reports
PASS / FAIL with the expected exit code in each case.

The mismatch test also covers the "first mismatch position" report,
which docs/12-bench-validation.md step 3 relies on for diagnosing
CC1101 pulse-width drift.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.normpath(os.path.join(HERE, "..", "scripts"))
sys.path.insert(0, SCRIPTS)

from synth_capture import synth_burst  # noqa: E402


def _write(bits, path, seed=42):
    with open(path, "wb") as f:
        f.write(synth_burst(bits, seed=seed))


def _diff(path_a, path_b):
    """Run diff-captures.py; return (combined_output, rc)."""
    script = os.path.join(SCRIPTS, "diff-captures.py")
    r = subprocess.run(
        [sys.executable, script, path_a, path_b],
        capture_output=True, text=True,
    )
    return r.stdout + r.stderr, r.returncode


def test_identical_bits_different_seeds_match():
    """Same bit pattern, different noise realization -> PASS (exit 0)."""
    bits = "10110101101011010110101101011010110"
    with tempfile.TemporaryDirectory() as tmp:
        a = os.path.join(tmp, "a.bin"); _write(bits, a, seed=1)
        b = os.path.join(tmp, "b.bin"); _write(bits, b, seed=2)
        out, rc = _diff(a, b)
        assert rc == 0, f"expected PASS, got rc={rc}, out:\n{out}"
        assert "PASS" in out


def test_single_bit_flip_fails():
    """Flip one bit -> FAIL (exit 1), with the mismatch position reported."""
    a_bits = "10110101101011010110101101011010110"
    b_bits = "10110101101011010110101101011010111"  # last bit flipped
    with tempfile.TemporaryDirectory() as tmp:
        a = os.path.join(tmp, "a.bin"); _write(a_bits, a)
        b = os.path.join(tmp, "b.bin"); _write(b_bits, b)
        out, rc = _diff(a, b)
        assert rc != 0, f"expected FAIL, got rc=0"
        assert "FAIL" in out
        # The differing bit is at position 34 (zero-indexed) — last bit
        assert "34" in out, f"expected mismatch position in output:\n{out}"


def test_all_zeros_vs_all_ones_fails():
    with tempfile.TemporaryDirectory() as tmp:
        a = os.path.join(tmp, "a.bin"); _write("0" * 35, a)
        b = os.path.join(tmp, "b.bin"); _write("1" * 35, b)
        out, rc = _diff(a, b)
        assert rc != 0
        assert "FAIL" in out


def test_missing_file_returns_nonzero():
    out, rc = _diff("/nonexistent_a.bin", "/nonexistent_b.bin")
    assert rc != 0


def test_two_unrelated_real_world_patterns_fail():
    """Different patterns with the SAME 16-bit Remote ID prefix but
    differing tails — simulates the "you typed the wrong button into
    secrets.py" failure mode where the FOB is right but the button
    mapping is wrong."""
    prefix = "1010101111001101"  # 16-bit Remote ID = 0xABCD
    a_bits = prefix + "1110000000000000010"   # 16 + 19 = 35
    b_bits = prefix + "1110011111110000001"   # diverges at bit 19
    assert len(a_bits) == 35
    assert len(b_bits) == 35
    with tempfile.TemporaryDirectory() as tmp:
        a = os.path.join(tmp, "a.bin"); _write(a_bits, a)
        b = os.path.join(tmp, "b.bin"); _write(b_bits, b)
        out, rc = _diff(a, b)
        assert rc != 0
        assert "FAIL" in out
        # Both patterns share the Remote ID prefix, so the first mismatch
        # must be at bit 16 or later (somewhere in the suffix).
        import re
        m = re.search(r"first mismatch at bit (\d+)", out)
        assert m, f"expected 'first mismatch at bit N' in output:\n{out}"
        bit_pos = int(m.group(1))
        assert bit_pos >= 16, (
            f"first mismatch should be in the suffix (>= 16), got {bit_pos}"
        )


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
    print(f"\n{passed}/{len(tests)} diff_captures tests passed")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    run()
