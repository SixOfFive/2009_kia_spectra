// Host-side test for esp32-s3/voltage_monitor/obd_elm.cpp -- the ELM327 parsing
// the OBD pages depend on, checked against reply shapes from the ELM327 datasheet
// and SAE J1979, with no car and no dongle.
//
// From the repo root (one command, wrapped here):
//
//   g++ -std=c++17 -Wall -Wextra -I esp32-s3/voltage_monitor
//       esp32-s3/voltage_monitor/obd_elm.cpp esp32-s3/tests/test_obd_elm.cpp
//       -o /tmp/test_obd_elm && /tmp/test_obd_elm
#include "obd_elm.h"

#include <cmath>
#include <cstdio>
#include <cstring>

static int g_checks = 0, g_fails = 0;
#define CHECK(cond)                                                      \
  do {                                                                   \
    g_checks++;                                                          \
    if (!(cond)) {                                                       \
      g_fails++;                                                         \
      std::printf("FAIL line %d: %s\n", __LINE__, #cond);                \
    }                                                                    \
  } while (0)

static bool near(float a, float b) { return std::fabs(a - b) < 0.001f; }

static void testPidReplies() {
  uint8_t d[16];
  float v = 0;

  // Plain reply, spaces on (the ELM327 default).
  CHECK(obdFindPid("41 0C 0B 20 \r\r>", 0x01, 0x0C, d, 16) == 2);
  CHECK(obdDecode(0x0C, d, 2, &v) && near(v, 712.0f));

  // Spaces off.
  CHECK(obdFindPid("410C1AF8\r\r>", 0x01, 0x0C, d, 16) == 2);
  CHECK(obdDecode(0x0C, d, 2, &v) && near(v, 1726.0f));     // J1979 worked example

  // An echoed command is bytes too, but not a response -- it must not match.
  CHECK(obdFindPid("010D\r41 0D 32 \r\r>", 0x01, 0x0D, d, 16) == 1 && d[0] == 0x32);

  // The PID asked for is absent.
  CHECK(obdFindPid("41 0D 32 \r\r>", 0x01, 0x0C, d, 16) == -1);

  // Two ECUs answer; the first matching message wins.
  CHECK(obdFindPid("41 05 5A \r41 05 5B \r\r>", 0x01, 0x05, d, 16) == 1 && d[0] == 0x5A);

  // Truncated data must not decode.
  CHECK(!obdDecode(0x0C, d, 1, &v));

  // Formulas.
  const uint8_t volts[] = { 0x36, 0xB0 };
  CHECK(obdDecode(0x42, volts, 2, &v) && near(v, 14.0f));     // J1979 worked example
  const uint8_t coolant[] = { 0x5A };
  CHECK(obdDecode(0x05, coolant, 1, &v) && near(v, 50.0f));
  const uint8_t trim[] = { 0x80 };
  CHECK(obdDecode(0x06, trim, 1, &v) && near(v, 0.0f));
  const uint8_t timing[] = { 0x90 };
  CHECK(obdDecode(0x0E, timing, 1, &v) && near(v, 8.0f));
  const uint8_t lambda[] = { 0x80, 0x00 };
  CHECK(obdDecode(0x44, lambda, 2, &v) && near(v, 1.0f));
}

static void testNoVehicle() {
  CHECK(!elmVehicleAnswered("SEARCHING...\rUNABLE TO CONNECT\r\r>"));
  CHECK(std::strstr(elmFailText("SEARCHING...\rUNABLE TO CONNECT\r\r>"), "ignition") != nullptr);
  CHECK(!elmVehicleAnswered("NO DATA\r\r>"));
  CHECK(!elmVehicleAnswered("\r\rELM327 v1.5\r\r>"));
  CHECK(!elmVehicleAnswered("12.4V\r\r>"));
  CHECK(!elmVehicleAnswered(""));
  CHECK(elmVehicleAnswered("SEARCHING...\r41 00 BE 3E B8 11 \r\r>"));
  CHECK(elmVehicleAnswered("BUS INIT: ...OK\r41 00 BE 1F A8 13 \r\r>"));
}

static void testSupported() {
  uint32_t sup[4] = { 0, 0, 0, 0 };
  // Engine ECU plus a second module; the maps are OR-ed.
  CHECK(obdParseSupported("41 00 BE 3E B8 11 \r41 00 80 00 00 01 \r\r>", 0x00, sup));
  CHECK(sup[0] == 0xBE3EB811u);
  CHECK(obdIsSupported(sup, 0x01));
  CHECK(!obdIsSupported(sup, 0x02));
  CHECK(obdIsSupported(sup, 0x05));
  CHECK(!obdIsSupported(sup, 0x0A));
  CHECK(obdIsSupported(sup, 0x0C));
  CHECK(obdIsSupported(sup, 0x0D));
  CHECK(!obdIsSupported(sup, 0x10));
  CHECK(obdIsSupported(sup, 0x1C));
  CHECK(obdIsSupported(sup, 0x20));          // "the next map exists"
  CHECK(!obdIsSupported(sup, 0x21));         // sup[1] still empty
  CHECK(obdParseSupported("41 20 80 01 80 01\r\r>", 0x20, sup));
  CHECK(obdIsSupported(sup, 0x21));
  CHECK(!obdIsSupported(sup, 0x00));
  CHECK(!obdIsSupported(sup, 0x81));
  CHECK(!obdParseSupported("NO DATA\r\r>", 0x40, sup));
}

static void testDtcs() {
  char c[8][6];

  // CAN: a count byte follows the mode.
  CHECK(obdParseDtcs("43 02 01 33 01 34 \r\r>", 0x03, c, 8) == 2);
  CHECK(!std::strcmp(c[0], "P0133") && !std::strcmp(c[1], "P0134"));
  CHECK(obdParseDtcs("43 00 \r\r>", 0x03, c, 8) == 0);

  // Legacy protocols: always three pairs, zero-padded.
  CHECK(obdParseDtcs("43 01 33 00 00 00 00\r\r>", 0x03, c, 8) == 1 && !std::strcmp(c[0], "P0133"));

  // System letters and hex digits.
  CHECK(obdParseDtcs("47 02 C1 23 2A 00 \r\r>", 0x07, c, 8) == 2);
  CHECK(!std::strcmp(c[0], "U0123") && !std::strcmp(c[1], "P2A00"));
  obdDtcText(0x41, 0x00, c[0]);
  CHECK(!std::strcmp(c[0], "C0100"));
  obdDtcText(0x80, 0x01, c[0]);
  CHECK(!std::strcmp(c[0], "B0001"));

  // Two ECUs reporting the same code: listed once.
  CHECK(obdParseDtcs("43 01 01 33 \r43 01 01 33 \r\r>", 0x03, c, 8) == 1);

  // Wrong mode is ignored; no data is zero codes.
  CHECK(obdParseDtcs("47 01 01 33 \r\r>", 0x03, c, 8) == 0);
  CHECK(obdParseDtcs("NO DATA\r\r>", 0x0A, c, 8) == 0);

  // CAN multi-frame with four codes.
  CHECK(obdParseDtcs("00A\r0: 43 04 01 33 01 34\r1: 01 35 01 71 00\r\r>", 0x03, c, 8) == 4);
  CHECK(!std::strcmp(c[3], "P0171"));
}

static void testVin() {
  char vin[18];
  // CAN multi-frame: "014" = 20 bytes, split across three frames.
  CHECK(obdParseVin("014\r0: 49 02 01 31 48 47\r1: 43 4D 38 32 36 33 33\r2: 41 30 30 34 33 35 32\r\r>", vin));
  CHECK(!std::strcmp(vin, "1HGCM82633A004352"));
  // Legacy: five sequence-numbered messages, the first zero-padded.
  CHECK(obdParseVin("49 02 01 00 00 00 31\r49 02 02 48 47 43 4D\r49 02 03 38 32 36 33\r"
                    "49 02 04 33 41 30 30\r49 02 05 34 33 35 32\r\r>", vin));
  CHECK(!std::strcmp(vin, "1HGCM82633A004352"));
  // Legacy, messages out of order.
  CHECK(obdParseVin("49 02 02 48 47 43 4D\r49 02 01 00 00 00 31\r49 02 03 38 32 36 33\r"
                    "49 02 05 34 33 35 32\r49 02 04 33 41 30 30\r\r>", vin));
  CHECK(!std::strcmp(vin, "1HGCM82633A004352"));
  CHECK(!obdParseVin("NO DATA\r\r>", vin));
  CHECK(!obdParseVin("49 02 01 31 48 47\r\r>", vin));        // too short to be a VIN
}

static void testSplitAndText() {
  ElmMsg m[4];
  CHECK(elmSplit("014\r0: 49 02 01 31 48 47\r1: 43 4D 38 32 36 33 33\r2: 41 30 30 34 33 35 32\r\r>", m, 4) == 1);
  CHECK(m[0].n == 20);
  CHECK(elmSplit("SEARCHING...\rBUS INIT: ...OK\rOK\r12.4V\rELM327 v1.5\r\r>", m, 4) == 0);

  char txt[40];
  CHECK(obdParseText09("49 0A 01 45 43 4D 00 2D 45 6E 67 69 6E 65\r\r>", 0x0A, txt, sizeof(txt)));
  CHECK(!std::strcmp(txt, "ECM-Engine"));
  CHECK(!obdParseText09("NO DATA\r\r>", 0x04, txt, sizeof(txt)));
}

static void testProtocolAndMonitors() {
  CHECK(elmProtocolNum("A6\r\r>") == 6);
  CHECK(elmProtocolNum("6\r\r>") == 6);
  CHECK(elmProtocolNum("A3\r\r>") == 3);
  CHECK(elmProtocolNum("A\r\r>") == 10);
  CHECK(elmProtocolNum("NO DATA\r\r>") == -1);

  const uint8_t d[4] = { 0x81, 0x07, 0x65, 0x04 };
  ObdMonitor mon[11];
  bool mil = false, diesel = true;
  int n = 0;
  CHECK(obdMonitors(d, mon, 11, &mil, &n, &diesel) == 11);
  CHECK(mil && n == 1 && !diesel);
  CHECK(mon[0].available && mon[0].complete);                    // misfire
  CHECK(!std::strcmp(mon[3].name, "Catalyst") && mon[3].available && mon[3].complete);
  CHECK(!mon[4].available);                                      // heated catalyst
  CHECK(!std::strcmp(mon[5].name, "Evaporative system") && mon[5].available && !mon[5].complete);
  CHECK(mon[8].available && mon[9].available && !mon[10].available);
}

static void testTables() {
  const uint8_t zeros[8] = { 0 };
  float v = 0;
  for (int i = 0; i < OBD_PID_COUNT; i++) {
    const ObdPid& p = OBD_PIDS[i];
    CHECK(p.cat < OBD_CAT_COUNT);
    CHECK(obdPidLen(p.pid) > 0);
    CHECK(obdDecode(p.pid, zeros, obdPidLen(p.pid), &v));
    CHECK(std::strchr(p.tip, '"') == nullptr && std::strchr(p.name, '"') == nullptr);
    for (int j = 0; j < i; j++) CHECK(std::strcmp(OBD_PIDS[j].key, p.key) != 0);
  }
  for (int i = 0; i < OBD_CAT_COUNT; i++) CHECK(obdCatIndex(OBD_CATS[i].key) == i);
  CHECK(obdCatIndex("nope") == -1);
  CHECK(!std::strcmp(obdEnumText(OF_FUELSYS, 2), "Closed loop"));
  CHECK(!std::strcmp(obdEnumText(OF_FUELTYPE, 1), "Gasoline"));
}

static void testEngineTracker() {
  // Silence before the ECU ever answered says nothing about the engine.
  ObdEngine e;
  obdEngReset(&e);
  obdEngObserve(&e, false, 0, true, -1, 1000, 1000000);
  obdEngObserve(&e, false, 0, true, -1, 2000, 1000001);
  CHECK(e.state == OE_UNKNOWN);

  // First running reading, with PID 1F: the start is dated by the ECU's counter.
  obdEngObserve(&e, true, 812, false, 12, 100000, 1000100);
  CHECK(e.state == OE_RUNNING && e.changeMs == 88000 && e.onEpoch == 1000088);

  // One zero reading is not a stop, and a running reading resets the streak.
  obdEngObserve(&e, true, 0, false, -1, 101000, 1000101);
  CHECK(e.state == OE_RUNNING && e.notRunning == 1);
  obdEngObserve(&e, true, 790, false, -1, 102000, 1000102);
  CHECK(e.state == OE_RUNNING && e.notRunning == 0);

  // Two zero readings are a stop, dated at the first of them.
  obdEngObserve(&e, true, 0, false, -1, 103000, 1000103);
  obdEngObserve(&e, true, 0, false, -1, 104000, 1000104);
  CHECK(e.state == OE_STOPPED && e.changeMs == 103000 && e.updMs == 104000 && e.onEpoch == 0);

  // Staying stopped keeps the view fresh without moving the edge.
  obdEngObserve(&e, true, 0, false, -1, 110000, 1000110);
  CHECK(e.state == OE_STOPPED && e.changeMs == 103000 && e.updMs == 110000);

  // A run time longer than millis() has existed is clamped, not wrapped.
  ObdEngine f;
  obdEngReset(&f);
  obdEngObserve(&f, true, 900, false, 600, 5000, 2000000);
  CHECK(f.state == OE_RUNNING && f.changeMs >= 1 && f.changeMs <= 5000 && f.onEpoch == 1999400);

  // An absurd run time is not trusted to date the start.
  ObdEngine big;
  obdEngReset(&big);
  obdEngObserve(&big, true, 900, false, 999999, 500000, 2000000);
  CHECK(big.state == OE_RUNNING && big.changeMs == 500000 && big.onEpoch == 2000000);

  // Key off after running: silence counts once the ECU had been answering.
  ObdEngine g;
  obdEngReset(&g);
  obdEngObserve(&g, true, 750, false, -1, 10000, 3000000);
  obdEngObserve(&g, false, 0, true, -1, 12000, 3000002);
  CHECK(g.state == OE_RUNNING);
  obdEngObserve(&g, false, 0, true, -1, 14000, 3000004);
  CHECK(g.state == OE_STOPPED && g.changeMs == 12000);

  // Once stopped, further silence is no reading at all -- the view goes stale.
  uint32_t upd = g.updMs;
  obdEngObserve(&g, false, 0, true, -1, 16000, 3000006);
  CHECK(g.updMs == upd);

  // A failed RPM read while the car answered other PIDs is no reading either.
  obdEngObserve(&g, false, 0, false, -1, 20000, 3000010);
  CHECK(g.updMs == upd);

  // No wall clock yet: the run still starts, just without an epoch.
  ObdEngine h;
  obdEngReset(&h);
  obdEngObserve(&h, true, 700, false, 5, 50000, 0);
  CHECK(h.state == OE_RUNNING && h.onEpoch == 0 && h.changeMs == 45000);

  // Ignition on, engine off: the ECU answers with RPM 0.
  ObdEngine k;
  obdEngReset(&k);
  obdEngObserve(&k, true, 0, false, -1, 1000, 1);
  obdEngObserve(&k, true, 0, false, -1, 2000, 2);
  CHECK(k.state == OE_STOPPED);

  // Idle RPM is running; cranking-speed noise below the floor is not.
  ObdEngine idle;
  obdEngReset(&idle);
  obdEngObserve(&idle, true, 300, false, -1, 1000, 1);
  CHECK(idle.state == OE_RUNNING);
  ObdEngine low;
  obdEngReset(&low);
  obdEngObserve(&low, true, 120, false, -1, 1000, 1);
  CHECK(low.state == OE_UNKNOWN && low.notRunning == 1);
}

static void testCsv() {
  char cell[40];
  auto cellFor = [&](const char* key) -> const char* {
    cell[0] = 0;
    for (int i = 0; i < OBD_PID_COUNT; i++)
      if (!std::strcmp(OBD_PIDS[i].key, key)) obdCsvHeaderCell(OBD_PIDS[i], cell, sizeof(cell));
    return cell;
  };
  CHECK(!std::strcmp(cellFor("rpm"), "rpm"));
  CHECK(!std::strcmp(cellFor("coolant"), "coolant_C"));
  CHECK(!std::strcmp(cellFor("speed"), "speed_kmh"));
  CHECK(!std::strcmp(cellFor("timing"), "timing_deg"));
  CHECK(!std::strcmp(cellFor("lambda"), "lambda"));
  CHECK(!std::strcmp(cellFor("throttle"), "throttle_pct"));
  CHECK(!std::strcmp(cellFor("maf"), "maf_gps"));
  CHECK(!std::strcmp(cellFor("ecuv"), "ecuv_V"));
  CHECK(!std::strcmp(cellFor("runtime"), "runtime_s"));
  CHECK(!std::strcmp(cellFor("warmups"), "warmups"));
  CHECK(!std::strcmp(cellFor("fuelsys"), "fuelsys"));

  int cols[64];
  int n = obdLogColumns(cols, 64);
  const int vehicle = obdCatIndex("vehicle");
  int expect = 0;
  for (int i = 0; i < OBD_PID_COUNT; i++)
    if (OBD_PIDS[i].cat != vehicle) expect++;
  CHECK(n == expect && n > 0);
  for (int c = 0; c < n; c++) CHECK(OBD_PIDS[cols[c]].cat != vehicle);

  // Every header cell and every enum text that lands in a cell is CSV-safe.
  for (int c = 0; c < n; c++) {
    obdCsvHeaderCell(OBD_PIDS[cols[c]], cell, sizeof(cell));
    bool safe = cell[0] != 0;
    for (const char* p = cell; *p; p++)
      if (*p <= 0x20 || *p >= 0x7F || *p == ',' || *p == '"') safe = false;
    CHECK(safe);
  }
  for (int code = 0; code <= 16; code++)
    CHECK(std::strchr(obdEnumText(OF_FUELSYS, code), ',') == nullptr);
}

int main() {
  testEngineTracker();
  testCsv();
  testPidReplies();
  testNoVehicle();
  testSupported();
  testDtcs();
  testVin();
  testSplitAndText();
  testProtocolAndMonitors();
  testTables();
  std::printf("%d checks, %d failed\n", g_checks, g_fails);
  return g_fails ? 1 : 0;
}
