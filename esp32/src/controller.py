"""
Vroom controller — the main state machine for the ESP32.

State transitions:

    MONITORING ──low V sustained──> STARTING ──RF sent, Pi up──> RUNNING
        ▲                                                          │
        │                                                          │ run duration elapsed
        │                                                          │   or Pi sent STOP cmd
        │                                                          ▼
    COOLDOWN ◀────Pi shutdown grace, MOSFET off──── STOPPING ◀──RF stop sent

  - MONITORING: Pi off, ESP32 deep-sleeps WAKE_INTERVAL_S between samples
  - STARTING:   Pi powered on, Start RF packet transmitted
  - RUNNING:    OBD polling, status broadcast to Pi via UART
  - STOPPING:   Stop RF packet, shutdown command sent to Pi
  - COOLDOWN:   no new triggers for START_COOLDOWN_S after a run

Persistence:
  - Consecutive-low-voltage sample count in RTC memory via lib.persistence
    (survives deep sleep, NOT power loss — fresh boot starts at 0)
  - The Compustar 1WSHR-PRO is a FIXED-CODE FOB so no rolling counter
    needs to be persisted. (Earlier revisions of this controller saved
    a KeeLoq counter to flash. That logic was removed after SDR analysis
    confirmed the protocol — see sdr/analysis/framing.md.)

Deep sleep behavior:
  - In MONITORING, after each sample the controller calls machine.deepsleep
    for WAKE_INTERVAL_S * 1000 ms. The board resets on wake and the
    streak is reloaded from RTC. Average current drops from ~80 mA awake
    to <10 µA between samples.
  - In all other states the Pi is on and we need UART comms, so we just
    sleep_ms in a tight loop (no deep sleep).
  - Trade-off: manual remote-start from the dashboard requires the ESP32
    to be awake, which it only is briefly between deep-sleep cycles or
    once a trigger has fired. By design — primary mode is voltage-trigger.

Pi-side commands accepted via UART:
  - start_engine: manual trigger from dashboard
  - stop_engine:  manual stop
  - set_threshold: change LOW_V_TRIGGER at runtime
  - ping:          health check
  - shutdown_pi:   the Pi tells us it's about to power down (we cut the rail)

Runs on MicroPython on ESP32; importable in CPython for unit testing with
mocked hardware.
"""

from lib import pi_link, persistence

try:
    from machine import Pin, UART, deepsleep  # noqa: F401
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

    def deepsleep(ms):  # noqa
        pass

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

        self.last_trigger_ts = 0
        self.state = State.MONITORING
        # Sample-based streak counter survives deep sleep via RTC memory.
        # Each MONITORING tick samples once; if V is low we increment, else reset.
        # Trigger fires when low_v_count * WAKE_INTERVAL_S >= LOW_V_SUSTAIN_S.
        self.low_v_count = persistence.load_streak()
        # Hardware watchdog (optional). Constructed lazily so tests/CPython
        # don't accidentally pull in machine.WDT.
        self.wdt = self._init_wdt()
        self.run_start_ts = None
        self.stop_grace_until = None
        self.last_voltage = None
        self.last_obd_poll_ts = 0
        self.obd_values = {}
        # Whether the most recent _monitor_step just sampled (used by run()
        # to decide whether to deep-sleep before the next tick).
        self._just_sampled = False

    # ----- Pi power control -----

    def _power_on_pi(self):
        self.pi_power.off()       # active-low → ON
        self._log("Pi power: ON")

    def _power_off_pi(self):
        self.pi_power.on()        # high → OFF
        self._log("Pi power: OFF")

    # ----- RF transmit -----

    def _transmit_button(self, button_name):
        """Transmit the Compustar packet for the given button.

        The bit-pattern for each button is captured once from the FOB via
        SDR (see sdr/06-framing-extraction.md) and stored verbatim in
        ``cfg.COMPUSTAR_PACKETS``. Same press = identical packet on the
        wire — no rolling code, no counter, no key.
        """
        from lib import compustar
        packets = getattr(self.cfg, "COMPUSTAR_PACKETS", None)
        if packets is None or button_name not in packets:
            self._log("ERROR no packet stored for %s" % button_name)
            self.uart.send(pi_link.event(
                pi_link.EVENT_COMPUSTAR_TX_FAIL,
                button=button_name, reason="no_packet",
            ))
            return False

        try:
            pulses = compustar.build_pulses_for_button(button_name, packets)
        except (ValueError, KeyError) as e:
            self._log("ERROR malformed packet for %s: %s" % (button_name, e))
            self.uart.send(pi_link.event(
                pi_link.EVENT_COMPUSTAR_TX_FAIL,
                button=button_name, reason="malformed_packet",
            ))
            return False

        self.radio.transmit_burst(
            pulses,
            repeats=self.cfg.RF_BURST_REPEATS,
            guard_ms=self.cfg.RF_GUARD_MS,
        )
        self._log("RF %s sent" % button_name)
        self.uart.send(pi_link.event(
            pi_link.EVENT_COMPUSTAR_TX,
            button=button_name,
        ))
        return True

    # ----- ADC / battery voltage -----

    def _read_battery_v(self):
        adc_v = self.adc.read_voltage()
        battery_v = adc_v * self.cfg.ADC_DIVIDER_RATIO + self.cfg.ADC_CALIBRATION_OFFSET
        self.last_voltage = battery_v
        return battery_v

    def _samples_needed(self):
        """How many consecutive low samples constitute a sustained low-V condition."""
        wake = max(1, int(self.cfg.WAKE_INTERVAL_S))
        return max(1, int(self.cfg.LOW_V_SUSTAIN_S // wake))

    # ----- State machine steps -----

    def _monitor_step(self):
        v = self._read_battery_v()
        self._just_sampled = True
        self._log("V=%0.2f count=%d/%d" % (v, self.low_v_count, self._samples_needed()))

        if v < self.cfg.LOW_V_TRIGGER:
            self.low_v_count += 1
        else:
            self.low_v_count = 0
        persistence.save_streak(self.low_v_count)

        # Pi is off in MONITORING, so this status message goes nowhere on hardware.
        # Still emit for log visibility when debugging via the dev dashboard.
        self.uart.send(pi_link.status(v, self.state))

        if self.low_v_count >= self._samples_needed():
            self._trigger_start("low_voltage")

    def _starting_step(self):
        # transmit already happened during _trigger_start; just advance state.
        self.run_start_ts = self._now()
        self.state = State.RUNNING
        self.uart.send(pi_link.event(pi_link.EVENT_ENGINE_STARTED))

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
            self.low_v_count = 0
            persistence.clear_streak()

    # ----- High-level transitions -----

    def _trigger_start(self, reason):
        if self.state != State.MONITORING:
            return
        self.uart.send(pi_link.event(
            pi_link.EVENT_LOW_VOLTAGE_TRIGGER,
            reason=reason, voltage=self.last_voltage,
        ))
        # Clear the streak so we don't immediately re-trigger after the run.
        self.low_v_count = 0
        persistence.clear_streak()
        self.state = State.STARTING
        self._power_on_pi()
        from lib import compustar
        self._transmit_button(compustar.Button.START)

    def _trigger_stop(self, reason):
        if self.state != State.RUNNING:
            return
        from lib import compustar
        # Compustar 1-way protocol: re-pressing Start while running stops the
        # engine. The 1WSHR-PRO doesn't have a dedicated stop button — same
        # 35-bit Start packet starts AND stops, depending on engine state.
        self._transmit_button(compustar.Button.START)
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

    def _init_wdt(self):
        """Return a WDT instance or None if disabled / unavailable."""
        if not getattr(self.cfg, "WDT_ENABLED", False):
            return None
        if not _MICROPYTHON:
            return None
        try:
            from machine import WDT
            return WDT(timeout=int(self.cfg.WDT_TIMEOUT_MS))
        except Exception as e:
            self._log("WARN WDT init failed: %s" % e)
            return None

    def _feed_wdt(self):
        if self.wdt is not None:
            try:
                self.wdt.feed()
            except Exception:
                pass

    def tick(self):
        """One pass through the state machine. Call repeatedly from run()."""
        self._feed_wdt()
        self._just_sampled = False
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
        self._log("controller starting in state=%s (low_v_count=%d, wake_reason=%s)" % (
            self.state, self.low_v_count,
            "deep_sleep" if persistence.was_deep_sleep_wake() else "cold_boot",
        ))
        while True:
            try:
                self.tick()
            except Exception as e:
                self._log("ERROR in tick: %s" % e)

            if not _MICROPYTHON:
                return

            # If we just took a MONITORING sample and didn't trigger, deep-sleep
            # until the next sample interval. Board resets on wake; we'll
            # reload low_v_count from RTC memory at the top of run().
            if self.state == State.MONITORING and self._just_sampled:
                wake_ms = max(1, int(self.cfg.WAKE_INTERVAL_S * 1000))
                self._log("deep sleep %d ms" % wake_ms)
                deepsleep(wake_ms)
                # deepsleep does not return; the line below is unreachable on real hardware.

            time.sleep_ms(tick_ms)

    # ----- Helpers -----

    def _now(self):
        if _MICROPYTHON:
            return time.time()
        return 0

    def _log(self, msg):
        print("%s %s" % (LOG_TAG, msg))
