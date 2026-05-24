"""
Tests for the OBD-II PID query/response module.
"""

import os
import sys

_HERE = os.path.dirname(os.path.abspath(__file__))
_SRC = os.path.normpath(os.path.join(_HERE, "..", "src"))
if _SRC not in sys.path:
    sys.path.insert(0, _SRC)

from lib import obd2  # noqa: E402


def test_query_format():
    """A standard PID query should be exactly 8 bytes with correct framing."""
    q = obd2.query(0x0C)  # RPM
    assert len(q) == 8
    assert q[0] == 0x02            # PCI: single-frame, 2 data bytes
    assert q[1] == 0x01            # mode 01
    assert q[2] == 0x0C            # PID
    # Remaining bytes are padding
    assert all(b == 0x55 for b in q[3:])


def test_query_alternate_mode():
    """Mode 09 (vehicle info) query should set byte 1 accordingly."""
    q = obd2.query(0x02, mode=0x09)  # VIN
    assert q[1] == 0x09


def test_parse_rpm_response():
    """Mode 01 PID 0x0C with 2 data bytes -> RPM = ((A*256)+B)/4"""
    # 0x1AF8 / 4 = 1726.0 rpm
    frame = bytes([0x04, 0x41, 0x0C, 0x1A, 0xF8, 0x55, 0x55, 0x55])
    parsed = obd2.parse_response(frame)
    assert parsed is not None
    assert parsed["mode"] == 0x41
    assert parsed["pid"] == 0x0C
    assert parsed["name"] == "rpm"
    assert parsed["units"] == "rpm"
    assert abs(parsed["value"] - 1726.0) < 0.01


def test_parse_speed_response():
    """PID 0x0D with 1 data byte -> speed in km/h"""
    # 0x50 = 80 km/h
    frame = bytes([0x03, 0x41, 0x0D, 0x50, 0x55, 0x55, 0x55, 0x55])
    parsed = obd2.parse_response(frame)
    assert parsed["name"] == "speed"
    assert parsed["value"] == 80.0
    assert parsed["units"] == "km/h"


def test_parse_coolant_response():
    """PID 0x05: coolant temp = A - 40 (deg C)"""
    # 0x5F = 95, -40 = 55 deg C
    frame = bytes([0x03, 0x41, 0x05, 0x5F, 0x55, 0x55, 0x55, 0x55])
    parsed = obd2.parse_response(frame)
    assert parsed["name"] == "coolant_temp"
    assert parsed["value"] == 55
    assert parsed["units"] == "C"


def test_parse_control_module_voltage():
    """PID 0x42: control module voltage = ((A*256)+B)/1000 (V)"""
    # 0x36B0 = 14000 -> 14.000 V
    frame = bytes([0x04, 0x41, 0x42, 0x36, 0xB0, 0x55, 0x55, 0x55])
    parsed = obd2.parse_response(frame)
    assert parsed["name"] == "control_module_voltage"
    assert abs(parsed["value"] - 14.0) < 0.001


def test_parse_unknown_pid():
    """An unrecognized PID should return raw bytes but no parsed value."""
    frame = bytes([0x03, 0x41, 0xFE, 0xAB, 0x55, 0x55, 0x55, 0x55])
    parsed = obd2.parse_response(frame)
    assert parsed["pid"] == 0xFE
    assert parsed["name"] is None
    assert parsed["value"] is None
    assert parsed["units"] is None
    assert parsed["raw"] == bytes([0xAB])


def test_parse_malformed_frame():
    """Frames that don't look like single-frame ISO-TP should return None."""
    # Too short
    assert obd2.parse_response(bytes([0x02, 0x41])) is None
    # PCI says multi-frame (high nibble != 0)
    multi = bytes([0x10, 0x41, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00])
    assert obd2.parse_response(multi) is None
    # PCI says 0 bytes follow
    zero = bytes([0x00, 0x41, 0x0C, 0x00, 0x00, 0x00, 0x00, 0x00])
    assert obd2.parse_response(zero) is None


def test_pids_dict_consistency():
    """Every PID entry should have a valid (name, length, scale, units) tuple."""
    for pid, entry in obd2.PIDS.items():
        assert 0 <= pid <= 0xFF
        name, length, scale, units = entry
        assert isinstance(name, str) and name
        assert 1 <= length <= 4
        assert callable(scale)
        assert isinstance(units, str)


if __name__ == "__main__":
    tests = [
        ("query_format",                test_query_format),
        ("query_alternate_mode",        test_query_alternate_mode),
        ("parse_rpm_response",          test_parse_rpm_response),
        ("parse_speed_response",        test_parse_speed_response),
        ("parse_coolant_response",      test_parse_coolant_response),
        ("parse_control_module_voltage", test_parse_control_module_voltage),
        ("parse_unknown_pid",           test_parse_unknown_pid),
        ("parse_malformed_frame",       test_parse_malformed_frame),
        ("pids_dict_consistency",       test_pids_dict_consistency),
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
