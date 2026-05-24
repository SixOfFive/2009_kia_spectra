"""
Tests for the Flask dashboard endpoints — specifically the new
/api/transmit/<button> manual-transmit surface.

We use Flask's test_client so no socket gets bound. The ESP32 link is
swapped out for a recording fake via state.set_link().
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))

from pi.app import state  # noqa: E402
from pi.app.display import server  # noqa: E402


class _FakeLink:
    """Records every send() so tests can assert on what went to the wire."""

    def __init__(self):
        self.sent = []

    def send(self, msg):
        self.sent.append(msg)


def _make_client_with_link():
    link = _FakeLink()
    state.set_link(link)
    client = server.app.test_client()
    return client, link


def _reset_link():
    state.set_link(None)


def test_transmit_start_forwards_start_engine_command():
    client, link = _make_client_with_link()
    try:
        res = client.post("/api/transmit/start")
        body = res.get_json()
        assert res.status_code == 200, body
        assert body["ok"] is True
        assert body["button"] == "start"
        # The fake link should have received exactly one COMMAND msg.
        assert len(link.sent) == 1
        msg = link.sent[0]
        assert msg["type"] == "COMMAND"
        assert msg["cmd"] == "start_engine"
        assert msg["trigger_source"] == "manual"
    finally:
        _reset_link()


def test_transmit_lock_returns_501_with_protocol_extension_detail():
    client, _link = _make_client_with_link()
    try:
        res = client.post("/api/transmit/lock")
        assert res.status_code == 501
        body = res.get_json()
        assert body["ok"] is False
        assert body["button"] == "lock"
        assert "CMD_TRANSMIT_BUTTON" in body["detail"]
    finally:
        _reset_link()


def test_transmit_unlock_and_trunk_also_501():
    client, _link = _make_client_with_link()
    try:
        for button in ("unlock", "trunk"):
            res = client.post(f"/api/transmit/{button}")
            assert res.status_code == 501, button
            body = res.get_json()
            assert body["ok"] is False
            assert body["button"] == button
    finally:
        _reset_link()


def test_transmit_unknown_button_returns_400():
    client, _link = _make_client_with_link()
    try:
        res = client.post("/api/transmit/foo")
        assert res.status_code == 400
        body = res.get_json()
        assert body["ok"] is False
        assert "foo" in body["detail"]
    finally:
        _reset_link()


def test_transmit_start_without_link_returns_503():
    state.set_link(None)
    client = server.app.test_client()
    res = client.post("/api/transmit/start")
    assert res.status_code == 503
    body = res.get_json()
    assert body["ok"] is False
    assert "link" in body["error"].lower()


def run():
    tests = [v for k, v in globals().items() if k.startswith("test_") and callable(v)]
    passed = 0
    failed = 0
    for t in tests:
        try:
            t()
            passed += 1
            print(f"PASS {t.__name__}")
        except AssertionError as e:
            failed += 1
            print(f"FAIL {t.__name__}: {e}")
        except Exception as e:
            failed += 1
            print(f"ERROR {t.__name__}: {type(e).__name__}: {e}")
    print(f"\n{passed}/{len(tests)} display_server tests passed")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    run()
