"""
Tests for the Compustar 1WSHR-PRO fixed-code packet builder.

Verify the parser, pulse-rendering, button-lookup, and validation paths.
Bit-pattern values used here are SYNTHETIC; real per-FOB patterns live in
``secrets.py`` (gitignored).
"""

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_SRC = os.path.normpath(os.path.join(_HERE, "..", "src"))
if _SRC not in sys.path:
    sys.path.insert(0, _SRC)

from lib import compustar  # noqa: E402


# Synthetic 35-bit patterns for testing — not real FOB data
SAMPLE_PATTERNS = {
    "START":  "0" * 35,
    "LOCK":   "1" * 35,
    "UNLOCK": "01" * 17 + "0",   # 35 alternating bits
    "TRUNK":  "11111000001111100000111110000011111",  # arbitrary 35-bit
}


# ----- parse_bit_pattern -----

def test_parse_bit_pattern_basic():
    bits = compustar.parse_bit_pattern("01010101010101010101010101010101010")
    assert len(bits) == 35
    assert all(b in (0, 1) for b in bits)
    assert bits[0] == 0
    assert bits[1] == 1


def test_parse_bit_pattern_strips_whitespace_and_separators():
    """Whitespace, underscores, hyphens are ignored."""
    bits = compustar.parse_bit_pattern("0010 1101_11010-1100001000010011111011")
    assert len(bits) == 35
    assert bits[:4] == [0, 0, 1, 0]


def test_parse_bit_pattern_rejects_bad_chars():
    try:
        compustar.parse_bit_pattern("0" * 34 + "X")
        assert False, "should have raised ValueError"
    except ValueError as e:
        assert "invalid bit char" in str(e)


def test_parse_bit_pattern_rejects_wrong_length():
    try:
        compustar.parse_bit_pattern("0" * 34)  # 1 short
        assert False, "should have raised ValueError"
    except ValueError as e:
        assert "expected" in str(e)

    try:
        compustar.parse_bit_pattern("0" * 36)  # 1 long
        assert False, "should have raised ValueError"
    except ValueError:
        pass


def test_parse_bit_pattern_custom_length():
    """expected_bits override should be honored (for tests / debug use)."""
    bits = compustar.parse_bit_pattern("0110", expected_bits=4)
    assert bits == [0, 1, 1, 0]


# ----- packet_to_pulses -----

def test_packet_to_pulses_length():
    """3 sync pulses + 35 data pulses = 38 total."""
    bits = compustar.parse_bit_pattern("0" * 35)
    pulses = compustar.packet_to_pulses(bits)
    assert len(pulses) == compustar.SYNC_COUNT + 35
    assert len(pulses) == 38


def test_packet_to_pulses_sync_first():
    """The first SYNC_COUNT entries are the sync pulse pair."""
    bits = [0] * 35
    pulses = compustar.packet_to_pulses(bits)
    for i in range(compustar.SYNC_COUNT):
        assert pulses[i] == (compustar.SYNC_HIGH_US, compustar.SYNC_LOW_US)


def test_packet_to_pulses_zero_bit_encoding():
    """A 0 bit is (SHORT_HIGH, SHORT_LOW)."""
    pulses = compustar.packet_to_pulses([0] * 35)
    # Skip sync pulses; first data pulse is at index SYNC_COUNT
    assert pulses[compustar.SYNC_COUNT] == (
        compustar.SHORT_HIGH_US, compustar.SHORT_LOW_US,
    )


def test_packet_to_pulses_one_bit_encoding():
    """A 1 bit is (LONG_HIGH, LONG_LOW)."""
    pulses = compustar.packet_to_pulses([1] * 35)
    assert pulses[compustar.SYNC_COUNT] == (
        compustar.LONG_HIGH_US, compustar.LONG_LOW_US,
    )


def test_packet_to_pulses_mixed_bits():
    """Alternating 0/1 bits produce alternating pulse types."""
    bits = [0, 1] * 17 + [0]   # 35 bits
    pulses = compustar.packet_to_pulses(bits)
    # First data bit (0) → short pulse
    assert pulses[compustar.SYNC_COUNT] == (
        compustar.SHORT_HIGH_US, compustar.SHORT_LOW_US,
    )
    # Second data bit (1) → long pulse
    assert pulses[compustar.SYNC_COUNT + 1] == (
        compustar.LONG_HIGH_US, compustar.LONG_LOW_US,
    )


def test_packet_to_pulses_pulse_pair_sum_consistent():
    """Each pulse pair sums to roughly 1860 us (within ~10 us tolerance)."""
    bits = [0, 1] * 17 + [0]
    pulses = compustar.packet_to_pulses(bits)
    # Sync pair: ~1476 + 1500 = 2976 (different — sync is intentionally longer)
    sync_h, sync_l = pulses[0]
    assert 2900 <= sync_h + sync_l <= 3100
    # Data pairs: short = 732+1136=1868, long = 1100+756=1856
    for h, low in pulses[compustar.SYNC_COUNT:]:
        assert 1800 <= h + low <= 1920


def test_packet_to_pulses_override_pulse_widths():
    """Caller-provided pulse widths override module defaults."""
    pulses = compustar.packet_to_pulses(
        [0, 1],
        sync_count=1,
        short_high_us=100, short_low_us=200,
        long_high_us=300, long_low_us=400,
        sync_high_us=500, sync_low_us=600,
    )
    assert pulses[0] == (500, 600)
    assert pulses[1] == (100, 200)
    assert pulses[2] == (300, 400)


# ----- build_pulses_for_button -----

def test_build_pulses_for_button_known():
    pulses = compustar.build_pulses_for_button(
        compustar.Button.START, SAMPLE_PATTERNS,
    )
    assert len(pulses) == compustar.SYNC_COUNT + compustar.PACKET_BITS


def test_build_pulses_for_button_unknown_raises():
    try:
        compustar.build_pulses_for_button("NONEXISTENT", SAMPLE_PATTERNS)
        assert False, "should have raised KeyError"
    except KeyError:
        pass


def test_build_pulses_for_button_validates_pattern():
    """A malformed stored pattern should bubble up as ValueError."""
    bad = dict(SAMPLE_PATTERNS)
    bad["START"] = "X" * 35
    try:
        compustar.build_pulses_for_button(compustar.Button.START, bad)
        assert False, "should have raised ValueError"
    except ValueError:
        pass


# ----- Button class -----

def test_button_constants():
    assert compustar.Button.START == "START"
    assert compustar.Button.LOCK == "LOCK"
    assert compustar.Button.UNLOCK == "UNLOCK"
    assert compustar.Button.TRUNK == "TRUNK"
    assert "START" in compustar.Button.ALL
    assert len(compustar.Button.ALL) == 4


# ----- validate_packets -----

def test_validate_packets_complete():
    errors = compustar.validate_packets(SAMPLE_PATTERNS)
    assert errors == []


def test_validate_packets_none():
    errors = compustar.validate_packets(None)
    assert len(errors) == 1
    assert "None" in errors[0]


def test_validate_packets_not_dict():
    errors = compustar.validate_packets("not a dict")
    assert len(errors) == 1
    assert "not a dict" in errors[0]


def test_validate_packets_missing_button():
    incomplete = dict(SAMPLE_PATTERNS)
    del incomplete["TRUNK"]
    errors = compustar.validate_packets(incomplete)
    assert any("TRUNK" in e for e in errors)


def test_validate_packets_bad_pattern():
    bad = dict(SAMPLE_PATTERNS)
    bad["LOCK"] = "0" * 30  # too short
    errors = compustar.validate_packets(bad)
    assert any("LOCK" in e for e in errors)


# ----- Module-level constants sanity -----

def test_pulse_widths_in_reasonable_range():
    """All pulse widths should be in the 100-2000 us range — gut check
    against accidentally setting them in seconds or hundreds of µs."""
    for name in ("SHORT_HIGH_US", "SHORT_LOW_US",
                 "LONG_HIGH_US", "LONG_LOW_US",
                 "SYNC_HIGH_US", "SYNC_LOW_US"):
        v = getattr(compustar, name)
        assert 100 < v < 2000, "%s = %d out of sane range" % (name, v)


def test_packet_bits_constant():
    assert compustar.PACKET_BITS == 35
    assert compustar.SYNC_COUNT == 3


if __name__ == "__main__":
    tests = [(name, fn) for name, fn in globals().items()
             if name.startswith("test_") and callable(fn)]
    failed = 0
    for name, fn in tests:
        try:
            fn()
            print("PASS  {}".format(name))
        except AssertionError as exc:
            failed += 1
            print("FAIL  {}: {}".format(name, exc))
        except Exception as exc:
            failed += 1
            print("ERROR {} ({}: {})".format(name, type(exc).__name__, exc))
    print()
    print("{}/{} tests passed".format(len(tests) - failed, len(tests)))
    sys.exit(0 if failed == 0 else 1)
