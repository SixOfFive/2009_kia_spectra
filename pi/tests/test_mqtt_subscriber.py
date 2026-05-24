"""Tests for the MQTT command-subscriber message handler + state helpers."""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))

from pi.app import state  # noqa: E402
from pi.app.comms import mqtt_subscriber  # noqa: E402


class FakeLink:
    def __init__(self):
        self.sent = []
    def send(self, msg):
        self.sent.append(msg)


class FakeMsg:
    def __init__(self, payload_obj, topic="vroom/spectra/cmd"):
        self.topic = topic
        self.payload = json.dumps(payload_obj).encode("utf-8")


class FakeMsgRaw:
    def __init__(self, payload_bytes, topic="vroom/spectra/cmd"):
        self.topic = topic
        self.payload = payload_bytes


def _set_link(link):
    state.set_link(link)


def _clear_link():
    state.set_link(None)


# ----- state.send_to_esp32 -----

def test_send_to_esp32_returns_false_when_no_link():
    _clear_link()
    assert state.send_to_esp32({"type": "COMMAND"}) is False


def test_send_to_esp32_calls_link_when_set():
    link = FakeLink()
    _set_link(link)
    try:
        result = state.send_to_esp32({"type": "COMMAND", "cmd": "ping"})
        assert result is True
        assert len(link.sent) == 1
        assert link.sent[0] == {"type": "COMMAND", "cmd": "ping"}
    finally:
        _clear_link()


# ----- mqtt_subscriber._on_message -----

def test_allowed_command_is_forwarded_to_link():
    link = FakeLink()
    _set_link(link)
    try:
        mqtt_subscriber._on_message(None, None, FakeMsg({"cmd": "start_engine"}))
        assert len(link.sent) == 1
        msg = link.sent[0]
        assert msg["type"] == "COMMAND"
        assert msg["cmd"] == "start_engine"
    finally:
        _clear_link()


def test_disallowed_command_is_dropped():
    link = FakeLink()
    _set_link(link)
    try:
        mqtt_subscriber._on_message(None, None, FakeMsg({"cmd": "shutdown_pi"}))
        # shutdown_pi is NOT in the subscriber's whitelist (it's an ESP32-issued
        # command, not a Pi-to-ESP32 one)
        assert link.sent == []
    finally:
        _clear_link()


def test_extra_args_are_forwarded():
    link = FakeLink()
    _set_link(link)
    try:
        mqtt_subscriber._on_message(None, None, FakeMsg(
            {"cmd": "set_threshold", "value": 11.8},
        ))
        assert len(link.sent) == 1
        assert link.sent[0]["value"] == 11.8
    finally:
        _clear_link()


def test_bad_json_does_not_crash():
    link = FakeLink()
    _set_link(link)
    try:
        mqtt_subscriber._on_message(None, None, FakeMsgRaw(b"not-json"))
        assert link.sent == []
    finally:
        _clear_link()


def test_unknown_cmd_field_is_dropped():
    link = FakeLink()
    _set_link(link)
    try:
        mqtt_subscriber._on_message(None, None, FakeMsg({"cmd": "rm_rf_slash"}))
        assert link.sent == []
    finally:
        _clear_link()


def test_missing_cmd_field_is_dropped():
    link = FakeLink()
    _set_link(link)
    try:
        mqtt_subscriber._on_message(None, None, FakeMsg({"foo": "bar"}))
        assert link.sent == []
    finally:
        _clear_link()


def test_no_link_silently_drops_command():
    _clear_link()
    # Should not throw
    mqtt_subscriber._on_message(None, None, FakeMsg({"cmd": "ping"}))


def run():
    tests = [v for k, v in globals().items() if k.startswith("test_") and callable(v)]
    for t in tests:
        t()
        print(f"PASS {t.__name__}")
    print(f"\n{len(tests)}/{len(tests)} mqtt_subscriber tests passed")


if __name__ == "__main__":
    run()
