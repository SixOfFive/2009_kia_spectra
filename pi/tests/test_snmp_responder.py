"""
Tests for pi/app/snmp_responder.py.

Three layers:

1. BER round-trip tests (encode-then-decode for each primitive type)
2. PDU round-trip tests (build a GET/GETNEXT request, hand to
   handle_request, parse the response back, assert correct value)
3. Live UDP server smoke test (bind on an ephemeral port, send a real
   SNMP packet via socket, confirm the daemon responds)

Note: we intentionally don't depend on pysnmp here — that would defeat
the whole point of this module being stdlib-only. The tests are
self-contained: they encode the request bytes via the same BER helpers
the responder exports.
"""
import os
import socket
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", ".."))

from pi.app import snmp_responder as r  # noqa: E402


# ----- Fixtures -----

DEFAULT_SNAPSHOT = {
    "v_battery": 12.567,
    "engine_running": True,
    "esp32_state": "RUNNING",
    "last_start_ts": time.time() - 120,   # started 2 min ago
    "uptime_s": 3600,
    "wifi_rssi": -42,
    "events": [
        {"event": "low_voltage_trigger", "ts": time.time() - 130},
        {"event": "engine_started", "ts": time.time() - 120},
    ],
    "counter": 42,
}


def fake_snapshot(overrides=None):
    snap = dict(DEFAULT_SNAPSHOT)
    if overrides:
        snap.update(overrides)
    return lambda: snap


# Helper to build a complete SNMP GET / GETNEXT request datagram.
def build_request(req_id, community, oid_list, pdu_tag=r.TAG_GET_REQUEST):
    varbinds = b"".join(
        r.enc_sequence(r.enc_oid(oid), r.enc_null()) for oid in oid_list
    )
    pdu = r._enc_tlv(
        pdu_tag,
        r.enc_integer(req_id)
        + r.enc_integer(0)
        + r.enc_integer(0)
        + r.enc_sequence(varbinds, tag=r.TAG_SEQUENCE)
        if False else
        r.enc_integer(req_id)
        + r.enc_integer(0)
        + r.enc_integer(0)
        + r._enc_tlv(r.TAG_SEQUENCE, varbinds)
    )
    return r.enc_sequence(
        r.enc_integer(1),
        r.enc_octet_string(community),
        pdu,
    )


def parse_response(buf):
    """Return list of (oid_tuple, value_tag, value_bytes) from a Response
    PDU. Errors raise."""
    tag, body, _ = r._dec_tlv(buf, 0)
    assert tag == r.TAG_SEQUENCE
    off = 0
    _, _, off = r._dec_tlv(body, off)  # version
    _, _, off = r._dec_tlv(body, off)  # community
    pdu_tag, pdu, _ = r._dec_tlv(body, off)
    assert pdu_tag == r.TAG_GET_RESPONSE, f"got tag 0x{pdu_tag:02X}"
    poff = 0
    _, _, poff = r._dec_tlv(pdu, poff)  # req_id
    _, _, poff = r._dec_tlv(pdu, poff)  # err
    _, _, poff = r._dec_tlv(pdu, poff)  # errIdx
    vb_tag, vbs, _ = r._dec_tlv(pdu, poff)
    assert vb_tag == r.TAG_SEQUENCE
    out = []
    vo = 0
    while vo < len(vbs):
        _, inner, vo = r._dec_tlv(vbs, vo)
        io = 0
        _, oid_b, io = r._dec_tlv(inner, io)
        vt, vb, _ = r._dec_tlv(inner, io)
        out.append((r.dec_oid(oid_b), vt, vb))
    return out


def response_request_id(buf):
    tag, body, _ = r._dec_tlv(buf, 0)
    off = 0
    _, _, off = r._dec_tlv(body, off)
    _, _, off = r._dec_tlv(body, off)
    _, pdu, _ = r._dec_tlv(body, off)
    _, val, _ = r._dec_tlv(pdu, 0)
    return r.dec_integer(val)


# ----- BER primitive tests -----

def test_integer_roundtrip():
    for n in [0, 1, 127, 128, 255, 256, 32767, 32768, 65535, 99999, 1 << 30,
              -1, -128, -129, -32768, -(1 << 30)]:
        enc = r.enc_integer(n)
        tag, val, _ = r._dec_tlv(enc, 0)
        assert tag == r.TAG_INTEGER
        assert r.dec_integer(val) == n, f"{n} -> {r.dec_integer(val)}"


def test_unsigned_high_bit_gets_leading_zero():
    """Counter32(0x80000000) must have a leading 0x00 so it doesn't get
    misread as a negative INTEGER. RFC 1906 §3."""
    enc = r.enc_unsigned(0x80000000, r.TAG_COUNTER32)
    tag, val, _ = r._dec_tlv(enc, 0)
    assert tag == r.TAG_COUNTER32
    assert val[0] == 0x00, f"missing leading zero: {val.hex()}"
    assert val == b"\x00\x80\x00\x00\x00"


def test_oid_roundtrip():
    cases = [
        (1, 3, 6, 1),
        (1, 3, 6, 1, 4, 1, 99999, 7, 1, 0),
        (1, 3, 6, 1, 4, 1, 99999, 7, 10, 0),
        (1, 3, 6, 1, 4, 1, 0xFFFFFFF),       # 28-bit arc
        (0, 0),
        (2, 100, 999),
    ]
    for oid in cases:
        enc = r.enc_oid(oid)
        tag, val, _ = r._dec_tlv(enc, 0)
        assert tag == r.TAG_OID
        assert r.dec_oid(val) == oid


def test_octet_string_roundtrip():
    for s in ["", "x", "hello", "low_voltage_trigger", "RUNNING"]:
        enc = r.enc_octet_string(s)
        tag, val, _ = r._dec_tlv(enc, 0)
        assert tag == r.TAG_OCTET_STRING
        assert val.decode("utf-8") == s


def test_length_encoding_long_form():
    """Lengths >= 128 must use the multi-byte form (0x81 NN, 0x82 NN MM, ...)."""
    big_str = b"x" * 300
    enc = r.enc_octet_string(big_str)
    assert enc[1] == 0x82      # 2 length bytes follow
    assert (enc[2] << 8) | enc[3] == 300
    tag, val, _ = r._dec_tlv(enc, 0)
    assert val == big_str


# ----- PDU-level GET tests -----

def test_get_battery_mv():
    """OID .2.0 = battery_mv (Gauge32, millivolts)."""
    snap = fake_snapshot()
    req = build_request(1234, "public",
                        [(1, 3, 6, 1, 4, 1, 99999, 7, 2, 0)])
    resp = r.handle_request(req, snap, "public")
    vb = parse_response(resp)
    assert len(vb) == 1
    oid, tag, val = vb[0]
    assert oid == (1, 3, 6, 1, 4, 1, 99999, 7, 2, 0)
    assert tag == r.TAG_GAUGE32
    assert r.dec_integer(val) == 12567


def test_get_engine_running():
    snap = fake_snapshot()
    req = build_request(7, "public",
                        [(1, 3, 6, 1, 4, 1, 99999, 7, 3, 0)])
    resp = r.handle_request(req, snap, "public")
    oid, tag, val = parse_response(resp)[0]
    assert tag == r.TAG_INTEGER
    assert r.dec_integer(val) == 1


def test_get_state_name_octet_string():
    snap = fake_snapshot()
    req = build_request(7, "public",
                        [(1, 3, 6, 1, 4, 1, 99999, 7, 4, 0)])
    resp = r.handle_request(req, snap, "public")
    oid, tag, val = parse_response(resp)[0]
    assert tag == r.TAG_OCTET_STRING
    assert val == b"RUNNING"


def test_get_uptime_timeticks():
    """uptime_s=3600 -> TimeTicks = 360000 (hundredths)."""
    snap = fake_snapshot()
    req = build_request(7, "public",
                        [(1, 3, 6, 1, 4, 1, 99999, 7, 5, 0)])
    resp = r.handle_request(req, snap, "public")
    oid, tag, val = parse_response(resp)[0]
    assert tag == r.TAG_TIMETICKS
    assert r.dec_integer(val) == 360000


def test_get_negative_rssi_signed_integer():
    """Wi-Fi RSSI -42 dBm must encode as a negative INTEGER."""
    snap = fake_snapshot()
    req = build_request(7, "public",
                        [(1, 3, 6, 1, 4, 1, 99999, 7, 6, 0)])
    resp = r.handle_request(req, snap, "public")
    oid, tag, val = parse_response(resp)[0]
    assert tag == r.TAG_INTEGER
    assert r.dec_integer(val) == -42


def test_get_event_count_counter32():
    snap = fake_snapshot()
    req = build_request(7, "public",
                        [(1, 3, 6, 1, 4, 1, 99999, 7, 7, 0)])
    resp = r.handle_request(req, snap, "public")
    oid, tag, val = parse_response(resp)[0]
    assert tag == r.TAG_COUNTER32
    assert r.dec_integer(val) == 2


def test_get_last_event_name():
    snap = fake_snapshot()
    req = build_request(7, "public",
                        [(1, 3, 6, 1, 4, 1, 99999, 7, 8, 0)])
    resp = r.handle_request(req, snap, "public")
    oid, tag, val = parse_response(resp)[0]
    assert tag == r.TAG_OCTET_STRING
    assert val == b"engine_started"


def test_get_multiple_oids_in_one_request():
    """One GET, three OIDs -> one Response, three varbinds in order."""
    snap = fake_snapshot()
    req = build_request(99, "public", [
        (1, 3, 6, 1, 4, 1, 99999, 7, 2, 0),    # battery_mv
        (1, 3, 6, 1, 4, 1, 99999, 7, 4, 0),    # state_name
        (1, 3, 6, 1, 4, 1, 99999, 7, 6, 0),    # rssi
    ])
    resp = r.handle_request(req, snap, "public")
    vb = parse_response(resp)
    assert len(vb) == 3
    assert vb[0][2] == r.enc_integer(12567)[2:]   # value bytes only
    assert vb[1][2] == b"RUNNING"
    assert r.dec_integer(vb[2][2]) == -42


def test_get_unknown_oid_returns_no_such_object():
    snap = fake_snapshot()
    req = build_request(7, "public",
                        [(1, 3, 6, 1, 4, 1, 99999, 7, 999, 0)])
    resp = r.handle_request(req, snap, "public")
    oid, tag, val = parse_response(resp)[0]
    assert tag == r.TAG_NO_SUCH_OBJECT
    assert val == b""


def test_request_id_echoed():
    snap = fake_snapshot()
    for rid in [1, 42, 99999, 0x7FFFFFFF]:
        req = build_request(rid, "public",
                            [(1, 3, 6, 1, 4, 1, 99999, 7, 2, 0)])
        resp = r.handle_request(req, snap, "public")
        assert response_request_id(resp) == rid


def test_community_mismatch_drops_silently():
    snap = fake_snapshot()
    req = build_request(7, "secret123",
                        [(1, 3, 6, 1, 4, 1, 99999, 7, 2, 0)])
    resp = r.handle_request(req, snap, "public")
    assert resp is None, "wrong community should not get a response"


# ----- GETNEXT tests -----

def test_getnext_walks_to_first_oid():
    """GETNEXT on the enterprise prefix itself -> first OID in table."""
    snap = fake_snapshot()
    req = build_request(7, "public",
                        [(1, 3, 6, 1, 4, 1, 99999, 7)],
                        pdu_tag=r.TAG_GETNEXT_REQUEST)
    resp = r.handle_request(req, snap, "public")
    oid, tag, val = parse_response(resp)[0]
    assert oid == (1, 3, 6, 1, 4, 1, 99999, 7, 1, 0)
    assert tag == r.TAG_INTEGER
    # run_state value = RUN_STATE_INT["RUNNING"] = 3
    assert r.dec_integer(val) == 3


def test_getnext_walks_through_table():
    """Full WALK: GETNEXT-chain until END_OF_MIB_VIEW. Must visit every
    OID in OID_TABLE in lex order."""
    snap = fake_snapshot()
    visited = []
    cur = (1, 3, 6, 1, 4, 1, 99999, 7)
    for _ in range(50):   # bound the walk so a bug can't infinite-loop
        req = build_request(1, "public", [cur],
                            pdu_tag=r.TAG_GETNEXT_REQUEST)
        resp = r.handle_request(req, snap, "public")
        oid, tag, val = parse_response(resp)[0]
        if tag == r.TAG_END_OF_MIB_VIEW:
            break
        visited.append(oid)
        cur = oid
    expected = sorted(e[0] for e in r.OID_TABLE)
    assert visited == expected, (
        f"walk visited {visited}, expected {expected}"
    )


def test_getnext_past_end_returns_end_of_mib():
    snap = fake_snapshot()
    last = max(e[0] for e in r.OID_TABLE)
    req = build_request(7, "public", [last],
                        pdu_tag=r.TAG_GETNEXT_REQUEST)
    resp = r.handle_request(req, snap, "public")
    oid, tag, val = parse_response(resp)[0]
    assert tag == r.TAG_END_OF_MIB_VIEW


# ----- Edge cases -----

def test_missing_battery_voltage_returns_zero():
    snap = fake_snapshot({"v_battery": None})
    req = build_request(7, "public",
                        [(1, 3, 6, 1, 4, 1, 99999, 7, 2, 0)])
    resp = r.handle_request(req, snap, "public")
    oid, tag, val = parse_response(resp)[0]
    assert tag == r.TAG_GAUGE32
    assert r.dec_integer(val) == 0


def test_unknown_state_name_returns_99():
    snap = fake_snapshot({"esp32_state": "WEIRD_STATE"})
    req = build_request(7, "public",
                        [(1, 3, 6, 1, 4, 1, 99999, 7, 1, 0)])
    resp = r.handle_request(req, snap, "public")
    oid, tag, val = parse_response(resp)[0]
    assert r.dec_integer(val) == 99


def test_empty_events_yields_empty_last_event_name():
    snap = fake_snapshot({"events": []})
    req = build_request(7, "public",
                        [(1, 3, 6, 1, 4, 1, 99999, 7, 8, 0)])
    resp = r.handle_request(req, snap, "public")
    oid, tag, val = parse_response(resp)[0]
    assert tag == r.TAG_OCTET_STRING
    assert val == b""


def test_malformed_packet_dropped_silently():
    snap = fake_snapshot()
    # Truncated SEQUENCE — should drop, not crash
    assert r.handle_request(b"\x30\x05\x02", snap, "public") is None
    # Total junk
    assert r.handle_request(b"\xff\xff\xff", snap, "public") is None


# ----- Live UDP smoke test -----

def test_live_udp_server_responds():
    """Bind the responder on a free port, send a real packet via UDP,
    parse the response off the socket."""
    snap = fake_snapshot()
    stop = threading.Event()
    # Let the OS pick a free port by binding a temp socket first
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    probe.bind(("127.0.0.1", 0))
    port = probe.getsockname()[1]
    probe.close()

    t = threading.Thread(
        target=r.serve_forever,
        args=("127.0.0.1", port, "public", snap),
        kwargs={"stop_event": stop},
        daemon=True,
    )
    t.start()
    try:
        time.sleep(0.2)   # let the server bind

        client = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        client.settimeout(2.0)
        req = build_request(5555, "public",
                            [(1, 3, 6, 1, 4, 1, 99999, 7, 2, 0)])
        client.sendto(req, ("127.0.0.1", port))
        data, _ = client.recvfrom(8192)
        client.close()

        assert response_request_id(data) == 5555
        vb = parse_response(data)
        oid, tag, val = vb[0]
        assert tag == r.TAG_GAUGE32
        assert r.dec_integer(val) == 12567
    finally:
        stop.set()
        t.join(timeout=2.0)
        assert not t.is_alive(), "server thread didn't shut down"


# ----- Test harness -----

def run():
    tests = [v for k, v in globals().items()
             if k.startswith("test_") and callable(v)]
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
    print(f"\n{passed}/{len(tests)} snmp_responder tests passed")
    sys.exit(0 if failed == 0 else 1)


if __name__ == "__main__":
    run()
