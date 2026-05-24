"""
Run the VroomController through a complete trigger cycle in CPython,
with mocked hardware and a stepped clock. Useful for:

- Watching the state machine in action without burning a board
- Debugging the UART JSON protocol - every emitted message is printed
- Sanity-checking config tweaks (thresholds, durations) before flashing

Run from the repo root:

    python esp32/scripts/simulate.py
    python esp32/scripts/simulate.py --scenario manual
    python esp32/scripts/simulate.py --scenario long_idle --sustain 60
"""
import argparse
import os
import sys
import textwrap

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "src"))

from controller import VroomController, State  # noqa: E402


# ----- Fake hardware -----

class FakePin:
    def __init__(self, label):
        self.label = label
        self._v = 0
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


class TracingUart:
    """Prints every message the controller emits, in human-readable form."""
    def __init__(self):
        self.inbox = []
    def send(self, msg):
        mtype = msg.get("type", "?")
        if mtype == "STATUS":
            print(f"  -> STATUS  v={msg.get('v_battery'):.2f}  state={msg.get('state')}")
        elif mtype == "EVENT":
            print(f"  ->EVENT   {msg.get('event')}  detail={msg.get('detail')}")
        elif mtype == "COMMAND":
            print(f"  ->COMMAND {msg.get('cmd')}")
        elif mtype == "ACK":
            print(f"  ->ACK     {msg.get('in_reply_to')}  ok={msg.get('ok')}  detail={msg.get('detail')}")
        elif mtype == "OBD":
            print(f"  ->OBD     {msg.get('name')}={msg.get('value')} {msg.get('units')}")
        elif mtype == "LOG":
            print(f"  ->LOG     [{msg.get('level')}] {msg.get('msg')}")
        else:
            print(f"  ->{mtype}: {msg}")
    def recv(self):
        return self.inbox.pop(0) if self.inbox else None


_SIM_PACKETS = {
    "START":  "0" * 35,
    "LOCK":   "1" * 35,
    "UNLOCK": "01" * 17 + "0",
    "TRUNK":  "10" * 17 + "1",
}


class Config:
    """Modifiable config defaults for the simulator."""
    ADC_DIVIDER_RATIO = 4.0
    ADC_CALIBRATION_OFFSET = 0.0
    LOW_V_TRIGGER = 12.2
    LOW_V_SUSTAIN_S = 300
    RUN_DURATION_S = 900
    WAKE_INTERVAL_S = 60
    START_COOLDOWN_S = 7200
    RF_BURST_REPEATS = 8
    RF_GUARD_MS = 39
    OBD_POLL_INTERVAL_S = 1
    OBD_POLL_PIDS = ()
    PI_SHUTDOWN_GRACE_S = 30
    WDT_ENABLED = False
    COMPUSTAR_REMOTE_ID = 0xABCD
    COMPUSTAR_PACKETS = _SIM_PACKETS


def _build(cfg_overrides=None):
    cfg = Config()
    for k, v in (cfg_overrides or {}).items():
        setattr(cfg, k, v)
    adc = FakeAdc(voltage=12.8)
    radio = FakeRadio()
    uart = TracingUart()
    pi_power = FakePin("pi_power")
    ctrl = VroomController(adc, radio, None, uart, pi_power, cfg)
    return ctrl, adc, radio, uart, pi_power, cfg


def _section(title):
    print()
    print(f"--- {title} ---")


# ----- Scenarios -----

def scenario_low_voltage(args):
    """Battery drops to 11.9V, sustains, controller triggers, runs, stops."""
    ctrl, adc, radio, uart, pi_power, cfg = _build()
    samples = ctrl._samples_needed()

    _section(f"phase 1: drop battery to 11.9V (need {samples} consecutive low samples)")
    adc.voltage = 11.9 / cfg.ADC_DIVIDER_RATIO   # 2.975V at ADC -> 11.9V battery
    for i in range(samples):
        print(f"\nsample {i+1}/{samples}:")
        ctrl._monitor_step()
        if ctrl.state != State.MONITORING:
            break
    print(f"\n>>> controller transitioned to: {ctrl.state}")
    print(f">>> Pi power MOSFET: {'ON (Pi powered)' if pi_power.value() == 0 else 'OFF'}")
    print(f">>> RF bursts transmitted: {len(radio.bursts)}")

    _section("phase 2: STARTING -> RUNNING")
    ctrl.tick()
    print(f">> state now: {ctrl.state}")

    _section(f"phase 3: simulate run duration ({cfg.RUN_DURATION_S}s) elapsed")
    ctrl._now = lambda: cfg.RUN_DURATION_S + 1
    ctrl._running_step()
    print(f">> state now: {ctrl.state}")
    print(f">>> RF bursts total: {len(radio.bursts)} (1 start + 1 stop expected)")

    _section("phase 4: STOPPING - send shutdown_pi, wait grace, cut MOSFET")
    times = iter([0, cfg.PI_SHUTDOWN_GRACE_S + 1])
    ctrl._now = lambda: next(times)
    ctrl._stopping_step()
    ctrl._stopping_step()
    print(f">>> Pi power: {'ON' if pi_power.value() == 0 else 'OFF (MOSFET cut)'}")
    print(f">> state now: {ctrl.state}")

    _section("phase 5: COOLDOWN elapses")
    ctrl._now = lambda: cfg.START_COOLDOWN_S + 100
    ctrl._cooldown_step()
    print(f">> state now: {ctrl.state}")
    print(f">>> low_v_count cleared: {ctrl.low_v_count == 0}")


def scenario_manual(args):
    """Pi sends start_engine; verify ESP32 dispatches + transmits."""
    ctrl, adc, radio, uart, pi_power, cfg = _build()

    _section("phase 1: Pi -> start_engine command")
    uart.inbox.append({"type": "COMMAND", "cmd": "start_engine"})
    ctrl._handle_uart()
    print(f">> state now: {ctrl.state}")
    print(f">>> Pi power: {'ON' if pi_power.value() == 0 else 'OFF'}")
    print(f">>> RF bursts: {len(radio.bursts)}")

    _section("phase 2: ping -> ACK")
    uart.inbox.append({"type": "COMMAND", "cmd": "ping"})
    ctrl._handle_uart()

    _section("phase 3: set_threshold to 11.8")
    uart.inbox.append({"type": "COMMAND", "cmd": "set_threshold", "value": 11.8})
    ctrl._handle_uart()
    print(f">>> LOW_V_TRIGGER is now: {ctrl.cfg.LOW_V_TRIGGER}")


def scenario_long_idle(args):
    """Voltage drops below threshold but recovers each cycle - should never trigger."""
    ctrl, adc, radio, uart, pi_power, cfg = _build({"LOW_V_SUSTAIN_S": args.sustain})

    _section(f"phase 1: alternate low/high voltage for 20 samples (sustain={args.sustain}s)")
    print(f"    samples_needed = {ctrl._samples_needed()}")
    for i in range(20):
        adc.voltage = (11.9 if i % 2 == 0 else 13.0) / cfg.ADC_DIVIDER_RATIO
        ctrl._monitor_step()
        if ctrl.state != State.MONITORING:
            print(f"\n>>> UNEXPECTED TRIGGER after {i+1} samples")
            return
    print("\n>>> good: stayed in MONITORING for 20 alternating samples")
    print(f">>> low_v_count = {ctrl.low_v_count}")
    print(f">>> RF bursts: {len(radio.bursts)} (expected 0)")


SCENARIOS = {
    "low_voltage": scenario_low_voltage,
    "manual": scenario_manual,
    "long_idle": scenario_long_idle,
}


def main():
    ap = argparse.ArgumentParser(
        description="Run VroomController through a scenario in CPython with mocked hardware.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=textwrap.dedent("""
            scenarios:
              low_voltage  Battery drops sustained -> start -> run -> stop -> cooldown
              manual       Pi sends start_engine + ping + set_threshold
              long_idle    Alternating low/high voltage - verifies no spurious triggers
        """).strip(),
    )
    ap.add_argument("--scenario", choices=list(SCENARIOS), default="low_voltage")
    ap.add_argument("--sustain", type=int, default=120,
                    help="for long_idle: LOW_V_SUSTAIN_S override")
    args = ap.parse_args()

    print(f"=== Simulating scenario: {args.scenario} ===")
    SCENARIOS[args.scenario](args)
    print()
    print("=== Simulation complete ===")


if __name__ == "__main__":
    main()
