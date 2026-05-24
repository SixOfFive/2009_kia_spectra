"""
Tests for the trip-log module (JSONL persistence + in-memory open trip)
and the uart_listener hooks that drive it.
"""
import json
import os
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))

from pi.app import state, trip_log  # noqa: E402
from pi.app.comms import uart_listener  # noqa: E402


def _tmp_jsonl():
    fh = tempfile.NamedTemporaryFile(
        delete=False, suffix=".jsonl", mode="w", encoding="utf-8",
    )
    fh.close()
    return fh.name


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
    trip_log.reset_for_tests()


def test_start_and_end_trip_writes_jsonl_row():
    _reset_state()
    path = _tmp_jsonl()
    try:
        trip_log.start_trip(
            v_start=12.4, reason="low_voltage", trigger_source="low_voltage",
        )
        trip_log.update_peak("rpm", 1800.0)
        trip_log.update_peak("rpm", 1400.0)   # lower — should not overwrite
        trip_log.update_peak("coolant_temp", 78)
        finalized = trip_log.end_trip(v_end=12.7, path=path)
        assert finalized["v_start"] == 12.4
        assert finalized["v_end"] == 12.7
        assert finalized["trigger_source"] == "low_voltage"
        assert finalized["peak_obd"]["rpm"] == 1800.0
        assert finalized["peak_obd"]["coolant_temp"] == 78.0
        # Should have been flushed to disk.
        with open(path, "r", encoding="utf-8") as fh:
            lines = [l for l in fh if l.strip()]
        assert len(lines) == 1
        row = json.loads(lines[0])
        assert row["v_start"] == 12.4
        assert row["v_end"] == 12.7
        assert row["peak_obd"]["rpm"] == 1800.0
    finally:
        os.unlink(path)


def test_read_recent_returns_newest_first_and_skips_garbage():
    _reset_state()
    path = _tmp_jsonl()
    try:
        # Three good rows + one garbage line.
        for i, v in enumerate([12.0, 12.3, 12.6]):
            trip_log.start_trip(v_start=v, trigger_source="manual")
            trip_log.end_trip(v_end=v + 0.2, path=path)
        with open(path, "a", encoding="utf-8") as fh:
            fh.write("not json at all\n")
        rows = trip_log.read_recent(path, limit=10)
        assert len(rows) == 3
        # newest first => v_start descends
        assert [r["v_start"] for r in rows] == [12.6, 12.3, 12.0]
    finally:
        os.unlink(path)


def test_update_peak_noop_when_no_open_trip():
    _reset_state()
    # Should not raise even though no trip is in progress.
    trip_log.update_peak("rpm", 2000)
    assert trip_log.open_trip_snapshot() is None


def test_end_trip_returns_none_when_no_open_trip():
    _reset_state()
    assert trip_log.end_trip(v_end=12.6, path=None) is None


def test_uart_listener_opens_trip_on_engine_started():
    _reset_state()
    # Seed STATUS so v_battery is set when EVENT engine_started fires.
    uart_listener.dispatch({"type": "STATUS", "v_battery": 11.9, "state": "monitoring"})
    uart_listener.dispatch({
        "type": "EVENT", "event": "engine_started", "ts": 1000,
        "detail": {"reason": "v<11.5", "trigger_source": "low_voltage"},
    })
    open_trip = trip_log.open_trip_snapshot()
    assert open_trip is not None
    assert open_trip["v_start"] == 11.9
    assert open_trip["trigger_source"] == "low_voltage"
    assert open_trip["reason"] == "v<11.5"


def test_uart_listener_closes_trip_on_engine_stopped_and_records_peaks():
    _reset_state()
    # Redirect the JSONL path so we don't touch /var/log.
    from pi.app import config
    path = _tmp_jsonl()
    original = config.TRIP_LOG_PATH
    config.TRIP_LOG_PATH = path
    try:
        # Open the trip.
        uart_listener.dispatch({"type": "STATUS", "v_battery": 11.9, "state": "running"})
        uart_listener.dispatch({
            "type": "EVENT", "event": "engine_started", "ts": 1000,
            "detail": {"trigger_source": "low_voltage"},
        })
        # Stream a few OBD samples — peak rpm should land on 2400.
        for rpm in (800, 2400, 1500):
            uart_listener.dispatch({
                "type": "OBD", "pid": 0x0C, "name": "rpm",
                "value": float(rpm), "units": "rpm",
            })
        uart_listener.dispatch({
            "type": "OBD", "pid": 0x05, "name": "coolant_temp",
            "value": 85, "units": "C",
        })
        # Voltage rises after the engine warms up; STATUS updates v_battery.
        uart_listener.dispatch({"type": "STATUS", "v_battery": 13.7, "state": "running"})
        # Close.
        uart_listener.dispatch({"type": "EVENT", "event": "engine_stopped"})
        assert trip_log.open_trip_snapshot() is None
        rows = trip_log.read_recent(path, limit=5)
        assert len(rows) == 1
        row = rows[0]
        assert row["v_start"] == 11.9
        assert row["v_end"] == 13.7
        assert row["peak_obd"]["rpm"] == 2400.0
        assert row["peak_obd"]["coolant_temp"] == 85.0
    finally:
        config.TRIP_LOG_PATH = original
        os.unlink(path)


def test_api_trips_endpoint_returns_recent_rows():
    """The Flask /api/trips endpoint should return persisted rows newest-first."""
    _reset_state()
    from pi.app import config
    from pi.app.display import server
    path = _tmp_jsonl()
    original = config.TRIP_LOG_PATH
    config.TRIP_LOG_PATH = path
    try:
        for v in (12.0, 12.4, 12.7):
            trip_log.start_trip(v_start=v, trigger_source="manual")
            trip_log.end_trip(v_end=v + 0.1, path=path)
        client = server.app.test_client()
        res = client.get("/api/trips?limit=2")
        body = res.get_json()
        assert res.status_code == 200
        assert body["ok"] is True
        assert len(body["trips"]) == 2
        assert body["trips"][0]["v_start"] == 12.7
        assert body["trips"][1]["v_start"] == 12.4
    finally:
        config.TRIP_LOG_PATH = original
        os.unlink(path)


def run():
    tests = [v for k, v in globals().items() if k.startswith("test_") and callable(v)]
    passed = 0
    failed = 0
    for t in tests:
        try:
            t()
            passed += 1
            print(f"PASS {t.__name__}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
        except Exception as e:
            failed += 1
            print(f"ERROR {t.__name__}: {type(e).__name__}: {e}")
    print(f"\n{passed}/{len(tests)} trip_log tests passed")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    run()
