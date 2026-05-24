"""Tests for the UART listener dispatch logic (pure-Python, no hardware)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))

from pi.app import state  # noqa: E402
from pi.app.comms import uart_listener  # noqa: E402


def _reset_state():
    with state.STATE_LOCK:
        state.STATE.clear()
        state.STATE.update({
            "v_battery": None,
            "engine_running": False,
            "esp32_state": "unknown",
            "last_obd": {},
            "events": [],
        })


def test_status_updates_voltage_and_state():
    _reset_state()
    uart_listener.dispatch({
        "type": "STATUS", "v_battery": 12.34, "state": "monitoring", "counter": 42,
    })
    snap = state.snapshot()
    assert snap["v_battery"] == 12.34
    assert snap["esp32_state"] == "monitoring"
    assert snap["engine_running"] is False
    assert snap["counter"] == 42


def test_status_running_state_implies_engine_running():
    _reset_state()
    uart_listener.dispatch({"type": "STATUS", "state": "running"})
    assert state.snapshot()["engine_running"] is True


def test_obd_updates_last_obd_by_name():
    _reset_state()
    uart_listener.dispatch({"type": "OBD", "pid": 0x0C, "name": "rpm",
                            "value": 1726.0, "units": "rpm"})
    uart_listener.dispatch({"type": "OBD", "pid": 0x0D, "name": "speed",
                            "value": 80.0, "units": "km/h"})
    snap = state.snapshot()
    assert snap["last_obd"]["rpm"] == 1726.0
    assert snap["last_obd"]["speed"] == 80.0


def test_event_engine_started_sets_running_and_timestamp():
    _reset_state()
    uart_listener.dispatch({
        "type": "EVENT", "ts": 5000, "event": "engine_started",
        "detail": {"counter": 101},
    })
    snap = state.snapshot()
    assert snap["engine_running"] is True
    assert snap["last_start_ts"] == 5000
    assert len(snap["events"]) == 1
    assert snap["events"][0]["event"] == "engine_started"


def test_event_engine_stopped_clears_running():
    _reset_state()
    state.update_state(engine_running=True)
    uart_listener.dispatch({"type": "EVENT", "event": "engine_stopped"})
    assert state.snapshot()["engine_running"] is False


def test_events_are_appended_and_capped():
    _reset_state()
    for i in range(state.MAX_EVENTS + 25):
        uart_listener.dispatch({"type": "EVENT", "event": f"e{i}"})
    snap = state.snapshot(recent_event_count=state.MAX_EVENTS)
    # state.snapshot caps to MAX_EVENTS implicitly via append_event trim
    assert len(snap["events"]) == state.MAX_EVENTS or len(snap["events"]) == state.MAX_EVENTS
    # The oldest events should have been trimmed
    assert snap["events"][-1]["event"] == f"e{state.MAX_EVENTS + 24}"


def test_log_messages_do_not_change_state():
    _reset_state()
    before = state.snapshot()
    uart_listener.dispatch({"type": "LOG", "level": "info", "msg": "hello"})
    after = state.snapshot()
    assert before == after


def test_shutdown_command_invokes_handler(monkeypatch=None):
    _reset_state()
    captured = {}
    def fake_handler():
        captured["called"] = True
    orig = uart_listener.handle_shutdown_request
    uart_listener.handle_shutdown_request = fake_handler
    try:
        uart_listener.dispatch({"type": "COMMAND", "cmd": "shutdown_pi"})
    finally:
        uart_listener.handle_shutdown_request = orig
    assert captured.get("called") is True


def test_unknown_message_type_is_silently_ignored():
    _reset_state()
    before = state.snapshot()
    uart_listener.dispatch({"type": "GARBAGE", "foo": "bar"})
    assert state.snapshot() == before


def run():
    tests = [v for k, v in globals().items() if k.startswith("test_") and callable(v)]
    passed = 0
    for t in tests:
        t()
        passed += 1
        print(f"PASS {t.__name__}")
    print(f"\n{passed}/{len(tests)} uart_listener tests passed")


if __name__ == "__main__":
    run()
