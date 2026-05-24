"""
Tests for the OBD-II history ring used by the dashboard sparklines.

Verifies state.update_obd appends [ts, value] pairs in order, caps at
OBD_HISTORY_MAX, handles non-numeric values gracefully, and that
snapshot() returns isolated copies that callers can mutate freely.
"""
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


def test_update_obd_appends_to_history_in_order():
    _reset_state()
    for i, (ts, v) in enumerate([(1000, 800.0), (1100, 1200.0), (1200, 1600.0)]):
        state.update_obd("rpm", v, ts_ms=ts)
    snap = state.snapshot()
    rpm_hist = snap["obd_history"]["rpm"]
    assert len(rpm_hist) == 3
    assert rpm_hist[0] == [1000, 800.0]
    assert rpm_hist[-1] == [1200, 1600.0]
    # last_obd should still hold the latest value
    assert snap["last_obd"]["rpm"] == 1600.0


def test_update_obd_caps_at_history_max():
    _reset_state()
    over = state.OBD_HISTORY_MAX + 20
    for i in range(over):
        state.update_obd("speed", float(i), ts_ms=10_000 + i)
    snap = state.snapshot()
    speed_hist = snap["obd_history"]["speed"]
    assert len(speed_hist) == state.OBD_HISTORY_MAX
    # Oldest entries should have been trimmed — first kept sample is
    # at index `over - OBD_HISTORY_MAX` => value equals that index.
    expected_first_value = float(over - state.OBD_HISTORY_MAX)
    assert speed_hist[0][1] == expected_first_value
    assert speed_hist[-1][1] == float(over - 1)


def test_update_obd_keeps_pids_separate():
    _reset_state()
    state.update_obd("rpm", 1500.0, ts_ms=100)
    state.update_obd("coolant_temp", 78.0, ts_ms=110)
    snap = state.snapshot()
    assert snap["obd_history"]["rpm"] == [[100, 1500.0]]
    assert snap["obd_history"]["coolant_temp"] == [[110, 78.0]]


def test_update_obd_skips_non_numeric_value_for_history():
    _reset_state()
    # Some PIDs (like fuel_type, vin) report strings — they should still
    # update last_obd but not crash on history append.
    state.update_obd("fuel_type", "gasoline", ts_ms=200)
    snap = state.snapshot()
    assert snap["last_obd"]["fuel_type"] == "gasoline"
    assert "fuel_type" not in snap["obd_history"]


def test_uart_listener_populates_history_through_dispatch():
    _reset_state()
    for ts, rpm in [(1000, 800.0), (1100, 2400.0), (1200, 1500.0)]:
        uart_listener.dispatch({
            "type": "OBD", "ts": ts, "pid": 0x0C, "name": "rpm",
            "value": rpm, "units": "rpm",
        })
    snap = state.snapshot()
    hist = snap["obd_history"]["rpm"]
    assert [s[1] for s in hist] == [800.0, 2400.0, 1500.0]
    assert [s[0] for s in hist] == [1000, 1100, 1200]


def test_snapshot_history_is_isolated_from_internal_state():
    _reset_state()
    state.update_obd("rpm", 1000.0, ts_ms=1)
    snap = state.snapshot()
    snap["obd_history"]["rpm"].append([99999, 9999.0])
    # The next snapshot must not reflect the caller's local mutation.
    snap2 = state.snapshot()
    assert len(snap2["obd_history"]["rpm"]) == 1


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
    print(f"\n{passed}/{len(tests)} obd_history tests passed")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    run()
