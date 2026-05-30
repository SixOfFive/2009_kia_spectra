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
            "obd_history": {},
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


# ----- listen_forever error-suppression tests -----
#
# Run the listener with a fake link that always raises the same OSError,
# then count how many ERROR lines reach stdout. With suppression on, the
# first error logs immediately and subsequent identical errors stay quiet
# until the window expires. With suppression off (window=0), every error
# logs (the pre-fix behavior).

class _FakeFailingLink:
    """recv() always raises a configurable exception. Tracks call count."""
    def __init__(self, exc):
        self.exc = exc
        self.recv_calls = 0

    def recv(self):
        self.recv_calls += 1
        raise self.exc


class _FakeRecoveringLink:
    """recv() raises for the first N calls, then returns None forever."""
    def __init__(self, exc, fail_count):
        self.exc = exc
        self.fail_count = fail_count
        self.recv_calls = 0

    def recv(self):
        self.recv_calls += 1
        if self.recv_calls <= self.fail_count:
            raise self.exc
        return None


def _capture_listen(link, max_iterations, **kwargs):
    """
    Run listen_forever with mocked time + a stop-after-N-iterations sleep.

    Returns (stdout_lines, recv_calls). The fake _sleep_fn advances a
    virtual clock and raises StopIteration on the N'th sleep call so the
    loop exits.
    """
    import io
    from contextlib import redirect_stdout

    virtual_now = [0.0]
    iteration_count = [0]

    def fake_time():
        return virtual_now[0]

    def fake_sleep(s):
        virtual_now[0] += s
        iteration_count[0] += 1
        if iteration_count[0] >= max_iterations:
            raise _StopLoop

    buf = io.StringIO()
    try:
        with redirect_stdout(buf):
            uart_listener.listen_forever(
                link, _time_fn=fake_time, _sleep_fn=fake_sleep, **kwargs,
            )
    except _StopLoop:
        pass
    return buf.getvalue().splitlines(), link.recv_calls


class _StopLoop(Exception):
    pass


def test_suppression_silences_repeated_identical_errors():
    """With window=60s, 100 identical errors → 1 ERROR line + maybe 1 summary."""
    link = _FakeFailingLink(OSError(2, "No such file or directory: '/dev/serial0'"))
    lines, recv_calls = _capture_listen(link, max_iterations=100)
    error_lines = [ln for ln in lines if "ERROR recv" in ln]
    # First error must log immediately. Subsequent identical errors
    # accumulate, and a "still failing" summary logs every 60s of
    # virtual time. With 100 iterations of 1-second sleeps, virtual
    # clock advances ~100s, so we expect roughly 1 initial + 1 summary.
    assert 1 <= len(error_lines) <= 3, (
        f"expected 1-3 ERROR lines from 100 identical errors, got "
        f"{len(error_lines)}:\n  " + "\n  ".join(error_lines)
    )
    assert recv_calls >= 100, f"recv() should have run >= 100 times, got {recv_calls}"


def test_suppression_disabled_logs_every_error():
    """With window=0, every error logs (matches the original behavior)."""
    link = _FakeFailingLink(OSError(2, "No such file or directory: '/dev/serial0'"))
    lines, recv_calls = _capture_listen(
        link, max_iterations=20, error_suppression_window_s=0,
    )
    error_lines = [ln for ln in lines if "ERROR recv" in ln]
    # Each iteration sleeps after an error → ~20 errors logged
    assert len(error_lines) >= 15, (
        f"expected ~20 ERROR lines when suppression is disabled, got "
        f"{len(error_lines)}"
    )


def test_summary_line_includes_occurrence_count():
    """After the suppression window expires, the summary line names the
    suppressed count and the elapsed time."""
    link = _FakeFailingLink(OSError(2, "No such file or directory"))
    lines, _ = _capture_listen(link, max_iterations=80)
    summary_lines = [ln for ln in lines if "still failing" in ln]
    # At least one summary line should land in 80s of virtual time
    assert summary_lines, (
        "expected a 'still failing' summary line in:\n  "
        + "\n  ".join(lines)
    )
    summary = summary_lines[0]
    assert "occurrences in last" in summary
    assert "s]" in summary


def test_recovery_logs_when_recv_starts_returning():
    """After a stream of errors, the first successful recv() logs a
    recovery line so the journal makes it obvious things came back."""
    link = _FakeRecoveringLink(OSError(2, "transient"), fail_count=5)
    lines, _ = _capture_listen(link, max_iterations=10)
    recovery_lines = [ln for ln in lines if "recv() recovered" in ln]
    assert recovery_lines, (
        "expected a 'recv() recovered' line after errors stopped:\n  "
        + "\n  ".join(lines)
    )


def test_different_error_strings_break_suppression():
    """Two different error messages should each get their own log line —
    suppression keys on the exact error string, not on 'any error'."""
    class _AlternatingLink:
        def __init__(self):
            self.recv_calls = 0
        def recv(self):
            self.recv_calls += 1
            if self.recv_calls % 2 == 0:
                raise OSError(2, "first kind of error")
            raise OSError(5, "second kind of error")

    link = _AlternatingLink()
    lines, _ = _capture_listen(link, max_iterations=20)
    error_lines = [ln for ln in lines if "ERROR recv" in ln]
    # Each new-error-string transition triggers a fresh log line — so
    # we expect many more than 1 even though only 2 distinct errors exist.
    assert len(error_lines) >= 5, (
        f"alternating errors should each log; got {len(error_lines)} "
        f"lines from 20 iterations"
    )


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
