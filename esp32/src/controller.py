"""
Vroom controller — the main state machine for the ESP32.

State transitions:

    MONITORING ──low V sustained──> STARTING ──RF sent, Pi up──> RUNNING
        ▲                                                          │
        │                                                          │ run duration elapsed
        │                                                          │   or Pi sent STOP cmd
        │                                                          ▼
    COOLDOWN ◀────Pi shutdown grace, MOSFET off──── STOPPING ◀──RF stop sent

  - MONITORING: Pi off, ESP32 samples battery V every WAKE_INTERVAL_S
  - STARTING:   Pi powered on, Start RF packet transmitted
  - RUNNING:    OBD polling, status broadcast to Pi via UART
  - STOPPING:   Stop RF packet, shutdown command sent to Pi
  - COOLDOWN:   no new triggers for START_COOLDOWN_S after a run

Persistent state in flash (counter). Volatile state (low-V streak, run
duration timers) is in RAM — fine since we don't deep-sleep during the
critical phases. Deep-sleep optimization for MONITORING is a future TODO.

Pi-side commands accepted via UART:
  - start_engine: manual trigger from dashboard
  - stop_engine:  manual stop
  - set_threshold: change LOW_V_TRIGGER at runtime
  - ping:          health check
  - shutdown_pi:   the Pi tells us it's about to power down (we cut the rail)

Runs on MicroPython on ESP32; importable in CPython for unit testing with
mocked hardware.
"""

import json

from lib import pi_link

try:
    from machine import Pin, UART  # noqa: F401
    import time
    _MICROPYTHON = True
except ImportError:
    _MICROPYTHON = False

    class Pin:  # noqa
        OUT = 0
        def __init__(self, *a, **kw): self._v = 0
        def on(self): self._v = 1
        def off(self): self._v = 0
        def value(self, v=None):
            if v is None: return self._v
            self._v = int(v)

    class _Time:
        def time(self): return 0
        def sleep_ms(self, n): pass
    time = _Time()


class State:
    MONITORING = "monitoring"
    STARTING = "starting"
    RUNNING = "running"
    STOPPING = "stopping"
    COOLDOWN = "cooldown"


COUNTER_FILE = "compustar_counter.json"
LOG_TAG = "[controller]"


class VroomController:
    """
    Glues drivers into a state machine. Hardware references are injected
    so the class is testable without real hardware.

    :param adc:           ADS1115-like object with .read_voltage()
    :param radio:         CC1101-like object with .transmit_burst()
    :param can:           Can-like object or None
    :param uart:          pi_link.UartLink (or compatible mock)
    :param pi_power_pin:  machine.Pin controlling the Pi power MOSFET (active-low)
    :param config:        config module (or duck-typed namespace) with thresholds
    """

    def __init__(self, adc, radio, can, uart, pi_power_pin, config):
        self.adc = adc
        self.radio = radio
        self.can = can
        self.uart = uart
        self.pi_power = pi_power_pin
        self.cfg = config

        # Pi P-MOSFET is active-low: high = OFF. Boot with Pi OFF.
        self.pi_power.on()

        self.counter = self._load_counter()
        self.last_trigger_ts = 0
        self.state = State.MONITORING
        self.low_v_streak_start = None
        self.run_start_ts = None
        self.stop_grace_until = None
        self.last_voltage = None
        self.last_voltage_sample_ts = 0
        self.last_obd_poll_ts = 0
        self.obd_values = {}

    # ----- Persistent counter (Compustar rolling code) -----

    def _load_counter(self):
        cfg_counter = getattr(self.cfg, "COMPUSTAR_COUNTER", 0) or 0
        try:
            with open(COUNTER_FILE) as f:
                data = json.load(f)
            return max(cfg_counter, int(data.get("counter", 0)))
        except (OSError, ValueError):
            return cfg_counter

    def _save_counter(self):
        try:
            with open(COUNTER_FILE, "w") as f:
                json.dump({"counter": self.counter}, f)
        except OSError as e:
            self._log("WARN counter save failed: %s" % e)

    # ----- Pi power control -----

    def _power_on_pi(self):
        self.pi_power.off()       # active-low → ON
        self._log("Pi power: ON")

    def _power_off_pi(self):
        self.pi_power.on()        # high → OFF
        self._log("Pi power: OFF")

    # ----- RF transmit -----

    def _transmit_function(self, function_code, label):
        """Build, persist, and transmit a Compustar packet for the given function."""
        from lib import compustar
        device_key = getattr(self.cfg, "COMPUSTAR_DEVICE_KEY", None)
        serial = getattr(self.cfg, "COMPUSTAR_SERIAL", None)
        if device_key is None or serial is None:
            self._log("ERROR no device key or serial — cannot transmit %s" % label)
            self.uart.send(pi_link.event(
                pi_link.EVENT_KEELOQ_TX_FAIL,
                function=label, reason="no_secrets",
            ))
            return False

        # Persist BEFORE transmit so a crash mid-burst doesn't reuse counter.
        self.counter += 1
        self._save_counter()

        packet = compustar.build_packet(
            serial=serial,
            function_code=function_code,
            counter=self.counter,
            device_key=device_key,
        )
        pulses = compustar.packet_to_pulses(packet["bits"], te_us=self.cfg.RF_TE_US)
        self.radio.transmit_burst(
            pulses,
            repeats=self.cfg.RF_BURST_REPEATS,
            guard_ms=self.cfg.RF_GUARD_MS,
        )
        self._log("RF %s sent (counter=%d)" % (label, self.counter))
        self.uart.send(pi_link.event(
            pi_link.EVENT_KEELOQ_TX,
            function=label, counter=self.counter,
        ))
        return True

    # ----- ADC / battery voltage -----

    def _read_battery_v(self):
        adc_v = self.adc.read_voltage()
        battery_v = adc_v * self.cfg.ADC_DIVIDER_RATIO + self.cfg.ADC_CALIBRATION_OFFSET
        self.last_voltage = battery_v
        self.last_voltage_sample_ts = self._now()
        return battery_v

    # ----- State machine steps -----

    def _monitor_step(self):
        now = self._now()
        if now - self.last_voltage_sample_ts < self.cfg.WAKE_INTERVAL_S:
            return
        v = self._read_battery_v()
        self._log("V=%0.2f streak=%s" % (v, self.low_v_streak_start))
        self.uart.send(pi_link.status(v, self.state, counter=self.counter))

        if v < self.cfg.LOW_V_TRIGGER:
            if self.low_v_streak_start is None:
                self.low_v_streak_start = now
            elif now - self.low_v_streak_start >= self.cfg.LOW_V_SUSTAIN_S:
                self._trigger_start("low_voltage")
        else:
            self.low_v_streak_start = None

    def _starting_step(self):
        # transmit already happened during _trigger_start; just advance state.
        self.run_start_ts = self._now()
        self.state = State.RUNNING
        self.uart.send(pi_link.event(
            pi_link.EVENT_ENGINE_STARTED,
            counter=self.counter,
        ))

    def _running_step(self):
        now = self._now()
        if now - self.run_start_ts >= self.cfg.RUN_DURATION_S:
            self._trigger_stop("duration_elapsed")
            return
        if now - self.last_obd_poll_ts >= self.cfg.OBD_POLL_INTERVAL_S:
            self._poll_obd()
            self.last_obd_poll_ts = now

    def _stopping_step(self):
        now = self._now()
        if self.stop_grace_until is None:
            self.uart.send(pi_link.command(pi_link.CMD_SHUTDOWN_PI))
            self.stop_grace_until = now + self.cfg.PI_SHUTDOWN_GRACE_S
            self._log("shutdown sent, grace=%ds" % self.cfg.PI_SHUTDOWN_GRACE_S)
        elif now >= self.stop_grace_until:
            self._power_off_pi()
            self.stop_grace_until = None
            self.state = State.COOLDOWN
            self.last_trigger_ts = now
            self.uart.send(pi_link.event(pi_link.EVENT_ENGINE_STOPPED))

    def _cooldown_step(self):
        if self._now() - self.last_trigger_ts >= self.cfg.START_COOLDOWN_S:
            self.state = State.MONITORING
            self.low_v_streak_start = None

    # ----- High-level transitions -----

    def _trigger_start(self, reason):
        if self.state != State.MONITORING:
            return
        self.uart.send(pi_link.event(
            pi_link.EVENT_LOW_VOLTAGE_TRIGGER,
            reason=reason, voltage=self.last_voltage,
        ))
        self.state = State.STARTING
        self._power_on_pi()
        from lib import compustar
        self._transmit_function(compustar.Function.START, "START")

    def _trigger_stop(self, reason):
        if self.state != State.RUNNING:
            return
        from lib import compustar
        # Compustar protocol: re-pressing Start while running stops the engine.
        # If your variant uses a dedicated stop code, swap to it here.
        self._transmit_function(compustar.Function.START, "STOP")
        self.state = State.STOPPING
        self.uart.send(pi_link.log("info", "stopping: %s" % reason))

    # ----- OBD-II polling -----

    def _poll_obd(self):
        if self.can is None:
            return
        from lib import twai_can
        for pid in self.cfg.OBD_POLL_PIDS:
            result = twai_can.query_pid(self.can, pid, timeout_ms=200)
            if result is None:
                continue
            self.obd_values[result["name"]] = result["value"]
            self.uart.send(pi_link.obd(
                pid, result["name"], result["value"], result["units"],
            ))

    # ----- Incoming UART command handling -----

    def _handle_uart(self):
        if not hasattr(self.uart, "recv"):
            return
        msg = self.uart.recv()
        if msg is None:
            return
        if msg.get("type") != pi_link.TYPE_COMMAND:
            return
        self._handle_command(msg.get("cmd"), msg)

    def _handle_command(self, cmd, msg):
        if cmd == pi_link.CMD_START_ENGINE:
            self._trigger_start("manual")
            self.uart.send(pi_link.ack(cmd))
        elif cmd == pi_link.CMD_STOP_ENGINE:
            self._trigger_stop("manual")
            self.uart.send(pi_link.ack(cmd))
        elif cmd == pi_link.CMD_SET_THRESHOLD:
            new = float(msg.get("value", self.cfg.LOW_V_TRIGGER))
            self.cfg.LOW_V_TRIGGER = new
            self.uart.send(pi_link.ack(cmd, detail="threshold=%.2f" % new))
        elif cmd == pi_link.CMD_PING:
            self.uart.send(pi_link.ack(cmd, detail=self.state))
        elif cmd == pi_link.CMD_SHUTDOWN_PI:
            if self.state == State.RUNNING:
                self._trigger_stop("pi_shutdown")
            else:
                self._power_off_pi()
            self.uart.send(pi_link.ack(cmd))
        else:
            self.uart.send(pi_link.ack(cmd, ok=False, detail="unknown cmd"))

    # ----- Main loop -----

    def tick(self):
        """One pass through the state machine. Call repeatedly from run()."""
        if self.state == State.MONITORING:
            self._monitor_step()
        elif self.state == State.STARTING:
            self._starting_step()
        elif self.state == State.RUNNING:
            self._running_step()
        elif self.state == State.STOPPING:
            self._stopping_step()
        elif self.state == State.COOLDOWN:
            self._cooldown_step()
        self._handle_uart()

    def run(self, tick_ms=100):
        """Main loop. Returns only on unhandled exception or in CPython mode."""
        self._log("controller starting in state=%s" % self.state)
        while True:
            try:
                self.tick()
            except Exception as e:
                self._log("ERROR in tick: %s" % e)
            if _MICROPYTHON:
                time.sleep_ms(tick_ms)
            else:
                return

    # ----- Helpers -----

    def _now(self):
        if _MICROPYTHON:
            return time.time()
        return 0

    def _log(self, msg):
        print("%s %s" % (LOG_TAG, msg))
