"""
Flask dashboard server for the in-cab 5" touchscreen + LAN access.

Routes:
    GET  /            -> dashboard.html (the main UI rendered in the surf kiosk)
    GET  /api/state   -> current STATE snapshot as JSON (browser polls this 1Hz)
    POST /api/command -> manually trigger start/stop/etc via the UI buttons —
                         forwarded to the ESP32 via the shared Esp32Link

The dashboard.html template renders TWO views — the gauges/controls
view (default) and a Leaflet map view (lazy-init on first toggle).
A floating button in the top-right swaps between them; tap-to-toggle
on touch hardware = mouse click in a regular browser.

The actual STATE dict and ESP32 link live in pi.app.state — see daemon.py
for the process layout.
"""

from flask import Flask, jsonify, render_template, request, abort

from pi.app import config, state, trip_log
from pi.app.comms import esp32_link


app = Flask(
    __name__,
    template_folder="templates",
    static_folder="static",
)


# Initial fields for STATE so the dashboard renders sensibly before any
# real messages arrive. The UART listener overwrites these as data flows.
state.update_state(
    v_battery=12.6,
    engine_running=False,
    esp32_state="boot",
    last_obd={
        "rpm": 0.0,
        "speed": 0.0,
        "coolant_temp": 24,
        "throttle_position": 0.0,
        "fuel_level": 75.0,
        "control_module_voltage": 12.6,
    },
)


_VALID_COMMANDS = (
    esp32_link.CMD_START_ENGINE,
    esp32_link.CMD_STOP_ENGINE,
    esp32_link.CMD_PING,
)


@app.route("/")
def dashboard():
    return render_template(
        "dashboard.html",
        poll_ms=config.DASHBOARD_POLL_MS,
        map_center=config.MAP_DEFAULT_CENTER,
        map_zoom=config.MAP_DEFAULT_ZOOM,
        map_tile_url=config.MAP_TILE_URL,
        map_tile_attribution=config.MAP_TILE_ATTRIBUTION,
    )


@app.route("/api/state")
def api_state():
    return jsonify(state.snapshot())


@app.route("/api/command", methods=["POST"])
def api_command():
    payload = request.get_json(silent=True) or {}
    cmd = payload.get("cmd")
    if cmd not in _VALID_COMMANDS:
        abort(400, "unknown command")

    try:
        sent = state.send_to_esp32(
            esp32_link.command(cmd, **{k: v for k, v in payload.items() if k != "cmd"})
        )
    except Exception as e:
        return jsonify({"ok": False, "cmd": cmd, "error": str(e)}), 502

    if not sent:
        return jsonify({"ok": False, "cmd": cmd, "error": "esp32 link not initialized"}), 503

    return jsonify({"ok": True, "cmd": cmd, "queued": True})


# Manual-transmit buttons on the dashboard. "start" reuses the existing
# CMD_START_ENGINE plumbing (which goes through _trigger_start + power-on
# sequencing); the other three go through CMD_TRANSMIT_BUTTON which just
# transmits the stored 35-bit packet for that button without touching
# the state machine. Both paths require COMPUSTAR_PACKETS to be populated
# in the ESP32-side secrets.py.
_TRANSMIT_BUTTONS = ("start", "lock", "unlock", "trunk")


@app.route("/api/transmit/<button>", methods=["POST"])
def api_transmit(button):
    """
    Trigger a manual RF transmit for one of the fob buttons.

    "start" goes through CMD_START_ENGINE which runs the full
    voltage-trigger handshake (power-on Pi, transition to STARTING,
    etc.) — useful when you actually want the engine to start AND
    the dashboard to come up.

    "lock" / "unlock" / "trunk" go through CMD_TRANSMIT_BUTTON which
    just renders the stored packet for that button and toggles GDO0
    on the CC1101 — no state-machine transition, no Pi power-on
    side-effects. Useful for routine lock/unlock from the dashboard.
    """
    if button not in _TRANSMIT_BUTTONS:
        return jsonify({"ok": False, "detail": f"unknown button {button!r}"}), 400

    if button == "start":
        cmd = esp32_link.command(
            esp32_link.CMD_START_ENGINE, trigger_source="manual",
        )
    else:
        cmd = esp32_link.command(
            esp32_link.CMD_TRANSMIT_BUTTON,
            button=button,
            trigger_source="manual",
        )

    try:
        sent = state.send_to_esp32(cmd)
    except Exception as e:
        return jsonify({"ok": False, "button": button, "error": str(e)}), 502
    if not sent:
        return jsonify({
            "ok": False, "button": button,
            "error": "esp32 link not initialized",
        }), 503
    return jsonify({"ok": True, "button": button, "queued": True})


@app.route("/api/trips")
def api_trips():
    """Return the last N persisted trips, newest first."""
    try:
        limit = int(request.args.get("limit", config.TRIP_LOG_DEFAULT_LIMIT))
    except (TypeError, ValueError):
        limit = config.TRIP_LOG_DEFAULT_LIMIT
    limit = max(1, min(limit, 200))
    try:
        trips = trip_log.read_recent(config.TRIP_LOG_PATH, limit=limit)
    except OSError as e:
        return jsonify({"ok": False, "error": str(e)}), 500
    return jsonify({"ok": True, "trips": trips, "open": trip_log.open_trip_snapshot()})


# Backwards-compatible aliases — kept for any code that previously did
# `from pi.app.display.server import STATE, update_state, update_obd`.
STATE = state.STATE
update_state = state.update_state
update_obd = state.update_obd


if __name__ == "__main__":
    # Manual run for development; the daemon is the production entrypoint.
    app.run(
        host=config.DISPLAY_BIND_HOST,
        port=config.DISPLAY_BIND_PORT,
        debug=True,
    )
