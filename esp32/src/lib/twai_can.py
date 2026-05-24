"""
ESP32 TWAI / CAN bus driver — thin wrapper for OBD-II over ISO 15765-4.

ISO 15765-4 mandates 500 kbps CAN with 11-bit IDs for OBD-II on vehicles
2008 and newer (the 2009 Kia Spectra qualifies). Standard IDs:

    0x7DF — broadcast functional request (we send to this)
    0x7E0 — physical request to ECU #1 (alternative)
    0x7E8 — physical response from ECU #1 (we receive on this)

Wiring (SN65HVD230 transceiver → ESP32 → OBD-II):

    SN65HVD230 VCC → 3.3V
    SN65HVD230 GND → GND
    SN65HVD230 D   → ESP32 GPIO 5  (TWAI TX)
    SN65HVD230 R   → ESP32 GPIO 4  (TWAI RX)
    SN65HVD230 CANH → OBD-II pin 6
    SN65HVD230 CANL → OBD-II pin 14

MicroPython note:
    Stable MicroPython for ESP32 does NOT include a built-in CAN/TWAI
    module. You need either:
      (a) a community fork that exposes ``machine.CAN`` (some Lemariva,
          loboris, or custom-built firmware variants do), OR
      (b) build MicroPython from source with ESP-IDF TWAI bindings enabled,
          OR
      (c) swap to a MCP2515 SPI CAN controller (we don't have one in BOM).

This driver tries ``machine.CAN`` first; if unavailable, raises ImportError
with a clear message at construction time so the failure mode is obvious
during bench-test rather than mysterious during a live driving session.
"""

try:
    from machine import Pin
    import time
    _MICROPYTHON = True
except ImportError:
    _MICROPYTHON = False

    class Pin:  # noqa
        def __init__(self, *a, **kw): pass

    class _Time:
        def sleep_ms(self, n): pass
    time = _Time()


# Try to import CAN. Different MicroPython forks expose it under different paths.
_CAN_BACKEND = None
_CAN_IMPORT_ERROR = None
try:
    from machine import CAN as _CAN  # noqa
    _CAN_BACKEND = "machine.CAN"
except ImportError as e:
    _CAN_IMPORT_ERROR = str(e)


# OBD-II standard CAN IDs
OBD_REQUEST_BROADCAST = 0x7DF
OBD_REQUEST_ECU1 = 0x7E0
OBD_RESPONSE_ECU1 = 0x7E8
OBD_RESPONSE_RANGE = (0x7E8, 0x7EF)   # responses from ECU 1..8


class Can:
    """
    Thin wrapper around the underlying CAN implementation.

    Methods:
      send(can_id, data)      — transmit one frame
      receive(timeout_ms)     — wait for one frame, return (can_id, data) or None
    """

    def __init__(self, tx_pin=5, rx_pin=4, baudrate=500_000):
        if _CAN_BACKEND is None and _MICROPYTHON:
            raise ImportError(
                "machine.CAN not available in this MicroPython build. "
                "See lib/twai_can.py docstring for fixes (community fork, "
                "rebuild with TWAI, or swap to MCP2515)."
            )
        self._can = None
        if _MICROPYTHON:
            # API varies between forks; most accept these kwargs
            try:
                self._can = _CAN(0, mode=_CAN.NORMAL, baudrate=baudrate,
                                 tx=Pin(tx_pin), rx=Pin(rx_pin))
            except TypeError:
                # Older API
                self._can = _CAN(0)
                self._can.init(mode=_CAN.NORMAL, baudrate=baudrate,
                               tx=Pin(tx_pin), rx=Pin(rx_pin))

    def send(self, can_id, data):
        """
        Send a single CAN frame.

        :param can_id: 11-bit identifier (0..0x7FF)
        :param data:   bytes-like, up to 8 bytes (OBD-II always uses 8)
        """
        if self._can is None:
            return  # CPython no-op
        self._can.send(bytes(data), can_id)

    def receive(self, timeout_ms=1000):
        """
        Wait for a single CAN frame; return (can_id, data) or None on timeout.
        """
        if self._can is None:
            return None
        # machine.CAN.recv() interfaces vary — some return a tuple, some need a buffer
        try:
            msg = self._can.recv(timeout=timeout_ms)
        except Exception:
            return None
        if msg is None:
            return None
        # Common return shape: (id, is_extended, is_rtr, fmi, data)
        if isinstance(msg, tuple) and len(msg) >= 5:
            return msg[0], bytes(msg[4])
        # Fallback: object with .id and .data
        if hasattr(msg, "id") and hasattr(msg, "data"):
            return msg.id, bytes(msg.data)
        return None

    def deinit(self):
        if self._can is not None and hasattr(self._can, "deinit"):
            self._can.deinit()


def query_pid(can, pid, mode=0x01, timeout_ms=500):
    """
    Send a Mode-01 PID request and return the first matching response frame.

    Uses lib.obd2 to encode/decode. Returns the parsed dict from obd2.parse_response,
    or None on timeout / non-matching response.
    """
    from lib import obd2
    req = obd2.query(pid, mode=mode)
    can.send(OBD_REQUEST_BROADCAST, req)

    # Drain responses until we see one matching our PID or timeout expires
    deadline_ms = (
        time.ticks_ms() + timeout_ms if hasattr(time, "ticks_ms") else timeout_ms
    )
    while True:
        msg = can.receive(timeout_ms=min(100, timeout_ms))
        if msg is None:
            if hasattr(time, "ticks_ms") and time.ticks_diff(deadline_ms, time.ticks_ms()) <= 0:
                return None
            continue
        rx_id, data = msg
        if not (OBD_RESPONSE_RANGE[0] <= rx_id <= OBD_RESPONSE_RANGE[1]):
            continue
        try:
            parsed = obd2.parse_response(data)
        except Exception:
            continue
        if parsed.get("pid") == pid:
            return parsed
