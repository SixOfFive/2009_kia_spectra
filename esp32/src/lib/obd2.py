"""
OBD-II PID query/response helpers for ISO 15765-4 CAN bus.

Targets the 2009 Kia Spectra (and any other ISO 15765-4 vehicle from
2008 onward in North America). Standard 11-bit CAN identifiers:

  Functional request:  0x7DF   (broadcast — all OBD ECUs may respond)
  Physical request:    0x7E0   (engine ECU specifically)
  Response IDs:        0x7E8 .. 0x7EF  (each ECU adds 8 to its request ID)

Single-frame ISO-TP format (covers all standard mode 01 / mode 09 PIDs):
  Byte 0:    PCI byte. Low nibble = number of data bytes that follow (1..7).
  Byte 1:    Service (mode) — 0x01 for current data, 0x09 for vehicle info
  Byte 2:    PID number
  Bytes 3-7: padding 0x55 (or 0xAA, or 0x00 — doesn't matter for the ECU)

Response format:
  Byte 0:    PCI byte (number of data bytes)
  Byte 1:    Service + 0x40 (e.g. 0x41 = response to mode 01)
  Byte 2:    PID echoed back
  Bytes 3-N: PID-specific data
"""


# Standard CAN IDs (ISO 15765-4)
REQ_FUNCTIONAL = 0x7DF
REQ_PHYSICAL_ECU = 0x7E0
RESP_ECU_BASE = 0x7E8

# OBD-II service modes
MODE_CURRENT_DATA = 0x01
MODE_FREEZE_FRAME = 0x02
MODE_DTCS_STORED = 0x03
MODE_DTCS_PENDING = 0x07
MODE_VEHICLE_INFO = 0x09


# ----- PID dictionary -----
#
# Each PID has:
#   name:     short identifier
#   bytes:    number of data bytes in the response
#   scale:    function (data_bytes -> human-readable value)
#   units:    units string for display
#
# Limited to PIDs that work on the 2009 Spectra (ISO 15765-4 mandatory set +
# common Kia-supported extras). Verified against the SAE J1979 standard.


def _u16(d):
    """Unsigned 16-bit from two bytes."""
    return (d[0] << 8) | d[1]


def _scale_rpm(d):
    """0x0C - Engine RPM: ((A*256)+B)/4"""
    return _u16(d) / 4.0


def _scale_speed(d):
    """0x0D - Vehicle speed: A (km/h)"""
    return float(d[0])


def _scale_coolant(d):
    """0x05 - Coolant temp: A - 40 (deg C)"""
    return d[0] - 40


def _scale_intake_temp(d):
    """0x0F - Intake air temp: A - 40 (deg C)"""
    return d[0] - 40


def _scale_ambient_temp(d):
    """0x46 - Ambient air temp: A - 40 (deg C)"""
    return d[0] - 40


def _scale_throttle(d):
    """0x11 - Throttle position: A*100/255 (%)"""
    return d[0] * 100.0 / 255.0


def _scale_fuel_level(d):
    """0x2F - Fuel tank level: A*100/255 (%)"""
    return d[0] * 100.0 / 255.0


def _scale_control_module_voltage(d):
    """0x42 - Control module voltage: ((A*256)+B)/1000 (V)"""
    return _u16(d) / 1000.0


def _scale_maf(d):
    """0x10 - MAF air flow rate: ((A*256)+B)/100 (g/s)"""
    return _u16(d) / 100.0


def _scale_distance_mil(d):
    """0x21 - Distance traveled with MIL on: (A*256)+B (km)"""
    return _u16(d)


def _scale_distance_clear(d):
    """0x31 - Distance since codes cleared: (A*256)+B (km)"""
    return _u16(d)


def _scale_run_time(d):
    """0x1F - Run time since engine start: (A*256)+B (s)"""
    return _u16(d)


PIDS = {
    0x04: ("engine_load",           1, lambda d: d[0] * 100.0 / 255.0, "%"),
    0x05: ("coolant_temp",          1, _scale_coolant,                  "C"),
    0x0B: ("intake_manifold_pressure", 1, lambda d: float(d[0]),        "kPa"),
    0x0C: ("rpm",                   2, _scale_rpm,                      "rpm"),
    0x0D: ("speed",                 1, _scale_speed,                    "km/h"),
    0x0F: ("intake_air_temp",       1, _scale_intake_temp,              "C"),
    0x10: ("maf_rate",              2, _scale_maf,                      "g/s"),
    0x11: ("throttle_position",     1, _scale_throttle,                 "%"),
    0x1F: ("run_time_since_start",  2, _scale_run_time,                 "s"),
    0x21: ("distance_with_mil",     2, _scale_distance_mil,             "km"),
    0x2F: ("fuel_level",            1, _scale_fuel_level,               "%"),
    0x31: ("distance_since_clear",  2, _scale_distance_clear,           "km"),
    0x42: ("control_module_voltage", 2, _scale_control_module_voltage,  "V"),
    0x46: ("ambient_air_temp",      1, _scale_ambient_temp,             "C"),
}


# ----- Query encoding -----

def query(pid, mode=MODE_CURRENT_DATA):
    """
    Build the 8-byte CAN data field for a single-frame OBD-II PID request.

    The CAN ID for this frame should be REQ_FUNCTIONAL (0x7DF) for broadcast
    queries or REQ_PHYSICAL_ECU (0x7E0) for engine-specific.

    :param pid:  the PID number (e.g. 0x0C for RPM)
    :param mode: OBD-II service mode (default MODE_CURRENT_DATA = 0x01)
    :return:     bytes object of length 8
    """
    # PCI byte: 0x02 = single-frame, 2 data bytes follow
    return bytes([0x02, mode, pid, 0x55, 0x55, 0x55, 0x55, 0x55])


# ----- Response parsing -----

def parse_response(can_data):
    """
    Parse an 8-byte OBD-II response data field.

    :param can_data: 8 bytes (the data payload of the response CAN frame)
    :return: dict with keys:
        - pid: the PID number from the response
        - mode: the response mode (e.g. 0x41 for mode 01 response)
        - raw: the data bytes for this PID
        - name: PID name from the PIDS dictionary, or None
        - value: scaled value, or None if no scale function
        - units: units string, or None
        Returns None if the frame is malformed or not a valid response.
    """
    if len(can_data) < 3:
        return None

    pci = can_data[0]
    # Single-frame PCI: high nibble = 0, low nibble = byte count
    if (pci & 0xF0) != 0x00:
        # Multi-frame ISO-TP — not supported here; standard PID responses
        # always fit in a single frame.
        return None
    byte_count = pci & 0x0F
    if byte_count < 2 or byte_count > 7:
        return None

    mode = can_data[1]
    pid = can_data[2]
    raw = bytes(can_data[3:3 + (byte_count - 2)])

    name = None
    value = None
    units = None
    if pid in PIDS:
        name, expected_len, scale, units = PIDS[pid]
        if len(raw) >= expected_len:
            try:
                value = scale(raw[:expected_len])
            except (IndexError, ValueError, ZeroDivisionError):
                value = None

    return {
        "pid": pid,
        "mode": mode,
        "raw": raw,
        "name": name,
        "value": value,
        "units": units,
    }
