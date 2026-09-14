// obd_elm.h -- ELM327 reply parsing and OBD-II (SAE J1979) decoding for the
// board's OBD pages.
//
// Deliberately free of Arduino headers: the same file compiles on the board and
// on a PC, so the parsing is tested against known reply shapes without a car or
// a dongle (esp32-s3/tests/test_obd_elm.cpp). The car only answers with the
// ignition on, and a parser that is wrong in a way today's drive happens not to
// exercise is exactly the bug that turns up months later.
#pragma once
#include <cstddef>
#include <cstdint>

// ---- categories: one per OBD sub-page -----------------------------------------
// Adding a page is one row here. "codes" and "vehicle" also have content of
// their own (fault codes, VIN ...) beyond the PIDs tagged with them.
struct ObdCat {
  const char* key;     // URL segment: /obd/<key>
  const char* name;
  const char* blurb;   // one line for its card on the main OBD page
};
extern const ObdCat OBD_CATS[];
extern const int    OBD_CAT_COUNT;
int obdCatIndex(const char* key);   // -1 if unknown

enum ObdFmt : uint8_t { OF_NUM = 0, OF_FUELSYS, OF_OBDSTD, OF_FUELTYPE };

// ---- mode 01 PIDs shown on the pages ---------------------------------------------
// Adding a value is one row here, plus one case in obdDecode() if its formula is
// new. A row is only ever shown when the car reports the PID as supported.
struct ObdPid {
  uint8_t     pid;
  uint8_t     cat;     // index into OBD_CATS
  bool        hub;     // also shown on the main OBD page
  const char* key;     // JSON key, unique
  const char* name;
  const char* unit;    // JSON-ready, e.g. "\\u00b0C"; "" for text values
  uint8_t     dec;     // decimals to display
  ObdFmt      fmt;
  const char* tip;     // one line for the tooltip; no quotes or backslashes
};
extern const ObdPid OBD_PIDS[];
extern const int    OBD_PID_COUNT;

int         obdPidLen(uint8_t pid);        // data bytes needed to decode; 0 = unknown PID
bool        obdDecode(uint8_t pid, const uint8_t* d, int n, float* out);
const char* obdEnumText(ObdFmt fmt, int code);

// ---- ELM327 replies ------------------------------------------------------------------
// One message per responding ECU. CAN multi-frame output ("014" / "0: ..." /
// "1: ...") is joined back into one message. Text lines such as "SEARCHING...",
// "NO DATA" or "ELM327 v1.5" carry no bytes and are skipped.
struct ElmMsg { uint8_t b[96]; int n; };
int elmSplit(const char* reply, ElmMsg* msgs, int maxMsgs);

bool        elmVehicleAnswered(const char* reply);   // any OBD response message at all
const char* elmFailText(const char* reply);          // short reason when it did not
int         elmProtocolNum(const char* reply);       // ATDPN "A6" or "6" -> 6; -1 if none

// Data bytes following <mode|0x40> <pid>; returns the count, or -1 if absent.
int obdFindPid(const char* reply, uint8_t mode, uint8_t pid, uint8_t* data, int cap);

// PIDs 00/20/40/60 -> 32-bit maps OR-ed across ECUs into sup[base / 32].
// The caller zeroes sup before the first request.
bool obdParseSupported(const char* reply, uint8_t base, uint32_t sup[4]);
bool obdIsSupported(const uint32_t sup[4], uint8_t pid);

// Fault codes from a mode 03 / 07 / 0A reply, de-duplicated across ECUs.
void obdDtcText(uint8_t a, uint8_t b, char out[6]);
int  obdParseDtcs(const char* reply, uint8_t mode, char codes[][6], int max);

// Mode 09: the VIN (PID 02, CAN or legacy framing) and text items such as the
// calibration ID (04) or ECU name (0A).
bool obdParseVin(const char* reply, char out[18]);
bool obdParseText09(const char* reply, uint8_t pid, char* out, int cap);

// Readiness monitors from PID 01's four data bytes.
struct ObdMonitor { const char* name; bool available; bool complete; };
int obdMonitors(const uint8_t d[4], ObdMonitor* out, int max, bool* mil, int* dtcCount, bool* diesel);

// ---- the engine, as the ECU sees it ---------------------------------------------------
// One observation per poll, with enough hysteresis that a single bad reading cannot
// end a run. It has no clock of its own -- the caller passes millis() and the wall
// clock -- which is what lets the logic that feeds the run log be tested on a PC.
const float    OBD_ENG_RPM_MIN   = 300.0f;            // at or above: running under its own power
const uint8_t  OBD_ENG_STOP_N    = 2;                 // consecutive not-running readings to call a stop
const uint32_t OBD_ENG_MAX_RUN_S = 12UL * 3600UL;     // a longer PID 1F is not trusted to date a start

enum ObdEngState : uint8_t { OE_UNKNOWN = 0, OE_RUNNING, OE_STOPPED };
struct ObdEngine {
  ObdEngState state;
  uint8_t  notRunning;     // consecutive not-running readings so far
  uint32_t changeMs;       // millis() the current state began: the edge
  uint32_t updMs;          // millis() of the last reading that counted
  uint32_t firstStopMs;    // millis() of the first not-running reading in the current streak
  uint32_t onEpoch;        // wall-clock start of the current run; 0 = unknown
};
void obdEngReset(ObdEngine* e);
// rpmOk: the ECU answered PID 0C with `rpm`. carSilent: a whole pass got no answer --
// which only counts as a stop once the ECU had been reporting the engine running,
// because a car that never answered says nothing about its engine. runtimeS: PID 1F,
// seconds since the start, or -1. nowEpoch: wall clock, or 0 if not valid yet.
void obdEngObserve(ObdEngine* e, bool rpmOk, float rpm, bool carSilent, long runtimeS,
                   uint32_t nowMs, uint32_t nowEpoch);

// ---- CSV for the OBD log ----------------------------------------------------------------
// The logged columns: every PID except the Vehicle page's static facts. Writes
// indices into OBD_PIDS and returns how many.
int  obdLogColumns(int* cols, int max);
// A header cell: the key plus an ASCII unit -- "coolant_C", "speed_kmh", "rpm".
void obdCsvHeaderCell(const ObdPid& p, char* out, size_t cap);
