"""
Raspberry Pi application configuration.

Non-secret tunables live here. Secret values (WiFi, MQTT credentials,
optional API keys) live in `secrets.py` which is .gitignored — copy
`secrets.py.example` and fill in real values before running.
"""

# ----- UART link to ESP32 -----

# Pi's primary serial port. /dev/serial0 aliases to /dev/ttyAMA0 on the
# Pi Zero 2 W after disabling the serial console in raspi-config.
UART_PORT = "/dev/serial0"
UART_BAUDRATE = 115_200

# How long to wait for an ACK from the ESP32 after sending a command.
COMMAND_ACK_TIMEOUT_S = 5.0

# Poll interval for the UART listener thread (between recv() calls).
UART_POLL_INTERVAL_S = 0.05


# ----- Display server (Flask) -----

# Bind to 0.0.0.0 so other devices on the LAN can access the dashboard,
# not just localhost. Port chosen to avoid conflicts with default services.
DISPLAY_BIND_HOST = "0.0.0.0"
DISPLAY_BIND_PORT = 8000

# Dashboard polling interval in milliseconds (browser -> /api/state).
DASHBOARD_POLL_MS = 1000

# Maximum number of recent OBD-II sample points to keep in memory for charts.
OBD_HISTORY_POINTS = 600  # ~10 minutes at 1 sample/sec


# ----- State persistence -----

# Where to write log / state files. systemd will create this directory.
STATE_DIR = "/var/lib/vroom"
LOG_DIR = "/var/log/vroom"

# JSONL trip log — one line per remote-start cycle. See pi.app.trip_log.
TRIP_LOG_PATH = "/var/log/vroom/trips.jsonl"

# How many trips the /api/trips endpoint returns by default.
TRIP_LOG_DEFAULT_LIMIT = 20


# ----- MQTT publisher -----

# Whether to push state to an MQTT broker. Set to False to skip MQTT
# entirely. Even when True, missing MQTT_BROKER in secrets.py disables it.
MQTT_ENABLED = True

# How often to publish a state snapshot via MQTT.
MQTT_PUBLISH_INTERVAL_S = 10


# ----- Logging -----

LOG_LEVEL = "INFO"  # DEBUG / INFO / WARNING / ERROR


# ----- Secrets (loaded from secrets.py, which is gitignored) -----

try:
    from pi.app.secrets import (  # noqa: F401
        WIFI_SSID,
        WIFI_PASSWORD,
        MQTT_BROKER,
        MQTT_PORT,
        MQTT_USERNAME,
        MQTT_PASSWORD,
        MQTT_TOPIC_PREFIX,
    )
except ImportError:
    # secrets.py doesn't exist yet
    WIFI_SSID = None
    WIFI_PASSWORD = None
    MQTT_BROKER = None
    MQTT_PORT = 1883
    MQTT_USERNAME = None
    MQTT_PASSWORD = None
    MQTT_TOPIC_PREFIX = "vroom/spectra"


def mqtt_ready():
    """Return True iff MQTT is enabled and the broker is configured."""
    return MQTT_ENABLED and MQTT_BROKER is not None
