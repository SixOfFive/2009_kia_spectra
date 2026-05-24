"""
Pre-flight check - runs from your dev machine before flashing the ESP32 or
deploying the Pi. Catches missing secrets, sketchy threshold values, and
broken imports in <2 seconds so you don't burn ten minutes flashing only
to discover secrets.py is empty.

Usage:
    python tools/preflight.py
    python tools/preflight.py --side esp32
    python tools/preflight.py --side pi
    python tools/preflight.py --strict   # exit nonzero on warnings too

Exit codes:
    0  all checks passed (warnings allowed unless --strict)
    1  at least one error
    2  --strict and at least one warning
"""
import argparse
import importlib
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROJECT_ROOT = os.path.abspath(os.path.join(HERE, ".."))

OK = "OK   "
WARN = "WARN "
ERR = "FAIL "


class CheckResult:
    def __init__(self):
        self.errors = []
        self.warnings = []
        self.passed = []

    def ok(self, msg):
        self.passed.append(msg)
        print(f"  [{OK}] {msg}")

    def warn(self, msg):
        self.warnings.append(msg)
        print(f"  [{WARN}] {msg}")

    def err(self, msg):
        self.errors.append(msg)
        print(f"  [{ERR}] {msg}")


def section(title):
    print()
    print(f"=== {title} ===")


# ---------- ESP32 checks ----------

def check_esp32(res):
    section("ESP32 config + secrets")
    esp_src = os.path.join(PROJECT_ROOT, "esp32", "src")
    sys.path.insert(0, esp_src)

    try:
        config = importlib.import_module("config")
    except Exception as e:
        res.err(f"esp32/src/config.py failed to import: {e}")
        return

    # Required threshold sanity
    for name, lo, hi in [
        ("LOW_V_TRIGGER", 10.5, 13.0),
        ("LOW_V_SUSTAIN_S", 30, 3600),
        ("RUN_DURATION_S", 60, 1800),
        ("WAKE_INTERVAL_S", 10, 600),
        ("START_COOLDOWN_S", 600, 86400),
        ("RF_TE_US", 25, 1600),
        ("RF_BURST_REPEATS", 1, 10),
    ]:
        if not hasattr(config, name):
            res.err(f"config.{name} missing")
            continue
        v = getattr(config, name)
        if not (lo <= v <= hi):
            res.warn(f"config.{name} = {v} is outside sane range [{lo}, {hi}]")
        else:
            res.ok(f"config.{name} = {v}")

    # Secrets
    secrets_path = os.path.join(esp_src, "secrets.py")
    if not os.path.exists(secrets_path):
        res.warn(
            "esp32/src/secrets.py missing - copy secrets.py.example before flashing"
        )
    else:
        if config.secrets_ready():
            res.ok("config.secrets_ready() = True (WiFi + Compustar key + serial set)")
        else:
            res.warn("config.secrets_ready() = False - fill in WiFi or Compustar values")

        # Spot-check Compustar values for obvious placeholders
        for name in ("COMPUSTAR_DEVICE_KEY", "COMPUSTAR_SERIAL"):
            v = getattr(config, name, None)
            if v is None:
                res.warn(f"secrets.{name} is None - recover via sdr/07 before deploy")
            elif isinstance(v, int) and v in (0, 0x1234, 0xDEADBEEF, 0xDEADBEEFCAFEBABE):
                res.warn(f"secrets.{name} = {hex(v)} looks like a placeholder")
            else:
                res.ok(f"secrets.{name} populated (looks real)")

        counter = getattr(config, "COMPUSTAR_COUNTER", 0)
        if counter == 0:
            res.warn("secrets.COMPUSTAR_COUNTER = 0 - set to current FOB counter + 10 margin")
        else:
            res.ok(f"secrets.COMPUSTAR_COUNTER = {counter}")

    # Module imports
    section("ESP32 firmware modules")
    for mod in [
        "lib.keeloq", "lib.compustar", "lib.obd2", "lib.pi_link",
        "lib.cc1101", "lib.ads1115", "lib.twai_can", "lib.persistence",
        "controller",
    ]:
        try:
            importlib.import_module(mod)
            res.ok(f"import {mod}")
        except Exception as e:
            res.err(f"import {mod} failed: {e}")

    sys.path.remove(esp_src)


# ---------- Pi checks ----------

def check_pi(res):
    section("Pi config + secrets")
    sys.path.insert(0, PROJECT_ROOT)

    try:
        from pi.app import config as pi_config
    except Exception as e:
        res.err(f"pi/app/config.py failed to import: {e}")
        return

    for name in ("UART_PORT", "DISPLAY_BIND_PORT", "DASHBOARD_POLL_MS"):
        if hasattr(pi_config, name):
            res.ok(f"pi.config.{name} = {getattr(pi_config, name)}")
        else:
            res.err(f"pi.config.{name} missing")

    secrets_path = os.path.join(PROJECT_ROOT, "pi", "app", "secrets.py")
    if not os.path.exists(secrets_path):
        res.warn("pi/app/secrets.py missing - provision.sh will warn at runtime")
    else:
        if getattr(pi_config, "MQTT_BROKER", None):
            res.ok(f"MQTT broker configured: {pi_config.MQTT_BROKER}:{pi_config.MQTT_PORT}")
        else:
            res.warn("MQTT_BROKER not set in secrets - MQTT publish/subscribe disabled")

    section("Pi modules")
    for mod in [
        "pi.app.config", "pi.app.state",
        "pi.app.comms.esp32_link", "pi.app.comms.uart_listener",
        "pi.app.comms.mqtt_publisher", "pi.app.comms.mqtt_subscriber",
        "pi.app.daemon",
    ]:
        try:
            importlib.import_module(mod)
            res.ok(f"import {mod}")
        except Exception as e:
            res.err(f"import {mod} failed: {e}")

    # Flask is required for the display server
    try:
        importlib.import_module("flask")
        res.ok("flask installed")
    except ImportError:
        res.err("flask not installed - pip install flask")

    # paho-mqtt is optional but recommended
    try:
        importlib.import_module("paho.mqtt.client")
        res.ok("paho-mqtt installed (MQTT pub/sub available)")
    except ImportError:
        res.warn("paho-mqtt not installed - MQTT publish/subscribe will no-op")

    sys.path.remove(PROJECT_ROOT)


# ---------- Main ----------

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--side", choices=("esp32", "pi", "both"), default="both")
    ap.add_argument("--strict", action="store_true",
                    help="exit nonzero on warnings too")
    args = ap.parse_args()

    res = CheckResult()
    if args.side in ("esp32", "both"):
        check_esp32(res)
    if args.side in ("pi", "both"):
        check_pi(res)

    print()
    print(f"Summary: {len(res.passed)} OK, {len(res.warnings)} warnings, {len(res.errors)} errors")

    if res.errors:
        sys.exit(1)
    if args.strict and res.warnings:
        sys.exit(2)
    sys.exit(0)


if __name__ == "__main__":
    main()
