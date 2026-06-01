"""
Tests for the Flask dashboard endpoints — specifically the new
/api/transmit/<button> manual-transmit surface.

We use Flask's test_client so no socket gets bound. The ESP32 link is
swapped out for a recording fake via state.set_link().
"""
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


def test_transmit_lock_forwards_transmit_button_command():
    client, link = _make_client_with_link()
    try:
        res = client.post("/api/transmit/lock")
        body = res.get_json()
        assert res.status_code == 200, body
        assert body["ok"] is True
        assert body["button"] == "lock"
        assert len(link.sent) == 1
        msg = link.sent[0]
        assert msg["type"] == "COMMAND"
        assert msg["cmd"] == "transmit_button"
        assert msg["button"] == "lock"
        assert msg["trigger_source"] == "manual"
    finally:
        _reset_link()


def test_transmit_unlock_and_trunk_use_transmit_button():
    client, link = _make_client_with_link()
    try:
        for button in ("unlock", "trunk"):
            link.sent.clear()
            res = client.post(f"/api/transmit/{button}")
            assert res.status_code == 200, button
            body = res.get_json()
            assert body["ok"] is True
            assert body["button"] == button
            assert len(link.sent) == 1
            assert link.sent[0]["cmd"] == "transmit_button"
            assert link.sent[0]["button"] == button
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


# ----- Map view rendering -----

def test_dashboard_includes_map_view_and_toggle():
    """The root route renders a page with the view-toggle button, a
    hidden map container, the Leaflet CDN refs, and the map config
    constants injected from pi.app.config."""
    client = server.app.test_client()
    res = client.get("/")
    assert res.status_code == 200
    html = res.data.decode("utf-8")
    # Toggle button is present and starts with the map icon (initial
    # state is the gauges view, button offers the map).
    assert 'id="view-toggle"' in html
    assert "🗺" in html or "Map" in html  # button has a map-related label
    # Map container is in the page, hidden by default.
    assert 'id="view-map"' in html
    assert 'class="view-map view-hidden"' in html
    assert 'id="map"' in html
    # Leaflet CSS + JS refs from the public CDN.
    assert "leaflet@1.9.4/dist/leaflet.css" in html
    assert "leaflet@1.9.4/dist/leaflet.js" in html
    # Server-side map config bridged into JS land via window.MAP_*
    assert "window.MAP_CENTER" in html
    assert "window.MAP_ZOOM" in html
    assert "window.MAP_TILE_URL" in html


def test_dashboard_map_center_matches_config():
    """The injected MAP_CENTER must come from config.MAP_DEFAULT_CENTER
    (Edmonton city center as shipped). Renumber the assertion if the
    default is ever changed in pi/app/config.py."""
    import re
    from pi.app import config
    client = server.app.test_client()
    html = client.get("/").data.decode("utf-8")
    lat, lon = config.MAP_DEFAULT_CENTER
    # tojson renders the tuple as a JSON array [lat, lon]
    assert f"[{lat}, {lon}]" in html, (
        f"expected MAP_CENTER=[{lat}, {lon}] in rendered HTML"
    )
    # Zoom + tile URL also wired through. Jinja's tojson + indent can
    # introduce extra whitespace around the `=`, so match loosely.
    assert re.search(
        rf"window\.MAP_ZOOM\s*=\s*{config.MAP_DEFAULT_ZOOM}\s*;", html
    ), "MAP_ZOOM not injected"
    assert config.MAP_TILE_URL in html


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
