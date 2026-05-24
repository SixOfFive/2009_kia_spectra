"""
VroomController self-test — automated smoke-test of the controller's
low-voltage-trigger path with mocked hardware.

Where ``simulate.py`` is a developer-observation tool (prints messages,
no assertions), this script *asserts* that each transition happens
correctly and exits non-zero on the first failure. Use it after a
fresh firmware flash, after pulling new code, or as a CI sanity check.

Phases exercised (each prints one [PASS] / [FAIL] line):

  1. Import + instantiate controller with fake hardware
  2. Boot state is MONITORING with Pi power OFF
  3. Sustained low voltage transitions MONITORING -> STARTING
  4. STARTING transmits the correct button name with the configured
     repeat count, then advances to RUNNING
  5. Simulated time advance into RUNNING fires the stop transition
  6. STOPPING sends shutdown_pi, waits the grace window, cuts the
     MOSFET, advances to COOLDOWN
  7. COOLDOWN expires and the controller returns to MONITORING

Run from the repo root:

    python esp32/scripts/self_test.py

Exit code:
    0  all phases passed
    1  one or more phases failed (details printed)
"""
import os
import sys
import traceback


HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")
sys.path.insert(0, SRC)


# ----- Inline mock hardware (intentionally NOT imported from tests/) -----

class FakePin:
    """Mimics machine.Pin enough for the controller's active-low Pi MOSFET."""
    def __init__(self, label):
        self.label = label
        self._v = 0
    def on(self):
        self._v = 1
    def off(self):
        self._v = 0
    def value(self, v=None):
        if v is None:
            return self._v
        self._v = int(v)


class FakeAdc:
    """Returns whatever voltage the test sets on ``.voltage``."""
    def __init__(self, voltage=12.8):
        self.voltage = voltage
    def read_voltage(self, *_a, **_kw):
        return self.voltage


class FakeRadio:
    """Records every transmit_burst() call."""
    def __init__(self):
        self.bursts = []
    def transmit_burst(self, pulses, repeats, guard_ms):
        self.bursts.append({
            "pulses": pulses,
            "n_pulses": len(pulses),
            "repeats": repeats,
            "guard_ms": guard_ms,
        })


class CapturingUart:
    """Captures every message the controller emits; lets tests check semantics."""
    def __init__(self):
        self.sent = []
        self.inbox = []
    def send(self, msg):
        self.sent.append(msg)
    def recv(self):
        return self.inbox.pop(0) if self.inbox else None

    def events(self, name=None):
        out = [m for m in self.sent if m.get("type") == "EVENT"]
        if name:
            out = [m for m in out if m.get("event") == name]
        return out

    def commands(self, name=None):
        out = [m for m in self.sent if m.get("type") == "COMMAND"]
        if name:
            out = [m for m in out if m.get("cmd") == name]
        return out


# Per-button placeholder bit patterns — the controller uses
# build_pulses_for_button() to expand them into RF pulse pairs. Any
# 35-character "0"/"1" string is structurally valid.
_SELFTEST_PACKETS = {
    "START":  "0" * 35,
    "LOCK":   "1" * 35,
    "UNLOCK": "01" * 17 + "0",
    "TRUNK":  "10" * 17 + "1",
}


class Config:
    """Minimal config that lets the controller's state machine run fast."""
    ADC_DIVIDER_RATIO = 4.0
    ADC_CALIBRATION_OFFSET = 0.0
    LOW_V_TRIGGER = 12.2
    # Tight sustain + short wake so a handful of monitor steps trips it.
    LOW_V_SUSTAIN_S = 30
    WAKE_INTERVAL_S = 10
    RUN_DURATION_S = 60
    START_COOLDOWN_S = 30
    RF_BURST_REPEATS = 8
    RF_GUARD_MS = 39
    OBD_POLL_INTERVAL_S = 1
    OBD_POLL_PIDS = ()
    PI_SHUTDOWN_GRACE_S = 15
    WDT_ENABLED = False
    COMPUSTAR_REMOTE_ID = 0xABCD
    COMPUSTAR_PACKETS = _SELFTEST_PACKETS


# ----- Phase runner -----

class PhaseResult:
    def __init__(self, name):
        self.name = name
        self.ok = False
        self.detail = ""
        self.exc = None


def run_phase(name, fn, *args, **kwargs):
    res = PhaseResult(name)
    try:
        detail = fn(*args, **kwargs) or ""
        res.ok = True
        res.detail = detail
    except AssertionError as e:
        res.detail = str(e) or "assertion failed"
    except Exception as e:
        res.detail = "%s: %s" % (type(e).__name__, e)
        res.exc = traceback.format_exc()
    status = "[PASS]" if res.ok else "[FAIL]"
    msg = f"{status} {name}"
    if res.detail:
        msg += f"  — {res.detail}"
    print(msg)
    if res.exc:
        print(res.exc)
    return res


# ----- Phases -----

# Shared fixture, threaded through the phases by run_all() below.
_ctx = {}


def phase_1_instantiate():
    from controller import VroomController, State  # noqa: F401
    cfg = Config()
    adc = FakeAdc(voltage=13.0)
    radio = FakeRadio()
    uart = CapturingUart()
    pi_power = FakePin("pi_power")
    ctrl = VroomController(adc, radio, None, uart, pi_power, cfg)
    _ctx.update(
        ctrl=ctrl, cfg=cfg, adc=adc, radio=radio,
        uart=uart, pi_power=pi_power, State=State,
    )
    assert ctrl is not None, "controller failed to instantiate"
    return "controller built with mocked ADC/radio/UART/Pi-power pin"


def phase_2_initial_state():
    ctrl = _ctx["ctrl"]; State = _ctx["State"]; pi_power = _ctx["pi_power"]
    assert ctrl.state == State.MONITORING, \
        f"expected MONITORING, got {ctrl.state}"
    # active-low MOSFET: high = OFF
    assert pi_power.value() == 1, \
        f"expected Pi power OFF (pin high), got pin={pi_power.value()}"
    assert ctrl.low_v_count == 0, \
        f"expected low_v_count=0 at boot, got {ctrl.low_v_count}"
    return f"state={ctrl.state}, Pi power=OFF, streak=0"


def phase_3_low_voltage_triggers_start():
    ctrl = _ctx["ctrl"]; cfg = _ctx["cfg"]; adc = _ctx["adc"]; State = _ctx["State"]
    # Drop battery below threshold (ADC sees V/divider)
    low_v_at_adc = (cfg.LOW_V_TRIGGER - 0.5) / cfg.ADC_DIVIDER_RATIO
    adc.voltage = low_v_at_adc
    needed = ctrl._samples_needed()
    for i in range(needed):
        if ctrl.state != State.MONITORING:
            break
        ctrl._monitor_step()
    assert ctrl.state == State.STARTING, \
        f"after {needed} low samples, expected STARTING, got {ctrl.state}"
    # Pi should now be powered on
    assert _ctx["pi_power"].value() == 0, \
        f"expected Pi power ON after trigger, got pin={_ctx['pi_power'].value()}"
    return f"transitioned to STARTING after {needed} samples; Pi powered ON"


def phase_4_starting_transmits_and_runs():
    ctrl = _ctx["ctrl"]; cfg = _ctx["cfg"]; radio = _ctx["radio"]
    uart = _ctx["uart"]; State = _ctx["State"]
    # The Start RF actually transmits during _trigger_start (phase 3),
    # but _starting_step is what flips to RUNNING.
    assert len(radio.bursts) >= 1, \
        f"expected >=1 RF burst by STARTING, got {len(radio.bursts)}"
    last = radio.bursts[-1]
    assert last["repeats"] == cfg.RF_BURST_REPEATS, \
        f"expected RF_BURST_REPEATS={cfg.RF_BURST_REPEATS}, got {last['repeats']}"
    assert last["guard_ms"] == cfg.RF_GUARD_MS, \
        f"expected RF_GUARD_MS={cfg.RF_GUARD_MS}, got {last['guard_ms']}"
    # Confirm the controller emitted a compustar_tx event for START
    tx_events = uart.events("compustar_tx")
    assert tx_events, "expected at least one compustar_tx event"
    # pi_link.event() nests kwargs under "detail"
    detail = tx_events[-1].get("detail") or {}
    button_name = detail.get("button") or tx_events[-1].get("button")
    assert button_name == "START", \
        f"expected button=START, got {button_name!r}"
    # Now advance STARTING -> RUNNING
    ctrl._starting_step()
    assert ctrl.state == State.RUNNING, \
        f"expected RUNNING after _starting_step, got {ctrl.state}"
    started_events = uart.events("engine_started")
    assert started_events, "expected engine_started event after STARTING tick"
    return (f"RF: button=START, repeats={last['repeats']}, "
            f"guard={last['guard_ms']}ms; state=RUNNING")


def phase_5_running_to_stopping():
    ctrl = _ctx["ctrl"]; cfg = _ctx["cfg"]; State = _ctx["State"]
    radio = _ctx["radio"]
    bursts_before = len(radio.bursts)
    # Fast-forward the simulated clock past RUN_DURATION_S
    ctrl._now = lambda: cfg.RUN_DURATION_S + 1
    ctrl._running_step()
    assert ctrl.state == State.STOPPING, \
        f"expected STOPPING after run duration, got {ctrl.state}"
    # _trigger_stop must transmit another START packet (1WSHR-PRO uses
    # the same Start packet to stop the engine — see controller.py)
    assert len(radio.bursts) == bursts_before + 1, (
        f"expected one more RF burst (stop), got "
        f"{len(radio.bursts) - bursts_before}"
    )
    return "RUNNING -> STOPPING; stop RF transmitted"


def phase_6_stopping_to_cooldown():
    ctrl = _ctx["ctrl"]; cfg = _ctx["cfg"]; State = _ctx["State"]
    uart = _ctx["uart"]; pi_power = _ctx["pi_power"]
    # First _stopping_step: sends shutdown_pi and sets the grace deadline.
    # Second one (after grace elapses): cuts the MOSFET and moves to COOLDOWN.
    base_time = cfg.RUN_DURATION_S + 100
    times = iter([base_time, base_time + cfg.PI_SHUTDOWN_GRACE_S + 5])
    ctrl._now = lambda: next(times)
    ctrl._stopping_step()
    # shutdown_pi command emitted
    cmds = uart.commands("shutdown_pi")
    assert cmds, "expected shutdown_pi command after first STOPPING tick"
    # Grace not yet elapsed; should still be STOPPING
    assert ctrl.state == State.STOPPING, \
        f"expected still STOPPING within grace, got {ctrl.state}"
    ctrl._stopping_step()
    assert ctrl.state == State.COOLDOWN, \
        f"expected COOLDOWN after grace, got {ctrl.state}"
    assert pi_power.value() == 1, \
        f"expected Pi power OFF after grace (pin high), got pin={pi_power.value()}"
    stopped = uart.events("engine_stopped")
    assert stopped, "expected engine_stopped event after MOSFET cut"
    return "shutdown_pi sent; grace observed; MOSFET cut; state=COOLDOWN"


def phase_7_cooldown_releases():
    ctrl = _ctx["ctrl"]; cfg = _ctx["cfg"]; State = _ctx["State"]
    # Advance past START_COOLDOWN_S relative to the cooldown anchor
    # (last_trigger_ts was set when we entered COOLDOWN).
    ctrl._now = lambda: ctrl.last_trigger_ts + cfg.START_COOLDOWN_S + 10
    ctrl._cooldown_step()
    assert ctrl.state == State.MONITORING, \
        f"expected MONITORING after cooldown, got {ctrl.state}"
    assert ctrl.low_v_count == 0, \
        f"expected low_v_count=0 after cooldown release, got {ctrl.low_v_count}"
    return "cooldown elapsed; back to MONITORING; streak cleared"


# ----- Orchestration -----

PHASES = [
    ("1. import + instantiate controller", phase_1_instantiate),
    ("2. boot state is MONITORING / Pi off", phase_2_initial_state),
    ("3. sustained low V -> STARTING", phase_3_low_voltage_triggers_start),
    ("4. STARTING transmits then -> RUNNING", phase_4_starting_transmits_and_runs),
    ("5. RUNNING -> STOPPING on duration", phase_5_running_to_stopping),
    ("6. STOPPING -> COOLDOWN (grace + MOSFET)", phase_6_stopping_to_cooldown),
    ("7. COOLDOWN -> MONITORING", phase_7_cooldown_releases),
]


def main():
    print("=== VroomController self-test ===")
    results = []
    for name, fn in PHASES:
        res = run_phase(name, fn)
        results.append(res)
        # If a fundamental phase (1 or 2) fails, later phases will cascade.
        # Continue running them anyway so the user sees the full picture.
    print()
    n_pass = sum(1 for r in results if r.ok)
    n_fail = len(results) - n_pass
    print(f"Summary: {n_pass} passed, {n_fail} failed (of {len(results)})")
    return 0 if n_fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
