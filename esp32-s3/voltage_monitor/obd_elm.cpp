// obd_elm.cpp -- see obd_elm.h. No Arduino headers, on purpose.
#include "obd_elm.h"

#include <cstdio>
#include <cstring>

// ---- tables -----------------------------------------------------------------------------

const ObdCat OBD_CATS[] = {
  { "engine",     "Engine",          "Speed, RPM, load, temperatures, throttle, airflow" },
  { "fuel",       "Fuel & air",      "Fuel system, fuel trims, oxygen sensors, pressures" },
  { "electrical", "Electrical",      "ECU supply voltage beside the dongle's and the board's own" },
  { "trip",       "Distance & time", "Run time, and distance and warm-ups since codes were cleared" },
  { "codes",      "Fault codes",     "Check-engine light, stored, pending and permanent codes, readiness" },
  { "vehicle",    "Vehicle",         "VIN, calibration, OBD standard, protocol, and the dongle itself" },
};
const int OBD_CAT_COUNT = sizeof(OBD_CATS) / sizeof(OBD_CATS[0]);

int obdCatIndex(const char* key) {
  for (int i = 0; i < OBD_CAT_COUNT; i++)
    if (!strcmp(OBD_CATS[i].key, key)) return i;
  return -1;
}

#define DEG "\\u00b0"
const ObdPid OBD_PIDS[] = {
  // pid  cat hub    key         name                                   unit      dec fmt
  { 0x0C, 0, true,  "rpm",      "Engine speed",                        "rpm",    0, OF_NUM,      "Crankshaft revolutions per minute" },
  { 0x0D, 0, true,  "speed",    "Vehicle speed",                       "km/h",   0, OF_NUM,      "Road speed as the ECU sees it" },
  { 0x05, 0, true,  "coolant",  "Coolant temperature",                 DEG "C",  0, OF_NUM,      "Engine coolant temperature" },
  { 0x04, 0, false, "load",     "Engine load",                         "%",      0, OF_NUM,      "Calculated load: airflow as a share of peak airflow" },
  { 0x11, 0, false, "throttle", "Throttle position",                   "%",      0, OF_NUM,      "Absolute throttle position" },
  { 0x0F, 0, false, "iat",      "Intake air temperature",              DEG "C",  0, OF_NUM,      "Air temperature at the intake" },
  { 0x0E, 0, false, "timing",   "Timing advance",                      DEG,      1, OF_NUM,      "Ignition timing before top dead centre, cylinder 1" },
  { 0x10, 0, false, "maf",      "Mass air flow",                       "g/s",    2, OF_NUM,      "Mass of air entering the engine" },
  { 0x0B, 0, false, "map",      "Intake manifold pressure",            "kPa",    0, OF_NUM,      "Absolute pressure in the intake manifold" },
  { 0x5C, 0, false, "oil",      "Oil temperature",                     DEG "C",  0, OF_NUM,      "Engine oil temperature" },
  { 0x03, 1, false, "fuelsys",  "Fuel system",                         "",       0, OF_FUELSYS,  "Open loop ignores the oxygen sensors; closed loop trims fuel by them" },
  { 0x06, 1, false, "stft",     "Short-term fuel trim",                "%",      1, OF_NUM,      "Fast fuel correction, bank 1. Positive means adding fuel" },
  { 0x07, 1, false, "ltft",     "Long-term fuel trim",                 "%",      1, OF_NUM,      "Learned fuel correction, bank 1. Positive means adding fuel" },
  { 0x14, 1, false, "o2s1",     "O2 sensor 1 (upstream)",              "V",      3, OF_NUM,      "Bank 1 sensor 1. Swings around 0.45 V in closed loop" },
  { 0x15, 1, false, "o2s2",     "O2 sensor 2 (downstream)",            "V",      3, OF_NUM,      "Bank 1 sensor 2, after the catalyst. Steadier than sensor 1 on a healthy catalyst" },
  { 0x44, 1, false, "lambda",   "Commanded air-fuel ratio",            "\\u03bb", 3, OF_NUM,     "Lambda; 1.000 is stoichiometric" },
  { 0x0A, 1, false, "fuelpres", "Fuel pressure",                       "kPa",    0, OF_NUM,      "Fuel pressure (gauge)" },
  { 0x2F, 1, false, "fuellvl",  "Fuel level",                          "%",      0, OF_NUM,      "Fuel tank level input" },
  { 0x33, 1, false, "baro",     "Barometric pressure",                 "kPa",    0, OF_NUM,      "Ambient absolute pressure" },
  { 0x42, 2, true,  "ecuv",     "ECU supply voltage",                  "V",      2, OF_NUM,      "Control module voltage: what the engine computer is being fed" },
  { 0x1F, 3, false, "runtime",  "Run time since start",                "s",      0, OF_NUM,      "Seconds since this engine start" },
  { 0x31, 3, false, "clrkm",    "Distance since codes cleared",        "km",     0, OF_NUM,      "Kilometres since the fault codes were last cleared" },
  { 0x30, 3, false, "warmups",  "Warm-ups since codes cleared",        "",       0, OF_NUM,      "Warm-up cycles since the fault codes were last cleared" },
  { 0x21, 3, false, "milkm",    "Distance with check-engine light on", "km",     0, OF_NUM,      "Kilometres driven with the check-engine light lit" },
  { 0x46, 3, false, "ambient",  "Ambient air temperature",             DEG "C",  0, OF_NUM,      "Outside air temperature" },
  { 0x1C, 5, false, "obdstd",   "OBD standard",                        "",       0, OF_OBDSTD,   "The OBD regulation the car was certified to" },
  { 0x51, 5, false, "fueltype", "Fuel type",                           "",       0, OF_FUELTYPE, "Fuel type as the ECU reports it" },
};
#undef DEG
const int OBD_PID_COUNT = sizeof(OBD_PIDS) / sizeof(OBD_PIDS[0]);

// Minimum data bytes needed to decode. PIDs that may send a second byte for a
// second bank or sensor (03, 06, 07, 14, 15) only need the first.
int obdPidLen(uint8_t pid) {
  switch (pid) {
    case 0x01:
      return 4;
    case 0x0C: case 0x10: case 0x1F: case 0x21: case 0x31: case 0x42: case 0x44:
      return 2;
    case 0x03: case 0x04: case 0x05: case 0x06: case 0x07: case 0x0A: case 0x0B: case 0x0D:
    case 0x0E: case 0x0F: case 0x11: case 0x14: case 0x15: case 0x1C: case 0x2F: case 0x30:
    case 0x33: case 0x46: case 0x51: case 0x5C:
      return 1;
    default:
      return 0;
  }
}

// SAE J1979 formulas. A = d[0], B = d[1].
bool obdDecode(uint8_t pid, const uint8_t* d, int n, float* out) {
  int need = obdPidLen(pid);
  if (need == 0 || n < need) return false;
  float A = d[0], B = (n > 1) ? d[1] : 0;
  switch (pid) {
    case 0x03: case 0x0B: case 0x0D: case 0x1C: case 0x30: case 0x33: case 0x51:
      *out = A; break;
    case 0x04: case 0x11: case 0x2F:
      *out = A * 100.0f / 255.0f; break;
    case 0x05: case 0x0F: case 0x46: case 0x5C:
      *out = A - 40.0f; break;
    case 0x06: case 0x07:
      *out = (A - 128.0f) * 100.0f / 128.0f; break;
    case 0x0A:
      *out = A * 3.0f; break;
    case 0x0C:
      *out = (256.0f * A + B) / 4.0f; break;
    case 0x0E:
      *out = A / 2.0f - 64.0f; break;
    case 0x10:
      *out = (256.0f * A + B) / 100.0f; break;
    case 0x14: case 0x15:
      *out = A / 200.0f; break;
    case 0x1F: case 0x21: case 0x31:
      *out = 256.0f * A + B; break;
    case 0x42:
      *out = (256.0f * A + B) / 1000.0f; break;
    case 0x44:
      *out = (256.0f * A + B) * 2.0f / 65536.0f; break;
    default:
      return false;
  }
  return true;
}

const char* obdEnumText(ObdFmt fmt, int code) {
  switch (fmt) {
    case OF_FUELSYS:
      switch (code) {
        case 0:  return "not reported";
        case 1:  return "Open loop (still warming up)";
        case 2:  return "Closed loop";
        case 4:  return "Open loop (load or deceleration)";
        case 8:  return "Open loop (system fault)";
        case 16: return "Closed loop (sensor fault)";
        default: return "unknown state";
      }
    case OF_OBDSTD:
      switch (code) {
        case 1:  return "OBD-II (California ARB)";
        case 2:  return "OBD (EPA)";
        case 3:  return "OBD and OBD-II";
        case 4:  return "OBD-I";
        case 5:  return "Not OBD compliant";
        case 6:  return "EOBD (Europe)";
        case 7:  return "EOBD and OBD-II";
        case 8:  return "EOBD and OBD";
        case 9:  return "EOBD, OBD and OBD-II";
        case 10: return "JOBD (Japan)";
        case 11: return "JOBD and OBD-II";
        case 12: return "JOBD and EOBD";
        case 13: return "JOBD, EOBD and OBD-II";
        default: return "other standard";
      }
    case OF_FUELTYPE:
      switch (code) {
        case 1:  return "Gasoline";
        case 2:  return "Methanol";
        case 3:  return "Ethanol";
        case 4:  return "Diesel";
        case 5:  return "LPG";
        case 6:  return "CNG";
        case 7:  return "Propane";
        case 8:  return "Electric";
        default: return "other fuel";
      }
    default:
      return "";
  }
}

// ---- ELM327 replies --------------------------------------------------------------------

static int hexv(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  return -1;
}

int elmSplit(const char* reply, ElmMsg* msgs, int maxMsgs) {
  int nm = 0;
  int multi = -1;        // message that "N:" frame lines are appended to
  int multiLen = 0;      // byte count the multi-frame header declared
  const char* p = reply;
  while (*p) {
    const char* e = p;
    while (*e && *e != '\r' && *e != '\n') e++;
    const char* s = p;
    while (s < e && *s == ' ') s++;
    const char* t = e;
    while (t > s && t[-1] == ' ') t--;

    if (t > s) {
      bool frame = (t - s) >= 2 && hexv(s[0]) >= 0 && s[1] == ':';
      const char* body = frame ? s + 2 : s;
      bool allHex = true;
      int digits = 0;
      for (const char* q = body; q < t; q++) {
        if (*q == ' ') continue;
        if (hexv(*q) < 0) { allHex = false; break; }
        digits++;
      }
      if (allHex && !frame && digits == 3 && (t - s) == 3) {
        // CAN multi-frame: a bare 3-digit byte count, then "0:", "1:" ... lines.
        if (nm < maxMsgs) {
          multi = nm;
          multiLen = hexv(s[0]) * 256 + hexv(s[1]) * 16 + hexv(s[2]);
          msgs[nm++].n = 0;
        }
      } else if (allHex && digits > 0 && digits % 2 == 0) {
        ElmMsg* m = nullptr;
        if (frame && multi >= 0) {
          m = &msgs[multi];
        } else if (nm < maxMsgs) {
          m = &msgs[nm];
          m->n = 0;
          multi = frame ? nm : -1;     // a headerless frame run still joins up
          multiLen = 0;
          nm++;
        }
        if (m) {
          int hi = -1;
          for (const char* q = body; q < t; q++) {
            int v = hexv(*q);
            if (v < 0) continue;
            if (hi < 0) {
              hi = v;
            } else {
              if (m->n < (int)sizeof(m->b)) m->b[m->n++] = (uint8_t)(hi * 16 + v);
              hi = -1;
            }
          }
          if (multiLen > 0 && m->n > multiLen) m->n = multiLen;
        }
      } else if (!frame) {
        multi = -1;                    // a text line ends any frame run
      }
    }
    p = *e ? e + 1 : e;
  }
  return nm;
}

bool elmVehicleAnswered(const char* reply) {
  ElmMsg m[8];
  int nm = elmSplit(reply, m, 8);
  for (int i = 0; i < nm; i++)
    if (m[i].n >= 1 && m[i].b[0] >= 0x41 && m[i].b[0] <= 0x4A) return true;
  return false;
}

const char* elmFailText(const char* reply) {
  if (strstr(reply, "UNABLE TO CONNECT")) return "no OBD bus found - is the ignition on?";
  if (strstr(reply, "NO DATA"))           return "the vehicle did not answer";
  if (strstr(reply, "CAN ERROR"))         return "CAN bus error";
  if (strstr(reply, "BUS BUSY"))          return "bus busy";
  if (strstr(reply, "BUS INIT") && strstr(reply, "ERROR")) return "bus initialisation failed";
  if (strstr(reply, "STOPPED"))           return "request interrupted";
  if (strstr(reply, "SEARCHING"))         return "still searching for a protocol";
  if (strchr(reply, '?'))                 return "the dongle did not understand the request";
  if (!*reply)                            return "no reply from the dongle";
  return "unrecognised reply";
}

int elmProtocolNum(const char* reply) {
  for (const char* p = reply; *p; p++) {
    bool startOfToken = (p == reply) || p[-1] == '\r' || p[-1] == '\n' || p[-1] == ' ';
    if (!startOfToken) continue;
    const char* q = (p[0] == 'A' && hexv(p[1]) >= 0) ? p + 1 : p;   // "A6" = automatic, found 6
    char end = q[1];
    if (hexv(q[0]) >= 0 && (end == 0 || end == '\r' || end == '\n' || end == ' ' || end == '>'))
      return hexv(q[0]);
  }
  return -1;
}

int obdFindPid(const char* reply, uint8_t mode, uint8_t pid, uint8_t* data, int cap) {
  ElmMsg m[8];
  int nm = elmSplit(reply, m, 8);
  for (int i = 0; i < nm; i++) {
    if (m[i].n >= 2 && m[i].b[0] == (uint8_t)(mode | 0x40) && m[i].b[1] == pid) {
      int k = 0;
      for (int j = 2; j < m[i].n && k < cap; j++) data[k++] = m[i].b[j];
      return k;
    }
  }
  return -1;
}

bool obdParseSupported(const char* reply, uint8_t base, uint32_t sup[4]) {
  if (base > 0x60 || base % 0x20) return false;
  ElmMsg m[8];
  int nm = elmSplit(reply, m, 8);
  bool any = false;
  for (int i = 0; i < nm; i++) {
    if (m[i].n >= 6 && m[i].b[0] == 0x41 && m[i].b[1] == base) {
      sup[base / 0x20] |= ((uint32_t)m[i].b[2] << 24) | ((uint32_t)m[i].b[3] << 16) |
                          ((uint32_t)m[i].b[4] << 8)  |  (uint32_t)m[i].b[5];
      any = true;
    }
  }
  return any;
}

bool obdIsSupported(const uint32_t sup[4], uint8_t pid) {
  if (pid == 0 || pid > 0x80) return false;
  int idx = (pid - 1) / 32, bit = 31 - (pid - 1) % 32;
  return (sup[idx] >> bit) & 1;
}

void obdDtcText(uint8_t a, uint8_t b, char out[6]) {
  static const char L[] = "PCBU";
  static const char H[] = "0123456789ABCDEF";
  out[0] = L[a >> 6];
  out[1] = (char)('0' + ((a >> 4) & 3));
  out[2] = H[a & 0xF];
  out[3] = H[b >> 4];
  out[4] = H[b & 0xF];
  out[5] = 0;
}

int obdParseDtcs(const char* reply, uint8_t mode, char codes[][6], int max) {
  ElmMsg m[8];
  int nm = elmSplit(reply, m, 8);
  int n = 0;
  for (int i = 0; i < nm; i++) {
    if (m[i].n < 1 || m[i].b[0] != (uint8_t)(mode | 0x40)) continue;
    // ISO 15765 (CAN) replies carry a count byte after the mode, which makes the
    // message length odd; the older protocols always send three pairs, even.
    int start = ((m[i].n - 1) % 2 == 1) ? 2 : 1;
    for (int j = start; j + 1 < m[i].n; j += 2) {
      if (!m[i].b[j] && !m[i].b[j + 1]) continue;    // padding
      char c[6];
      obdDtcText(m[i].b[j], m[i].b[j + 1], c);
      bool dup = false;
      for (int k = 0; k < n; k++)
        if (!strcmp(codes[k], c)) dup = true;
      if (!dup && n < max) memcpy(codes[n++], c, 6);
    }
  }
  return n;
}

bool obdParseVin(const char* reply, char out[18]) {
  ElmMsg m[8];
  int nm = elmSplit(reply, m, 8);
  char buf[64];
  int bn = 0;
  // CAN: one joined message, 49 02 01 then the 17 characters.
  for (int i = 0; i < nm && bn < 17; i++) {
    const ElmMsg& x = m[i];
    if (x.n >= 3 + 17 && x.b[0] == 0x49 && x.b[1] == 0x02) {
      bn = 0;
      for (int j = 3; j < x.n && bn < 63; j++)
        if (x.b[j] > 0x20 && x.b[j] < 0x7F) buf[bn++] = (char)x.b[j];
    }
  }
  // Older protocols: five messages 49 02 <seq> with 4 bytes each, the first
  // zero-padded, reassembled in sequence order.
  if (bn < 17) {
    bn = 0;
    for (int seq = 1; seq <= 5; seq++)
      for (int i = 0; i < nm; i++) {
        const ElmMsg& x = m[i];
        if (x.n >= 7 && x.b[0] == 0x49 && x.b[1] == 0x02 && x.b[2] == seq)
          for (int j = 3; j < 7 && bn < 63; j++)
            if (x.b[j] > 0x20 && x.b[j] < 0x7F) buf[bn++] = (char)x.b[j];
      }
  }
  if (bn < 17) return false;
  memcpy(out, buf + bn - 17, 17);
  out[17] = 0;
  return true;
}

bool obdParseText09(const char* reply, uint8_t pid, char* out, int cap) {
  ElmMsg m[8];
  int nm = elmSplit(reply, m, 8);
  int k = 0;
  for (int i = 0; i < nm && k < cap - 1; i++) {
    const ElmMsg& x = m[i];
    if (x.n < 3 || x.b[0] != 0x49 || x.b[1] != pid) continue;
    for (int j = 3; j < x.n && k < cap - 1; j++) {
      uint8_t c = x.b[j];
      if (c >= 0x20 && c < 0x7F && c != '"' && c != '\\') out[k++] = (char)c;
    }
  }
  while (k > 0 && out[k - 1] == ' ') k--;
  out[k] = 0;
  return k > 0;
}

int obdMonitors(const uint8_t d[4], ObdMonitor* out, int max, bool* mil, int* dtcCount, bool* diesel) {
  static const char* CONT[3]  = { "Misfire", "Fuel system", "Components" };
  static const char* SPARK[8] = { "Catalyst", "Heated catalyst", "Evaporative system", "Secondary air",
                                  "A/C refrigerant", "Oxygen sensor", "Oxygen sensor heater", "EGR / VVT" };
  static const char* COMP[8]  = { "NMHC catalyst", "NOx / SCR", "Reserved", "Boost pressure",
                                  "Reserved", "Exhaust gas sensor", "Particulate filter", "EGR / VVT" };
  *mil = d[0] & 0x80;
  *dtcCount = d[0] & 0x7F;
  *diesel = d[1] & 0x08;
  int n = 0;
  for (int i = 0; i < 3 && n < max; i++, n++) {
    out[n].name = CONT[i];
    out[n].available = (d[1] >> i) & 1;
    out[n].complete = out[n].available && !((d[1] >> (4 + i)) & 1);
  }
  for (int i = 0; i < 8 && n < max; i++, n++) {
    out[n].name = (*diesel ? COMP : SPARK)[i];
    out[n].available = (d[2] >> i) & 1;
    out[n].complete = out[n].available && !((d[3] >> i) & 1);
  }
  return n;
}

// ---- the engine, as the ECU sees it -------------------------------------------------------

void obdEngReset(ObdEngine* e) {
  memset(e, 0, sizeof(*e));
}

void obdEngObserve(ObdEngine* e, bool rpmOk, float rpm, bool carSilent, long runtimeS,
                   uint32_t nowMs, uint32_t nowEpoch) {
  bool running = rpmOk && rpm >= OBD_ENG_RPM_MIN;
  // Silence is only evidence of a stop when the engine had been seen running: the
  // key going off after a drive. From a car that never answered it means nothing.
  bool stopped = (rpmOk && rpm < OBD_ENG_RPM_MIN) || (carSilent && e->state == OE_RUNNING);
  if (!running && !stopped) return;                  // nothing learned from this pass
  e->updMs = nowMs;

  if (running) {
    e->notRunning = 0;
    if (e->state != OE_RUNNING) {
      e->state = OE_RUNNING;
      // PID 1F dates the start by the ECU's own counter; without it, the edge is
      // this reading. Never back-dated past the moment millis() began.
      bool haveRt = runtimeS >= 0 && (uint32_t)runtimeS <= OBD_ENG_MAX_RUN_S;
      uint32_t backMs = haveRt ? (uint32_t)runtimeS * 1000UL : 0;
      if (backMs >= nowMs) backMs = nowMs ? nowMs - 1 : 0;
      e->changeMs = nowMs - backMs;
      e->onEpoch = nowEpoch ? nowEpoch - (haveRt ? (uint32_t)runtimeS : 0) : 0;
    }
    return;
  }

  if (e->notRunning == 0) e->firstStopMs = nowMs;
  if (e->notRunning < 255) e->notRunning++;
  if (e->state != OE_STOPPED && e->notRunning >= OBD_ENG_STOP_N) {
    e->state = OE_STOPPED;
    e->changeMs = e->firstStopMs;                    // stopped by the first reading that said so
    e->onEpoch = 0;
  }
}

// ---- CSV for the OBD log ----------------------------------------------------------------

int obdLogColumns(int* cols, int max) {
  const int vehicle = obdCatIndex("vehicle");
  int n = 0;
  for (int i = 0; i < OBD_PID_COUNT && n < max; i++)
    if (OBD_PIDS[i].cat != vehicle) cols[n++] = i;
  return n;
}

void obdCsvHeaderCell(const ObdPid& p, char* out, size_t cap) {
  // Spreadsheets and scripts both cope better with plain ASCII column names, and a
  // unit that only repeats the key ("rpm") adds nothing.
  static const struct { const char* unit; const char* ascii; } MAP[] = {
    { "\\u00b0C", "C" }, { "\\u00b0", "deg" }, { "\\u03bb", "" }, { "km/h", "kmh" },
    { "g/s", "gps" },    { "%", "pct" },       { "rpm", "" },
  };
  const char* u = p.unit;
  for (const auto& m : MAP)
    if (!strcmp(p.unit, m.unit)) { u = m.ascii; break; }
  if (*u) snprintf(out, cap, "%s_%s", p.key, u);
  else    snprintf(out, cap, "%s", p.key);
}
