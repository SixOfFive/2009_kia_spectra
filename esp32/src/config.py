"""
ESP32 firmware configuration.

Non-secret tunable values live here in plaintext (checked into git).
Secret values (WiFi password, Compustar device key, MQTT credentials)
live in `secrets.py` which is .gitignored — copy `secrets.py.example`
to `secrets.py` and fill it in before deploying.
"""

# ----- Voltage monitoring -----

# Battery voltage at OBD-II pin 16 is scaled through a voltage divider
# (e.g. 30k over 10k) so the ADS1115 sees ~3V at 12V battery. These
# coefficients turn the raw ADC reading into an estimated battery V.
ADC_DIVIDER_RATIO = 4.0   # (R1 + R2) / R2 — pin 16 V = ADC V * ratio
ADC_REFERENCE_V = 3.3     # ADS1115 input range with default PGA = ±4.096V
ADC_CALIBRATION_OFFSET = 0.0  # post-divider correction; tweak after measuring known good battery

# Trigger thresholds. The ESP32 starts the car when V_BATTERY drops below
# LOW_V_TRIGGER for at least LOW_V_SUSTAIN_S consecutive seconds.
LOW_V_TRIGGER = 12.2       # volts
LOW_V_SUSTAIN_S = 300      # seconds (5 minutes) of sustained low before triggering
RUN_DURATION_S = 15 * 60   # how long to run the engine once started (15 min)

# Wakeup cadence. ESP32 deep-sleeps between voltage samples.
# Shorter = faster reaction but more average current.
WAKE_INTERVAL_S = 60       # check voltage once per minute when parked

# Minimum interval between successive start attempts (cool-down).
START_COOLDOWN_S = 2 * 3600  # don't re-trigger for at least 2 hours


# ----- Compustar RF -----

# Frequency for CC1101 (Hz). 1WSHR-PRO is 433.92 MHz nominal. Our specific
# FOB transmits ~82 kHz below this per rtl_433 -A measurement; receivers
# tolerate a few hundred kHz offset so the nominal value works fine.
RF_FREQUENCY_HZ = 433_920_000

# Number of repeat packets in a burst (mimics FOB hold-to-press behavior).
# The genuine FOB transmits 8 internal packet repeats per burst; that
# baseline plus a couple of extra burst attempts gives healthy margin.
RF_BURST_REPEATS = 8

# Inter-packet guard time in milliseconds. The genuine FOB leaves
# ~30-40 ms between bursts.
RF_GUARD_MS = 39


# ----- UART link to Pi -----

# UART pins on the ESP32 (chosen to avoid strapping pins).
UART_TX_PIN = 17
UART_RX_PIN = 16
UART_BAUDRATE = 115_200

# Pi power-gating MOSFET (P-channel AO3401A). Active LOW (GPIO low = MOSFET on).
PI_POWER_GPIO = 25
# How long to wait after sending SHUTDOWN before cutting power, in seconds.
PI_SHUTDOWN_GRACE_S = 30


# ----- CAN bus (TWAI) -----

# ESP32 TWAI controller pins (any free GPIOs work).
CAN_TX_PIN = 5
CAN_RX_PIN = 4
CAN_BAUDRATE = 500_000     # ISO 15765-4 mandates 500 kbps for cars 2008+

# How often to poll OBD-II PIDs while the engine is running.
OBD_POLL_INTERVAL_S = 1

# PIDs to poll regularly while the engine is running.
OBD_POLL_PIDS = (
    0x05,  # coolant_temp
    0x0C,  # rpm
    0x0D,  # speed
    0x11,  # throttle_position
    0x2F,  # fuel_level
    0x42,  # control_module_voltage
)


# ----- WiFi + reporting -----

# How often to report state via MQTT / web while WiFi is up.
REPORT_INTERVAL_S = 10


# ----- Logging -----

LOG_LEVEL = "info"  # debug / info / warn / error


# ----- Hardware watchdog -----

# WDT auto-resets the ESP32 if the main loop hangs for longer than the
# timeout. Set to comfortably exceed WAKE_INTERVAL_S + boot time so deep
# sleep doesn't trip it.
WDT_ENABLED = True
WDT_TIMEOUT_MS = 120_000   # 2 minutes; default WAKE_INTERVAL_S is 60s


# ----- Secrets (loaded from secrets.py, which is gitignored) -----

try:
    from secrets import (  # noqa: F401
        WIFI_SSID,
        WIFI_PASSWORD,
        MQTT_BROKER,
        MQTT_PORT,
        MQTT_USERNAME,
        MQTT_PASSWORD,
        MQTT_TOPIC_PREFIX,
        COMPUSTAR_REMOTE_ID,
        COMPUSTAR_PACKETS,
    )
except ImportError:
    # secrets.py doesn't exist yet. Provide sentinels so the rest of
    # the codebase loads — but the start logic will refuse to run.
    WIFI_SSID = None
    WIFI_PASSWORD = None
    MQTT_BROKER = None
    MQTT_PORT = 1883
    MQTT_USERNAME = None
    MQTT_PASSWORD = None
    MQTT_TOPIC_PREFIX = "vroom"
    COMPUSTAR_REMOTE_ID = None
    COMPUSTAR_PACKETS = None


def secrets_ready():
    """Return True iff all secrets needed for start operations are set."""
    if WIFI_SSID is None or WIFI_PASSWORD is None:
        return False
    # Late import to avoid the lib/* path needing to be set up at config
    # import time. validate_packets returns [] when packets dict has all
    # four buttons with valid 35-bit patterns.
    try:
        from lib import compustar
        return not compustar.validate_packets(COMPUSTAR_PACKETS)
    except ImportError:
        return COMPUSTAR_PACKETS is not None
