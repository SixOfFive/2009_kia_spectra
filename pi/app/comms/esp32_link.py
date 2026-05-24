"""
Raspberry Pi side of the UART link to the ESP32.

Mirrors the message schema defined in esp32/src/lib/pi_link.py. Keep the
two files in sync — they must speak the same protocol.

Wire protocol: line-delimited JSON over UART at 115200 baud, 8N1.

On a Pi Zero 2 W with the primary UART enabled (raspi-config -> Interface
Options -> Serial Port -> No login shell, Yes serial hardware), the port
is /dev/ttyAMA0 (or /dev/serial0 alias). Wire ESP32 TX to Pi RX (GPIO15)
and Pi TX (GPIO14) to ESP32 RX. Don't forget a common ground.

Typical usage:

    from pi.app.comms.esp32_link import Esp32Link, command, CMD_START_ENGINE

    link = Esp32Link("/dev/serial0", baudrate=115200)
    link.send(command(CMD_START_ENGINE))
    for msg in link.iter_messages(timeout=5.0):
        if msg["type"] == "ACK" and msg.get("in_reply_to") == CMD_START_ENGINE:
            print("ESP32 acked the start request")
            break
"""

import json
import time


# Message type constants — kept in sync with esp32/src/lib/pi_link.py
TYPE_STATUS = "STATUS"
TYPE_OBD = "OBD"
TYPE_EVENT = "EVENT"
TYPE_COMMAND = "COMMAND"
TYPE_ACK = "ACK"
TYPE_LOG = "LOG"

CMD_START_ENGINE = "start_engine"
CMD_STOP_ENGINE = "stop_engine"
CMD_SET_THRESHOLD = "set_threshold"
CMD_SHUTDOWN_PI = "shutdown_pi"
CMD_PING = "ping"

EVENT_ENGINE_STARTED = "engine_started"
EVENT_ENGINE_STOPPED = "engine_stopped"
EVENT_LOW_VOLTAGE_TRIGGER = "low_voltage_trigger"
EVENT_PI_BOOT = "pi_boot"
EVENT_PI_SHUTDOWN_REQUEST = "pi_shutdown_request"
EVENT_KEELOQ_TX = "keeloq_tx"
EVENT_KEELOQ_TX_FAIL = "keeloq_tx_fail"


def _now_ms():
    return int(time.monotonic() * 1000)


# ----- Encoding / decoding (mirror of pi_link.py) -----

def encode(msg):
    if "ts" not in msg:
        msg["ts"] = _now_ms()
    return (json.dumps(msg) + "\n").encode("utf-8")


def decode(line):
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
    return {"type": TYPE_STATUS, "ts": _now_ms(),
            "v_battery": v_battery, "state": state, **extra}


def obd(pid, name, value, units):
    return {"type": TYPE_OBD, "ts": _now_ms(),
            "pid": pid, "name": name, "value": value, "units": units}


def event(name, **detail):
    return {"type": TYPE_EVENT, "ts": _now_ms(), "event": name, "detail": detail}


def command(cmd, **kwargs):
    return {"type": TYPE_COMMAND, "ts": _now_ms(), "cmd": cmd, **kwargs}


def ack(in_reply_to, ok=True, detail=None):
    return {"type": TYPE_ACK, "ts": _now_ms(),
            "in_reply_to": in_reply_to, "ok": ok, "detail": detail}


def log(level, msg):
    return {"type": TYPE_LOG, "ts": _now_ms(), "level": level, "msg": msg}


# ----- Runtime helper -----

class Esp32Link:
    """
    Threadsafe-friendly wrapper around pyserial for line-based JSON.

    The serial port is opened lazily so the class can be instantiated in
    tests without hardware. ``port`` may also be a file-like object for
    tests / mocking — anything with ``.read(n)`` and ``.write(bytes)``.
    """

    def __init__(self, port, baudrate=115200):
        self._port_arg = port
        self._baudrate = baudrate
        self._serial = None
        self._buf = bytearray()

    def _open(self):
        if self._serial is not None:
            return self._serial
        # If port is already a file-like (for tests), just use it directly.
        if hasattr(self._port_arg, "read") and hasattr(self._port_arg, "write"):
            self._serial = self._port_arg
        else:
            # Lazy import so tests don't require pyserial.
            import serial
            self._serial = serial.Serial(
                self._port_arg,
                baudrate=self._baudrate,
                timeout=0,  # non-blocking
            )
        return self._serial

    def send(self, msg):
        self._open().write(encode(msg))

    def recv(self):
        """
        Return one decoded message, or None if no complete line is buffered.
        Non-blocking. Call repeatedly from a polling loop.
        """
        s = self._open()
        chunk = s.read(4096)
        if chunk:
            self._buf.extend(chunk)
        nl = self._buf.find(b"\n")
        if nl < 0:
            return None
        line = bytes(self._buf[:nl])
        del self._buf[:nl + 1]
        return decode(line)

    def iter_messages(self, timeout=None, poll_interval=0.02):
        """
        Generator yielding messages as they arrive. Stops after ``timeout``
        seconds of inactivity (or runs forever if timeout is None).
        """
        deadline = None if timeout is None else time.monotonic() + timeout
        while deadline is None or time.monotonic() < deadline:
            msg = self.recv()
            if msg is not None:
                yield msg
                # Reset the deadline on successful receive
                if timeout is not None:
                    deadline = time.monotonic() + timeout
            else:
                time.sleep(poll_interval)

    def close(self):
        if self._serial is not None and hasattr(self._serial, "close"):
            self._serial.close()
        self._serial = None
