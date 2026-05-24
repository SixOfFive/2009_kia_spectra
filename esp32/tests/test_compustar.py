"""
Tests for the Compustar HCS packet builder.

These verify the packet *structure* (bit layout, sizes, encoder integration).
Actual ciphertext values can't be locked in until we have authoritative test
vectors from real FOB captures.
"""

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_SRC = os.path.normpath(os.path.join(_HERE, "..", "src"))
if _SRC not in sys.path:
    sys.path.insert(0, _SRC)

from lib import compustar, keeloq  # noqa: E402


def test_build_hopping_code_calls_keeloq():
    """Hopping code should match a direct KeeLoq encrypt of the packed plaintext."""
    counter = 0x1234
    function_code = 0x4
    discrimination = 0xA
    device_key = 0x0123456789ABCDEF

    expected_plaintext = 0x1234 | (0xA << 16) | (0x4 << 20)
    expected = keeloq.encrypt(expected_plaintext, device_key)

    result = compustar.build_hopping_code(counter, function_code, discrimination, device_key)
    assert result == expected


def test_build_packet_structure():
    """Packet should be exactly 66 bits and contain all expected fields."""
    serial = 0x0ABCDEF  # 28-bit
    function_code = compustar.Function.START
    counter = 0x0042
    device_key = 0xDEADBEEFCAFEBABE

    packet = compustar.build_packet(serial, function_code, counter, device_key)

    # Structural checks
    assert len(packet["bits"]) == 66
    assert all(b in (0, 1) for b in packet["bits"])

    # Fixed-code structure: 28-bit serial in high bits, 4-bit function in low
    assert packet["fixed_code"] >> 4 == serial
    assert packet["fixed_code"] & 0xF == function_code

    # Hopping code should be 32 bits
    assert 0 <= packet["hopping_code"] <= 0xFFFFFFFF

    # Status defaults to 0 (no V-low, not a repeat)
    assert packet["status"] == 0


def test_build_packet_status_flags():
    """V-low and repeat flags should pack into the 2-bit status field."""
    p1 = compustar.build_packet(0, 0, 0, 0, v_low=1, repeat=0)
    p2 = compustar.build_packet(0, 0, 0, 0, v_low=0, repeat=1)
    p3 = compustar.build_packet(0, 0, 0, 0, v_low=1, repeat=1)

    # v_low is the MSB of the 2-bit status, repeat is the LSB
    assert p1["status"] == 0b10
    assert p2["status"] == 0b01
    assert p3["status"] == 0b11


def test_serial_and_function_masking():
    """Oversized inputs should be masked to their declared widths, not crash."""
    big_serial = 0xFFFFFFFF        # 32-bit input for 28-bit slot
    big_function = 0xFF            # 8-bit input for 4-bit slot
    packet = compustar.build_packet(big_serial, big_function, 0, 0)
    # Top 4 bits of the 32-bit serial input should have been dropped
    assert packet["fixed_code"] >> 4 == 0x0FFFFFFF
    assert packet["fixed_code"] & 0xF == 0xF  # function masked to 4 bits


def test_packet_to_pulses_structure():
    """Pulse train should have correct preamble + gap + bit count."""
    packet = compustar.build_packet(0x12345, 0x4, 0x0001, 0x0)
    pulses = compustar.packet_to_pulses(packet["bits"])

    expected_len = (
        compustar.PREAMBLE_HALF_BITS  # preamble cycles
        + 1                            # header gap pulse
        + 66                           # data bits
    )
    assert len(pulses) == expected_len

    # Each entry is a (high, low) tuple of non-negative ints
    for high, low in pulses:
        assert high >= 0
        assert low >= 0


def test_packet_to_pulses_pwm_encoding():
    """PWM bit encoding: 0 = (TE, 2TE), 1 = (2TE, TE)."""
    # Build a packet whose first data bit we control by handcrafting bits.
    # Easier: directly call packet_to_pulses on a known bit list.
    bits = [1, 0]  # one of each
    pulses = compustar.packet_to_pulses(
        bits,
        te_us=400,
        preamble_half_bits=0,  # skip preamble for this test
        header_gap_te=0,       # skip gap
    )
    # The gap pulse is still emitted (0 high, 0 low), then 2 data bits.
    # Skip past the gap pulse (index 0) — it's (0, 0) when header_gap_te=0.
    assert pulses[0] == (0, 0)
    # bit 1 = high 2*TE, low TE
    assert pulses[1] == (800, 400)
    # bit 0 = high TE, low 2*TE
    assert pulses[2] == (400, 800)


if __name__ == "__main__":
    tests = [
        ("build_hopping_code_calls_keeloq", test_build_hopping_code_calls_keeloq),
        ("build_packet_structure",          test_build_packet_structure),
        ("build_packet_status_flags",       test_build_packet_status_flags),
        ("serial_and_function_masking",     test_serial_and_function_masking),
        ("packet_to_pulses_structure",      test_packet_to_pulses_structure),
        ("packet_to_pulses_pwm_encoding",   test_packet_to_pulses_pwm_encoding),
    ]
    failed = 0
    for name, fn in tests:
        try:
            fn()
            print("PASS  {}".format(name))
        except AssertionError as exc:
            failed += 1
            print("FAIL  {}".format(name))
            print("      {}".format(exc))
        except Exception as exc:
            failed += 1
            print("ERROR {} ({}: {})".format(name, type(exc).__name__, exc))
    print()
    print("{}/{} tests passed".format(len(tests) - failed, len(tests)))
    sys.exit(0 if failed == 0 else 1)
