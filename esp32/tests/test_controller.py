"""Controller state-machine tests — pure logic, no hardware required."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "src"))

from controller import VroomController, State  # noqa: E402
from lib import compustar  # noqa: E402


# Synthetic 35-bit patterns for tests. Not real FOB data — the controller
# only cares that the patterns parse + render to pulses; the radio is mocked.
TEST_PACKETS = {
    "START":  "0" * 35,
    "LOCK":   "1" * 35,
    "UNLOCK": "01" * 17 + "0",
    "TRUNK":  "10" * 17 + "1",
}


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
    ADC_DRIFT_WARN_V = 1.0
    LOW_V_TRIGGER = 12.2
    LOW_V_SUSTAIN_S = 300
    RUN_DURATION_S = 900
    WAKE_INTERVAL_S = 60
    START_COOLDOWN_S = 7200
    RF_FREQUENCY_HZ = 433_920_000
    RF_BURST_REPEATS = 8
    RF_GUARD_MS = 39
    OBD_POLL_INTERVAL_S = 1
    OBD_POLL_PIDS = ()
    PI_SHUTDOWN_GRACE_S = 30
    # Captured 35-bit packets (synthetic for tests)
    COMPUSTAR_REMOTE_ID = 0xABCD
    COMPUSTAR_PACKETS = TEST_PACKETS


def _make_ctrl(voltage=12.8):
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
    assert radio.bursts[0]["repeats"] == 8
    # Streak reset after trigger (otherwise we'd immediately re-trigger after cooldown)
    assert ctrl.low_v_count == 0
    events = [m for m in uart.sent if m["type"] == "EVENT"]
    event_names = [m["event"] for m in events]
    assert "low_voltage_trigger" in event_names
    assert "compustar_tx" in event_names


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


def test_transmit_button_renders_correct_pulse_count():
    """The Start packet should produce 3 sync + 35 data = 38 pulses."""
    ctrl, _, radio, _, _ = _make_ctrl()
    ctrl._transmit_button(compustar.Button.START)
    assert len(radio.bursts) == 1
    pulses = radio.bursts[0]["pulses"]
    assert len(pulses) == compustar.SYNC_COUNT + compustar.PACKET_BITS


def test_transmit_button_missing_packet_fails_gracefully():
    """No COMPUSTAR_PACKETS → emit failure event, don't crash."""
    ctrl, _, radio, uart, _ = _make_ctrl()
    ctrl.cfg.COMPUSTAR_PACKETS = None
    result = ctrl._transmit_button(compustar.Button.START)
    assert result is False
    assert len(radio.bursts) == 0
    fail_events = [m for m in uart.sent
                   if m["type"] == "EVENT" and m["event"] == "compustar_tx_fail"]
    assert len(fail_events) == 1


def test_transmit_button_malformed_pattern_fails_gracefully():
    """Wrong-length pattern in packets dict → failure event, no crash."""
    ctrl, _, radio, uart, _ = _make_ctrl()
    ctrl.cfg.COMPUSTAR_PACKETS = dict(TEST_PACKETS)
    ctrl.cfg.COMPUSTAR_PACKETS["START"] = "01010"  # 5 bits, expected 35
    result = ctrl._transmit_button(compustar.Button.START)
    assert result is False
    assert len(radio.bursts) == 0
    fail_events = [m for m in uart.sent
                   if m["type"] == "EVENT" and m["event"] == "compustar_tx_fail"]
    assert len(fail_events) == 1


def test_compustar_tx_event_has_button_in_detail():
    ctrl, _, _, uart, _ = _make_ctrl()
    ctrl._transmit_button(compustar.Button.START)
    tx_event = next(m for m in uart.sent
                    if m["type"] == "EVENT" and m["event"] == "compustar_tx")
    assert tx_event["detail"]["button"] == "START"


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


def test_wdt_is_none_in_cpython():
    ctrl, *_ = _make_ctrl()
    assert ctrl.wdt is None        # CPython has no machine.WDT


def test_wdt_feed_is_called_each_tick():
    ctrl, *_ = _make_ctrl()
    fed = {"count": 0}
    class FakeWDT:
        def feed(self): fed["count"] += 1
    ctrl.wdt = FakeWDT()
    ctrl.tick()
    ctrl.tick()
    ctrl.tick()
    assert fed["count"] == 3


def test_adc_drift_above_threshold_emits_warn():
    """A >1V jump between successive samples should emit a warn LOG event."""
    ctrl, adc, _, uart, _ = _make_ctrl(voltage=12.5 / 4.0)
    ctrl._monitor_step()                  # first sample: prev was None, no warn
    warns_before = [m for m in uart.sent
                    if m["type"] == "LOG" and m.get("level") == "warn"]
    assert warns_before == []
    # Jump from 12.5 V to 10.2 V (delta = -2.3 V, exceeds 1.0 threshold)
    adc.voltage = 10.2 / 4.0
    ctrl._monitor_step()
    drift_warns = [m for m in uart.sent
                   if m["type"] == "LOG"
                   and m.get("level") == "warn"
                   and "ADC drift" in m.get("msg", "")]
    assert len(drift_warns) == 1
    # The trigger logic still runs — state machine isn't short-circuited.
    # First sample 12.5 V is above the 12.2 V trigger so streak stayed 0.
    # Second sample 10.2 V is below trigger so streak advanced to 1.
    assert ctrl.state == State.MONITORING
    assert ctrl.low_v_count == 1


def test_adc_drift_below_threshold_does_not_warn():
    """A <1V jump should NOT emit a drift warn — normal cabling slop."""
    ctrl, adc, _, uart, _ = _make_ctrl(voltage=12.5 / 4.0)
    ctrl._monitor_step()                  # first sample
    adc.voltage = 12.0 / 4.0              # 0.5 V delta — well under threshold
    ctrl._monitor_step()
    adc.voltage = 12.8 / 4.0              # 0.8 V delta from previous — still under
    ctrl._monitor_step()
    drift_warns = [m for m in uart.sent
                   if m["type"] == "LOG"
                   and m.get("level") == "warn"
                   and "ADC drift" in m.get("msg", "")]
    assert drift_warns == []


def test_adc_drift_first_sample_after_boot_does_not_warn():
    """No previous reading exists on first sample — must not false-positive."""
    ctrl, _, _, uart, _ = _make_ctrl(voltage=12.5 / 4.0)
    assert ctrl.last_voltage is None
    ctrl._monitor_step()                  # the very first sample
    drift_warns = [m for m in uart.sent
                   if m["type"] == "LOG"
                   and m.get("level") == "warn"
                   and "ADC drift" in m.get("msg", "")]
    assert drift_warns == []


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
