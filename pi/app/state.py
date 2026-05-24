"""
Shared application state for the Pi-side daemon.

Three threads coexist in the daemon process:
    - the Flask request handlers (reading STATE)
    - the UART listener (writing STATE from ESP32 messages)
    - the MQTT publisher (reading STATE for periodic snapshots)

All STATE mutations happen through the helper functions below, which take
the STATE_LOCK. ``snapshot()`` returns a safe shallow copy for serialization.
"""

import threading


STATE = {
    "v_battery": None,
    "engine_running": False,
    "esp32_state": "unknown",
    "last_start_ts": None,
    "last_obd": {},
    "events": [],
    "wifi_rssi": None,
    "uptime_s": 0,
    "counter": None,
}

STATE_LOCK = threading.Lock()

# Set by the daemon at startup. Module-level so Flask request handlers can
# reach it without passing context through every call. ``None`` means the
# UART link isn't up — request handlers should refuse to forward commands.
ESP32_LINK = None

# Held while writing to the ESP32 link from any thread (Flask, MQTT
# subscriber, etc.) to avoid interleaving JSON lines on the UART wire.
ESP32_LINK_LOCK = threading.Lock()


def send_to_esp32(msg):
    """Thread-safe send wrapper. Returns True if sent, False if no link."""
    link = ESP32_LINK
    if link is None:
        return False
    with ESP32_LINK_LOCK:
        link.send(msg)
    return True

# Maximum number of EVENT records retained for the dashboard's recent-events feed.
MAX_EVENTS = 100


def update_state(**fields):
    """Merge ``fields`` into STATE under the lock."""
    with STATE_LOCK:
        STATE.update(fields)


def update_obd(name, value):
    """Update one OBD-II field by name under the lock."""
    with STATE_LOCK:
        STATE.setdefault("last_obd", {})[name] = value


def append_event(event_dict):
    """Append an event record; trim to MAX_EVENTS retained."""
    with STATE_LOCK:
        events = STATE.setdefault("events", [])
        events.append(event_dict)
        if len(events) > MAX_EVENTS:
            del events[: len(events) - MAX_EVENTS]


def snapshot(recent_event_count=20):
    """Return a JSON-serializable shallow copy of STATE for outbound use."""
    with STATE_LOCK:
        events = STATE.get("events", [])
        return {
            "v_battery": STATE.get("v_battery"),
            "engine_running": STATE.get("engine_running"),
            "esp32_state": STATE.get("esp32_state"),
            "last_start_ts": STATE.get("last_start_ts"),
            "last_obd": dict(STATE.get("last_obd", {})),
            "events": list(events[-recent_event_count:]),
            "wifi_rssi": STATE.get("wifi_rssi"),
            "uptime_s": STATE.get("uptime_s"),
            "counter": STATE.get("counter"),
        }


def set_link(link):
    """Install the Esp32Link instance. Called once at daemon startup."""
    global ESP32_LINK
    ESP32_LINK = link


def get_link():
    return ESP32_LINK
