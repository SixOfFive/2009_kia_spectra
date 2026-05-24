"""
Flask dashboard server for the in-cab 5" touchscreen + LAN access.

Routes:
    GET  /            -> dashboard.html (the main UI rendered in chromium kiosk)
    GET  /api/state   -> current STATE snapshot as JSON (browser polls this 1Hz)
    POST /api/command -> manually trigger start/stop/etc via the UI buttons —
                         forwarded to the ESP32 via the shared Esp32Link

The actual STATE dict and ESP32 link live in pi.app.state — see daemon.py
for the process layout.
"""

from flask import Flask, jsonify, render_template, request, abort

from pi.app import config, state
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
