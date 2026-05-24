"""
End-to-end integration tests for the ESP32 ↔ Pi protocol.

Creates a paired in-memory "UART" pipe with two endpoints — one wrapped
in the ESP32-side ``pi_link.UartLink``, the other in the Pi-side
``esp32_link.Esp32Link``. Drives the controller through realistic
scenarios and asserts the Pi side decodes messages with the expected
field names and types.

This is the test that catches:
  - constant name drift (e.g. CMD_START_ENGINE rename on one side only)
  - wire-format drift (different JSON structure between modules)
  - field name drift (e.g. ``cmd`` vs ``command``)
"""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
# ESP32 src for controller + pi_link
sys.path.insert(0, os.path.join(HERE, "..", "src"))
# Project root for pi.app.* imports
sys.path.insert(0, os.path.join(HERE, "..", ".."))

from controller import VroomController  # noqa: E402
import controller as ctrl_mod          # noqa: E402
from lib.pi_link import UartLink         # noqa: E402
from pi.app.comms.esp32_link import Esp32Link  # noqa: E402
from pi.app.comms import uart_listener   # noqa: E402
from pi.app.comms import esp32_link as pi_side  # noqa: E402
from pi.app import state as pi_state     # noqa: E402

# Redirect counter file to tmp so each test gets a clean slate
ctrl_mod.COUNTER_FILE = os.path.join(tempfile.gettempdir(), "test_integration_counter.json")


class FakeUartPort:
    """Half of a bidirectional in-memory UART pipe."""
    def __init__(self, read_buf, write_buf):
        self._read_buf = read_buf
        self._write_buf = write_buf

    def write(self, data):
        self._write_buf.extend(data)

    def read(self, n=None):
        if n is None or n >= len(self._read_buf):
            data = bytes(self._read_buf)
            del self._read_buf[:]
        else:
            data = bytes(self._read_buf[:n])
            del self._read_buf[:n]
        return data

    def close(self):
        pass


def make_pair():
    """Return (esp32_endpoint, pi_endpoint), each suitable for the matching link wrapper."""
    a_to_b = bytearray()
    b_to_a = bytearray()
    esp32_side = FakeUartPort(read_buf=b_to_a, write_buf=a_to_b)
    pi_endpoint = FakeUartPort(read_buf=a_to_b, write_buf=b_to_a)
    return esp32_side, pi_endpoint


class FakePin:
    def __init__(self): self._v = 0
    def on(self): self._v = 1
    def off(self): self._v = 0
    def value(self, v=None):
        if v is None: return self._v
        self._v = int(v)


class FakeAdc:
    def __init__(self, voltage=12.8):
        self.voltage = voltage
    def read_voltage(self, *a, **kw):
        return self.voltage


class FakeRadio:
    def __init__(self):
        self.bursts = []
    def transmit_burst(self, pulses, repeats, guard_ms):
        self.bursts.append((len(pulses), repeats, guard_ms))


class Config:
    ADC_DIVIDER_RATIO = 4.0
    ADC_CALIBRATION_OFFSET = 0.0
    LOW_V_TRIGGER = 12.2
    LOW_V_SUSTAIN_S = 300
    RUN_DURATION_S = 900
    WAKE_INTERVAL_S = 60
    START_COOLDOWN_S = 7200
    RF_TE_US = 400
    RF_BURST_REPEATS = 4
    RF_GUARD_MS = 39
    OBD_POLL_INTERVAL_S = 1
    OBD_POLL_PIDS = ()
    PI_SHUTDOWN_GRACE_S = 30
    COMPUSTAR_DEVICE_KEY = 0xDEADBEEFCAFEBABE
    COMPUSTAR_SERIAL = 0x0ABCDE1
    COMPUSTAR_COUNTER = 100


def _reset_pi_state():
    with pi_state.STATE_LOCK:
        pi_state.STATE.clear()
        pi_state.STATE.update({
            "v_battery": None, "engine_running": False,
            "esp32_state": "unknown", "last_obd": {}, "events": [],
        })


def _build():
    # Clean up counter file from prior tests
    try:
        os.remove(ctrl_mod.COUNTER_FILE)
    except OSError:
        pass
    esp_uart, pi_uart = make_pair()
    esp_link = UartLink(esp_uart)
    pi_link = Esp32Link(pi_uart)
    adc = FakeAdc(voltage=12.8)
    radio = FakeRadio()
    pi_power = FakePin()
    ctrl = VroomController(adc, radio, None, esp_link, pi_power, Config())
    _reset_pi_state()
    return ctrl, esp_link, pi_link, adc, radio, pi_power


def _cleanup_counter_file():
    try:
        os.remove(ctrl_mod.COUNTER_FILE)
    except OSError:
        pass


# ---------- Tests ----------

def test_esp32_status_message_decodes_on_pi_side():
    ctrl, esp_link, pi_link, _, _, _ = _build()
    # Trigger a monitor sample → ESP32 emits a STATUS message
    ctrl._monitor_step()
    msg = pi_link.recv()
    assert msg is not None
    assert msg["type"] == "STATUS"
    assert "v_battery" in msg
    assert msg["state"] == "monitoring"
    assert msg["counter"] == 100


def test_status_message_flows_into_pi_state():
    ctrl, esp_link, pi_link, _, _, _ = _build()
    ctrl._monitor_step()
    msg = pi_link.recv()
    uart_listener.dispatch(msg)
    snap = pi_state.snapshot()
    assert abs(snap["v_battery"] - 12.8 * 4.0) < 0.01
    assert snap["esp32_state"] == "monitoring"


def test_pi_command_decodes_on_esp32_side_and_acks():
    ctrl, esp_link, pi_link, _, radio, _ = _build()
    pi_link.send(pi_side.command(pi_side.CMD_PING))
    ctrl._handle_uart()
    # ESP32 should have written back an ACK
    ack_msg = pi_link.recv()
    assert ack_msg is not None
    assert ack_msg["type"] == "ACK"
    assert ack_msg["in_reply_to"] == "ping"
    assert ack_msg["ok"] is True


def test_pi_start_engine_triggers_full_path_and_emits_event():
    ctrl, esp_link, pi_link, _, radio, pi_power = _build()
    pi_link.send(pi_side.command(pi_side.CMD_START_ENGINE))
    ctrl._handle_uart()

    # Drain all messages the ESP32 emitted
    received = []
    while True:
        m = pi_link.recv()
        if m is None:
            break
        received.append(m)

    types = [m["type"] for m in received]
    assert "EVENT" in types, f"expected EVENT in {types}"
    assert "ACK" in types
    events = [m for m in received if m["type"] == "EVENT"]
    event_names = [m["event"] for m in events]
    assert "low_voltage_trigger" in event_names
    assert "keeloq_tx" in event_names
    # Controller should have actually transmitted (since secrets are populated)
    assert len(radio.bursts) == 1
    assert pi_power.value() == 0  # Pi powered on


def test_keeloq_tx_event_has_function_and_counter_in_detail():
    ctrl, esp_link, pi_link, _, _, _ = _build()
    from lib import compustar
    ctrl._transmit_function(compustar.Function.START, "START")
    # Drain
    keeloq_event = None
    while True:
        m = pi_link.recv()
        if m is None: break
        if m.get("type") == "EVENT" and m.get("event") == "keeloq_tx":
            keeloq_event = m
            break
    assert keeloq_event is not None
    detail = keeloq_event["detail"]
    assert detail["function"] == "START"
    assert detail["counter"] == 101  # counter was 100, bumped before tx


def test_obd_message_round_trips_with_correct_fields():
    ctrl, esp_link, pi_link, _, _, _ = _build()
    # Manually send an OBD message from the ESP32 side
    from lib import pi_link as esp32_pi_link
    esp_link.send(esp32_pi_link.obd(0x0C, "rpm", 1726.0, "rpm"))
    msg = pi_link.recv()
    assert msg["type"] == "OBD"
    assert msg["pid"] == 0x0C
    assert msg["name"] == "rpm"
    assert msg["value"] == 1726.0
    assert msg["units"] == "rpm"
    # And it dispatches into STATE
    uart_listener.dispatch(msg)
    snap = pi_state.snapshot()
    assert snap["last_obd"]["rpm"] == 1726.0


def test_message_type_constants_match_between_sides():
    from lib import pi_link as esp_side
    for name in ("TYPE_STATUS", "TYPE_OBD", "TYPE_EVENT",
                 "TYPE_COMMAND", "TYPE_ACK", "TYPE_LOG"):
        assert getattr(esp_side, name) == getattr(pi_side, name), \
            f"{name} differs between esp32 and pi pi_link/esp32_link modules"


def test_command_constants_match_between_sides():
    from lib import pi_link as esp_side
    for name in ("CMD_START_ENGINE", "CMD_STOP_ENGINE",
                 "CMD_SET_THRESHOLD", "CMD_SHUTDOWN_PI", "CMD_PING"):
        assert getattr(esp_side, name) == getattr(pi_side, name), \
            f"{name} differs between esp32 and pi pi_link/esp32_link modules"


def test_event_constants_match_between_sides():
    from lib import pi_link as esp_side
    for name in ("EVENT_ENGINE_STARTED", "EVENT_ENGINE_STOPPED",
                 "EVENT_LOW_VOLTAGE_TRIGGER", "EVENT_PI_BOOT",
                 "EVENT_PI_SHUTDOWN_REQUEST", "EVENT_KEELOQ_TX",
                 "EVENT_KEELOQ_TX_FAIL"):
        assert getattr(esp_side, name) == getattr(pi_side, name), \
            f"{name} differs between esp32 and pi pi_link/esp32_link modules"


def run():
    tests = [v for k, v in globals().items() if k.startswith("test_") and callable(v)]
    for t in tests:
        t()
        print(f"PASS {t.__name__}")
    print(f"\n{len(tests)}/{len(tests)} integration tests passed")


if __name__ == "__main__":
    run()
