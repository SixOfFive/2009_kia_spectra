"""
Flask dashboard server for the in-cab 5" touchscreen + LAN access.

Routes:
    GET  /            -> dashboard.html (the main UI rendered in chromium kiosk)
    GET  /api/state   -> current state as JSON (browser polls this every 1s)
    POST /api/command -> manually trigger start/stop/etc. via the UI buttons

Shared state lives in module-level ``STATE`` and is updated by the UART
listener thread (separate file). For now we only mock the data — the real
listener gets wired up once the ESP32 is on the bench.
"""

from flask import Flask, jsonify, render_template, request, abort

from pi.app import config


app = Flask(
    __name__,
    template_folder="templates",
    static_folder="static",
)


# Module-level state. In production this is updated by the UART listener
# thread; here we initialize with mocked values so the UI renders sensibly
# before any hardware is attached.
STATE = {
    "v_battery": 12.6,
    "engine_running": False,
    "esp32_state": "idle",
    "last_start_ts": None,
    "last_obd": {
        "rpm": 0.0,
        "speed": 0.0,
        "coolant_temp": 24,
        "throttle_position": 0.0,
        "fuel_level": 75.0,
        "control_module_voltage": 12.6,
    },
    "wifi_rssi": None,
    "uptime_s": 0,
}


@app.route("/")
def dashboard():
    return render_template(
        "dashboard.html",
        poll_ms=config.DASHBOARD_POLL_MS,
    )


@app.route("/api/state")
def api_state():
    return jsonify(STATE)


@app.route("/api/command", methods=["POST"])
def api_command():
    payload = request.get_json(silent=True) or {}
    cmd = payload.get("cmd")
    if cmd not in ("start_engine", "stop_engine", "ping"):
        abort(400, "unknown command")
    # TODO: forward to the ESP32 via the UART link once it's wired up.
    # For now just acknowledge so the UI button feedback works.
    return jsonify({"ok": True, "cmd": cmd, "queued": True})


def update_state(**fields):
    """Merge fields into the shared STATE dict. Called by the UART listener."""
    STATE.update(fields)


def update_obd(name, value):
    """Update one OBD-II field by name."""
    STATE["last_obd"][name] = value


if __name__ == "__main__":
    # Manual run for development / testing.
    app.run(
        host=config.DISPLAY_BIND_HOST,
        port=config.DISPLAY_BIND_PORT,
        debug=True,
    )
