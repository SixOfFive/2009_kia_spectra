"""
SNMPv2c read-only responder for vroom-specific metrics.

Serves a small custom OID tree under ``1.3.6.1.4.1.99999.7`` (unregistered
squatter prefix — same convention used in [[asus2snmp Asus router SNMP
project]]). GET and GETNEXT only, no SET. Designed to be polled by an
existing NMS (LibreNMS, Cacti, observium, snmpwalk from a laptop) so the
vroom unit shows up as just another data source under the operator's
private-enterprise subtree.

The standard host MIBs (sysUptime, ifTable, hrStorageTable, etc.) are
intentionally not implemented here — the host NMS gets those from
whatever else runs on the box. This module is *only* the vroom OIDs.

Why stdlib instead of pysnmp-lextudio
-------------------------------------

SNMPv2c GET/GETNEXT over UDP is roughly 200 lines of BER encoding and a
PDU parser. Pulling in pysnmp's full agent framework — with its OID
trees, MIB compilation, SMI types — for nine read-only scalar OIDs would
weigh more than the code we'd save. The asus2snmp project made the same
call for the same reason.

Wire format reminder (SNMPv2c message structure)
------------------------------------------------

::

    SEQUENCE {
      INTEGER version            -- 1 for SNMPv2c
      OCTET STRING community     -- e.g. "public"
      Get/GetNext/Response PDU [ctx 0|1|2] {
        INTEGER request-id
        INTEGER error-status     -- 0 for OK
        INTEGER error-index      -- 0 for OK
        SEQUENCE OF VarBind {
          SEQUENCE {
            OBJECT IDENTIFIER name
            ANY value            -- NULL for requests
          }
        }
      }
    }

Threading model
---------------

The responder is single-threaded — a UDP request handler that builds the
response inline and writes back. Each SNMP request hits ``state.snapshot()``
once and renders all requested OIDs from that snapshot, so values within
one response are mutually consistent.
"""

from __future__ import annotations

import logging
import socket
import time
from typing import Callable, Optional, Tuple


log = logging.getLogger("vroom.snmp")


# ----- BER (Basic Encoding Rules) primitives -----

# Universal tags
TAG_INTEGER = 0x02
TAG_OCTET_STRING = 0x04
TAG_NULL = 0x05
TAG_OID = 0x06
TAG_SEQUENCE = 0x30

# Application tags
TAG_IPADDRESS = 0x40
TAG_COUNTER32 = 0x41
TAG_GAUGE32 = 0x42
TAG_TIMETICKS = 0x43

# Context-specific PDU tags
TAG_GET_REQUEST = 0xA0
TAG_GETNEXT_REQUEST = 0xA1
TAG_GET_RESPONSE = 0xA2
TAG_SET_REQUEST = 0xA3
TAG_GETBULK_REQUEST = 0xA5  # we reject these

# SNMP error-status codes (subset)
ERR_NO_ERROR = 0
ERR_TOO_BIG = 1
ERR_NO_SUCH_NAME = 2  # used in v1; v2c uses noSuchObject exception value
ERR_GEN_ERR = 5
ERR_NO_ACCESS = 6

# v2c exception "values" sent inside Response PDUs (replace NULL value)
TAG_NO_SUCH_OBJECT = 0x80
TAG_NO_SUCH_INSTANCE = 0x81
TAG_END_OF_MIB_VIEW = 0x82


class BerError(Exception):
    pass


def _enc_len(n: int) -> bytes:
    if n < 0x80:
        return bytes([n])
    body = b""
    while n:
        body = bytes([n & 0xFF]) + body
        n >>= 8
    return bytes([0x80 | len(body)]) + body


def _dec_len(buf: bytes, off: int) -> Tuple[int, int]:
    """Return (length, next_offset)."""
    b = buf[off]
    if b < 0x80:
        return b, off + 1
    n = b & 0x7F
    if n == 0:
        raise BerError("indefinite length not supported")
    if off + 1 + n > len(buf):
        raise BerError("length runs past end of buffer")
    val = 0
    for i in range(n):
        val = (val << 8) | buf[off + 1 + i]
    return val, off + 1 + n


def _enc_tlv(tag: int, value: bytes) -> bytes:
    return bytes([tag]) + _enc_len(len(value)) + value


def _dec_tlv(buf: bytes, off: int) -> Tuple[int, bytes, int]:
    """Return (tag, value_bytes, next_offset)."""
    if off >= len(buf):
        raise BerError("read past end of buffer")
    tag = buf[off]
    length, after_len = _dec_len(buf, off + 1)
    end = after_len + length
    if end > len(buf):
        raise BerError(f"TLV length {length} runs past buffer end")
    return tag, buf[after_len:end], end


def enc_integer(n: int, tag: int = TAG_INTEGER) -> bytes:
    """Two's complement, shortest representation."""
    if n == 0:
        return _enc_tlv(tag, b"\x00")
    if n > 0:
        body = b""
        x = n
        while x:
            body = bytes([x & 0xFF]) + body
            x >>= 8
        # If high bit set on first byte, prepend 0x00 to keep it positive.
        if body[0] & 0x80:
            body = b"\x00" + body
    else:
        # Two's complement negative
        # Find minimum number of bytes that fit n
        nb = 1
        while not (-(1 << (8 * nb - 1)) <= n < (1 << (8 * nb - 1))):
            nb += 1
        body = (n & ((1 << (8 * nb)) - 1)).to_bytes(nb, "big")
    return _enc_tlv(tag, body)


def dec_integer(value: bytes) -> int:
    if not value:
        return 0
    n = int.from_bytes(value, "big", signed=(value[0] & 0x80) != 0)
    return n


def enc_unsigned(n: int, tag: int) -> bytes:
    """Encode a non-negative INTEGER under an application tag (Counter32,
    Gauge32, TimeTicks). Per RFC 2578, these are unsigned but encoded with
    the same INTEGER rules, so we still prepend 0x00 if the high bit is
    set in order to avoid ambiguity with two's-complement negatives."""
    if n < 0:
        raise ValueError(f"unsigned application type can't encode {n}")
    if n > 0xFFFFFFFF:
        n &= 0xFFFFFFFF
    return enc_integer(n, tag=tag)


def enc_octet_string(s: bytes | str, tag: int = TAG_OCTET_STRING) -> bytes:
    if isinstance(s, str):
        s = s.encode("utf-8")
    return _enc_tlv(tag, s)


def enc_null() -> bytes:
    return _enc_tlv(TAG_NULL, b"")


def enc_oid(oid: Tuple[int, ...]) -> bytes:
    if len(oid) < 2:
        raise ValueError("OID must have at least two arcs")
    if oid[0] > 2 or (oid[0] < 2 and oid[1] >= 40):
        raise ValueError(f"invalid first two arcs {oid[:2]}")
    body = bytes([oid[0] * 40 + oid[1]])
    for arc in oid[2:]:
        if arc < 0:
            raise ValueError("negative OID arc")
        if arc < 0x80:
            body += bytes([arc])
        else:
            # base-128, MSB set on all but the last byte
            chunks = []
            x = arc
            while x:
                chunks.append(x & 0x7F)
                x >>= 7
            chunks.reverse()
            for i in range(len(chunks) - 1):
                chunks[i] |= 0x80
            body += bytes(chunks)
    return _enc_tlv(TAG_OID, body)


def dec_oid(value: bytes) -> Tuple[int, ...]:
    if not value:
        raise BerError("empty OID")
    # The encoding combines first two arcs as 40*X + Y. For X in {0,1},
    # Y is in 0..39 (so the combined byte is < 80). For X == 2, Y is
    # unbounded — the combined value is >= 80 and we recover (2, byte-80).
    first = value[0]
    if first < 80:
        arcs = [first // 40, first % 40]
    else:
        arcs = [2, first - 80]
    i = 1
    acc = 0
    while i < len(value):
        b = value[i]
        acc = (acc << 7) | (b & 0x7F)
        if not (b & 0x80):
            arcs.append(acc)
            acc = 0
        i += 1
    if acc:
        raise BerError("OID ended with continuation bit set")
    return tuple(arcs)


def enc_sequence(*items: bytes, tag: int = TAG_SEQUENCE) -> bytes:
    return _enc_tlv(tag, b"".join(items))


# ----- OID tree -----

# Vroom uses 1.3.6.1.4.1.99999.7 as its enterprise subtree (the .99999
# squatter prefix is documented in docs/20-snmp-integration.md).
VROOM_ENTERPRISE = (1, 3, 6, 1, 4, 1, 99999, 7)


def oid_under(*tail: int) -> Tuple[int, ...]:
    return VROOM_ENTERPRISE + tail


# OID metadata: each entry is (oid_tuple, snmp_tag, getter_fn(snapshot) -> python_value)
# Getter receives the state.snapshot() dict; returns the Python value to encode.
#
# Conventions:
#   - Integer-valued statuses map to TAG_INTEGER
#   - Counts map to TAG_COUNTER32
#   - Levels / instantaneous values map to TAG_GAUGE32
#   - Elapsed time maps to TAG_TIMETICKS (1/100-second units!)
#   - Strings map to TAG_OCTET_STRING
#
# Add new OIDs by appending entries below; the GETNEXT walk re-sorts on
# every request, so insertion order doesn't matter.

# State name -> integer for OID .1.0
RUN_STATE_INT = {
    "BOOT": 0,
    "MONITORING": 1,
    "STARTING": 2,
    "RUNNING": 3,
    "STOPPING": 4,
    "COOLDOWN": 5,
    "unknown": 99,
}


def _g_run_state(snap):
    return RUN_STATE_INT.get(snap.get("esp32_state") or "unknown", 99)


def _g_battery_mv(snap):
    v = snap.get("v_battery")
    return int(round(v * 1000)) if isinstance(v, (int, float)) else 0


def _g_engine_running(snap):
    return 1 if snap.get("engine_running") else 0


def _g_state_name(snap):
    return (snap.get("esp32_state") or "unknown")


def _g_uptime_ticks(snap):
    """TimeTicks are hundredths of a second."""
    return int((snap.get("uptime_s") or 0) * 100)


def _g_wifi_rssi(snap):
    r = snap.get("wifi_rssi")
    return int(r) if isinstance(r, (int, float)) else 0


def _g_event_count(snap):
    return len(snap.get("events") or [])


def _g_last_event_name(snap):
    events = snap.get("events") or []
    if not events:
        return ""
    last = events[-1]
    return str(last.get("event") or last.get("type") or "")


def _g_secs_since_start(snap):
    ts = snap.get("last_start_ts")
    if not ts:
        return 0
    elapsed = max(0, int(time.time() - ts))
    return elapsed * 100   # TimeTicks


def _g_esp32_message_counter(snap):
    c = snap.get("counter")
    return int(c) if isinstance(c, (int, float)) else 0


OID_TABLE = [
    (oid_under(1, 0), TAG_INTEGER,      _g_run_state,            "run_state (0=BOOT 1=MONITORING 2=STARTING 3=RUNNING 4=STOPPING 5=COOLDOWN)"),
    (oid_under(2, 0), TAG_GAUGE32,      _g_battery_mv,           "battery_mv (millivolts at the ESP32 ADC)"),
    (oid_under(3, 0), TAG_INTEGER,      _g_engine_running,       "engine_running (0|1)"),
    (oid_under(4, 0), TAG_OCTET_STRING, _g_state_name,           "state_name (free-form ESP32 state string)"),
    (oid_under(5, 0), TAG_TIMETICKS,    _g_uptime_ticks,         "esp32_uptime_ticks (hundredths of a second since ESP32 boot)"),
    (oid_under(6, 0), TAG_INTEGER,      _g_wifi_rssi,            "pi_wifi_rssi_dbm (signed dBm)"),
    (oid_under(7, 0), TAG_COUNTER32,    _g_event_count,          "event_count (events visible in the dashboard ring)"),
    (oid_under(8, 0), TAG_OCTET_STRING, _g_last_event_name,      "last_event_name"),
    (oid_under(9, 0), TAG_TIMETICKS,    _g_secs_since_start,     "ticks_since_last_engine_start (0 if never started)"),
    (oid_under(10, 0), TAG_COUNTER32,   _g_esp32_message_counter, "esp32_message_counter (UART frame sequence)"),
]


# ----- PDU build / parse -----

def _parse_request(buf: bytes, expected_community: str
                   ) -> Tuple[int, list[Tuple[int, ...]], int]:
    """Return (request-id, oid_list, pdu_tag).

    Raises BerError on malformed input or community mismatch.
    """
    msg_tag, msg_body, _ = _dec_tlv(buf, 0)
    if msg_tag != TAG_SEQUENCE:
        raise BerError(f"top-level not SEQUENCE: 0x{msg_tag:02X}")

    off = 0
    tag, value, off = _dec_tlv(msg_body, off)
    if tag != TAG_INTEGER:
        raise BerError("version field not INTEGER")
    version = dec_integer(value)
    if version != 1:  # 0 = v1, 1 = v2c
        raise BerError(f"unsupported SNMP version {version} (want v2c=1)")

    tag, value, off = _dec_tlv(msg_body, off)
    if tag != TAG_OCTET_STRING:
        raise BerError("community field not OCTET STRING")
    community = value.decode("utf-8", "replace")
    if community != expected_community:
        raise BerError(f"community mismatch (got {community!r})")

    tag, pdu_body, off = _dec_tlv(msg_body, off)
    if tag not in (TAG_GET_REQUEST, TAG_GETNEXT_REQUEST):
        raise BerError(f"unsupported PDU tag 0x{tag:02X}")

    pdu_off = 0
    _, value, pdu_off = _dec_tlv(pdu_body, pdu_off)
    request_id = dec_integer(value)
    _, _, pdu_off = _dec_tlv(pdu_body, pdu_off)  # error-status, ignored
    _, _, pdu_off = _dec_tlv(pdu_body, pdu_off)  # error-index, ignored

    vb_tag, vb_body, _ = _dec_tlv(pdu_body, pdu_off)
    if vb_tag != TAG_SEQUENCE:
        raise BerError("varbind list not SEQUENCE")

    oid_list = []
    vbo = 0
    while vbo < len(vb_body):
        one_tag, one_body, vbo = _dec_tlv(vb_body, vbo)
        if one_tag != TAG_SEQUENCE:
            raise BerError("varbind not SEQUENCE")
        oo = 0
        oid_tag, oid_value, oo = _dec_tlv(one_body, oo)
        if oid_tag != TAG_OID:
            raise BerError("varbind name not OBJECT IDENTIFIER")
        # We don't care about the value side on requests (should be NULL)
        oid_list.append(dec_oid(oid_value))
    return request_id, oid_list, tag


def _build_varbind(oid: Tuple[int, ...], tag: int, value) -> bytes:
    """Encode a (oid, value) pair. value is interpreted per tag."""
    if tag == TAG_INTEGER:
        v = enc_integer(int(value))
    elif tag == TAG_OCTET_STRING:
        v = enc_octet_string(value if isinstance(value, (bytes, str)) else str(value))
    elif tag in (TAG_COUNTER32, TAG_GAUGE32, TAG_TIMETICKS):
        v = enc_unsigned(int(value), tag)
    elif tag == TAG_NO_SUCH_OBJECT or tag == TAG_END_OF_MIB_VIEW:
        v = _enc_tlv(tag, b"")
    else:
        raise ValueError(f"unhandled tag 0x{tag:02X}")
    return enc_sequence(enc_oid(oid), v)


def _build_response(request_id: int, community: str,
                    varbind_list: list,
                    error_status: int = 0,
                    error_index: int = 0) -> bytes:
    """Build a complete SNMP Response datagram.

    ``varbind_list`` is a list of already-encoded varbind TLV bytes
    (each one a SEQUENCE { OID, value }).
    """
    pdu = _enc_tlv(
        TAG_GET_RESPONSE,
        enc_integer(request_id)
        + enc_integer(error_status)
        + enc_integer(error_index)
        + enc_sequence(*varbind_list),
    )
    return enc_sequence(
        enc_integer(1),               # v2c
        enc_octet_string(community),
        pdu,
    )


# ----- Request handling -----

def _lookup_get(oid: Tuple[int, ...]):
    """Return (tag, value, table_entry) for an exact-match GET, or
    (TAG_NO_SUCH_OBJECT, None, None) if missing."""
    for entry in OID_TABLE:
        if entry[0] == oid:
            return entry[1], entry[2], entry
    return TAG_NO_SUCH_OBJECT, None, None


def _lookup_getnext(oid: Tuple[int, ...]):
    """Return the first table entry with OID > the given one (lexicographic),
    or None if past the end of view."""
    sorted_entries = sorted(OID_TABLE, key=lambda e: e[0])
    for entry in sorted_entries:
        if entry[0] > oid:
            return entry
    return None


def handle_request(buf: bytes, snapshot_fn: Callable[[], dict],
                   community: str) -> Optional[bytes]:
    """Parse one SNMP datagram and return the response datagram (or None
    on malformed/unsupported input — we drop silently per RFC 3416)."""
    try:
        request_id, oid_list, pdu_tag = _parse_request(buf, community)
    except BerError as e:
        log.debug("dropping malformed SNMP packet: %s", e)
        return None

    snap = snapshot_fn()
    varbinds = []
    for req_oid in oid_list:
        if pdu_tag == TAG_GET_REQUEST:
            tag, getter, _ = _lookup_get(req_oid)
            if tag == TAG_NO_SUCH_OBJECT:
                varbinds.append(_build_varbind(req_oid, TAG_NO_SUCH_OBJECT, None))
            else:
                try:
                    varbinds.append(_build_varbind(req_oid, tag, getter(snap)))
                except Exception:
                    log.exception("getter for %s failed", req_oid)
                    varbinds.append(_build_varbind(req_oid, TAG_NO_SUCH_OBJECT, None))
        else:  # GETNEXT
            nxt = _lookup_getnext(req_oid)
            if nxt is None:
                varbinds.append(_build_varbind(req_oid, TAG_END_OF_MIB_VIEW, None))
            else:
                next_oid, tag, getter, _doc = nxt
                try:
                    varbinds.append(_build_varbind(next_oid, tag, getter(snap)))
                except Exception:
                    log.exception("getter for %s failed (GETNEXT)", next_oid)
                    varbinds.append(_build_varbind(next_oid, TAG_NO_SUCH_OBJECT, None))

    return _build_response(request_id, community, varbinds)


# ----- UDP daemon loop -----

def serve_forever(bind_host: str, port: int, community: str,
                  snapshot_fn: Callable[[], dict],
                  stop_event=None) -> None:
    """Block-serving UDP loop. ``stop_event`` is an optional threading.Event
    that causes the loop to exit cleanly (used by tests)."""
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((bind_host, port))
    sock.settimeout(0.5)
    log.info("SNMP responder listening on %s:%d (community=%r)",
             bind_host, port, community)
    try:
        while True:
            if stop_event is not None and stop_event.is_set():
                return
            try:
                data, addr = sock.recvfrom(8192)
            except socket.timeout:
                continue
            except OSError as e:
                log.warning("recvfrom error: %s", e)
                continue
            response = handle_request(data, snapshot_fn, community)
            if response is None:
                continue
            try:
                sock.sendto(response, addr)
            except OSError as e:
                log.warning("sendto %s error: %s", addr, e)
    finally:
        sock.close()


def main() -> None:
    """Entry point for `python -m pi.app.snmp_responder` (standalone) and
    for the systemd unit. Reads config from pi.app.secrets and pulls the
    state snapshot from pi.app.state."""
    import logging as _logging
    _logging.basicConfig(level=_logging.INFO,
                         format="%(asctime)s %(name)s %(levelname)s %(message)s")
    try:
        from pi.app import secrets
    except ImportError:
        log.error("pi/app/secrets.py not found — copy secrets.py.example first")
        raise SystemExit(1)
    from pi.app import state

    community = getattr(secrets, "SNMP_COMMUNITY", "public")
    port = getattr(secrets, "SNMP_PORT", 1161)
    bind_host = getattr(secrets, "SNMP_BIND_HOST", "0.0.0.0")

    serve_forever(bind_host, port, community, state.snapshot)


if __name__ == "__main__":
    main()
