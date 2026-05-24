"""
Round-trip tests for sdr/scripts/capture-to-secrets.py via synth_capture.

Generate a synthetic rtl_sdr cu8 capture with a known 35-bit pattern and
known Remote ID (first 16 bits), run capture-to-secrets.py against it,
and confirm the printed secrets.py line matches what we put in.

This is the contract that protects users from a demodulator regression:
if a code change breaks the synth -> demod round trip, these tests fail.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.normpath(os.path.join(HERE, "..", "scripts"))
sys.path.insert(0, SCRIPTS)

from synth_capture import synth_burst  # noqa: E402


def _write_synth(bits, path, **kwargs):
    data = synth_burst(bits, **kwargs)
    with open(path, "wb") as f:
        f.write(data)


def _run(button, path):
    """Invoke capture-to-secrets.py as a subprocess.

    Returns (combined_output, rc) — stderr is folded into the output
    string so tests can match against error messages without juggling
    two streams.
    """
    script = os.path.join(SCRIPTS, "capture-to-secrets.py")
    r = subprocess.run(
        [sys.executable, script, "--button", button, path],
        capture_output=True, text=True,
    )
    return r.stdout + r.stderr, r.returncode


def test_round_trip_start_zeros():
    """All-zero pattern: Remote ID 0x0000, pattern 35 zeros."""
    bits = "0" * 35
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, "synth-start-zeros.bin")
        _write_synth(bits, p)
        out, rc = _run("START", p)
        assert rc == 0, f"exit code {rc}, output:\n{out}"
        assert bits in out, f"expected pattern in output, got:\n{out}"
        assert "0x0000" in out, f"expected Remote ID 0x0000, got:\n{out}"
        assert '"START":' in out


def test_round_trip_lock_mixed_pattern():
    """Mixed pattern: Remote ID 0x6969 (= 0110 1001 0110 1001 binary)."""
    bits = "01101001011010010110100101101001011"
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, "synth-lock.bin")
        _write_synth(bits, p)
        out, rc = _run("LOCK", p)
        assert rc == 0, out
        assert bits in out
        assert "0x6969" in out
        assert '"LOCK":' in out


def test_round_trip_unlock_high_bit_pattern():
    """Pattern starting with 1: Remote ID upper-half (>= 0x8000)."""
    bits = "10110101101011010110101101011010110"
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, "synth-unlock.bin")
        _write_synth(bits, p)
        out, rc = _run("UNLOCK", p)
        assert rc == 0, out
        assert bits in out
        assert '"UNLOCK":' in out
        # First 16 bits = 1011010110101101 = 0xB5AD
        assert "0xB5AD" in out


def test_round_trip_trunk_all_ones():
    bits = "1" * 35
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, "synth-trunk.bin")
        _write_synth(bits, p)
        out, rc = _run("TRUNK", p)
        assert rc == 0, out
        assert bits in out
        assert "0xFFFF" in out
        assert '"TRUNK":' in out


def test_missing_file_returns_error():
    out, rc = _run("START", "/nonexistent/path/to/capture.bin")
    assert rc != 0
    assert "not found" in out.lower() or "not found" in out


def test_button_case_insensitive():
    """--button should accept lowercase too (argparse type=str.upper)."""
    bits = "0" * 35
    with tempfile.TemporaryDirectory() as tmp:
        p = os.path.join(tmp, "synth.bin")
        _write_synth(bits, p)
        # Run with lowercase
        script = os.path.join(SCRIPTS, "capture-to-secrets.py")
        r = subprocess.run(
            [sys.executable, script, "--button", "start", p],
            capture_output=True, text=True,
        )
        assert r.returncode == 0, r.stdout + r.stderr
        assert '"START":' in r.stdout


def test_consistent_remote_id_across_buttons():
    """Synthesize 4 captures all sharing the same Remote ID prefix; verify
    capture-to-secrets reports the same Remote ID on each."""
    prefix = "1010101010101010"  # = 0xAAAA
    patterns = {
        "START":  prefix + "1110000000000000010",
        "LOCK":   prefix + "1110000000000000001",
        "UNLOCK": prefix + "1110000000000000100",
        "TRUNK":  prefix + "1110000000000001000",
    }
    with tempfile.TemporaryDirectory() as tmp:
        for button, bits in patterns.items():
            p = os.path.join(tmp, f"synth-{button.lower()}.bin")
            _write_synth(bits, p)
            out, rc = _run(button, p)
            assert rc == 0, f"{button}: {out}"
            assert bits in out
            assert "0xAAAA" in out, f"{button} Remote ID drifted: {out}"


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
    print(f"\n{passed}/{len(tests)} capture_to_secrets tests passed")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    run()
