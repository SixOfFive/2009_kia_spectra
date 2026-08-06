// snmp_agent.cpp — see snmp_agent.h.
//
// Hand-rolled BER because the alternative is dragging in a full SNMP library
// for what amounts to "parse a nested TLV, walk a sorted table, emit a nested
// TLV". Roughly 300 lines, no allocations beyond the response buffer.

#include "snmp_agent.h"

// Enterprise subtree: 1.3.6.1.4.1.99999.8
// (.99999 is a squatted Private Enterprise Number — see docs/20-snmp-integration.md)
static const uint32_t BASE_OID[] = {1, 3, 6, 1, 4, 1, 99999, 8};
static const uint8_t  BASE_LEN   = sizeof(BASE_OID) / sizeof(BASE_OID[0]);

#define MAX_OID_LEN 32

// ---------------- BER primitives ----------------

// Encode a length. Returns bytes written.
static int encLen(uint8_t* b, uint32_t len) {
  if (len < 0x80) { b[0] = (uint8_t)len; return 1; }
  if (len <= 0xFF) { b[0] = 0x81; b[1] = (uint8_t)len; return 2; }
  b[0] = 0x82; b[1] = (uint8_t)(len >> 8); b[2] = (uint8_t)(len & 0xFF); return 3;
}

// Decode a length at *p, advancing p. Returns -1 on malformed input.
static int32_t decLen(const uint8_t*& p, const uint8_t* end) {
  if (p >= end) return -1;
  uint8_t f = *p++;
  if (!(f & 0x80)) return f;
  uint8_t n = f & 0x7F;
  if (n == 0 || n > 4 || p + n > end) return -1;
  uint32_t v = 0;
  while (n--) v = (v << 8) | *p++;
  return (int32_t)v;
}

// Minimal signed INTEGER encoding.
static int encInt(uint8_t* b, int32_t v) {
  uint8_t tmp[5]; int n = 0;
  if (v == 0) { tmp[n++] = 0; }
  else {
    // Emit big-endian, then trim redundant leading bytes.
    uint8_t raw[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    int i = 0;
    if (v > 0) { while (i < 3 && raw[i] == 0x00 && !(raw[i + 1] & 0x80)) i++; }
    else       { while (i < 3 && raw[i] == 0xFF &&  (raw[i + 1] & 0x80)) i++; }
    for (; i < 4; i++) tmp[n++] = raw[i];
  }
  int o = 0;
  b[o++] = SNMP_INT;
  o += encLen(b + o, n);
  memcpy(b + o, tmp, n); o += n;
  return o;
}

// Unsigned application types (Counter32/Gauge32/TimeTicks) — never negative,
// so a leading 0x00 is prepended if the top bit would otherwise set.
static int encUint(uint8_t* b, uint8_t tag, uint32_t v) {
  uint8_t tmp[5]; int n = 0;
  uint8_t raw[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
  int i = 0;
  while (i < 3 && raw[i] == 0x00) i++;
  if (raw[i] & 0x80) tmp[n++] = 0x00;
  for (; i < 4; i++) tmp[n++] = raw[i];
  int o = 0;
  b[o++] = tag;
  o += encLen(b + o, n);
  memcpy(b + o, tmp, n); o += n;
  return o;
}

static int encOctet(uint8_t* b, const char* s, size_t len) {
  int o = 0;
  b[o++] = SNMP_OCTET;
  o += encLen(b + o, len);
  memcpy(b + o, s, len); o += len;
  return o;
}

// Encode one OID sub-identifier in base-128, high bit set on continuation.
static int encSubId(uint8_t* b, uint32_t v) {
  uint8_t tmp[5]; int n = 0;
  tmp[n++] = (uint8_t)(v & 0x7F); v >>= 7;
  while (v) { tmp[n++] = (uint8_t)((v & 0x7F) | 0x80); v >>= 7; }
  for (int i = 0; i < n; i++) b[i] = tmp[n - 1 - i];
  return n;
}

static int encOid(uint8_t* b, const uint32_t* oid, uint8_t len) {
  uint8_t body[MAX_OID_LEN * 5]; int n = 0;
  n += encSubId(body + n, oid[0] * 40 + oid[1]);          // first two are packed
  for (int i = 2; i < len; i++) n += encSubId(body + n, oid[i]);
  int o = 0;
  b[o++] = SNMP_OIDTYPE;
  o += encLen(b + o, n);
  memcpy(b + o, body, n); o += n;
  return o;
}

// Parse an OID body into subidentifiers.
static int decOid(const uint8_t* p, int len, uint32_t* out, int maxOut) {
  if (len < 1) return 0;
  int n = 0;
  uint32_t first = p[0];
  // X.Y packed as 40X+Y; X is 2 for anything >= 80.
  if (first >= 80) { out[n++] = 2; out[n++] = first - 80; }
  else             { out[n++] = first / 40; out[n++] = first % 40; }
  uint32_t v = 0;
  for (int i = 1; i < len; i++) {
    v = (v << 7) | (p[i] & 0x7F);
    if (!(p[i] & 0x80)) { if (n < maxOut) out[n++] = v; v = 0; }
  }
  return n;
}

// Lexicographic OID compare: <0, 0, >0.
static int cmpOid(const uint32_t* a, int an, const uint32_t* b, int bn) {
  int m = an < bn ? an : bn;
  for (int i = 0; i < m; i++) {
    if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
  }
  return an == bn ? 0 : (an < bn ? -1 : 1);
}

// Build the full OID for table row `idx`: BASE.<leaf>.0
static int fullOid(const SnmpEntry* tab, size_t idx, uint32_t* out) {
  int n = 0;
  for (int i = 0; i < BASE_LEN; i++) out[n++] = BASE_OID[i];
  out[n++] = tab[idx].leaf;
  out[n++] = 0;
  return n;
}

// ---------------- agent ----------------

bool SnmpAgent::begin(uint16_t port, const char* community,
                      const SnmpEntry* table, size_t count) {
  _port = port;
  _community = community ? community : "public";
  _tab = table;
  _n = count;
  _up = _udp.begin(_port);
  return _up;
}

void SnmpAgent::poll() {
  if (!_up) return;
  int sz = _udp.parsePacket();
  if (sz <= 0) return;
  if (sz > 1400) { _udp.flush(); return; }
  // STATIC, not stack: the Arduino loop task gets an 8 KB stack and these
  // buffers plus the ones in handle() blew straight through it (the board
  // rebooted on the third SNMP request). poll() is only ever called from
  // loop(), single-threaded, so statics are safe here.
  static uint8_t in[1400];
  int len = _udp.read(in, sizeof(in));
  if (len <= 0) return;
  static uint8_t out[1400];
  int rlen = handle(in, len, out, sizeof(out));
  if (rlen > 0) {
    _udp.beginPacket(_udp.remoteIP(), _udp.remotePort());
    _udp.write(out, rlen);
    _udp.endPacket();
    _reqs++;
  }
}

// Parse the request and build the response in place.
int SnmpAgent::handle(const uint8_t* in, int len, uint8_t* out, int outMax) {
  const uint8_t* p = in;
  const uint8_t* end = in + len;

  if (p >= end || *p++ != SNMP_SEQUENCE) return 0;
  if (decLen(p, end) < 0) return 0;

  // version
  if (p >= end || *p++ != SNMP_INT) return 0;
  int32_t vl = decLen(p, end); if (vl < 0 || p + vl > end) return 0;
  int32_t version = 0; for (int i = 0; i < vl; i++) version = (version << 8) | *p++;
  if (version != 0 && version != 1) return 0;        // v1 / v2c only

  // community
  if (p >= end || *p++ != SNMP_OCTET) return 0;
  int32_t cl = decLen(p, end); if (cl < 0 || p + cl > end) return 0;
  if ((size_t)cl != _community.length() ||
      memcmp(p, _community.c_str(), cl) != 0) return 0;   // silent drop, like snmpd
  p += cl;

  // PDU
  if (p >= end) return 0;
  uint8_t pduType = *p++;
  if (decLen(p, end) < 0) return 0;
  const uint8_t GET = 0xA0, GETNEXT = 0xA1, GETBULK = 0xA5;
  if (pduType != GET && pduType != GETNEXT && pduType != GETBULK) return 0;  // SET etc. ignored

  // request-id
  if (p >= end || *p++ != SNMP_INT) return 0;
  int32_t rl = decLen(p, end); if (rl < 0 || p + rl > end) return 0;
  int32_t reqId = 0; for (int i = 0; i < rl; i++) reqId = (reqId << 8) | *p++;

  // error-status / non-repeaters, error-index / max-repetitions
  if (p >= end || *p++ != SNMP_INT) return 0;
  int32_t al = decLen(p, end); if (al < 0 || p + al > end) return 0;
  int32_t nonRep = 0; for (int i = 0; i < al; i++) nonRep = (nonRep << 8) | *p++;
  if (p >= end || *p++ != SNMP_INT) return 0;
  int32_t bl = decLen(p, end); if (bl < 0 || p + bl > end) return 0;
  int32_t maxRep = 0; for (int i = 0; i < bl; i++) maxRep = (maxRep << 8) | *p++;

  // varbind list
  if (p >= end || *p++ != SNMP_SEQUENCE) return 0;
  int32_t vbl = decLen(p, end); if (vbl < 0) return 0;
  const uint8_t* vbEnd = p + vbl; if (vbEnd > end) vbEnd = end;

  // Build the varbind payload first so lengths can be back-filled.
  // static for the same stack-budget reason as the buffers in poll().
  static uint8_t body[1200]; int bo = 0;
  int emitted = 0;

  while (p < vbEnd && emitted < 48) {
    if (*p++ != SNMP_SEQUENCE) break;
    int32_t one = decLen(p, vbEnd); if (one < 0) break;
    const uint8_t* oneEnd = p + one; if (oneEnd > vbEnd) oneEnd = vbEnd;

    if (p >= oneEnd || *p++ != SNMP_OIDTYPE) break;
    int32_t ol = decLen(p, oneEnd); if (ol < 0 || p + ol > oneEnd) break;
    uint32_t reqOid[MAX_OID_LEN];
    int reqN = decOid(p, ol, reqOid, MAX_OID_LEN);
    p = oneEnd;                                     // skip the (null) value

    // How many results this varbind produces: GETBULK repeats the walk.
    int reps = 1;
    if (pduType == GETBULK) {
      reps = maxRep; if (reps < 1) reps = 1; if (reps > 24) reps = 24;
    }

    uint32_t curOid[MAX_OID_LEN];
    int curN = reqN;
    memcpy(curOid, reqOid, sizeof(uint32_t) * reqN);

    for (int r = 0; r < reps && bo < (int)sizeof(body) - 220; r++) {
      int match = -1;
      if (pduType == GET) {
        for (size_t i = 0; i < _n; i++) {
          uint32_t eo[MAX_OID_LEN]; int en = fullOid(_tab, i, eo);
          if (cmpOid(curOid, curN, eo, en) == 0) { match = (int)i; break; }
        }
      } else {                                       // GETNEXT / GETBULK
        int best = -1; uint32_t bestOid[MAX_OID_LEN]; int bestN = 0;
        for (size_t i = 0; i < _n; i++) {
          uint32_t eo[MAX_OID_LEN]; int en = fullOid(_tab, i, eo);
          if (cmpOid(eo, en, curOid, curN) > 0) {     // strictly greater
            if (best < 0 || cmpOid(eo, en, bestOid, bestN) < 0) {
              best = (int)i; memcpy(bestOid, eo, sizeof(uint32_t) * en); bestN = en;
            }
          }
        }
        match = best;
        if (best >= 0) { memcpy(curOid, bestOid, sizeof(uint32_t) * bestN); curN = bestN; }
      }

      uint8_t vb[220]; int vo = 0;
      if (match < 0) {
        // Past the end of our tree (or unknown scalar).
        uint32_t eo[MAX_OID_LEN]; int en;
        if (pduType == GET) { memcpy(eo, curOid, sizeof(uint32_t) * curN); en = curN; }
        else                { memcpy(eo, curOid, sizeof(uint32_t) * curN); en = curN; }
        vo += encOid(vb + vo, eo, en);
        vb[vo++] = (pduType == GET) ? SNMP_NOSUCHOBJECT : SNMP_ENDOFMIBVIEW;
        vb[vo++] = 0x00;                              // zero-length exception
      } else {
        uint32_t eo[MAX_OID_LEN]; int en = fullOid(_tab, match, eo);
        vo += encOid(vb + vo, eo, en);
        SnmpValue val; val.type = SNMP_INT; val.i = 0; val.u = 0;
        _tab[match].get(val);
        switch (val.type) {
          case SNMP_INT:       vo += encInt(vb + vo, val.i); break;
          case SNMP_COUNTER32:
          case SNMP_GAUGE32:
          case SNMP_TIMETICKS: vo += encUint(vb + vo, val.type, val.u); break;
          case SNMP_OCTET:     vo += encOctet(vb + vo, val.s.c_str(), val.s.length()); break;
          default:             vb[vo++] = SNMP_NULL; vb[vo++] = 0x00; break;
        }
        if (pduType != GET) { memcpy(curOid, eo, sizeof(uint32_t) * en); curN = en; }
      }

      // wrap this varbind in its SEQUENCE
      uint8_t hdr[4]; int hn = 0;
      hdr[hn++] = SNMP_SEQUENCE; hn += encLen(hdr + hn, vo);
      if (bo + hn + vo > (int)sizeof(body)) break;
      memcpy(body + bo, hdr, hn); bo += hn;
      memcpy(body + bo, vb, vo);  bo += vo;
      emitted++;

      if (match < 0) break;                           // stop this bulk run
    }
  }

  if (emitted == 0) return 0;

  // ----- assemble: SEQ { version, community, PDU { reqId, 0, 0, SEQ{varbinds} } } -----
  static uint8_t pdu[1300]; int po = 0;
  po += encInt(pdu + po, reqId);
  po += encInt(pdu + po, 0);                          // error-status = noError
  po += encInt(pdu + po, 0);                          // error-index
  pdu[po++] = SNMP_SEQUENCE; po += encLen(pdu + po, bo);
  if (po + bo > (int)sizeof(pdu)) return 0;
  memcpy(pdu + po, body, bo); po += bo;

  static uint8_t msg[1350]; int mo = 0;
  mo += encInt(msg + mo, version);
  mo += encOctet(msg + mo, _community.c_str(), _community.length());
  msg[mo++] = 0xA2;                                   // GetResponse-PDU
  mo += encLen(msg + mo, po);
  if (mo + po > (int)sizeof(msg)) return 0;
  memcpy(msg + mo, pdu, po); mo += po;

  int o = 0;
  out[o++] = SNMP_SEQUENCE;
  o += encLen(out + o, mo);
  if (o + mo > outMax) return 0;
  memcpy(out + o, msg, mo); o += mo;
  return o;
}
