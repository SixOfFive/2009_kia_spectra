"""
MQTT publisher thread — pushes STATE snapshots to a broker periodically.

Topics published (with MQTT_TOPIC_PREFIX from config):
    {prefix}/state    — full snapshot dict, retained (one per interval)
    {prefix}/event    — every individual event as it arrives (not retained)

Soft-failure design: if the broker is unreachable, log and back off rather
than crashing the daemon. The dashboard keeps running locally without MQTT.

paho-mqtt is the only third-party dependency — provisioned via provision.sh.
"""

import json
import threading
import time

from pi.app import config, state


LOG_TAG = "[mqtt]"


def publish_forever(interval_s=None, reconnect_delay_s=30):
    """Connect once, then publish STATE snapshots every ``interval_s``."""
    if not config.mqtt_ready():
        print(f"{LOG_TAG} disabled (MQTT_ENABLED=False or no broker in secrets)")
        return

    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        print(f"{LOG_TAG} paho-mqtt not installed — skipping MQTT")
        return

    interval = interval_s or config.MQTT_PUBLISH_INTERVAL_S
    client_id = f"vroom-{int(time.time())}"

    while True:
        try:
            client = mqtt.Client(client_id=client_id)
            if config.MQTT_USERNAME:
                client.username_pw_set(config.MQTT_USERNAME, config.MQTT_PASSWORD)
            client.connect(config.MQTT_BROKER, config.MQTT_PORT, keepalive=60)
            client.loop_start()
            print(f"{LOG_TAG} connected to {config.MQTT_BROKER}:{config.MQTT_PORT}")
            _publish_loop(client, interval)
            # _publish_loop returns only on disconnect — fall through to reconnect
        except Exception as e:
            print(f"{LOG_TAG} ERROR: {e} — retrying in {reconnect_delay_s}s")
            time.sleep(reconnect_delay_s)


def _publish_loop(client, interval_s):
    """Inner loop: publish a snapshot every ``interval_s`` until publish fails."""
    topic = f"{config.MQTT_TOPIC_PREFIX}/state"
    while True:
        snap = state.snapshot()
        try:
            client.publish(topic, json.dumps(snap, default=_json_default),
                           qos=0, retain=True)
        except Exception as e:
            print(f"{LOG_TAG} ERROR publish: {e}")
            return
        time.sleep(interval_s)


def _json_default(o):
    """Best-effort fallback for non-JSON-native types in STATE snapshots."""
    return str(o)


def start_thread():
    """Spawn publish_forever in a daemon thread, return the Thread."""
    t = threading.Thread(
        target=publish_forever, daemon=True, name="mqtt-publisher",
    )
    t.start()
    return t
