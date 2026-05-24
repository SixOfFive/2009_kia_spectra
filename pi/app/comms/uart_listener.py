"""
UART listener thread — reads messages from the ESP32 and updates STATE.

Runs as a background daemon thread inside the main process so it shares
STATE with the Flask request handlers (no IPC needed).

Message dispatch:
  STATUS  -> update v_battery, esp32_state, derived engine_running
  OBD     -> update last_obd[name]
  EVENT   -> append to events ring, update derived state on engine_started/stopped
  LOG     -> printed to stdout (systemd journal)
  COMMAND -> only shutdown_pi is meaningful from ESP32; we initiate `shutdown -h`
"""

import threading
import time

from pi.app import config, state, trip_log
from pi.app.comms import esp32_link


LOG_TAG = "[uart-listener]"


def listen_forever(link, poll_interval_s=0.05, on_message=None):
    """
    Read messages from ``link`` forever, dispatching each into STATE.

    :param link:             pi.app.comms.esp32_link.Esp32Link (or compatible)
    :param poll_interval_s:  sleep between empty recv() calls
    :param on_message:       optional callback invoked after dispatch with the raw msg
    """
    print(f"{LOG_TAG} starting")
    while True:
        try:
            msg = link.recv()
        except Exception as e:
            print(f"{LOG_TAG} ERROR recv: {e}")
            time.sleep(1.0)
            continue
        if msg is None:
            time.sleep(poll_interval_s)
            continue
        try:
            dispatch(msg)
        except Exception as e:
            print(f"{LOG_TAG} ERROR dispatch: {e} (msg={msg})")
        if on_message is not None:
            try:
                on_message(msg)
            except Exception as e:
                print(f"{LOG_TAG} ERROR on_message: {e}")


def dispatch(msg):
    """
    Apply one decoded message to STATE. Idempotent — safe to call repeatedly
    with the same input.
    """
    mtype = msg.get("type")

    if mtype == esp32_link.TYPE_STATUS:
        updates = {}
        if "v_battery" in msg:
            updates["v_battery"] = msg["v_battery"]
        if "state" in msg:
            updates["esp32_state"] = msg["state"]
            updates["engine_running"] = msg["state"] == "running"
        if "counter" in msg:
            updates["counter"] = msg["counter"]
        if updates:
            state.update_state(**updates)

    elif mtype == esp32_link.TYPE_OBD:
        name = msg.get("name")
        if name is not None:
            state.update_obd(name, msg.get("value"), ts_ms=msg.get("ts"))
            # Track peak values for the currently-open trip (if any).
            trip_log.update_peak(name, msg.get("value"))

    elif mtype == esp32_link.TYPE_EVENT:
        event_name = msg.get("event")
        detail = msg.get("detail", {}) or {}
        state.append_event({
            "ts": msg.get("ts"),
            "event": event_name,
            "detail": detail,
        })
        if event_name == esp32_link.EVENT_ENGINE_STARTED:
            state.update_state(engine_running=True, last_start_ts=msg.get("ts"))
            _open_trip_from_event(detail)
        elif event_name == esp32_link.EVENT_ENGINE_STOPPED:
            state.update_state(engine_running=False)
            _close_trip_from_event(detail)

    elif mtype == esp32_link.TYPE_LOG:
        level = msg.get("level", "info")
        body = msg.get("msg", "")
        print(f"{LOG_TAG} esp32 [{level}] {body}")

    elif mtype == esp32_link.TYPE_COMMAND:
        if msg.get("cmd") == esp32_link.CMD_SHUTDOWN_PI:
            handle_shutdown_request()


def _open_trip_from_event(detail):
    """Translate an engine_started EVENT into a new trip row in-memory."""
    snap = state.snapshot(recent_event_count=0)
    v_start = snap.get("v_battery")
    reason = detail.get("reason") if isinstance(detail, dict) else None
    trigger_source = (
        detail.get("trigger_source") if isinstance(detail, dict) else None
    )
    # Common case for the field-installed system: low-battery auto-start.
    # If the ESP32 didn't tag it explicitly, infer from the event chain
    # by leaving it as "unknown" — better than a misleading guess.
    trip_log.start_trip(
        v_start=v_start, reason=reason, trigger_source=trigger_source,
    )


def _close_trip_from_event(detail):
    """Finalize the in-memory trip and append it to the JSONL log."""
    snap = state.snapshot(recent_event_count=0)
    v_end = snap.get("v_battery")
    path = getattr(config, "TRIP_LOG_PATH", None)
    try:
        trip_log.end_trip(v_end=v_end, path=path)
    except OSError as e:
        print(f"{LOG_TAG} ERROR appending trip log: {e}")


def handle_shutdown_request():
    """
    The ESP32 told us to power down. It'll cut the rail after
    PI_SHUTDOWN_GRACE_S seconds; we have to halt cleanly before then.

    Uses `shutdown -h +1` so the user (or any active SSH session) gets a
    minute of warning rather than an instant cut.
    """
    print(f"{LOG_TAG} shutdown requested by ESP32")
    import subprocess
    try:
        subprocess.Popen(
            ["sudo", "shutdown", "-h", "+1", "Vroom ESP32 requested shutdown"]
        )
    except Exception as e:
        print(f"{LOG_TAG} ERROR initiating shutdown: {e}")


def start_thread(link, **kwargs):
    """Spawn listen_forever in a daemon thread, return the Thread."""
    t = threading.Thread(
        target=listen_forever,
        args=(link,),
        kwargs=kwargs,
        daemon=True,
        name="uart-listener",
    )
    t.start()
    return t
