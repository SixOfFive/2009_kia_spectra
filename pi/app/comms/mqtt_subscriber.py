"""
MQTT command subscriber — closes the loop so Home Assistant / Node-RED /
any MQTT client can trigger start_engine, stop_engine, ping, set_threshold
without going through the on-device dashboard.

Topic structure (relative to ``MQTT_TOPIC_PREFIX`` in config):
    <prefix>/cmd     -> incoming command, JSON body {"cmd": "...", ...}

The subscriber forwards the parsed command to the ESP32 via
``state.get_link().send(...)`` using the same Esp32Link instance the
Flask request handlers use. ``ESP32_LINK_LOCK`` is held during send to
avoid interleaving bytes between the dashboard and MQTT threads.

Example Home Assistant automation:

    service: mqtt.publish
    data:
      topic: vroom/spectra/cmd
      payload: '{"cmd": "start_engine"}'

Soft-failure: any exception (broker unreachable, malformed payload,
ESP32 link not initialized) is logged but doesn't kill the daemon.
"""

import json
import threading
import time

from pi.app import config, state  # noqa: F401  (state used via send_to_esp32)
from pi.app.comms import esp32_link


LOG_TAG = "[mqtt-sub]"

# Whitelist — only forward known commands to avoid arbitrary message
# injection if the broker is compromised or topic gets misused.
_ALLOWED_CMDS = {
    esp32_link.CMD_START_ENGINE,
    esp32_link.CMD_STOP_ENGINE,
    esp32_link.CMD_SET_THRESHOLD,
    esp32_link.CMD_PING,
}


def _on_message(client, userdata, msg):
    try:
        payload = json.loads(msg.payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as e:
        print(f"{LOG_TAG} bad payload on {msg.topic}: {e}")
        return

    cmd = payload.get("cmd")
    if cmd not in _ALLOWED_CMDS:
        print(f"{LOG_TAG} disallowed cmd {cmd!r} on {msg.topic} - ignoring")
        return

    extras = {k: v for k, v in payload.items() if k != "cmd"}
    try:
        sent = state.send_to_esp32(esp32_link.command(cmd, **extras))
    except Exception as e:
        print(f"{LOG_TAG} ERROR forwarding {cmd!r}: {e}")
        return
    if not sent:
        print(f"{LOG_TAG} ESP32 link not initialized - dropping {cmd!r}")
    else:
        print(f"{LOG_TAG} forwarded {cmd!r} to ESP32 (extras={extras})")


def subscribe_forever(reconnect_delay_s=30):
    if not config.mqtt_ready():
        print(f"{LOG_TAG} disabled (MQTT_ENABLED=False or no broker in secrets)")
        return

    try:
        import paho.mqtt.client as mqtt
    except ImportError:
        print(f"{LOG_TAG} paho-mqtt not installed - skipping MQTT subscribe")
        return

    topic = f"{config.MQTT_TOPIC_PREFIX}/cmd"
    client_id = f"vroom-sub-{int(time.time())}"

    while True:
        try:
            client = mqtt.Client(client_id=client_id)
            if config.MQTT_USERNAME:
                client.username_pw_set(config.MQTT_USERNAME, config.MQTT_PASSWORD)
            client.on_message = _on_message
            client.connect(config.MQTT_BROKER, config.MQTT_PORT, keepalive=60)
            client.subscribe(topic, qos=1)
            print(f"{LOG_TAG} connected, subscribed to {topic}")
            client.loop_forever()    # blocks until disconnect/error
            print(f"{LOG_TAG} loop_forever returned - reconnecting")
        except Exception as e:
            print(f"{LOG_TAG} ERROR: {e} - retrying in {reconnect_delay_s}s")
            time.sleep(reconnect_delay_s)


def start_thread():
    t = threading.Thread(
        target=subscribe_forever, daemon=True, name="mqtt-subscriber",
    )
    t.start()
    return t
