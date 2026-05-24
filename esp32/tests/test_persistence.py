"""Persistence helpers — CPython smoke tests (no real RTC available)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "src"))

from lib import persistence  # noqa: E402


def test_load_streak_returns_zero_without_rtc():
    # CPython fallback: _HAS_RTC is False, load returns 0
    assert persistence.load_streak() == 0


def test_save_streak_is_no_op_in_cpython():
    persistence.save_streak(42)
    assert persistence.load_streak() == 0


def test_clear_streak_does_not_throw():
    persistence.clear_streak()


def test_was_deep_sleep_wake_returns_false_without_rtc():
    assert persistence.was_deep_sleep_wake() is False


def test_rtc_magic_and_format_match_layout():
    # Sanity: blob layout doc claims 8 bytes total (magic + count + reserved)
    import struct
    assert persistence.RTC_MAGIC == b"VRM2"
    assert struct.calcsize(persistence._RTC_FORMAT) == 2


def test_reset_cause_string_cpython_fallback():
    # No machine.reset_cause available in CPython → stable sentinel.
    assert persistence.reset_cause_string() == "cpython"


def test_reset_cause_string_known_codes_via_monkeypatch():
    # Simulate MicroPython by injecting a reset_cause() returning each known
    # code, and confirm we get the documented label.
    saved = persistence.reset_cause
    try:
        for code, expected in [
            (0, "PWRON_RESET"),
            (1, "HARD_RESET"),
            (2, "WDT_RESET"),
            (4, "DEEPSLEEP_RESET"),
            (5, "SOFT_RESET"),
        ]:
            persistence.reset_cause = (lambda c=code: c)
            assert persistence.reset_cause_string() == expected, (
                "code %d → %s, got %s" % (code, expected, persistence.reset_cause_string()))
        # An unknown code returns "unknown(<n>)" rather than blowing up.
        persistence.reset_cause = lambda: 99
        assert persistence.reset_cause_string() == "unknown(99)"
    finally:
        persistence.reset_cause = saved


def run():
    tests = [v for k, v in globals().items() if k.startswith("test_") and callable(v)]
    for t in tests:
        t()
        print(f"PASS {t.__name__}")
    print(f"\n{len(tests)}/{len(tests)} persistence tests passed")


if __name__ == "__main__":
    run()
