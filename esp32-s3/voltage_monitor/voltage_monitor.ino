// voltage_monitor.ino
// ESP32-S3-N16R8 voltage monitor with on-device history + live web dashboard.
//
//   * Reads a resistor divider on GPIO1, scales to the real source voltage.
//   * Records a rolling 24 h history (1440 samples @ 60 s) in PSRAM of:
//       voltage, chip temp, free heap, disk used, net bytes in, net bytes out.
//     History is snapshotted to the LittleFS filesystem every 10 min and reloaded
//     on boot, so it survives reboots. Served as CSV at /history -> the browser
//     RENDERS the graphs from the ESP32 (refresh / AP mode show real history).
//   * WiFi STA; on boot-failure OR 5 min of lost connection -> Access-Point
//     fallback (own DHCP/router at 192.168.4.1) so the dashboard stays reachable.
//
// Build / flash (KEEP the PartitionScheme - maps the full 16 MB incl. the FS):
//   arduino-cli compile --fqbn esp32:esp32:esp32s3:PSRAM=opi,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB -u -p COM6 <this folder>

#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <DNSServer.h>
#include <LittleFS.h>
#include <Update.h>
#include <SPI.h>
#include <time.h>
#include <Preferences.h>   // NVS-backed persistence for CPU clock + WiFi power-save
#include <Adafruit_GFX.h>
#include <Adafruit_ILI9341.h>
#include <XPT2046_Touchscreen.h>
#include <Fonts/FreeSansBold24pt7b.h>
#include <Fonts/FreeSans12pt7b.h>
#include "cc1101_compustar.h"
#include "snmp_agent.h"

// secrets.h (gitignored) holds WiFi credentials + the per-FOB captured
// Compustar patterns. The repo ships only secrets.h.example. Without a
// real secrets.h the firmware still builds and runs (AP-only, RF disabled)
// using the placeholders below — nothing secret is committed.
#if __has_include("secrets.h")
  #include "secrets.h"
#else
  #define SECRET_WIFI_SSID "your-wifi"
  #define SECRET_WIFI_PASS "your-password"
  #define SECRET_AP_PASS   "esp32volt"
  #define COMPUSTAR_PATTERNS_CAPTURED 0
  #define COMPUSTAR_START  "00000000000000000000000000000000000"
  #define COMPUSTAR_LOCK   "00000000000000000000000000000000000"
  #define COMPUSTAR_UNLOCK "00000000000000000000000000000000000"
  #define COMPUSTAR_TRUNK  "00000000000000000000000000000000000"
#endif

// ----------------------- user config -----------------------
// WiFi + AP credentials come from secrets.h (gitignored). See the
// secrets include block above for the placeholder fallback.
const char* WIFI_SSID = SECRET_WIFI_SSID;
const char* WIFI_PASS = SECRET_WIFI_PASS;
const char* HOSTNAME  = "esp32-volt";       // -> http://esp32-volt.local/

const char* AP_SSID   = "ESP32-Volt";       // fallback Access Point (its own DHCP @ 192.168.4.1)
const char* AP_PASS   = SECRET_AP_PASS;      // from secrets.h (8+ chars, or "" for open)

const char* FW_VERSION = "2.7";             // 2.7 = native read-only SNMP agent (Cacti/LibreNMS)

// ----- NTP time sync (only when WiFi STA is connected) -----
const char* NTP_SERVER1 = "time.windows.com";
const char* NTP_SERVER2 = "pool.ntp.org";
// POSIX TZ for Mountain Time (Edmonton/Alberta) incl. DST. Change this one
// string if you're elsewhere — it only affects on-device localtime; the
// dashboard formats per-sample timestamps in the browser's own timezone.
const char* TZ_INFO = "MST7MDT,M3.2.0,M11.1.0";
const uint32_t NTP_RESYNC_MS = 6UL * 3600UL * 1000UL;   // re-sync every 6 h (drift)

const int   VSENSE_PIN = 1;                 // GPIO1 = ADC1_CH0 (ADC1 = safe with WiFi on)
const float DIVIDER    = 5.545f;            // (1M + 220k) / 220k
float       CAL        = 1.000f;            // calibration trim

const int      HIST_N     = 1440;           // ring-buffer length (24 h @ 60 s)
const uint32_t SAMPLE_MS  = 60000;          // one history sample per 60 s
const uint32_t SAVE_MS    = 600000;         // snapshot history to flash every 10 min
const uint32_t AP_AFTER_DOWN_MS = 300000;   // 5 min of lost WiFi -> Access Point
const int      SAMPLES    = 64;             // ADC averaging

// Onboard RGB LED voltage indicator (volts):
//   > 14.6 or < 9.0  -> RED    (over-voltage or too low)
//   12.0 .. 14.6     -> GREEN  (good)
//   9.0  .. 12.0     -> BLUE   (low-ish)
const bool     LED_ENABLED = false;         // physical LED OFF for now; set true near deployment
const int      LED_PIN    = 48;             // onboard WS2812; if unresponsive try 38 or 47
const float    V_LOW      = 9.0f;
const float    V_MID      = 12.0f;
const float    V_HIGH     = 14.6f;
const uint8_t  LED_BRIGHT = 32;             // 0-255

// 2.4" ILI9341 + XPT2046 SPI display (set false to disable)
const bool DISPLAY_ENABLED = false;         // display set aside for now; flip true to re-enable
const int  TFT_SCLK = 12, TFT_MOSI = 11, TFT_MISO = 13;
const int  TFT_CS = 10, TFT_DC = 9, TFT_RST = 14, TFT_BL = 21;
const int  TS_CS = 8, TS_IRQ = 7;

// CC1101 433.92 MHz OOK transmitter (Compustar 1WG3R replay).
// 3.3V logic ONLY — the CC1101 is not 5V tolerant. Pins chosen to avoid
// the ADC voltage pin (GPIO1), the strapping pins (0/3/45/46), the
// flash/PSRAM pins (26-37), native USB (19/20), UART0 (43/44), the RGB
// LED (48), and the display pins (7-14, 21). See cc1101_compustar.h.
const bool     RF_ENABLED  = true;          // set false to skip CC1101 init entirely
const int      CC1101_SCK  = 18;
const int      CC1101_MISO = 17;
const int      CC1101_MOSI = 16;
const int      CC1101_CS   = 15;
const int      CC1101_GDO0 = 4;
const uint8_t  RF_TX_POWER = 0xC0;          // PATABLE[1]: 0xC0 ~ +10 dBm (max)
const uint8_t  RF_REPEATS  = 8;             // packet repeats per press (FOB sends ~8)
const uint16_t RF_GUARD_MS = 39;            // silence between repeats

// ----- Low-voltage auto-start (OPT-IN — ships DISABLED) -----
// When ARMED, the firmware fires the Compustar START code by itself once
// battery voltage has stayed at or below the threshold continuously for the
// hold time. The point is to start the engine while the battery can still
// crank, so the alternator puts charge back before it goes flat.
//
// Why a sustain time at all: cranking the engine yanks terminal voltage down
// to ~9-10 V for a second or two. Requiring the low reading to persist for a
// full minute means a real crank (or a momentary load like the blower) can
// never be mistaken for a flat battery.
// Default trigger is 12.4 V, not the 12.2 V the older MicroPython tree used.
// Two reasons, both specific to a car parked outside in Alberta:
//   * Cold cranking. Near -20 C the engine needs roughly double the torque
//     while the battery can deliver only about half its rated power, so the
//     "still cranks" line moves UP a couple tenths from the mild-weather value.
//   * Electrolyte freezing. 12.2 V resting is about SG 1.19, which slushes up
//     around -26 C — i.e. right at the local design low. 12.4 V is ~SG 1.23,
//     good to about -37 C. Firing at 12.2 can mean firing at a battery that is
//     already too cold-soaked to turn the engine.
// In a mild climate 12.2 V is perfectly reasonable — it's one field on the page.
const float    AS_DEF_VOLTS     = 12.4f;    // suggested trigger (V)
const uint32_t AS_DEF_HOLD_S    = 60;       // must stay below threshold this long
const uint32_t AS_DEF_COOL_S    = 7200;     // 2 h between auto-starts (anti-loop)
const float    AS_V_FLOOR       = 8.0f;     // below this the divider isn't on a battery
const float    AS_V_CEIL        = 16.0f;    // above this the reading isn't a plausible battery
const float    AS_ALT_V         = 13.2f;    // above this the alternator is running
const uint32_t AS_PARK_S        = 900;      // must sit below AS_ALT_V this long before arming
// Re-arm hysteresis: after a start the battery must climb back above the
// trigger by this margin and hold, so a battery sitting right at the trigger
// doesn't re-fire every cooldown. Deliberately RELATIVE to the threshold, not a
// fixed 12.55 V — on a tired battery that can only recover to, say, 12.5 V a
// fixed bar would never be met and auto-start would silently stop working.
// AS_REARM_MAX_COOLDOWNS is the escape hatch for the same reason: hysteresis
// may delay a start, it must never be able to block one permanently.
const float    AS_REARM_MARGIN  = 0.15f;    // must recover to (threshold + this)...
const uint32_t AS_REARM_S       = 600;      // ...and hold it this long
const uint8_t  AS_REARM_MAX_COOLDOWNS = 2;  // after this many cooldowns, re-arm anyway
const uint32_t AS_VERIFY_S      = 180;      // after firing, expect charging within this
const uint8_t  AS_MAX_FAILS     = 2;        // consecutive unverified starts -> lockout
// Optional cap on auto-starts per rolling 24 h. 0 (or any value <= 0) means
// UNLIMITED, which is the default: if the battery genuinely needs starting six
// times in a cold night, refusing on the seventh is its own kind of failure.
// The lockout-on-no-charge guard is what stops a runaway, not this.
const int      AS_DEF_MAX24     = 0;        // 0 / -1 = unlimited
const uint32_t AS_BOOT_GRACE_MS = 120000;   // no auto-start in the first 2 min after boot
const float    AS_V_MIN_CFG     = 10.0f;    // accepted config range for the threshold
const float    AS_V_MAX_CFG     = 13.0f;
const int      START_N          = 64;       // start-event ring buffer length

// ----- SNMP (read-only, for Cacti / LibreNMS / snmpwalk) -----
// Enterprise subtree 1.3.6.1.4.1.99999.**8** — the Pi-side responder uses .7,
// so both can live on one network. Port 161 is fine here: no OS privilege
// rules on bare metal. Community is read-only; there is no SET path at all.
const bool     SNMP_ENABLED   = true;
const uint16_t SNMP_PORT      = 161;
const char*    SNMP_COMMUNITY = "public";
// -----------------------------------------------------------

struct Sample {
  uint32_t ts;          // unix epoch seconds when sampled (0 if NTP not yet synced)
  float    vbatt;       // volts
  float    temp;        // chip C
  uint16_t heap_kb;     // free heap, KB
  uint16_t disk_kb;     // filesystem used, KB
  uint32_t net_in;      // bytes received this interval (approx, HTTP)
  uint32_t net_out;     // bytes sent this interval
  int16_t  rssi;        // WiFi RSSI dBm (0 in AP mode)
  uint8_t  cpu0;        // core 0 load %, averaged across the interval
  uint8_t  cpu1;        // core 1 load %, averaged across the interval
};

// ----- time sync state -----
uint32_t lastNtpSyncMs = 0;     // millis() of the last configTzTime() kick
bool     g_timeSynced  = false; // true once the clock reads a plausible year

bool timeIsValid() {
  return time(nullptr) > 1700000000UL;   // ~2023-11-14; means NTP set the clock
}

// Kick off (or refresh) SNTP. Safe to call repeatedly; each call re-queries
// the servers. Only meaningful with a live STA connection.
void syncTimeNow() {
  configTzTime(TZ_INFO, NTP_SERVER1, NTP_SERVER2);
  lastNtpSyncMs = millis();
}

WebServer server(80);
DNSServer dnsServer;
bool      apMode    = false;
int       g_last_mv = 0;

// Persisted power/perf settings (NVS). Loaded in setup(), re-saved on each
// toggle. NVS lives in a flash partition, so these survive reboot AND a
// brownout — the chosen CPU clock and WiFi power-save state come back.
Preferences prefs;
uint32_t  g_cpu_mhz = 240;       // restored at boot, then applied
bool      g_wifi_ps = true;      // restored at boot, then applied (true = modem-sleep)

// ----- low-voltage auto-start: persisted config + live state -----
// Config lives in NVS (survives reboot/brownout). Ships disabled; arming is
// a deliberate click in the dashboard.
bool      g_as_en    = false;              // ARMED?  NVS "as_en"
float     g_as_volts = AS_DEF_VOLTS;       // trigger threshold, V   NVS "as_volts"
uint32_t  g_as_hold  = AS_DEF_HOLD_S;      // sustain seconds        NVS "as_hold"
uint32_t  g_as_cool  = AS_DEF_COOL_S;      // cooldown seconds       NVS "as_cool"
uint32_t  g_lowSince    = 0;               // millis() when V first went below threshold (0 = not low)
uint32_t  g_lastStartMs = 0;               // millis() of the last auto-start this session
uint32_t  g_lastStartTs = 0;               // epoch of the last auto-start  NVS "as_last"
float     g_lastV       = 0.0f;            // most recent voltage reading

// Extra restraint state. These stop the three ways an automatic starter
// misbehaves: firing while you're driving, firing over and over on a battery
// that never really recovers, and cranking a car that isn't going to start.
uint32_t  g_parkS       = 0;               // seconds continuously below AS_ALT_V (alternator off)
uint32_t  g_rearmS      = 0;               // seconds continuously at/above AS_REARM_V
bool      g_needRearm   = false;           // set after a start; blocks refiring until recovered
bool      g_asLock      = false;           // latched lockout  NVS "as_lock"
uint8_t   g_asFails     = 0;               // consecutive unverified starts  NVS "as_fails"
bool      g_verifying   = false;           // waiting to see the alternator come up
uint32_t  g_verifyMs    = 0;               // millis() when verification started
int       g_pendingIdx  = -1;              // index of the start event awaiting verification
bool      g_verifyAuto  = false;           // was the pending start automatic? (only those can lock out)
uint32_t  g_win24Ms     = 0;               // millis() at the start of the rolling 24 h window
uint8_t   g_fires24     = 0;               // auto-starts fired in that window (informational)
int       g_as_max24    = AS_DEF_MAX24;    // cap per 24 h; <= 0 = unlimited  NVS "as_max24"

// One logged engine-start event. Written through to flash immediately so a
// start is never lost to the brownout that a cranking engine can cause.
struct StartEvent {
  uint32_t ts;        // unix epoch when fired (0 if NTP hadn't synced)
  uint32_t up_s;      // uptime seconds at fire — orders events even with no clock
  float    vbatt;     // battery voltage at the moment it fired
  uint8_t  src;       // 0 = auto (low voltage), 1 = manual (dashboard button)
  uint8_t  ok;        // 1 = the CC1101 reported the burst went out
  uint8_t  ver;       // did the engine actually run? 0 = unknown, 1 = confirmed, 2 = no charge seen
  uint8_t  _pad;
};
StartEvent g_starts[START_N];
int       g_startCount = 0;
int       g_startHead  = 0;

Sample*   hist      = nullptr;               // ring buffer (in PSRAM)
int       histCount = 0;                     // valid samples (<= HIST_N)
int       histHead  = 0;                     // next write index
uint32_t  g_in_total = 0, g_out_total = 0;   // cumulative HTTP byte counters
uint32_t  lastInSnap = 0, lastOutSnap = 0;
size_t    g_dashLen = 0;

SPIClass  tftSPI(HSPI);
Adafruit_ILI9341 tft(&tftSPI, TFT_DC, TFT_CS, TFT_RST);
XPT2046_Touchscreen ts(TS_CS, TS_IRQ);
int       dispMode = 0;                       // 0 = voltage, 1 = temperature
bool      dispTouchPrev = false;
char      dispLastVal[16] = "";

// CC1101 radio on its own SPI bus (FSPI), separate from the display's HSPI.
SPIClass        radioSPI(FSPI);
CC1101Compustar radio(&radioSPI, CC1101_SCK, CC1101_MISO, CC1101_MOSI,
                      CC1101_CS, CC1101_GDO0);
bool            rfReady = false;

// RF transmit status as a short string for the dashboard / JSON.
//   off     = RF_ENABLED is false
//   absent  = enabled but the CC1101 didn't answer on SPI
//   blocked = chip present but patterns not captured (placeholder secrets)
//   armed   = chip present AND real patterns loaded -> /transmit will TX
const char* rfStatusStr() {
  if (!RF_ENABLED) return "off";
  if (!rfReady)    return "absent";
  return COMPUSTAR_PATTERNS_CAPTURED ? "armed" : "blocked";
}

// Map a button name to its captured 35-bit pattern. nullptr if unknown.
const char* compustarPattern(const String& btn) {
  if (btn == "START")  return COMPUSTAR_START;
  if (btn == "LOCK")   return COMPUSTAR_LOCK;
  if (btn == "UNLOCK") return COMPUSTAR_UNLOCK;
  if (btn == "TRUNK")  return COMPUSTAR_TRUNK;
  return nullptr;
}

// ---------- CPU load ----------
// Real per-core utilisation, not a proxy. The Arduino ESP32 core ships with
// CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS + USE_TRACE_FACILITY enabled and the
// counters clocked off esp_timer, so every task (including the two idle tasks)
// carries a microsecond run-time counter. Load on a core is simply the share of
// wall time its IDLE task did NOT get.
//
// Deliberately NOT the "spin a low-priority counter task" trick: that keeps a
// runnable task on the CPU forever, which defeats FreeRTOS tick-idling and would
// quietly raise the parked-car current draw.
float g_cpu0 = 0.0f, g_cpu1 = 0.0f;   // percent, 0..100 (latest 2 s window)
// Accumulated across the history interval so the graph plots the average load
// over the whole minute, not whichever 2 s window happened to land on it.
float    g_cpuAcc0 = 0.0f, g_cpuAcc1 = 0.0f;
uint16_t g_cpuN    = 0;

void sampleCpuLoad() {
  static uint32_t pIdle0 = 0, pIdle1 = 0, pTotal = 0;
  UBaseType_t n = uxTaskGetNumberOfTasks();
  TaskStatus_t* st = (TaskStatus_t*) malloc(n * sizeof(TaskStatus_t));
  if (!st) return;
  uint32_t total = 0;
  n = uxTaskGetSystemState(st, n, &total);
  uint32_t idle0 = 0, idle1 = 0;
  for (UBaseType_t i = 0; i < n; i++) {
    const char* nm = st[i].pcTaskName;
    if (nm && strncmp(nm, "IDLE", 4) == 0) {
      if      (nm[4] == '1') idle1 = st[i].ulRunTimeCounter;
      else                   idle0 = st[i].ulRunTimeCounter;   // "IDLE0" or plain "IDLE"
    }
  }
  free(st);
  uint32_t dT = total - pTotal;              // unsigned math handles the wrap
  if (pTotal != 0 && dT > 0) {
    uint32_t d0 = idle0 - pIdle0, d1 = idle1 - pIdle1;
    float l0 = 100.0f - (100.0f * (float)d0 / (float)dT);
    float l1 = 100.0f - (100.0f * (float)d1 / (float)dT);
    g_cpu0 = l0 < 0 ? 0 : (l0 > 100 ? 100 : l0);
    g_cpu1 = l1 < 0 ? 0 : (l1 > 100 ? 100 : l1);
    g_cpuAcc0 += g_cpu0; g_cpuAcc1 += g_cpu1; g_cpuN++;
  }
  pIdle0 = idle0; pIdle1 = idle1; pTotal = total;
}

float readBatteryVolts() {
  uint32_t acc = 0;
  for (int i = 0; i < SAMPLES; i++) acc += analogReadMilliVolts(VSENSE_PIN);
  g_last_mv = (int)(acc / SAMPLES);
  g_lastV = (g_last_mv / 1000.0f) * DIVIDER * CAL;
  return g_lastV;
}

void trackReq() { g_in_total += server.uri().length() + 120; }   // approx request size

const char* voltStatus(float v) {
  if (v < V_LOW || v > V_HIGH) return "red";   // too low or over-voltage
  if (v < V_MID) return "blue";                // V_LOW <= v < V_MID
  return "green";                              // V_MID <= v <= V_HIGH
}

void updateLed(float v) {
  if (!LED_ENABLED) { rgbLedWrite(LED_PIN, 0, 0, 0); return; }   // disabled -> LED off
  const char* s = voltStatus(v);
  uint8_t r = (s[0] == 'r') ? LED_BRIGHT : 0;
  uint8_t g = (s[0] == 'g') ? LED_BRIGHT : 0;
  uint8_t b = (s[0] == 'b') ? LED_BRIGHT : 0;
  rgbLedWrite(LED_PIN, r, g, b);
}

uint16_t statusColor16(float v) {
  const char* s = voltStatus(v);
  if (s[0] == 'r') return tft.color565(240, 70, 70);
  if (s[0] == 'g') return tft.color565(60, 200, 90);
  return tft.color565(80, 150, 255);
}

void dispDrawStatic() {
  tft.fillScreen(ILI9341_BLACK);
  tft.setFont(&FreeSans12pt7b);
  tft.setTextColor(ILI9341_WHITE);
  tft.setCursor(12, 30);
  tft.print(dispMode == 0 ? "VOLTAGE" : "CHIP TEMP");
  const char* unit = (dispMode == 0) ? "VOLTS" : "deg C";
  int16_t x1, y1; uint16_t w, h;
  tft.getTextBounds(unit, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((320 - (int)w) / 2 - x1, 210);
  tft.print(unit);
  tft.setFont(NULL); tft.setTextSize(1); tft.setTextColor(0x7BEF);
  tft.setCursor(12, 230);
  tft.print("tap to toggle");
  dispLastVal[0] = 0;                           // force the value to redraw
}

void dispDrawValue(float v, float tC) {
  char buf[16];
  uint16_t col;
  if (dispMode == 0) { snprintf(buf, sizeof(buf), "%.2f", v);  col = statusColor16(v); }
  else               { snprintf(buf, sizeof(buf), "%.1f", tC); col = ILI9341_WHITE; }
  if (strcmp(buf, dispLastVal) == 0) return;    // unchanged -> skip (no flicker)
  strcpy(dispLastVal, buf);
  tft.fillRect(0, 55, 320, 120, ILI9341_BLACK); // clear the number band
  tft.setFont(&FreeSansBold24pt7b);
  tft.setTextSize(2);
  tft.setTextColor(col);
  int16_t x1, y1; uint16_t w, h;
  tft.getTextBounds(buf, 0, 0, &x1, &y1, &w, &h);
  tft.setCursor((320 - (int)w) / 2 - x1, 150);
  tft.print(buf);
}

void recordSample() {
  if (!hist) return;
  Sample s;
  s.ts      = timeIsValid() ? (uint32_t)time(nullptr) : 0;
  s.vbatt   = readBatteryVolts();
  s.temp    = temperatureRead();
  s.heap_kb = (uint16_t)(ESP.getFreeHeap() / 1024);
  s.disk_kb = (uint16_t)(LittleFS.usedBytes() / 1024);
  s.net_in  = g_in_total  - lastInSnap;   lastInSnap  = g_in_total;
  s.net_out = g_out_total - lastOutSnap;  lastOutSnap = g_out_total;
  s.rssi    = apMode ? 0 : (int16_t)WiFi.RSSI();
  s.cpu0    = g_cpuN ? (uint8_t)(g_cpuAcc0 / g_cpuN + 0.5f) : (uint8_t)(g_cpu0 + 0.5f);
  s.cpu1    = g_cpuN ? (uint8_t)(g_cpuAcc1 / g_cpuN + 0.5f) : (uint8_t)(g_cpu1 + 0.5f);
  g_cpuAcc0 = 0; g_cpuAcc1 = 0; g_cpuN = 0;      // start a fresh interval average
  hist[histHead] = s;
  histHead = (histHead + 1) % HIST_N;
  if (histCount < HIST_N) histCount++;
}

void saveHistory() {
  if (!hist) return;
  File f = LittleFS.open("/history.bin", FILE_WRITE);
  if (!f) return;
  uint32_t magic = 0x564F4C34;                 // "VOL4" (per-sample ts + CPU load)
  f.write((uint8_t*)&magic, 4);
  f.write((uint8_t*)&histCount, 4);
  f.write((uint8_t*)&histHead, 4);
  f.write((uint8_t*)hist, sizeof(Sample) * HIST_N);
  f.close();
}

void loadHistory() {
  if (!hist || !LittleFS.exists("/history.bin")) return;
  File f = LittleFS.open("/history.bin", FILE_READ);
  if (!f) return;
  uint32_t magic = 0;
  f.read((uint8_t*)&magic, 4);
  if (magic == 0x564F4C34) {           // older formats (no CPU columns) are discarded
    f.read((uint8_t*)&histCount, 4);
    f.read((uint8_t*)&histHead, 4);
    f.read((uint8_t*)hist, sizeof(Sample) * HIST_N);
    if (histCount < 0 || histCount > HIST_N) { histCount = 0; histHead = 0; }
  }
  f.close();
}

// ---------- start-event log (LittleFS, survives reboot/brownout) ----------

void saveStarts() {
  File f = LittleFS.open("/starts.bin", FILE_WRITE);
  if (!f) return;
  uint32_t magic = 0x53545231;                 // "STR1"
  f.write((uint8_t*)&magic, 4);
  f.write((uint8_t*)&g_startCount, 4);
  f.write((uint8_t*)&g_startHead, 4);
  f.write((uint8_t*)g_starts, sizeof(StartEvent) * START_N);
  f.close();
}

void loadStarts() {
  if (!LittleFS.exists("/starts.bin")) return;
  File f = LittleFS.open("/starts.bin", FILE_READ);
  if (!f) return;
  uint32_t magic = 0;
  f.read((uint8_t*)&magic, 4);
  if (magic == 0x53545231) {
    f.read((uint8_t*)&g_startCount, 4);
    f.read((uint8_t*)&g_startHead, 4);
    f.read((uint8_t*)g_starts, sizeof(StartEvent) * START_N);
    if (g_startCount < 0 || g_startCount > START_N ||
        g_startHead  < 0 || g_startHead  >= START_N) { g_startCount = 0; g_startHead = 0; }
  }
  f.close();
}

// Append a start event and flush it to flash right away. Called for BOTH the
// automatic trigger and a manual dashboard press, so the log answers "when did
// this car get started, and what started it".
// Returns the ring index it was written to, so the caller can patch in the
// verification result once we know whether the engine actually caught.
int recordStart(float v, uint8_t src, bool ok) {
  StartEvent e;
  e.ts    = timeIsValid() ? (uint32_t)time(nullptr) : 0;
  e.up_s  = millis() / 1000;
  e.vbatt = v;
  e.src   = src;
  e.ok    = ok ? 1 : 0;
  e.ver   = 0;                                  // pending
  e._pad  = 0;
  int idx = g_startHead;
  g_starts[idx] = e;
  g_startHead = (g_startHead + 1) % START_N;
  if (g_startCount < START_N) g_startCount++;
  saveStarts();                                 // write through — don't risk losing it
  return idx;
}

// Begin watching for the alternator to come up, which is the only real proof
// the engine caught. Used for manual presses too — it answers "did that work?"
// Only an AUTOMATIC start counts toward the lockout: a manual test press (on
// the bench, or with the battery out) must never latch the automatic system off.
void beginVerify(int idx, bool isAuto) {
  g_pendingIdx = idx;
  g_verifying  = true;
  g_verifyAuto = isAuto;
  g_verifyMs   = millis();
}

// ---------- low-voltage auto-start ----------

// Seconds left in the post-start cooldown (0 = clear). Prefers wall-clock so
// the cooldown SURVIVES A REBOOT — otherwise a brownout right after a start
// would clear it and let the car re-fire immediately.
uint32_t autoStartCooldownLeft() {
  if (timeIsValid() && g_lastStartTs > 0) {
    uint32_t nowTs = (uint32_t)time(nullptr);
    if (nowTs < g_lastStartTs) return 0;        // clock stepped backwards; don't wedge
    uint32_t el = nowTs - g_lastStartTs;
    return (el >= g_as_cool) ? 0 : (g_as_cool - el);
  }
  if (g_lastStartMs == 0) return 0;
  uint32_t el = (millis() - g_lastStartMs) / 1000;
  return (el >= g_as_cool) ? 0 : (g_as_cool - el);
}

// Short machine-readable state for the dashboard.
const char* autoStartState() {
  if (!g_as_en)                                                  return "off";
  if (g_asLock)                                                  return "lockout";
  if (!(RF_ENABLED && rfReady && COMPUSTAR_PATTERNS_CAPTURED))   return "not-armed";
  if (millis() < AS_BOOT_GRACE_MS)                               return "warmup";
  if (g_lastV < AS_V_FLOOR || g_lastV > AS_V_CEIL)               return "no-battery";
  if (g_verifying)                                               return "verifying";
  if (g_needRearm)                                               return "recovering";
  if (g_as_max24 > 0 && g_fires24 >= g_as_max24)                 return "daily-cap";
  if (autoStartCooldownLeft() > 0)                               return "cooldown";
  if (g_parkS < AS_PARK_S)                                       return "park-wait";
  if (g_lowSince != 0)                                           return "counting";
  return "watching";
}

// Evaluated every ~2 s from loop(). EVERY guard must pass before a packet goes
// out; any failure resets the low-voltage timer, so the hold time always means
// "continuously low", never "low on and off".
void evalAutoStart(float v) {
  uint32_t now = millis();
  static uint32_t lastEvalMs = 0;
  uint32_t dt = lastEvalMs ? (now - lastEvalMs) / 1000 : 0;   // seconds since last call
  lastEvalMs = now;

  // A reading outside this band isn't a car battery at all — bench rig, an
  // unplugged sense wire, or a glitch. Treat it as "no information", never "low".
  bool valid = (v >= AS_V_FLOOR && v <= AS_V_CEIL);

  // --- accumulators, maintained regardless of arm state ---
  // Parked = the alternator is not running. Requiring a stretch of this before
  // arming means the starter can't fire while you're driving (or in the minutes
  // right after shutdown, when surface charge makes voltage untrustworthy).
  if (valid && v < AS_ALT_V) { if (g_parkS < 1000000UL) g_parkS += dt; }
  else                         g_parkS = 0;

  // Re-arm hysteresis. Without this, a battery that recovers to just above the
  // trigger re-fires every cooldown, forever, until the tank is empty.
  if (valid && v >= (g_as_volts + AS_REARM_MARGIN)) { if (g_rearmS < 1000000UL) g_rearmS += dt; }
  else                                                g_rearmS = 0;
  if (g_needRearm && g_rearmS >= AS_REARM_S) {
    g_needRearm = false;
    Serial.println("auto-start: re-armed (battery recovered and held)");
  }
  // Escape hatch — a battery too tired to reach the re-arm bar must not wedge
  // auto-start off forever. This car has a known parasitic drain; being unable
  // to fire is the worse failure. Cooldown still spaces the starts out.
  if (g_needRearm && g_lastStartMs != 0 &&
      (now - g_lastStartMs) / 1000 >= (uint32_t)AS_REARM_MAX_COOLDOWNS * g_as_cool) {
    g_needRearm = false;
    Serial.println("auto-start: re-armed by timeout (battery never reached the re-arm bar)");
  }

  // --- did the last start actually work? the alternator coming up is the proof ---
  if (g_verifying) {
    if (valid && v >= AS_ALT_V) {
      if (g_pendingIdx >= 0) { g_starts[g_pendingIdx].ver = 1; saveStarts(); }
      g_verifying = false; g_pendingIdx = -1;
      if (g_verifyAuto) { g_asFails = 0; prefs.putUChar("as_fails", 0); }
      Serial.println("start verified: engine running (charging seen)");
    } else if (now - g_verifyMs >= AS_VERIFY_S * 1000UL) {
      if (g_pendingIdx >= 0) { g_starts[g_pendingIdx].ver = 2; saveStarts(); }
      g_verifying = false; g_pendingIdx = -1;
      if (!g_verifyAuto) {                      // manual press — record it, but never latch
        Serial.printf("manual start: no charging after %lu s (not counted toward lockout)\n",
                      (unsigned long)AS_VERIFY_S);
      } else {
        if (g_asFails < 255) g_asFails++;
        prefs.putUChar("as_fails", g_asFails);
        Serial.printf("auto-start: no charging after %lu s — fail %u of %u\n",
                      (unsigned long)AS_VERIFY_S, g_asFails, AS_MAX_FAILS);
        if (g_asFails >= AS_MAX_FAILS) {        // it isn't going to start; stop cranking it
          g_asLock = true; prefs.putBool("as_lock", true);
          Serial.println("*** auto-start LOCKED OUT after repeated failed starts ***");
        }
      }
    }
  }

  // rolling 24 h fire budget
  if (now - g_win24Ms >= 86400000UL) { g_win24Ms = now; g_fires24 = 0; }

  // --- gating ---
  if (!g_as_en)                                                { g_lowSince = 0; return; }
  if (g_asLock)                                                { g_lowSince = 0; return; }
  if (!(RF_ENABLED && rfReady && COMPUSTAR_PATTERNS_CAPTURED)) { g_lowSince = 0; return; }
  if (now < AS_BOOT_GRACE_MS)                                  { g_lowSince = 0; return; }
  if (!valid)                                                  { g_lowSince = 0; return; }
  if (g_verifying)                                             { g_lowSince = 0; return; }
  if (g_needRearm)                                             { g_lowSince = 0; return; }
  if (g_parkS < AS_PARK_S)                                     { g_lowSince = 0; return; }
  if (g_as_max24 > 0 && g_fires24 >= g_as_max24)               { g_lowSince = 0; return; }
  if (!(v < g_as_volts))                                       { g_lowSince = 0; return; }

  if (g_lowSince == 0) {                        // first low reading — start the clock
    g_lowSince = now;
    Serial.printf("auto-start: %.2f V below %.2f V, counting %lu s\n",
                  v, g_as_volts, (unsigned long)g_as_hold);
    return;
  }
  if ((now - g_lowSince) / 1000 < g_as_hold) return;    // still sustaining
  if (autoStartCooldownLeft() > 0) return;              // too soon after the last one

  // ---- every guard passed: fire the starter ----
  bool sent = radio.transmitButton(COMPUSTAR_START, RF_REPEATS, RF_GUARD_MS);
  int idx = recordStart(v, 0, sent);
  g_lastStartMs = now;
  g_lastStartTs = timeIsValid() ? (uint32_t)time(nullptr) : 0;
  prefs.putUInt("as_last", g_lastStartTs);
  g_lowSince   = 0;
  g_needRearm  = true;   g_rearmS = 0;
  if (g_fires24 < 255) g_fires24++;
  beginVerify(idx, true);
  Serial.printf("*** AUTO-START FIRED at %.2f V (tx %s) — cooldown %lu s, fire %u today (cap %s) ***\n",
                v, sent ? "ok" : "FAILED", (unsigned long)g_as_cool, g_fires24,
                g_as_max24 > 0 ? String(g_as_max24).c_str() : "none");
}

// ---------- SNMP OID table ----------
// Everything the dashboard shows, exposed read-only at
// 1.3.6.1.4.1.99999.8.<leaf>.0 . Floats are scaled to integers (millivolts,
// deci-degrees) because SNMP has no float type and Cacti is happier with
// integers it can apply a CDEF to. Keep this table and the table in
// docs/20-snmp-integration.md in step.
SnmpAgent snmp;

static void oidFw(SnmpValue& v)       { v.type = SNMP_OCTET;     v.s = FW_VERSION; }
static void oidMode(SnmpValue& v)     { v.type = SNMP_OCTET;     v.s = apMode ? "ap" : "sta"; }
static void oidIp(SnmpValue& v)       { v.type = SNMP_OCTET;     v.s = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString(); }
static void oidUptime(SnmpValue& v)   { v.type = SNMP_TIMETICKS; v.u = millis() / 10; }
static void oidVbattMv(SnmpValue& v)  { v.type = SNMP_GAUGE32;   v.u = (uint32_t)(g_lastV * 1000.0f + 0.5f); }
static void oidAdcMv(SnmpValue& v)    { v.type = SNMP_GAUGE32;   v.u = (uint32_t)g_last_mv; }
static void oidTempDc(SnmpValue& v)   { v.type = SNMP_INT;       v.i = (int32_t)(temperatureRead() * 10.0f); }
static void oidVstatus(SnmpValue& v)  { v.type = SNMP_OCTET;     v.s = voltStatus(g_lastV); }
static void oidRssi(SnmpValue& v)     { v.type = SNMP_INT;       v.i = apMode ? 0 : (int32_t)WiFi.RSSI(); }
static void oidHeapFree(SnmpValue& v) { v.type = SNMP_GAUGE32;   v.u = ESP.getFreeHeap(); }
static void oidHeapTot(SnmpValue& v)  { v.type = SNMP_GAUGE32;   v.u = ESP.getHeapSize(); }
static void oidPsFree(SnmpValue& v)   { v.type = SNMP_GAUGE32;   v.u = ESP.getFreePsram(); }
static void oidPsTot(SnmpValue& v)    { v.type = SNMP_GAUGE32;   v.u = ESP.getPsramSize(); }
static void oidDiskUsed(SnmpValue& v) { v.type = SNMP_GAUGE32;   v.u = LittleFS.usedBytes(); }
static void oidDiskTot(SnmpValue& v)  { v.type = SNMP_GAUGE32;   v.u = LittleFS.totalBytes(); }
static void oidNetIn(SnmpValue& v)    { v.type = SNMP_COUNTER32; v.u = g_in_total; }
static void oidNetOut(SnmpValue& v)   { v.type = SNMP_COUNTER32; v.u = g_out_total; }
static void oidCpuMhz(SnmpValue& v)   { v.type = SNMP_GAUGE32;   v.u = getCpuFrequencyMhz(); }
static void oidCpu0(SnmpValue& v)     { v.type = SNMP_GAUGE32;   v.u = (uint32_t)(g_cpu0 + 0.5f); }
static void oidCpu1(SnmpValue& v)     { v.type = SNMP_GAUGE32;   v.u = (uint32_t)(g_cpu1 + 0.5f); }
static void oidWifiPs(SnmpValue& v)   { v.type = SNMP_INT;       v.i = WiFi.getSleep() ? 1 : 0; }
static void oidSamples(SnmpValue& v)  { v.type = SNMP_GAUGE32;   v.u = (uint32_t)histCount; }
static void oidEpoch(SnmpValue& v)    { v.type = SNMP_GAUGE32;   v.u = timeIsValid() ? (uint32_t)time(nullptr) : 0; }
static void oidTimeOk(SnmpValue& v)   { v.type = SNMP_INT;       v.i = timeIsValid() ? 1 : 0; }
static void oidRf(SnmpValue& v)       { v.type = SNMP_OCTET;     v.s = rfStatusStr(); }
static void oidAsEn(SnmpValue& v)     { v.type = SNMP_INT;       v.i = g_as_en ? 1 : 0; }
static void oidAsVolts(SnmpValue& v)  { v.type = SNMP_GAUGE32;   v.u = (uint32_t)(g_as_volts * 1000.0f + 0.5f); }
static void oidAsHold(SnmpValue& v)   { v.type = SNMP_GAUGE32;   v.u = g_as_hold; }
static void oidAsCool(SnmpValue& v)   { v.type = SNMP_GAUGE32;   v.u = g_as_cool; }
static void oidAsState(SnmpValue& v)  { v.type = SNMP_OCTET;     v.s = autoStartState(); }
static void oidAsLowS(SnmpValue& v)   { v.type = SNMP_GAUGE32;   v.u = g_lowSince ? (millis() - g_lowSince) / 1000 : 0; }
static void oidAsCoolL(SnmpValue& v)  { v.type = SNMP_GAUGE32;   v.u = autoStartCooldownLeft(); }
static void oidAsLock(SnmpValue& v)   { v.type = SNMP_INT;       v.i = g_asLock ? 1 : 0; }
static void oidAsFails(SnmpValue& v)  { v.type = SNMP_GAUGE32;   v.u = g_asFails; }
static void oidAsF24(SnmpValue& v)    { v.type = SNMP_GAUGE32;   v.u = g_fires24; }
static void oidAsMax24(SnmpValue& v)  { v.type = SNMP_INT;       v.i = g_as_max24; }
static void oidAsParkS(SnmpValue& v)  { v.type = SNMP_GAUGE32;   v.u = g_parkS; }
static void oidStarts(SnmpValue& v)   { v.type = SNMP_COUNTER32; v.u = (uint32_t)g_startCount; }
static void oidLastStart(SnmpValue& v){ v.type = SNMP_GAUGE32;   v.u = g_lastStartTs; }

static const SnmpEntry SNMP_OIDS[] = {
  { 1,  oidFw,        "fwVersion"        },
  { 2,  oidMode,      "wifiMode"         },
  { 3,  oidIp,        "ipAddress"        },
  { 4,  oidUptime,    "uptime"           },
  { 5,  oidVbattMv,   "batteryMillivolts"},
  { 6,  oidAdcMv,     "adcMillivolts"    },
  { 7,  oidTempDc,    "chipTempDeciC"    },
  { 8,  oidVstatus,   "voltageStatus"    },
  { 9,  oidRssi,      "wifiRssiDbm"      },
  { 10, oidHeapFree,  "heapFreeBytes"    },
  { 11, oidHeapTot,   "heapTotalBytes"   },
  { 12, oidPsFree,    "psramFreeBytes"   },
  { 13, oidPsTot,     "psramTotalBytes"  },
  { 14, oidDiskUsed,  "diskUsedBytes"    },
  { 15, oidDiskTot,   "diskTotalBytes"   },
  { 16, oidNetIn,     "httpBytesIn"      },
  { 17, oidNetOut,    "httpBytesOut"     },
  { 18, oidCpuMhz,    "cpuClockMhz"      },
  { 19, oidCpu0,      "cpuLoad0Pct"      },
  { 20, oidCpu1,      "cpuLoad1Pct"      },
  { 21, oidWifiPs,    "wifiPowerSave"    },
  { 22, oidSamples,   "historySamples"   },
  { 23, oidEpoch,     "clockEpoch"       },
  { 24, oidTimeOk,    "clockSynced"      },
  { 25, oidRf,        "rfStatus"         },
  { 30, oidAsEn,      "autoStartEnabled" },
  { 31, oidAsVolts,   "autoStartMv"      },
  { 32, oidAsHold,    "autoStartHoldSec" },
  { 33, oidAsCool,    "autoStartCoolSec" },
  { 34, oidAsState,   "autoStartState"   },
  { 35, oidAsLowS,    "autoStartLowSec"  },
  { 36, oidAsCoolL,   "autoStartCoolLeft"},
  { 37, oidAsLock,    "autoStartLockout" },
  { 38, oidAsFails,   "autoStartFails"   },
  { 39, oidAsF24,     "autoStartFires24h"},
  { 40, oidAsMax24,   "autoStartMax24h"  },
  { 41, oidAsParkS,   "autoStartParkSec" },
  { 42, oidStarts,    "startEventCount"  },
  { 43, oidLastStart, "lastStartEpoch"   },
};

const char DASH_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32-S3 Voltage Monitor</title>
<style>
:root{--bg:#0d1117;--card:#161b22;--fg:#e6edf3;--mut:#8b949e}
*{box-sizing:border-box}
body{margin:0;font-family:system-ui,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--fg)}
header{padding:16px 20px;border-bottom:1px solid #21262d;display:flex;justify-content:space-between;align-items:center}
h1{font-size:16px;margin:0;font-weight:600}
#dot{width:10px;height:10px;border-radius:50%;background:var(--mut);display:inline-block;margin-right:6px}
#status{font-size:13px;color:var(--mut)}
.wrap{max-width:820px;margin:0 auto;padding:20px}
.hero{display:flex;gap:16px;flex-wrap:wrap}
.metric{flex:1;min-width:150px;text-align:center;background:var(--card);border:1px solid #21262d;border-radius:10px;padding:16px}
.metric .lbl{color:var(--mut);font-size:12px;text-transform:uppercase;letter-spacing:.04em;margin-bottom:6px}
.metric .big{font-size:42px;font-weight:700;line-height:1}
.metric .u{font-size:16px;color:var(--mut)}
.sub{color:var(--mut);font-size:13px;text-align:center;margin:10px 0}
.clbl{color:var(--mut);font-size:12px;margin:14px 0 5px;text-transform:uppercase;letter-spacing:.04em}
canvas{width:100%;height:104px;background:var(--card);border:1px solid #21262d;border-radius:10px;display:block;cursor:crosshair}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(120px,1fr));gap:12px;margin:18px 0}
.card{background:var(--card);border:1px solid #21262d;border-radius:10px;padding:12px}
.card .k{color:var(--mut);font-size:11px;text-transform:uppercase;letter-spacing:.04em}
.card .v{font-size:18px;font-weight:600;margin-top:4px}
footer{text-align:center;color:var(--mut);font-size:12px;padding:16px}
button.tx{background:#21262d;color:var(--fg);border:1px solid #30363d;border-radius:8px;padding:10px 16px;font-size:14px;cursor:pointer}
button.tx:active{background:#30363d}
button.tx.start{background:#238636;border-color:#2ea043;font-size:18px;font-weight:600;padding:14px}
button.tx.start:active{background:#2ea043}
#tip{position:fixed;display:none;pointer-events:none;z-index:50;background:#1f2733;color:var(--fg);border:1px solid #30363d;border-radius:6px;padding:6px 9px;font-size:12px;line-height:1.45;box-shadow:0 2px 10px rgba(0,0,0,.5);white-space:nowrap}
button.tx.seg.on{background:#1f6feb;border-color:#388bfd;color:#fff;font-weight:600}
.badge{display:inline-block;padding:2px 12px;border-radius:20px;font-size:15px;font-weight:700;letter-spacing:.03em}
.badge.on{background:#1a7f37;color:#fff}.badge.off{background:#30363d;color:#8b949e}
.pwrrow{display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:10px}
.badge.arm{background:#9e6a03;color:#fff}
.inp{background:#0d1117;color:var(--fg);border:1px solid #30363d;border-radius:6px;padding:7px 8px;font-size:14px;width:96px}
table.st{width:100%;border-collapse:collapse;font-size:13px}
table.st th{text-align:left;color:var(--mut);font-weight:600;font-size:11px;text-transform:uppercase;letter-spacing:.04em;padding:6px 4px;border-bottom:1px solid #21262d}
table.st td{padding:6px 4px;border-bottom:1px solid #1b2129}
.pill{display:inline-block;padding:1px 9px;border-radius:12px;font-size:11px;font-weight:600}
.pill.auto{background:#1f6feb;color:#fff}.pill.man{background:#30363d;color:#c9d1d9}
</style></head><body>
<div id="tip"></div>
<header><h1>&#9889; ESP32-S3 Voltage Monitor</h1>
<span id="status"><span id="dot"></span><span id="stxt">connecting&hellip;</span></span></header>
<div class="wrap">
<div class="hero">
<div class="metric"><div class="lbl">Voltage</div><span class="big" id="vbatt">--</span><span class="u"> V</span></div>
<div class="metric"><div class="lbl">Chip temp</div><span class="big" id="temp">--</span><span class="u"> &deg;C</span></div>
</div>
<div class="sub" id="sub">waiting for data&hellip;</div>
<div class="clbl">Voltage (24 h)</div><canvas id="c0" width="800" height="104"></canvas>
<div class="clbl">Temperature &deg;C (24 h)</div><canvas id="c1" width="800" height="104"></canvas>
<div class="clbl">Free memory KB (24 h)</div><canvas id="c2" width="800" height="104"></canvas>
<div class="clbl">Disk used KB (24 h)</div><canvas id="c3" width="800" height="104"></canvas>
<div class="clbl">Network in B/min (24 h)</div><canvas id="c4" width="800" height="104"></canvas>
<div class="clbl">Network out B/min (24 h)</div><canvas id="c5" width="800" height="104"></canvas>
<div class="clbl">WiFi RSSI dBm (24 h)</div><canvas id="c6" width="800" height="104"></canvas>
<div class="clbl">CPU core 0 load % (24 h)</div><canvas id="c7" width="800" height="104"></canvas>
<div class="clbl">CPU core 1 load % (24 h)</div><canvas id="c8" width="800" height="104"></canvas>
<div class="grid">
<div class="card"><div class="k">ADC node</div><div class="v"><span id="adc">--</span> mV</div></div>
<div class="card"><div class="k">WiFi RSSI</div><div class="v"><span id="rssi">--</span> dBm</div></div>
<div class="card"><div class="k">Uptime</div><div class="v" id="up">--</div></div>
<div class="card"><div class="k">Disk</div><div class="v"><span id="disk">--</span></div></div>
<div class="card"><div class="k">Free heap</div><div class="v"><span id="heap">--</span> KB</div></div>
<div class="card"><div class="k">Free PSRAM</div><div class="v"><span id="psram">--</span> MB</div></div>
<div class="card"><div class="k">CPU core 0</div><div class="v"><span id="cpu0">--</span> %</div></div>
<div class="card"><div class="k">CPU core 1</div><div class="v"><span id="cpu1">--</span> %</div></div>
</div>
<div class="clbl">Power &amp; performance</div>
<div class="card" style="margin-bottom:10px">
<div class="pwrrow">
<div><div class="k">CPU clock</div><div class="v"><span id="cpu">--</span> MHz</div></div>
<div style="display:flex;gap:6px"><button class="tx seg" data-mhz="80">80 MHz</button><button class="tx seg" data-mhz="240">240 MHz</button></div>
</div>
<div class="pwrrow" style="margin-top:12px;padding-top:12px;border-top:1px solid #21262d">
<div><div class="k">WiFi power saving</div><div class="v"><span id="psbadge">--</span></div></div>
<button class="tx" id="psbtn">&hellip;</button>
</div>
<div class="k" id="pwrmsg" style="margin-top:10px">&nbsp;</div>
</div>
<div class="clbl">Remote start &mdash; 433 MHz</div>
<div class="card" style="margin-bottom:10px">
<div class="k">CC1101 &middot; <span id="rfstat">&hellip;</span></div>
<button class="tx start" data-b="START" style="width:100%;margin-top:10px">&#128293; Start Engine</button>
<div style="margin-top:8px;display:flex;gap:8px;flex-wrap:wrap">
<button class="tx" data-b="LOCK">Lock</button>
<button class="tx" data-b="UNLOCK">Unlock</button>
<button class="tx" data-b="TRUNK">Trunk</button>
</div>
<div class="k" id="rfmsg" style="margin-top:10px">&nbsp;</div>
</div>
<div class="clbl">Low-voltage auto-start</div>
<div class="card" style="margin-bottom:10px">
<div class="pwrrow">
<div><div class="k">Auto-start</div><div class="v"><span id="asbadge">--</span></div></div>
<div style="display:flex;gap:8px"><button class="tx" id="asunlock" style="display:none;border-color:#8957e5">Clear lockout</button><button class="tx" id="asbtn">&hellip;</button></div>
</div>
<div class="k" id="asstate" style="margin-top:10px">&nbsp;</div>
<div style="margin-top:12px;padding-top:12px;border-top:1px solid #21262d;display:flex;gap:12px;flex-wrap:wrap;align-items:flex-end">
<div><div class="k" style="margin-bottom:4px">Start below</div><input id="asv" class="inp" type="number" step="0.1" min="10" max="13"> <span class="k">V</span></div>
<div><div class="k" style="margin-bottom:4px">Held for</div><input id="ash" class="inp" type="number" step="5" min="10" max="3600"> <span class="k">sec</span></div>
<div><div class="k" style="margin-bottom:4px">Cooldown</div><input id="asc" class="inp" type="number" step="300" min="300" max="86400"> <span class="k">sec</span></div>
<div><div class="k" style="margin-bottom:4px">Max per 24 h</div><input id="asm" class="inp" type="number" step="1" min="-1" max="255"> <span class="k">0 = &infin;</span></div>
<button class="tx" id="assave">Save</button>
</div>
<div class="k" style="margin-top:12px;line-height:1.7;text-transform:none;letter-spacing:0">
<b>12.4 V</b> is the suggested trigger for a cold climate &mdash; about 75&nbsp;% charge, where the engine
still cranks easily. Two reasons not to wait for 12.2 V here: near &minus;20&nbsp;&deg;C the engine wants
roughly double the torque while the battery delivers about half its power, and a battery down at
12.2&nbsp;V has electrolyte that slushes up around &minus;26&nbsp;&deg;C. In a mild climate 12.2&nbsp;V is fine.
Below about <b>11.8 V</b> it likely won't crank at all.<br>
The hold time ignores the brief dip while the engine is <i>actually cranking</i> (down to ~9&ndash;10 V for a
second or two). <b>60 s</b> is far longer than any crank or momentary load.<br>
It also won't fire while you're driving and won't re-fire until the battery recovers and holds.
<b>Max per 24 h</b> is off by default (<b>0</b> or <b>&minus;1</b> = unlimited) &mdash; set a number only if you
want a hard ceiling. The real runaway guard is the lockout: if <b>2</b> starts in a row draw no charge,
the engine clearly isn't catching and it latches off until you clear it.
</div>
<div class="k" id="asmsg" style="margin-top:8px">&nbsp;</div>
</div>
</div>
<div class="wrap" style="padding-top:0">
<div class="clbl">Start history</div>
<div class="card" style="margin-bottom:10px">
<div id="startbox"><div class="k">no starts recorded</div></div>
<div style="margin-top:10px"><button class="tx" id="asclear">Clear log</button></div>
</div>
</div>
<footer><span id="net">&hellip;</span> &middot; fw <span id="fw">?</span> &middot; samples <span id="ns">0</span>/1440 &middot; <span id="clk">--</span> &middot; <a href="/update">update</a></footer>
<script>
function $(i){return document.getElementById(i)}
function fmtUp(s){var h=Math.floor(s/3600),m=Math.floor(s%3600/60),x=s%60;return h?h+"h "+m+"m":m?m+"m "+x+"s":x+"s"}
// 9 graphs, in canvas order c0..c8, mapped to /history data columns 1..9.
var COLS=["#3fb950","#d29922","#58a6ff","#bc8cff","#39c5cf","#f778ba","#ffa657","#7ee787","#e3b341"];
var DEC=[2,1,0,0,0,0,0,0,0];
var UNITS=["V","°C","KB","KB","B/min","B/min","dBm","%","%"];
var PAD=8;
var CHARTS={};      // id -> {data, lo, hi, color, dec, unit}
var TS=[];          // per-sample epoch seconds (0 if NTP not synced), parallel to data
var hoverIdx=-1;
var IDS=["c0","c1","c2","c3","c4","c5","c6","c7","c8"];
function fmtWhen(i){
  var t=TS[i];
  if(t&&t>1700000000){return new Date(t*1000).toLocaleString();}   // absolute (browser-local)
  var m=Math.floor((TS.length-1-i)*60/60);                          // fallback: relative from index
  return m>=60?(Math.floor(m/60)+"h "+(m%60)+"m ago"):(m+"m ago");
}
function drawChart(id){
  var ch=CHARTS[id];if(!ch)return;
  var c=$(id),x=c.getContext("2d"),W=c.width,H=c.height;x.clearRect(0,0,W,H);
  var data=ch.data,N=data.length;if(N<2)return;var lo=ch.lo,hi=ch.hi;
  function gx(i){return PAD+i*(W-2*PAD)/(N-1)}function gy(v){return H-PAD-(v-lo)/(hi-lo)*(H-2*PAD)}
  x.strokeStyle=ch.color;x.lineWidth=1.5;x.beginPath();
  data.forEach(function(v,i){i?x.lineTo(gx(i),gy(v)):x.moveTo(gx(i),gy(v))});x.stroke();
  x.fillStyle="#8b949e";x.font="11px system-ui";x.fillText(hi.toFixed(ch.dec),6,13);x.fillText(lo.toFixed(ch.dec),6,H-5);
  if(hoverIdx>=0&&hoverIdx<N){var hx=gx(hoverIdx),hy=gy(data[hoverIdx]);
    x.strokeStyle="#6e7681";x.lineWidth=1;x.beginPath();x.moveTo(hx,PAD);x.lineTo(hx,H-PAD);x.stroke();
    x.fillStyle=ch.color;x.beginPath();x.arc(hx,hy,3.5,0,6.2832);x.fill();}
}
function drawAll(){IDS.forEach(drawChart)}
function idxFromEvent(ev,id){
  var ch=CHARTS[id];if(!ch||ch.data.length<2)return -1;
  var c=$(id),r=c.getBoundingClientRect(),W=c.width,N=ch.data.length;
  var xint=(ev.clientX-r.left)/r.width*W;
  var i=Math.round((xint-PAD)/((W-2*PAD)/(N-1)));
  return Math.max(0,Math.min(N-1,i));
}
function showTip(ev,id){
  var ch=CHARTS[id];if(!ch||hoverIdx<0)return;
  $("tip").innerHTML="<b>"+ch.data[hoverIdx].toFixed(ch.dec)+" "+ch.unit+"</b><br>"+fmtWhen(hoverIdx);
  $("tip").style.display="block";
  var tx=ev.clientX+12;if(tx+170>window.innerWidth)tx=ev.clientX-160;
  $("tip").style.left=tx+"px";$("tip").style.top=(ev.clientY+12)+"px";
}
function setupHover(){IDS.forEach(function(id){var c=$(id);
  c.addEventListener("mousemove",function(ev){hoverIdx=idxFromEvent(ev,id);drawAll();showTip(ev,id)});
  c.addEventListener("mouseleave",function(){hoverIdx=-1;drawAll();$("tip").style.display="none"});
})}
function loadHistory(){fetch("/history",{cache:"no-store"}).then(function(r){return r.text()}).then(function(t){
var ln=t.trim().split("\n"),rows=[];for(var i=1;i<ln.length;i++){rows.push(ln[i].split(",").map(Number))}
if(!rows.length)return;
TS=rows.map(function(r){return r[0]});
for(var col=0;col<9;col++){(function(col){
  var data=rows.map(function(r){return r[col+1]});           // data cols 1..9
  var lo=Math.min.apply(null,data),hi=Math.max.apply(null,data);if(hi-lo<1e-6){hi+=1;lo-=1}
  if(col>=7){lo=0;hi=Math.max(10,hi)}                        // CPU %: always anchor at 0
  CHARTS["c"+col]={data:data,lo:lo,hi:hi,color:COLS[col],dec:DEC[col],unit:UNITS[col]};
})(col)}
drawAll();
}).catch(function(e){})}
function poll(){fetch("/json",{cache:"no-store"}).then(function(r){return r.json()}).then(function(d){
$("vbatt").textContent=d.vbatt.toFixed(2);$("temp").textContent=d.temp_c.toFixed(1);
$("vbatt").style.color={red:"#f85149",blue:"#58a6ff",green:"#3fb950"}[d.led]||"#e6edf3";
$("adc").textContent=d.adc_mv;$("rssi").textContent=d.rssi;$("up").textContent=fmtUp(d.uptime_s);
$("heap").textContent=Math.floor(d.heap_free/1024);$("psram").textContent=(d.psram_free/1048576).toFixed(2);
$("disk").textContent=Math.floor(d.disk_used/1024)+"/"+Math.floor(d.disk_total/1024)+" KB";
$("sub").textContent="divider ×"+d.divider+" · cal "+d.cal+" · ADC "+d.adc_mv+" mV · 1 sample/"+d.interval_s+"s";
$("net").textContent=(d.mode?d.mode.toUpperCase():"")+" · "+(d.ip||"");$("ns").textContent=d.samples;$("fw").textContent=d.fw||"?";
$("clk").textContent=d.time_ok?new Date(d.epoch*1000).toLocaleTimeString():"no NTP";
var RF={armed:"armed",blocked:"present, no patterns",absent:"not detected",off:"disabled"};
$("rfstat").textContent=RF[d.rf]||d.rf||"?";
$("rfstat").style.color=(d.rf=="armed")?"#3fb950":(d.rf=="blocked"?"#d29922":"#8b949e");
$("cpu").textContent=d.cpu_mhz;
if(d.cpu0!==undefined){$("cpu0").textContent=d.cpu0.toFixed(1);$("cpu1").textContent=d.cpu1.toFixed(1);}
document.querySelectorAll("button.seg").forEach(function(b){b.classList.toggle("on",+b.getAttribute("data-mhz")===d.cpu_mhz)});
var ps=!!d.wifi_ps;
$("psbadge").innerHTML='<span class="badge '+(ps?"on":"off")+'">'+(ps?"ON":"OFF")+'</span>';
$("psbtn").textContent=ps?"Turn OFF":"Turn ON";$("psbtn").setAttribute("data-next",ps?"0":"1");
var ae=!!d.as_en;
$("asbadge").innerHTML='<span class="badge '+(ae?"arm":"off")+'">'+(ae?"ARMED":"OFF")+'</span>';
$("asbtn").textContent=ae?"Disable":"Enable";$("asbtn").setAttribute("data-next",ae?"0":"1");
var ST={off:"disabled — the car will not start itself",
lockout:"LOCKED OUT — "+d.as_fails+" starts in a row drew no charge. Check the car, then clear the lockout.",
"not-armed":"cannot fire — RF not armed (check CC1101 / patterns)",
warmup:"warming up after boot — holding off",
"no-battery":"sensor is not across a battery ("+d.vbatt.toFixed(2)+" V) — will not fire",
verifying:"just started — watching for the alternator to come up",
recovering:"waiting for the battery to recover and hold before it may fire again",
"daily-cap":"24 h cap reached ("+d.as_f24+" of "+d.as_max24+") — holding off",
"park-wait":"confirming the car is parked ("+d.as_park_s+"s of "+d.as_park_need+"s below 13.2 V)",
watching:"armed · watching — voltage is above "+(d.as_volts||0).toFixed(1)+" V"};
var s=ST[d.as_state]||d.as_state;
if(d.as_state=="counting")s="voltage low "+d.as_low_s+"s of "+d.as_hold+"s — starts in "+Math.max(0,d.as_hold-d.as_low_s)+"s";
if(d.as_state=="cooldown")s="cooldown — "+fmtUp(d.as_cool_s)+" before it can fire again";
$("asstate").textContent=s;
$("asstate").style.color=(d.as_state=="counting"||d.as_state=="lockout")?"#f85149":(ae?"#3fb950":"#8b949e");
$("asunlock").style.display=d.as_lock?"inline-block":"none";
var af=document.activeElement?document.activeElement.id:"";
if(af!="asv")$("asv").value=(d.as_volts||0).toFixed(1);
if(af!="ash")$("ash").value=d.as_hold;
if(af!="asc")$("asc").value=d.as_cool;
if(af!="asm")$("asm").value=d.as_max24;
$("dot").style.background="#3fb950";$("stxt").textContent="live";
}).catch(function(e){$("dot").style.background="#d29922";$("stxt").textContent="reconnecting…"})}
document.querySelectorAll("button.tx[data-b]").forEach(function(btn){btn.addEventListener("click",function(){
var b=btn.getAttribute("data-b");
if(b=="START"&&!confirm("Start the engine now? This transmits the real remote-start code and CRANKS THE ENGINE if the car is in range."))return;
$("rfmsg").textContent=b+" …";
fetch("/transmit?button="+b,{method:"POST"}).then(function(r){return r.json()}).then(function(d){
$("rfmsg").textContent=d.ok?(b+" sent ×"+d.repeats):(b+" failed: "+(d.detail||"error"));
}).catch(function(e){$("rfmsg").textContent=b+" request error"})})});
document.querySelectorAll("button.seg").forEach(function(b){b.addEventListener("click",function(){
var m=b.getAttribute("data-mhz");$("pwrmsg").textContent="setting CPU to "+m+" MHz…";
fetch("/cpu?mhz="+m,{method:"POST"}).then(function(r){return r.json()}).then(function(d){
$("pwrmsg").textContent=d.ok?("CPU now "+d.cpu_mhz+" MHz"):("CPU change failed: "+(d.detail||"error"));poll();
}).catch(function(e){$("pwrmsg").textContent="CPU request error"})})});
$("psbtn").addEventListener("click",function(){
var nx=$("psbtn").getAttribute("data-next")||"0";$("pwrmsg").textContent="updating WiFi power saving…";
fetch("/wifips?on="+nx,{method:"POST"}).then(function(r){return r.json()}).then(function(d){
$("pwrmsg").textContent="WiFi power saving "+(d.wifi_ps?"ON":"OFF");poll();
}).catch(function(e){$("pwrmsg").textContent="WiFi power-save request error"})});
function loadStarts(){fetch("/starts",{cache:"no-store"}).then(function(r){return r.json()}).then(function(a){
if(!a||!a.length){$("startbox").innerHTML='<div class="k">no starts recorded</div>';return}
var h='<table class="st"><tr><th>When</th><th>Voltage</th><th>Trigger</th><th>TX</th><th>Engine</th></tr>';
var VER=['<span style="color:#8b949e">unknown</span>','<span style="color:#3fb950">ran</span>','<span style="color:#f85149">no charge</span>'];
a.forEach(function(e){
var w=(e.ts>1700000000)?new Date(e.ts*1000).toLocaleString():("uptime "+fmtUp(e.up_s)+" · no clock");
h+='<tr><td>'+w+'</td><td>'+e.v.toFixed(2)+' V</td><td><span class="pill '+(e.src=="auto"?"auto":"man")+'">'+e.src+'</span></td><td>'+(e.ok?"sent":"<span style=\"color:#f85149\">failed</span>")+'</td><td>'+(VER[e.ver]||VER[0])+'</td></tr>';
});
$("startbox").innerHTML=h+'</table>';
}).catch(function(e){})}
$("asbtn").addEventListener("click",function(){
var nx=$("asbtn").getAttribute("data-next")||"0";
if(nx=="1"&&!confirm("ARM low-voltage auto-start?\n\nThe car will CRANK BY ITSELF, unattended, whenever battery voltage stays at or below the threshold for the hold time.\n\nNever leave this armed while the car is parked indoors or in an attached garage — engine exhaust in an enclosed space is lethal."))return;
$("asmsg").textContent="updating…";
fetch("/autostart?en="+nx,{method:"POST"}).then(function(r){return r.json()}).then(function(d){
$("asmsg").textContent="auto-start "+(d.as_en?"ARMED":"disabled");poll();
}).catch(function(e){$("asmsg").textContent="request error"})});
$("assave").addEventListener("click",function(){
var q="volts="+$("asv").value+"&hold="+$("ash").value+"&cool="+$("asc").value+"&max24="+$("asm").value;
$("asmsg").textContent="saving…";
fetch("/autostart?"+q,{method:"POST"}).then(function(r){return r.json()}).then(function(d){
$("asmsg").textContent=d.ok?("saved — start below "+d.as_volts.toFixed(1)+" V held "+d.as_hold+" s"):("save failed: "+(d.detail||"error"));poll();
}).catch(function(e){$("asmsg").textContent="save error"})});
$("asunlock").addEventListener("click",function(){
if(!confirm("Clear the auto-start lockout?\n\nIt latched because starts drew no charge — the engine did not catch. Make sure you know why before re-enabling."))return;
fetch("/autostart?unlock=1",{method:"POST"}).then(function(r){return r.json()}).then(function(d){
$("asmsg").textContent="lockout cleared";poll();
}).catch(function(e){$("asmsg").textContent="unlock error"})});
$("asclear").addEventListener("click",function(){
if(!confirm("Clear the entire start history?"))return;
fetch("/starts",{method:"POST"}).then(function(){loadStarts();$("asmsg").textContent="start log cleared"})
.catch(function(e){$("asmsg").textContent="clear error"})});
setupHover();setInterval(poll,2000);setInterval(loadHistory,30000);setInterval(loadStarts,15000);
poll();loadHistory();loadStarts();
</script></body></html>
)HTML";

void handleDash() {
  trackReq();
  g_out_total += g_dashLen;
  server.send_P(200, "text/html", DASH_HTML);
}

void handleJson() {
  trackReq();
  float v  = readBatteryVolts();
  float tC = temperatureRead();
  String ip = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  int    rssi = apMode ? 0 : (int)WiFi.RSSI();
  char json[1100];
  snprintf(json, sizeof(json),
    "{\"vbatt\":%.2f,\"temp_c\":%.1f,\"adc_mv\":%d,\"divider\":%.3f,\"cal\":%.3f,"
    "\"rssi\":%d,\"uptime_s\":%lu,\"heap_free\":%u,\"heap_total\":%u,"
    "\"psram_free\":%u,\"psram_total\":%u,\"disk_used\":%u,\"disk_total\":%u,"
    "\"mode\":\"%s\",\"ip\":\"%s\",\"interval_s\":%d,\"samples\":%d,\"led\":\"%s\",\"fw\":\"%s\",\"rf\":\"%s\","
    "\"epoch\":%lu,\"time_ok\":%s,\"cpu_mhz\":%u,\"wifi_ps\":%s,"
    "\"as_en\":%s,\"as_volts\":%.2f,\"as_hold\":%lu,\"as_cool\":%lu,"
    "\"as_state\":\"%s\",\"as_low_s\":%lu,\"as_cool_s\":%lu,\"as_n\":%d,"
    "\"as_lock\":%s,\"as_fails\":%u,\"as_park_s\":%lu,\"as_park_need\":%lu,\"as_f24\":%u,"
    "\"as_max24\":%d,\"cpu0\":%.1f,\"cpu1\":%.1f}",
    v, tC, g_last_mv, DIVIDER, CAL, rssi, (unsigned long)(millis() / 1000),
    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getHeapSize(),
    (unsigned)ESP.getFreePsram(), (unsigned)ESP.getPsramSize(),
    (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes(),
    apMode ? "ap" : "sta", ip.c_str(), (int)(SAMPLE_MS / 1000), histCount, voltStatus(v), FW_VERSION,
    rfStatusStr(),
    (unsigned long)(timeIsValid() ? (uint32_t)time(nullptr) : 0), timeIsValid() ? "true" : "false",
    (unsigned)getCpuFrequencyMhz(), WiFi.getSleep() ? "true" : "false",
    g_as_en ? "true" : "false", g_as_volts,
    (unsigned long)g_as_hold, (unsigned long)g_as_cool, autoStartState(),
    (unsigned long)(g_lowSince ? (millis() - g_lowSince) / 1000 : 0),
    (unsigned long)autoStartCooldownLeft(), g_startCount,
    g_asLock ? "true" : "false", g_asFails,
    (unsigned long)g_parkS, (unsigned long)AS_PARK_S, g_fires24,
    g_as_max24, g_cpu0, g_cpu1);
  g_out_total += strlen(json);
  server.send(200, "application/json", json);
}

// Streams the ring buffer oldest->newest as CSV (one line per sample).
void handleHistory() {
  trackReq();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");
  const char* hdr = "ts,vbatt,temp,heap_kb,disk_kb,net_in,net_out,rssi,cpu0,cpu1\n";
  g_out_total += strlen(hdr);
  server.sendContent(hdr);
  if (hist) {
    int oldest = (histCount < HIST_N) ? 0 : histHead;
    String chunk; chunk.reserve(2048);
    for (int n = 0; n < histCount; n++) {
      Sample& s = hist[(oldest + n) % HIST_N];
      chunk += s.ts;               chunk += ',';
      chunk += String(s.vbatt, 2); chunk += ',';
      chunk += String(s.temp, 1);  chunk += ',';
      chunk += s.heap_kb; chunk += ',';
      chunk += s.disk_kb; chunk += ',';
      chunk += s.net_in;  chunk += ',';
      chunk += s.net_out; chunk += ',';
      chunk += s.rssi;    chunk += ',';
      chunk += s.cpu0;    chunk += ',';
      chunk += s.cpu1;    chunk += '\n';
      if (chunk.length() > 1500) { g_out_total += chunk.length(); server.sendContent(chunk); chunk = ""; }
    }
    if (chunk.length()) { g_out_total += chunk.length(); server.sendContent(chunk); }
  }
  server.sendContent("");
}

// POST /transmit?button=START|LOCK|UNLOCK|TRUNK
// Replays the captured Compustar packet for the named button via the CC1101.
// Guarded at every layer: unknown button -> 400, RF disabled -> 503,
// chip absent -> 503, patterns not captured -> 409. So a bench board with
// no real secrets and/or no wired CC1101 can never transmit a live packet.
void handleTransmit() {
  trackReq();
  String btn = server.arg("button"); btn.toUpperCase();
  const char* pattern = compustarPattern(btn);

  auto fail = [&](int code, const char* msg) {
    char j[160];
    snprintf(j, sizeof(j),
             "{\"ok\":false,\"button\":\"%s\",\"detail\":\"%s\"}",
             btn.c_str(), msg);
    g_out_total += strlen(j);
    server.send(code, "application/json", j);
  };

  if (!pattern)                      { fail(400, "unknown button"); return; }
  if (!RF_ENABLED)                   { fail(503, "RF disabled in config"); return; }
  if (!rfReady)                      { fail(503, "CC1101 not detected"); return; }
  if (!COMPUSTAR_PATTERNS_CAPTURED)  { fail(409, "patterns not captured - TX blocked"); return; }

  bool sent = radio.transmitButton(pattern, RF_REPEATS, RF_GUARD_MS);
  // Log every engine-start attempt (manual ones too) so the dashboard history
  // answers "when was this car started, and by what".
  if (btn == "START") beginVerify(recordStart(g_lastV, 1, sent), false);
  if (!sent) { fail(500, "bad pattern or TX failed"); return; }

  char j[128];
  snprintf(j, sizeof(j), "{\"ok\":true,\"button\":\"%s\",\"repeats\":%d}",
           btn.c_str(), RF_REPEATS);
  g_out_total += strlen(j);
  server.send(200, "application/json", j);
  Serial.printf("RF TX: %s x%d\n", btn.c_str(), RF_REPEATS);
}

// GET /rftest — non-transmitting CC1101 health check (see CC1101::selfTest).
// Confirms SPI/power, config register read-back, and TX-state entry WITHOUT
// radiating a carrier. Cannot start the car. Safe to hit any time.
void handleRfTest() {
  trackReq();
  if (!RF_ENABLED) {
    const char* m = "{\"ok\":false,\"detail\":\"RF disabled in config\"}";
    g_out_total += strlen(m); server.send(503, "application/json", m); return;
  }
  CC1101SelfTest t = radio.selfTest();
  char j[360];
  snprintf(j, sizeof(j),
    "{\"ok\":%s,\"spi_ok\":%s,\"partnum\":\"0x%02X\",\"version\":\"0x%02X\","
    "\"regs_ok\":%s,\"tx_entered\":%s,\"marcstate\":\"0x%02X\",\"gdo0_ok\":%s,"
    "\"patterns_captured\":%s,\"note\":\"no carrier radiated; cannot start car\"}",
    t.ok ? "true" : "false", t.spiOk ? "true" : "false", t.partnum, t.version,
    t.regsOk ? "true" : "false", t.txEntered ? "true" : "false", t.marcstate,
    t.gdo0Ok ? "true" : "false",
    COMPUSTAR_PATTERNS_CAPTURED ? "true" : "false");
  g_out_total += strlen(j);
  server.send(t.ok ? 200 : 500, "application/json", j);
  Serial.printf("RF self-test: ok=%d spi=%d regs=%d txEntered=%d marc=0x%02X gdo0=%d\n",
                t.ok, t.spiOk, t.regsOk, t.txEntered, t.marcstate, t.gdo0Ok);
}

// POST /cpu?mhz=80|240 — set the CPU clock. 80 MHz is the floor that still
// keeps WiFi alive; 240 MHz is full speed. Returns the live (verified) freq.
// Lower clock = less self-heating and a bit less current draw (the radio
// dominates power, but every mA helps a parked-car monitor).
void handleCpu() {
  trackReq();
  long mhz = server.arg("mhz").toInt();
  if (mhz != 80 && mhz != 240) {
    const char* m = "{\"ok\":false,\"detail\":\"mhz must be 80 or 240\"}";
    g_out_total += strlen(m); server.send(400, "application/json", m); return;
  }
  setCpuFrequencyMhz((uint32_t)mhz);
  g_cpu_mhz = getCpuFrequencyMhz();
  prefs.putUInt("cpu_mhz", g_cpu_mhz);          // persist across reboot/brownout
  char j[80];
  snprintf(j, sizeof(j), "{\"ok\":true,\"cpu_mhz\":%u}", (unsigned)getCpuFrequencyMhz());
  g_out_total += strlen(j);
  server.send(200, "application/json", j);
  Serial.printf("CPU clock set to %u MHz (requested %ld)\n", (unsigned)getCpuFrequencyMhz(), mhz);
}

// POST /wifips?on=0|1 — enable/disable WiFi modem-sleep power saving.
//   on=1  -> radio dozes between DTIM beacons (default; saves power, adds latency)
//   on=0  -> radio always on (snappier, draws more current)
// Returns the live state from WiFi.getSleep().
void handleWifiPs() {
  trackReq();
  bool on = server.arg("on").toInt() != 0;
  WiFi.setSleep(on);
  g_wifi_ps = WiFi.getSleep();
  prefs.putBool("wifi_ps", g_wifi_ps);          // persist across reboot/brownout
  char j[80];
  snprintf(j, sizeof(j), "{\"ok\":true,\"wifi_ps\":%s}", WiFi.getSleep() ? "true" : "false");
  g_out_total += strlen(j);
  server.send(200, "application/json", j);
  Serial.printf("WiFi power-save %s\n", WiFi.getSleep() ? "ON" : "OFF");
}

// POST /autostart?en=0|1&volts=12.2&hold=60&cool=7200
// Any subset of params. Arming (en=1) is what makes the car able to start
// itself; it ships disabled and the setting persists in NVS.
void handleAutoStart() {
  trackReq();
  auto fail = [&](const char* msg) {
    char j[160];
    snprintf(j, sizeof(j), "{\"ok\":false,\"detail\":\"%s\"}", msg);
    g_out_total += strlen(j);
    server.send(400, "application/json", j);
  };

  if (server.hasArg("volts")) {
    float v = server.arg("volts").toFloat();
    if (v < AS_V_MIN_CFG || v > AS_V_MAX_CFG) { fail("volts must be 10.0-13.0"); return; }
    g_as_volts = v;  prefs.putFloat("as_volts", v);
  }
  if (server.hasArg("hold")) {
    long h = server.arg("hold").toInt();
    if (h < 10 || h > 3600) { fail("hold must be 10-3600 s"); return; }
    g_as_hold = (uint32_t)h;  prefs.putUInt("as_hold", g_as_hold);
  }
  if (server.hasArg("cool")) {
    long c = server.arg("cool").toInt();
    if (c < 300 || c > 86400) { fail("cool must be 300-86400 s"); return; }
    g_as_cool = (uint32_t)c;  prefs.putUInt("as_cool", g_as_cool);
  }
  if (server.hasArg("max24")) {
    long m = server.arg("max24").toInt();
    if (m > 255) { fail("max24 must be <= 255 (0 or -1 = unlimited)"); return; }
    g_as_max24 = (m <= 0) ? 0 : (int)m;      // 0 and -1 both mean unlimited
    prefs.putInt("as_max24", g_as_max24);
  }
  if (server.hasArg("en")) {
    g_as_en = server.arg("en").toInt() != 0;
    prefs.putBool("as_en", g_as_en);
  }
  // Clearing the latched lockout after a run of failed starts is deliberate —
  // you should have looked at why the car didn't catch before re-enabling it.
  if (server.hasArg("unlock") && server.arg("unlock").toInt() != 0) {
    g_asLock = false;  prefs.putBool("as_lock", false);
    g_asFails = 0;     prefs.putUChar("as_fails", 0);
    Serial.println("auto-start: lockout cleared by user");
  }
  g_lowSince = 0;              // any config change restarts the countdown

  char j[320];
  snprintf(j, sizeof(j),
    "{\"ok\":true,\"as_en\":%s,\"as_volts\":%.2f,\"as_hold\":%lu,\"as_cool\":%lu,"
    "\"as_state\":\"%s\",\"as_lock\":%s,\"as_max24\":%d}",
    g_as_en ? "true" : "false", g_as_volts,
    (unsigned long)g_as_hold, (unsigned long)g_as_cool, autoStartState(),
    g_asLock ? "true" : "false", g_as_max24);
  g_out_total += strlen(j);
  server.send(200, "application/json", j);
  Serial.printf("auto-start config: %s, <= %.2f V for %lu s, cooldown %lu s\n",
                g_as_en ? "ARMED" : "disabled", g_as_volts,
                (unsigned long)g_as_hold, (unsigned long)g_as_cool);
}

// GET /starts — the engine-start log, newest first, as a JSON array.
void handleStarts() {
  trackReq();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  server.sendContent("[");
  for (int n = 0; n < g_startCount; n++) {
    int idx = (g_startHead - 1 - n + 2 * START_N) % START_N;
    StartEvent& e = g_starts[idx];
    char rec[176];
    snprintf(rec, sizeof(rec),
      "%s{\"ts\":%lu,\"up_s\":%lu,\"v\":%.2f,\"src\":\"%s\",\"ok\":%s,\"ver\":%u}",
      n ? "," : "", (unsigned long)e.ts, (unsigned long)e.up_s, e.vbatt,
      e.src == 0 ? "auto" : "manual", e.ok ? "true" : "false", e.ver);
    g_out_total += strlen(rec);
    server.sendContent(rec);
  }
  server.sendContent("]");
  server.sendContent("");
}

// POST /starts — clear the start log.
void handleStartsClear() {
  trackReq();
  g_startCount = 0;
  g_startHead  = 0;
  saveStarts();
  const char* m = "{\"ok\":true,\"cleared\":true}";
  g_out_total += strlen(m);
  server.send(200, "application/json", m);
  Serial.println("start log cleared");
}

const char UPDATE_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>ESP32 OTA</title><style>
body{font-family:system-ui,sans-serif;background:#0d1117;color:#e6edf3;text-align:center;padding:40px}
.b{background:#161b22;border:1px solid #21262d;border-radius:12px;padding:26px;max-width:440px;margin:auto}
input[type=file]{margin:14px 0;color:#e6edf3}
button{background:#238636;color:#fff;border:0;border-radius:8px;padding:10px 22px;font-size:15px;cursor:pointer}
progress{width:100%;height:16px;margin-top:16px}#m{margin-top:12px;color:#8b949e;min-height:1.2em}
a{color:#58a6ff}</style></head><body>
<div class="b"><h2>&#11014;&#65039; ESP32-S3 Firmware Update</h2>
<input type="file" id="fw" accept=".bin"><br>
<button onclick="up()">Upload &amp; flash</button>
<progress id="pb" value="0" max="100"></progress>
<div id="m">pick the compiled .bin — the board flashes it and reboots</div>
<p><a href="/">&larr; back to dashboard</a></p></div>
<script>
function up(){var f=document.getElementById('fw').files[0];if(!f){return}
var x=new XMLHttpRequest(),fd=new FormData();fd.append('firmware',f);
x.upload.onprogress=function(e){if(e.lengthComputable){document.getElementById('pb').value=100*e.loaded/e.total}};
x.onload=function(){document.getElementById('m').textContent=x.responseText+'  (reconnect in ~5 s)'};
x.onerror=function(){document.getElementById('m').textContent='upload error'};
document.getElementById('m').textContent='uploading... do not power off...';
x.open('POST','/update');x.send(fd)}
</script></body></html>
)HTML";

void startAP() {
  apMode = true;
  WiFi.mode(WIFI_AP);
  IPAddress apIP(192, 168, 4, 1), gw(192, 168, 4, 1), mask(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, gw, mask);
  bool secured = strlen(AP_PASS) >= 8;
  WiFi.softAP(AP_SSID, secured ? AP_PASS : nullptr);
  dnsServer.start(53, "*", apIP);
  Serial.println("---- starting fallback ACCESS POINT ----");
  Serial.printf("  Join WiFi \"%s\" %s, then browse http://192.168.4.1/\n",
                AP_SSID, secured ? "(password set)" : "(OPEN)");
}

void setup() {
  Serial.begin(115200);
  delay(300);

  // Restore persisted power/perf settings from NVS and apply the CPU clock
  // now (before WiFi). Falls back to the compiled defaults on first boot or
  // a bad/garbage value.
  prefs.begin("vroom", false);
  g_cpu_mhz = prefs.getUInt("cpu_mhz", 240);
  g_wifi_ps = prefs.getBool("wifi_ps", true);
  if (g_cpu_mhz != 80 && g_cpu_mhz != 240) g_cpu_mhz = 240;   // sanity guard
  setCpuFrequencyMhz(g_cpu_mhz);
  Serial.printf("NVS restore: CPU %u MHz, WiFi power-save %s\n",
                (unsigned)g_cpu_mhz, g_wifi_ps ? "ON" : "OFF");

  // Low-voltage auto-start config (defaults = disabled, 12.2 V, 60 s, 2 h).
  g_as_en       = prefs.getBool("as_en", false);
  g_as_volts    = prefs.getFloat("as_volts", AS_DEF_VOLTS);
  g_as_hold     = prefs.getUInt("as_hold", AS_DEF_HOLD_S);
  g_as_cool     = prefs.getUInt("as_cool", AS_DEF_COOL_S);
  g_lastStartTs = prefs.getUInt("as_last", 0);
  g_asLock      = prefs.getBool("as_lock", false);
  g_asFails     = prefs.getUChar("as_fails", 0);
  g_as_max24    = prefs.getInt("as_max24", AS_DEF_MAX24);
  if (g_as_max24 < 0 || g_as_max24 > 255) g_as_max24 = 0;     // <=0 / bad -> unlimited
  // A corrupt stored value must fall back to the safe default, never widen the
  // trigger window.
  if (g_as_volts < AS_V_MIN_CFG || g_as_volts > AS_V_MAX_CFG) g_as_volts = AS_DEF_VOLTS;
  if (g_as_hold < 10 || g_as_hold > 3600)                     g_as_hold  = AS_DEF_HOLD_S;
  if (g_as_cool < 300 || g_as_cool > 86400)                   g_as_cool  = AS_DEF_COOL_S;
  Serial.printf("NVS restore: auto-start %s (below %.2f V for %lu s, cooldown %lu s)%s\n",
                g_as_en ? "ARMED" : "disabled", g_as_volts,
                (unsigned long)g_as_hold, (unsigned long)g_as_cool,
                g_asLock ? "  [LOCKED OUT]" : "");

  analogSetPinAttenuation(VSENSE_PIN, ADC_11db);

  if (LittleFS.begin(true)) Serial.printf("LittleFS mounted: %u / %u bytes used\n",
                                      (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
  else Serial.println("LittleFS mount FAILED");

  hist = (Sample*) ps_malloc(sizeof(Sample) * HIST_N);   // history lives in PSRAM
  Serial.printf("History buffer: %u bytes in %s\n", (unsigned)(sizeof(Sample) * HIST_N),
                hist ? "PSRAM" : "FAILED");
  loadHistory();
  Serial.printf("Loaded %d prior samples from flash.\n", histCount);
  loadStarts();
  Serial.printf("Loaded %d prior start events from flash.\n", g_startCount);
  g_dashLen = strlen_P(DASH_HTML);

  Serial.printf("Connecting to WiFi '%s' ", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(HOSTNAME);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  WiFi.setSleep(g_wifi_ps);            // apply persisted WiFi power-save state
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 20000) { delay(400); Serial.print("."); }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi OK in %lu ms. IP = %s   RSSI = %d dBm\n",
                  (unsigned long)(millis() - t0), WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    if (MDNS.begin(HOSTNAME)) { MDNS.addService("http", "tcp", 80);
      Serial.printf("Reachable at: http://%s.local/  or  http://%s/\n", HOSTNAME, WiFi.localIP().toString().c_str()); }
    syncTimeNow();   // kick off NTP now that STA is up
    Serial.printf("NTP sync requested (%s / %s)\n", NTP_SERVER1, NTP_SERVER2);
  } else {
    startAP();   // boot-time failure -> AP immediately (no NTP in AP mode)
  }

  server.on("/", handleDash);
  server.on("/json", handleJson);
  server.on("/history", handleHistory);
  server.on("/update", HTTP_GET, []() { trackReq(); server.send_P(200, "text/html", UPDATE_HTML); });
  server.on("/update", HTTP_POST,
    []() {                                    // runs after the upload finishes
      bool ok = !Update.hasError();
      server.send(200, "text/plain", ok ? "OK - flashed" : "FAILED");
      delay(800);
      if (ok) ESP.restart();
    },
    []() {                                    // streams the uploaded .bin into the spare OTA slot
      HTTPUpload& u = server.upload();
      if (u.status == UPLOAD_FILE_START) {
        Serial.printf("OTA start: %s\n", u.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
      } else if (u.status == UPLOAD_FILE_WRITE) {
        if (Update.write(u.buf, u.currentSize) != u.currentSize) Update.printError(Serial);
      } else if (u.status == UPLOAD_FILE_END) {
        if (Update.end(true)) Serial.printf("OTA done: %u bytes, rebooting\n", (unsigned)u.totalSize);
        else Update.printError(Serial);
      }
    });
  server.on("/transmit", HTTP_POST, handleTransmit);
  server.on("/rftest", HTTP_GET, handleRfTest);
  server.on("/cpu", HTTP_POST, handleCpu);
  server.on("/wifips", HTTP_POST, handleWifiPs);
  server.on("/autostart", HTTP_POST, handleAutoStart);
  server.on("/starts", HTTP_GET, handleStarts);
  server.on("/starts", HTTP_POST, handleStartsClear);
  server.onNotFound(handleDash);
  server.begin();
  Serial.println("HTTP up. / dashboard, /json data, /history CSV, /transmit RF.");

  // Bring up the CC1101 433 MHz transmitter.
  if (RF_ENABLED) {
    rfReady = radio.begin(RF_TX_POWER);
    if (rfReady) {
      Serial.printf("CC1101 detected (partnum 0x%02X, version 0x%02X). RF TX %s.\n",
                    radio.partnum(), radio.version(),
                    COMPUSTAR_PATTERNS_CAPTURED ? "ARMED" : "blocked (no captured patterns)");
      // Non-transmitting self-check at boot (no carrier radiated).
      CC1101SelfTest t = radio.selfTest();
      Serial.printf("CC1101 self-test: %s (spi=%d regs=%d txEntered=%d marc=0x%02X gdo0=%d)\n",
                    t.ok ? "PASS" : "FAIL", t.spiOk, t.regsOk, t.txEntered, t.marcstate, t.gdo0Ok);
    } else {
      Serial.println("CC1101 NOT detected - check wiring/power. RF TX disabled.");
    }
  } else {
    Serial.println("RF disabled in config (RF_ENABLED=false).");
  }
  // SNMP agent — read-only, so an NMS can poll the board directly with no Pi.
  if (SNMP_ENABLED) {
    size_t nOids = sizeof(SNMP_OIDS) / sizeof(SNMP_OIDS[0]);
    if (snmp.begin(SNMP_PORT, SNMP_COMMUNITY, SNMP_OIDS, nOids))
      Serial.printf("SNMP up on udp/%u, %u OIDs at 1.3.6.1.4.1.99999.8.x.0\n",
                    SNMP_PORT, (unsigned)nOids);
    else
      Serial.printf("SNMP failed to bind udp/%u\n", SNMP_PORT);
  }

  recordSample();                    // seed one sample now
  updateLed(readBatteryVolts());     // set the LED immediately

  if (DISPLAY_ENABLED) {
    pinMode(TFT_BL, OUTPUT); digitalWrite(TFT_BL, HIGH);    // backlight on
    tftSPI.begin(TFT_SCLK, TFT_MISO, TFT_MOSI, -1);
    tft.begin();
    tft.setRotation(1);                                    // landscape 320x240
    ts.begin(tftSPI);
    ts.setRotation(1);
    dispDrawStatic();
    dispDrawValue(readBatteryVolts(), temperatureRead());
    Serial.println("Display initialized.");
  }
}

uint32_t lastSample = 0, lastSave = 0, lastPrint = 0, downSince = 0, lastDisp = 0;
void loop() {
  if (apMode) dnsServer.processNextRequest();
  server.handleClient();
  if (SNMP_ENABLED) snmp.poll();
  uint32_t now = millis();

  if (DISPLAY_ENABLED) {
    bool t = ts.touched();
    if (t && !dispTouchPrev) {                     // tap toggles voltage <-> temperature
      dispMode = 1 - dispMode;
      dispDrawStatic();
      dispDrawValue(readBatteryVolts(), temperatureRead());
    }
    dispTouchPrev = t;
    if (now - lastDisp >= 400) { lastDisp = now; dispDrawValue(readBatteryVolts(), temperatureRead()); }
  }

  if (!apMode) {                                   // 5 min of lost WiFi -> AP
    if (WiFi.status() == WL_CONNECTED) downSince = 0;
    else { if (downSince == 0) downSince = now;
           else if (now - downSince >= AP_AFTER_DOWN_MS) startAP(); }
  }

  // NTP time: only meaningful with a live STA connection.
  if (!apMode && WiFi.status() == WL_CONNECTED) {
    if (!g_timeSynced) {
      // Not synced yet — retry every 30 s after the boot kick until the
      // clock reads a plausible year.
      if (timeIsValid()) {
        g_timeSynced = true;
        Serial.printf("NTP synced: %lu\n", (unsigned long)time(nullptr));
      } else if (now - lastNtpSyncMs >= 30000) {
        syncTimeNow();
      }
    } else if (now - lastNtpSyncMs >= NTP_RESYNC_MS) {
      syncTimeNow();                               // periodic re-sync (drift)
      Serial.println("NTP re-sync (6h)");
    }
  }

  if (now - lastSample >= SAMPLE_MS) { lastSample = now; recordSample(); }
  if (now - lastSave   >= SAVE_MS)   { lastSave   = now; saveHistory(); }
  if (now - lastPrint  >= 2000) {
    lastPrint = now;
    float v = readBatteryVolts();
    updateLed(v);
    sampleCpuLoad();           // per-core utilisation over the last 2 s
    evalAutoStart(v);          // low-voltage auto-start (no-op unless armed)
    Serial.printf("Vbatt=%6.2f V  temp=%4.1f C  heap=%uKB  disk=%uKB  samples=%d  %s\n",
                  v, temperatureRead(), (unsigned)(ESP.getFreeHeap() / 1024),
                  (unsigned)(LittleFS.usedBytes() / 1024), histCount, apMode ? "[AP]" : "[STA]");
  }
}
