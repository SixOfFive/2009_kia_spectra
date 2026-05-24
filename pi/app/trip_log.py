"""
Persistent trip log — one JSONL row per remote-start cycle.

A "trip" begins when the ESP32 fires an EVENT engine_started and ends
when it fires engine_stopped. Each row captures:

    ts_start         ISO8601 string
    ts_end           ISO8601 string or None (still open / crashed-before-close)
    reason           free-form short tag describing why the engine started
    v_start          float (battery voltage at the moment of start)
    v_end            float (battery voltage at engine_stopped) or None
    duration_s       int (whole-second duration) or None
    trigger_source   "low_voltage" | "manual" | "mqtt" | other tag
    peak_obd         dict of {pid_name: max_value_seen} during the trip
                     (currently tracks rpm + coolant_temp)

Persistence is append-only JSONL so the file is greppable, easy to tail,
and resistant to corruption from a sudden power cut mid-write — at worst
the last line is truncated and our reader skips it.

Thread-safety: every public function takes ``_TRIP_LOCK`` so the UART
listener (writes) and the Flask request handler for /api/trips (reads)
never interleave. The in-memory ``_OPEN_TRIP`` dict is the single source
of truth for the currently-running trip; on close we serialize it and
append. If the daemon crashes between start and stop, the open row is
lost (acceptable — we'd rather lose one trip than risk a half-broken
state machine on restart).
"""

import json
import os
import threading
from datetime import datetime, timezone


# Names of OBD-II PIDs we track peak values for during a trip. Keeping
# this small avoids bloating each trip row, and these two are the most
# useful for sanity-checking that the engine actually warmed up.
PEAK_PIDS = ("rpm", "coolant_temp")

_TRIP_LOCK = threading.Lock()
_OPEN_TRIP = {"trip": None}  # boxed so we can mutate inside the lock


def _iso_now():
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def _ensure_dir(path):
    d = os.path.dirname(path)
    if d and not os.path.isdir(d):
        try:
            os.makedirs(d, exist_ok=True)
        except OSError:
            # On a dev box without write access to /var/log we just swallow —
            # the append below will raise more loudly if the path truly fails.
            pass


def start_trip(*, v_start=None, reason=None, trigger_source=None,
               ts_start=None):
    """
    Begin a new in-memory trip row.

    Replaces any previously-open trip without writing it (the previous
    one's engine_stopped event never arrived — likely a crash or reboot).
    """
    trip = {
        "ts_start": ts_start or _iso_now(),
        "ts_end": None,
        "reason": reason,
        "v_start": v_start,
        "v_end": None,
        "duration_s": None,
        "trigger_source": trigger_source or "unknown",
        "peak_obd": {},
    }
    with _TRIP_LOCK:
        _OPEN_TRIP["trip"] = trip
    return trip


def update_peak(pid_name, value):
    """
    Record an OBD-II sample so we can keep the per-trip peak of each
    tracked PID. No-op if no trip is open or the PID isn't tracked.
    """
    if pid_name not in PEAK_PIDS or value is None:
        return
    try:
        v = float(value)
    except (TypeError, ValueError):
        return
    with _TRIP_LOCK:
        trip = _OPEN_TRIP["trip"]
        if trip is None:
            return
        peak = trip["peak_obd"].get(pid_name)
        if peak is None or v > peak:
            trip["peak_obd"][pid_name] = v


def end_trip(*, v_end=None, ts_end=None, duration_s=None, path=None):
    """
    Finalize the currently-open trip, append it to the JSONL file, and
    return the completed row. Returns None if no trip was open.
    """
    with _TRIP_LOCK:
        trip = _OPEN_TRIP["trip"]
        if trip is None:
            return None
        trip["ts_end"] = ts_end or _iso_now()
        trip["v_end"] = v_end
        if duration_s is not None:
            trip["duration_s"] = int(duration_s)
        else:
            trip["duration_s"] = _compute_duration_s(
                trip["ts_start"], trip["ts_end"],
            )
        _OPEN_TRIP["trip"] = None
        finalized = dict(trip)

    if path is not None:
        _append_jsonl(path, finalized)
    return finalized


def _compute_duration_s(ts_start, ts_end):
    try:
        s = datetime.fromisoformat(ts_start)
        e = datetime.fromisoformat(ts_end)
    except (TypeError, ValueError):
        return None
    return int((e - s).total_seconds())


def _append_jsonl(path, row):
    _ensure_dir(path)
    line = json.dumps(row, sort_keys=True) + "\n"
    with open(path, "a", encoding="utf-8") as fh:
        fh.write(line)


def read_recent(path, limit=20):
    """
    Return the last ``limit`` trip rows from the JSONL file, newest first.

    Tolerates partially-written / malformed trailing lines by skipping
    any line that fails to decode.
    """
    if not os.path.isfile(path):
        return []
    rows = []
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            try:
                rows.append(json.loads(line))
            except (ValueError, TypeError):
                continue
    rows.reverse()
    return rows[:limit]


def open_trip_snapshot():
    """Return a copy of the currently-open trip, or None."""
    with _TRIP_LOCK:
        trip = _OPEN_TRIP["trip"]
        if trip is None:
            return None
        # Shallow + nested copy of peak_obd so callers can't mutate state.
        return {**trip, "peak_obd": dict(trip["peak_obd"])}


def reset_for_tests():
    """Test helper — clear the in-memory open trip without writing it."""
    with _TRIP_LOCK:
        _OPEN_TRIP["trip"] = None
