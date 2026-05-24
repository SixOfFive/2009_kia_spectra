"""
UART link from ESP32 to Raspberry Pi.

Wire protocol: line-delimited JSON over UART at 115200 baud, 8N1.
One JSON object per line, terminated by '\\n'. Each object has at minimum:
    {"type": "STATUS"|"OBD"|"EVENT"|"COMMAND"|"ACK"|"LOG", "ts": <ms>, ...}

The "ts" field is the ESP32's monotonic millisecond clock since boot,
useful for correlating events when the Pi's clock isn't reliable yet.

Message types
-------------

STATUS  — periodic state update from ESP32 to Pi
    {"type": "STATUS", "ts": 12345, "v_battery": 12.3, "state": "idle",
     "rssi_wifi": -65, "uptime_s": 3600}

OBD     — single OBD-II PID reading
    {"type": "OBD", "ts": 12345, "pid": 12, "name": "rpm",
     "value": 1726.0, "units": "rpm"}

EVENT   — discrete event (engine started, stopped, low-V trigger fired)
    {"type": "EVENT", "ts": 12345, "event": "engine_started",
     "detail": {...}}

COMMAND — Pi -> ESP32 directive (manual start, manual stop, set threshold)
    {"type": "COMMAND", "ts": 12345, "cmd": "start_engine"}
    {"type": "COMMAND", "ts": 12345, "cmd": "set_threshold", "value": 12.2}
    {"type": "COMMAND", "ts": 12345, "cmd": "shutdown_pi"}

ACK     — acknowledgement of a received command
    {"type": "ACK", "ts": 12345, "in_reply_to": "start_engine",
     "ok": true, "detail": "..."}

LOG     — free-form log line (debug / info / warn / error)
    {"type": "LOG", "ts": 12345, "level": "info", "msg": "..."}

This module runs on the ESP32 in MicroPython. The Pi-side counterpart
lives at pi/app/comms/esp32_link.py and implements the same schema.
"""

import json


# Message type constants — use these instead of string literals so typos
# get caught at import time rather than at runtime.
TYPE_STATUS = "STATUS"
TYPE_OBD = "OBD"
TYPE_EVENT = "EVENT"
TYPE_COMMAND = "COMMAND"
TYPE_ACK = "ACK"
TYPE_LOG = "LOG"

# Common command values
CMD_START_ENGINE = "start_engine"
CMD_STOP_ENGINE = "stop_engine"
CMD_SET_THRESHOLD = "set_threshold"
CMD_SHUTDOWN_PI = "shutdown_pi"
CMD_PING = "ping"

# Common event names
EVENT_ENGINE_STARTED = "engine_started"
EVENT_ENGINE_STOPPED = "engine_stopped"
EVENT_LOW_VOLTAGE_TRIGGER = "low_voltage_trigger"
EVENT_PI_BOOT = "pi_boot"
EVENT_PI_SHUTDOWN_REQUEST = "pi_shutdown_request"
EVENT_KEELOQ_TX = "keeloq_tx"
EVENT_KEELOQ_TX_FAIL = "keeloq_tx_fail"


def _now_ms():
    """Monotonic milliseconds since boot, MicroPython-compatible."""
    try:
        # MicroPython on ESP32
        import utime
        return utime.ticks_ms()
    except ImportError:
        # CPython fallback for desktop testing
        import time
        return int(time.monotonic() * 1000)


def encode(msg):
    """
    Encode a message dict as a UTF-8 bytes line ready to write to UART.

    :param msg: dict with at minimum a "type" key
    :return: bytes ending in b'\\n'
    """
    if "ts" not in msg:
        msg["ts"] = _now_ms()
    return (json.dumps(msg) + "\n").encode("utf-8")


def decode(line):
    """
    Decode a single JSON line into a dict.

    :param line: bytes or str (with or without trailing newline)
    :return: dict, or None if the line is empty / malformed
    """
    if isinstance(line, bytes):
        try:
            line = line.decode("utf-8")
        except UnicodeDecodeError:
            return None
    line = line.strip()
    if not line:
        return None
    try:
        msg = json.loads(line)
    except (ValueError, TypeError):
        return None
    if not isinstance(msg, dict) or "type" not in msg:
        return None
    return msg


# ----- Convenience constructors -----

def status(v_battery, state, **extra):
    return {
        "type": TYPE_STATUS,
        "ts": _now_ms(),
        "v_battery": v_battery,
        "state": state,
        **extra,
    }


def obd(pid, name, value, units):
    return {
        "type": TYPE_OBD,
        "ts": _now_ms(),
        "pid": pid,
        "name": name,
        "value": value,
        "units": units,
    }


def event(name, **detail):
    return {
        "type": TYPE_EVENT,
        "ts": _now_ms(),
        "event": name,
        "detail": detail,
    }


def command(cmd, **kwargs):
    return {
        "type": TYPE_COMMAND,
        "ts": _now_ms(),
        "cmd": cmd,
        **kwargs,
    }


def ack(in_reply_to, ok=True, detail=None):
    return {
        "type": TYPE_ACK,
        "ts": _now_ms(),
        "in_reply_to": in_reply_to,
        "ok": ok,
        "detail": detail,
    }


def log(level, msg):
    return {
        "type": TYPE_LOG,
        "ts": _now_ms(),
        "level": level,
        "msg": msg,
    }


# ----- UART runtime helpers (ESP32 only — guarded for desktop testing) -----

class UartLink:
    """
    Wraps a MicroPython UART for line-based JSON message exchange.

    Usage (on ESP32):
        from machine import UART
        from lib import pi_link
        uart = UART(1, baudrate=115200, tx=17, rx=16)
        link = pi_link.UartLink(uart)
        link.send(pi_link.status(12.6, "idle"))
        msg = link.recv()  # returns dict or None if no full line yet
    """

    def __init__(self, uart):
        self._uart = uart
        self._buf = bytearray()

    def send(self, msg):
        self._uart.write(encode(msg))

    def recv(self):
        """
        Return one decoded message, or None if no complete line is available.
        Non-blocking — call repeatedly from main loop.
        """
        data = self._uart.read()
        if data:
            self._buf.extend(data)
        nl = self._buf.find(b"\n")
        if nl < 0:
            return None
        line = bytes(self._buf[:nl])
        del self._buf[:nl + 1]
        return decode(line)
