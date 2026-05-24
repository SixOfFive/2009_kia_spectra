"""Controller state-machine tests — pure logic, no hardware required."""
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "src"))

# Force controller.py into its CPython branch and ensure no leftover counter file
COUNTER_FILE_PATH = os.path.join(tempfile.gettempdir(), "test_compustar_counter.json")
try:
    os.remove(COUNTER_FILE_PATH)
except OSError:
    pass

from controller import VroomController, State  # noqa: E402
import controller as ctrl_mod  # noqa: E402
ctrl_mod.COUNTER_FILE = COUNTER_FILE_PATH


class FakePin:
    def __init__(self):
        self._v = 0
    def on(self): self._v = 1
    def off(self): self._v = 0
    def value(self, v=None):
        if v is None: return self._v
        self._v = int(v)


class FakeUart:
    def __init__(self):
        self.sent = []
        self.inbox = []
    def send(self, msg):
        self.sent.append(msg)
    def recv(self):
        return self.inbox.pop(0) if self.inbox else None


class FakeAdc:
    def __init__(self, voltage=12.8):
        self.voltage = voltage
        self.reads = 0
    def read_voltage(self, *a, **kw):
        self.reads += 1
        # Returns post-divider value; controller multiplies by ADC_DIVIDER_RATIO
        return self.voltage


class FakeRadio:
    def __init__(self):
        self.bursts = []
    def transmit_burst(self, pulses, repeats, guard_ms):
        self.bursts.append({"pulses": pulses, "repeats": repeats, "guard_ms": guard_ms})


class Config:
    """Duck-typed config namespace for tests."""
    ADC_DIVIDER_RATIO = 4.0
    ADC_CALIBRATION_OFFSET = 0.0
    LOW_V_TRIGGER = 12.2
    LOW_V_SUSTAIN_S = 300
    RUN_DURATION_S = 900
    WAKE_INTERVAL_S = 60
    START_COOLDOWN_S = 7200
    RF_FREQUENCY_HZ = 433_920_000
    RF_TE_US = 400
    RF_BURST_REPEATS = 4
    RF_GUARD_MS = 39
    OBD_POLL_INTERVAL_S = 1
    OBD_POLL_PIDS = ()
    PI_SHUTDOWN_GRACE_S = 30
    # Secrets — supplied so transmit doesn't bail
    COMPUSTAR_DEVICE_KEY = 0xDEADBEEFCAFEBABE
    COMPUSTAR_SERIAL = 0x0ABCDE1
    COMPUSTAR_COUNTER = 100


def _make_ctrl(voltage=12.8):
    # Clean counter file for each test
    try:
        os.remove(COUNTER_FILE_PATH)
    except OSError:
        pass
    adc = FakeAdc(voltage=voltage)
    radio = FakeRadio()
    uart = FakeUart()
    pi_power = FakePin()
    ctrl = VroomController(adc, radio, None, uart, pi_power, Config())
    return ctrl, adc, radio, uart, pi_power


def test_initial_state_is_monitoring_and_pi_off():
    ctrl, _, _, _, pi_power = _make_ctrl()
    assert ctrl.state == State.MONITORING
    assert pi_power.value() == 1   # MOSFET off → Pi off


def test_normal_voltage_does_not_trigger():
    ctrl, _, radio, _, _ = _make_ctrl(voltage=13.0 / 4.0)  # 13V battery / 4:1 divider
    ctrl._monitor_step()
    assert ctrl.state == State.MONITORING
    assert ctrl.low_v_count == 0
    assert len(radio.bursts) == 0


def test_normal_voltage_resets_streak():
    ctrl, adc, _, _, _ = _make_ctrl(voltage=12.0 / 4.0)
    # Accumulate a partial streak
    for _ in range(3):
        ctrl._monitor_step()
    assert ctrl.low_v_count == 3
    # Now voltage recovers
    adc.voltage = 13.5 / 4.0
    ctrl._monitor_step()
    assert ctrl.low_v_count == 0
    assert ctrl.state == State.MONITORING


def test_sustained_low_voltage_triggers_start():
    # LOW_V_SUSTAIN_S / WAKE_INTERVAL_S = 300 / 60 = 5 samples needed
    ctrl, _, radio, uart, pi_power = _make_ctrl(voltage=12.0 / 4.0)
    samples_needed = ctrl._samples_needed()
    assert samples_needed == 5
    for i in range(samples_needed):
        ctrl._monitor_step()
        if ctrl.state == State.STARTING:
            break
    assert ctrl.state == State.STARTING
    assert pi_power.value() == 0       # Pi powered on
    assert len(radio.bursts) == 1
    assert radio.bursts[0]["repeats"] == 4
    # Streak reset after trigger (otherwise we'd immediately re-trigger after cooldown)
    assert ctrl.low_v_count == 0
    events = [m for m in uart.sent if m["type"] == "EVENT"]
    event_names = [m["event"] for m in events]
    assert "low_voltage_trigger" in event_names
    assert "keeloq_tx" in event_names


def test_one_short_low_then_recovery_does_not_trigger():
    ctrl, adc, radio, _, _ = _make_ctrl(voltage=12.0 / 4.0)
    ctrl._monitor_step()                   # 1 low sample
    adc.voltage = 13.0 / 4.0
    for _ in range(10):                    # 10 high samples — streak stays 0
        ctrl._monitor_step()
    assert ctrl.state == State.MONITORING
    assert ctrl.low_v_count == 0
    assert len(radio.bursts) == 0


def test_cooldown_clears_streak_and_returns_to_monitoring():
    ctrl, _, _, _, _ = _make_ctrl()
    ctrl.state = State.COOLDOWN
    ctrl.last_trigger_ts = -ctrl.cfg.START_COOLDOWN_S - 1   # cooldown already elapsed
    ctrl.low_v_count = 99   # stale
    ctrl._cooldown_step()
    assert ctrl.state == State.MONITORING
    assert ctrl.low_v_count == 0


def test_counter_increments_and_persists():
    ctrl, _, radio, _, _ = _make_ctrl()
    initial_counter = ctrl.counter
    from lib import compustar
    ctrl._transmit_function(compustar.Function.START, "START")
    assert ctrl.counter == initial_counter + 1

    # New controller should load the bumped counter from flash
    ctrl2, *_ = _make_ctrl()
    # Note: _make_ctrl removes the counter file. To test persistence, manually save.
    ctrl.counter = 5555
    ctrl._save_counter()
    import json
    with open(COUNTER_FILE_PATH) as f:
        data = json.load(f)
    assert data["counter"] == 5555


def test_uart_start_command_triggers_start():
    ctrl, _, radio, uart, _ = _make_ctrl()
    uart.inbox.append({"type": "COMMAND", "cmd": "start_engine"})
    ctrl._handle_uart()
    assert ctrl.state == State.STARTING
    assert len(radio.bursts) == 1
    # ACK was sent
    acks = [m for m in uart.sent if m["type"] == "ACK"]
    assert len(acks) == 1
    assert acks[0]["in_reply_to"] == "start_engine"


def test_uart_ping_acks_with_state():
    ctrl, _, _, uart, _ = _make_ctrl()
    uart.inbox.append({"type": "COMMAND", "cmd": "ping"})
    ctrl._handle_uart()
    acks = [m for m in uart.sent if m["type"] == "ACK"]
    assert len(acks) == 1
    assert acks[0]["detail"] == "monitoring"


def test_set_threshold_command_updates_config():
    ctrl, _, _, uart, _ = _make_ctrl()
    uart.inbox.append({"type": "COMMAND", "cmd": "set_threshold", "value": 11.8})
    ctrl._handle_uart()
    assert ctrl.cfg.LOW_V_TRIGGER == 11.8


def test_stop_after_run_duration_powers_off_pi():
    ctrl, _, radio, uart, pi_power = _make_ctrl()
    # Manually advance into RUNNING state
    ctrl.state = State.RUNNING
    ctrl.run_start_ts = 0
    ctrl._now = lambda: ctrl.cfg.RUN_DURATION_S + 1
    ctrl._running_step()
    assert ctrl.state == State.STOPPING
    assert len(radio.bursts) == 1  # stop RF transmitted

    # Now run _stopping_step twice with time advancing past grace
    times = iter([0, ctrl.cfg.PI_SHUTDOWN_GRACE_S + 1])
    ctrl._now = lambda: next(times)
    ctrl._stopping_step()   # first call sends shutdown cmd, sets grace timer
    cmds = [m for m in uart.sent if m["type"] == "COMMAND"]
    assert any(m.get("cmd") == "shutdown_pi" for m in cmds)
    ctrl._stopping_step()   # second call: grace elapsed, MOSFET off
    assert pi_power.value() == 1   # Pi off
    assert ctrl.state == State.COOLDOWN


def test_unknown_command_acks_failure():
    ctrl, _, _, uart, _ = _make_ctrl()
    uart.inbox.append({"type": "COMMAND", "cmd": "fake_command"})
    ctrl._handle_uart()
    acks = [m for m in uart.sent if m["type"] == "ACK"]
    assert len(acks) == 1
    assert acks[0]["ok"] is False


def run():
    tests = [v for k, v in globals().items() if k.startswith("test_") and callable(v)]
    passed = 0
    for t in tests:
        t()
        passed += 1
        print(f"PASS {t.__name__}")
    print(f"\n{passed}/{len(tests)} controller tests passed")


if __name__ == "__main__":
    run()
