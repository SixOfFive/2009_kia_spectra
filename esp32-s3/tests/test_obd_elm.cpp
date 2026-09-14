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

int main() {
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
