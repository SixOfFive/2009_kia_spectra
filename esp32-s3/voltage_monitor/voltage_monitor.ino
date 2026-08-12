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
#include "esp_task_wdt.h"   // task watchdog -- auto-reboot if the loop or safety task stalls
#include <lwip/sockets.h>   // select() -- gate chunked sends without blocking (see waitWritable)
#include "esp_system.h"     // esp_reset_reason() -- why the last boot happened
#include "esp_sntp.h"       // NTP sync notification callback
#include "esp_wifi.h"       // esp_wifi_set_protocol() -- force 802.11b for range/stability
#include "esp_attr.h"       // RTC_NOINIT_ATTR -- WDT breadcrumbs that survive a reset
#include <stdarg.h>         // logLine() variadic formatting
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
// using the placeholders below -- nothing secret is committed.
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

// ---- Runtime-configurable WiFi (NVS-backed) -------------------------------
// Everything below can be changed from the WiFi tab and survives reboots. The
// constants above are only the FIRST-BOOT defaults, used until the NVS keys
// exist. secrets.h therefore seeds the config; it no longer dictates it.
//
// Safety note: this device lives behind a dash and can start a car. A bad SSID
// or password would strand it on its fallback AP with no way to fix it, so
// credential changes go through applyPendingWifi() -- try, verify, and revert
// automatically if the new network does not come up.
String   g_sta_ssid, g_sta_pass;          // home network
String   g_ap_ssid,  g_ap_pass;           // fallback AP
String   g_hostname;
uint8_t  g_ap_auth    = 1;                // 0 open, 1 WPA2-PSK, 2 WPA/WPA2 mixed, 3 WPA2/WPA3
uint8_t  g_ap_chan    = 1;                // fallback AP channel
bool     g_ap_hidden  = false;
uint8_t  g_sta_minsec = 2;                // min accepted AP security: 0 any, 1 WPA, 2 WPA2, 3 WPA3
                                          // (2 = the Arduino default, i.e. today's behaviour)
uint32_t g_ap_after_s = 300;              // home WiFi down this long -> raise the AP
uint32_t g_ap_retry_s = 600;              // from the AP, retry home this often
uint32_t g_ap_wait_s  = 10;               // how long each retry waits for a join
uint32_t g_boot_s     = 20;               // boot-time connect window
float    g_tx_dbm     = 19.5f;            // TX power (snapped to the nearest supported step)
uint8_t  g_proto      = 7;                // bit0 = 11b, bit1 = 11g, bit2 = 11n, bit3 = LR

// Pending credential switch, driven from loop() so the HTTP reply gets out first.
bool     g_wifiPend    = false;
String   g_pendSsid, g_pendPass;

// Forward declarations: the /wificfg handlers appear earlier in this file than
// these helpers, and Arduino's auto-prototype pass does not cover statics.
static void             applyWifiRangeProfile();
float                   longTermMvph(float vNow);
void                    seedLongTermFromHistory();
long                    longTermEtaS(float vNow);
static wifi_auth_mode_t staMinAuth();
static uint8_t          protoBits();
static wifi_power_t     txEnumFor(float dbm);


const char* FW_VERSION = "4.31";            // 4.31 = Voltage tab: next/last auto-start DATES, cycle interval, % elapsed + progress bar

// ----- NTP time sync (only when WiFi STA is connected) -----
const char* NTP_SERVER1 = "time.windows.com";
const char* NTP_SERVER2 = "pool.ntp.org";
// POSIX TZ for Mountain Time (Edmonton/Alberta) incl. DST. Change this one
// string if you're elsewhere -- it only affects on-device localtime; the
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
// ...and once in AP mode, keep trying to get back onto the home network. Without
// this the fallback is a one-way door: a brief router hiccup strands the board
// in SoftAP until someone power-cycles it. In a parked car that means it is
// unreachable AND drawing roughly double (AP mode has no power save at all).
const uint32_t AP_RETRY_STA_MS  = 600000;   // from AP, re-try the home WiFi every 10 min
const uint32_t AP_RETRY_WAIT_MS = 10000;    // how long to wait for the join
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
// 3.3V logic ONLY -- the CC1101 is not 5V tolerant. Pins chosen to avoid
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
// Remote-start is a press-AND-HOLD on the real FOB (the SDR captures were ~10
// bursts over many seconds), and the brain deliberately wants the start code
// sustained before it will crank -- an anti-accidental-start guard. A short
// lock-length burst isn't enough. START therefore repeats far more (~4.5 s of
// continuous packets) to mimic a held button.
// START wake-up structure (the FOB precedes each start packet with a long
// carrier to wake the receiver's duty-cycled listener):
const uint16_t RF_WAKEUP_MS    = 1450;      // continuous wake-up carrier per burst (FOB ~1436 ms)
const uint8_t  RF_TRAIN_CELLS  = 6;         // ~750 us equal on/off training cells after the carrier (FOB ~5-6)
const uint8_t  RF_START_DATAREPS = 8;       // sync+data repeats after EACH carrier (FOB sends 8)
const uint8_t  RF_START_BURSTS = 1;         // ONE burst per press, exactly like the FOB. Was 3 (over-
                                            // engineered from a long-hold capture); a 2nd start command
                                            // ~2.5 s later can read as a re-press and CANCEL the start.
const uint16_t RF_GUARD_MS = 39;            // silence between bursts
// fw 4.1: SDR timeline of the FOB (sdr/captures/fob-60s-2026-08-06.bin) showed
// the 8 data packets are spaced only ~1.1 ms apart -- back-to-back inside the
// window the carrier just woke -- and a ~0.5 s carrier trails the data. Our 4.0
// spaced packets 39 ms apart and sent no tail, so the receiver likely dropped
// bit-clock lock between packets. Match the FOB exactly.
const uint16_t RF_START_PKT_GAP_MS = 1;     // gap between the 8 data packets (FOB ~1.1 ms)
const uint16_t RF_TAIL_CARRIER_MS  = 525;   // trailing carrier after the data (FOB ~525 ms)

// ----- Low-voltage auto-start (OPT-IN -- ships DISABLED) -----
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
//     around -26 C -- i.e. right at the local design low. 12.4 V is ~SG 1.23,
//     good to about -37 C. Firing at 12.2 can mean firing at a battery that is
//     already too cold-soaked to turn the engine.
// In a mild climate 12.2 V is perfectly reasonable -- it's one field on the page.
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
// fixed 12.55 V -- on a tired battery that can only recover to, say, 12.5 V a
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
// Enterprise subtree 1.3.6.1.4.1.99999.**8** -- the Pi-side responder uses .7,
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
  int16_t  drain;       // battery drain rate at this sample, mV/h (signed; <0 = discharging, 0 = not fitted yet)
  uint16_t link_mbps;   // WiFi link rate at this sample (nominal Mbps from the negotiated PHY; 0 if not associated)
};

// Battery-drain regression result. Declared up here with Sample because the
// .ino auto-prototype pass emits `DrainFit computeDrain();` near the top of the
// file -- the type has to already exist by then or the build fails.
const int      DRAIN_MIN_N  = 30;      // need at least 30 min of samples
const float    DRAIN_FLAT_V = 11.8f;   // projection target: below this it won't crank
const uint32_t DRAIN_GAP_S  = 600;     // a >10 min hole ends the window (board was off)

struct DrainFit {
  bool     ok;
  float    mvph;    // signed millivolts/hour; negative = discharging. Temperature-
                    // compensated: the true depletion rate with the diurnal thermal
                    // swing regressed out (falls back to raw slope if temp is flat).
  float    r2;      // 0..1 fit quality (of the two-variable model)
  int      n;       // samples in the fit
  uint32_t win_s;   // span covered
  float    days;    // days until DRAIN_FLAT_V at this rate; <0 = n/a
  float    mv_per_c;// temperature coefficient the fit found, mV/degC (diagnostic)
};
DrainFit g_drain = {false, 0, 0, 0, 0, -1, 0};   // refreshed once per history sample

// ---------- rolling event log (bounded RAM ring; never touches flash) ----------
// A fixed ring of the most recent lines -- can't grow, so it can't fill RAM or
// the drive. Viewable at /logs (auto-refreshing page) and /logtext (raw). Written
// from both the loop and the safety task, so the ring update is under a spinlock.
static const int LOG_LINES = 1000;          // capacity; rarely full (events are sparse + debounced)
static const int LOG_LEN   = 108;
static char (*g_log)[LOG_LEN] = nullptr;    // ring in PSRAM (8 MB, like the history buffer) -- no DRAM cost
static int  g_logHead = 0, g_logCount = 0;
static portMUX_TYPE g_logMux = portMUX_INITIALIZER_UNLOCKED;
// Flash-persistence cursor. logLine() (called from BOTH cores) only bumps the
// ring + g_logSeq under the spinlock; the loop drains new lines to LittleFS via
// flushLogToFlash(), so all file I/O stays single-threaded on the loop core and
// never runs inside the critical section.
static volatile uint32_t g_logSeq = 0;          // total lines ever pushed (monotonic)
static volatile uint32_t g_logPersistedSeq = 0; // how many have reached flash

void logLine(const char* fmt, ...) {
  char msg[LOG_LEN];
  va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof(msg), fmt, ap); va_end(ap);
  char line[LOG_LEN];
  time_t tt = time(nullptr);
  if (tt > 1700000000L) {                       // wall clock: full date + time
    struct tm* lt = localtime(&tt);
    char stamp[24];
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", lt);
    snprintf(line, sizeof(line), "%s %s", stamp, msg);
  } else {                                       // pre-NTP: uptime seconds
    snprintf(line, sizeof(line), "+%lus %s", (unsigned long)(millis() / 1000), msg);
  }
  portENTER_CRITICAL(&g_logMux);
  if (g_log) {
    strncpy(g_log[g_logHead], line, LOG_LEN - 1); g_log[g_logHead][LOG_LEN - 1] = 0;
    g_logHead = (g_logHead + 1) % LOG_LINES;
    if (g_logCount < LOG_LINES) g_logCount++;
    g_logSeq++;                         // loop will persist anything past g_logPersistedSeq
  }
  portEXIT_CRITICAL(&g_logMux);
  Serial.println(line);                 // keep the USB console too
}

// Human name for a WiFi disconnect reason code (esp_wifi_types wifi_err_reason_t).
static const char* wifiReason(uint8_t r) {
  switch (r) {
    case 1:   return "unspecified";
    case 2:   return "auth-expire";
    case 3:   return "auth-leave";
    case 4:   return "assoc-expire";
    case 5:   return "assoc-toomany";
    case 8:   return "assoc-leave";
    case 15:  return "4way-handshake-timeout";
    case 200: return "beacon-timeout";
    case 201: return "no-AP-found";
    case 202: return "auth-fail";
    case 203: return "assoc-fail";
    case 204: return "handshake-timeout";
    case 205: return "connection-fail";
    default:  return "other";
  }
}

// Human name for the negotiated PHY mode (the closest thing to "what speed are
// we linked at" -- 11b/g are single rates, HT20/40 are the 11n widths).
static const char* phyModeName(wifi_phy_mode_t m) {
  switch (m) {
    case WIFI_PHY_MODE_LR:   return "LR";
    case WIFI_PHY_MODE_11B:  return "11b";
    case WIFI_PHY_MODE_11G:  return "11g";
    case WIFI_PHY_MODE_HT20: return "11n-HT20";
    case WIFI_PHY_MODE_HT40: return "11n-HT40";
    case WIFI_PHY_MODE_HE20: return "11ax-HE20";
    default: return "?";
  }
}

// Negotiated PHY mode as a string, or "n/a" if not currently associated.
static const char* staPhyMode() {
  wifi_phy_mode_t pm;
  if (WiFi.status() == WL_CONNECTED && esp_wifi_sta_get_negotiated_phymode(&pm) == ESP_OK)
    return phyModeName(pm);
  return "n/a";
}

// Nominal link rate (Mbps) for the negotiated PHY -- a graphable "speed": drops
// when the AP downshifts us on a weak link. 0 when not associated.
static uint16_t staLinkMbps() {
  wifi_phy_mode_t pm;
  if (WiFi.status() != WL_CONNECTED || esp_wifi_sta_get_negotiated_phymode(&pm) != ESP_OK) return 0;
  switch (pm) {
    case WIFI_PHY_MODE_11B:  return 11;
    case WIFI_PHY_MODE_11G:  return 54;
    case WIFI_PHY_MODE_HT20: return 72;
    case WIFI_PHY_MODE_HT40: return 150;
    case WIFI_PHY_MODE_HE20: return 143;
    case WIFI_PHY_MODE_LR:   return 1;
    default: return 0;
  }
}

// Verbose WiFi diagnostics into the event log: association, IP, and -- most
// usefully for the drop-out hunt -- the disconnect REASON code every time it
// falls off. (Per-request / SNMP-poll traffic is deliberately NOT logged.)
void onWiFiEvent(WiFiEvent_t ev, WiFiEventInfo_t info) {
  switch (ev) {
    case ARDUINO_EVENT_WIFI_STA_CONNECTED:
      logLine("WiFi assoc to AP (channel %d)", info.wifi_sta_connected.channel);
      break;
    case ARDUINO_EVENT_WIFI_STA_GOT_IP:
      logLine("WiFi got IP %s @ %d dBm, ch %d, %s, SSID '%s'",
              WiFi.localIP().toString().c_str(), (int)WiFi.RSSI(),
              WiFi.channel(), staPhyMode(), WiFi.SSID().c_str());
      break;
    case ARDUINO_EVENT_WIFI_STA_DISCONNECTED: {
      // Debounce: the driver retries every few seconds while out of range, each
      // firing this event. Log the first, then suppress identical repeats for
      // 2 min so a prolonged outage can't flood the ring -- and report the count.
      uint8_t r = info.wifi_sta_disconnected.reason;
      static uint8_t lastR = 255; static uint32_t lastMs = 0; static uint16_t supp = 0;
      uint32_t now = millis();
      if (r != lastR || (now - lastMs) > 120000UL) {
        if (supp) logLine("WiFi DISCONNECT: reason %u (%s) [+%u more suppressed]", r, wifiReason(r), supp);
        else      logLine("WiFi DISCONNECT: reason %u (%s)", r, wifiReason(r));
        lastR = r; lastMs = now; supp = 0;
      } else if (supp < 60000) supp++;
      break; }
    case ARDUINO_EVENT_WIFI_STA_LOST_IP:
      logLine("WiFi lost IP");
      break;
    default: break;
  }
}

// Why the last boot happened -- brownout / watchdog / panic are the ones that
// would silently make it "go unusable", so surface them at boot.
static const char* resetReasonName() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return "power-on";
    case ESP_RST_SW:        return "software";
    case ESP_RST_PANIC:     return "PANIC/exception";
    case ESP_RST_INT_WDT:   return "interrupt-watchdog";
    case ESP_RST_TASK_WDT:  return "TASK-WATCHDOG";
    case ESP_RST_WDT:       return "other-watchdog";
    case ESP_RST_BROWNOUT:  return "BROWNOUT";
    case ESP_RST_DEEPSLEEP: return "deep-sleep";
    case ESP_RST_EXT:       return "external-pin";
    default:                return "unknown";
  }
}

// NTP sync notification (initial + each periodic re-sync).
void onNtpSync(struct timeval* tv) {
  logLine("NTP sync: clock updated");
}

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
// brownout -- the chosen CPU clock and WiFi power-save state come back.
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
uint32_t  g_lastRunTs   = 0;               // epoch the engine was last seen RUNNING (charging),
                                           // whoever started it -- key/FOB/board.  NVS "last_run"
float     g_lastV       = 0.0f;            // most recent voltage reading (owned by the safety task)
float     g_lastTemp    = 0.0f;            // most recent chip temp C (owned by the safety task)

// Safety task + watchdog (definitions in setup / after evalAutoStart). Declared
// here so evalAutoStart() and handleTransmit() can see g_rfMutex.
TaskHandle_t      g_safetyTask   = nullptr;
SemaphoreHandle_t g_rfMutex      = nullptr;
const uint32_t    WDT_TIMEOUT_MS = 30000;  // reboot if a watched task stalls this long

// WDT-culprit breadcrumbs. Each watched task stamps "what am I doing" into RTC
// memory, which survives a TASK-WATCHDOG reset -- so the next boot can report
// which task was stuck on what, instead of us guessing. On a reset the stuck
// task's mark shows the blocking op; the healthy task's mark just shows its last
// normal phase. A magic guards against garbage on a cold (power-on) boot.
RTC_NOINIT_ATTR static char     g_loopMark[28];
RTC_NOINIT_ATTR static char     g_safetyMark[28];
RTC_NOINIT_ATTR static uint32_t g_markMagic;
static const uint32_t MARK_MAGIC = 0x574D4B31;   // "WMK1"
static inline void loopMark(const char* s)   { strncpy(g_loopMark,   s, sizeof(g_loopMark) - 1);   g_loopMark[sizeof(g_loopMark) - 1] = 0; }
static inline void safetyMark(const char* s) { strncpy(g_safetyMark, s, sizeof(g_safetyMark) - 1); g_safetyMark[sizeof(g_safetyMark) - 1] = 0; }

// Extra restraint state. These stop the three ways an automatic starter
// misbehaves: firing while you're driving, firing over and over on a battery
// that never really recovers, and cranking a car that isn't going to start.
uint32_t  g_parkS       = 0;               // seconds continuously below AS_ALT_V (alternator off)
uint32_t  g_rearmS      = 0;               // seconds continuously at/above AS_REARM_V
bool      g_needRearm   = false;           // set after a start; blocks refiring until recovered
bool      g_asLock      = false;           // latched lockout  NVS "as_lock"
uint8_t   g_asFails     = 0;               // consecutive unverified starts  NVS "as_fails"
// ---- Long-term drain reference -------------------------------------------
// A 24 h RAM ring cannot measure a drain that plays out over days, and every
// reboot restarts it -- which is exactly what the /history watchdog stalls were
// doing. So anchor a reference point in NVS instead: 12 h after the engine last
// stopped (skipping the fast, fluctuating settling phase while surface charge
// dissipates), record time+voltage. The long-term rate is then simply the slope
// from that anchor to now, and it SURVIVES REBOOTS because both ends are stored.
const uint32_t LT_SETTLE_S = 12UL * 3600UL;   // ignore the first 12 h after a run
const uint32_t LT_MIN_S    = 6UL  * 3600UL;   // need this much baseline to report
uint32_t  g_ltRefTs  = 0;                  // epoch of the settled reference
float     g_ltRefV   = 0.0f;               // voltage at that reference
uint32_t  g_ltDue    = 0;                  // epoch when the reference should be taken

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
  uint32_t up_s;      // uptime seconds at fire -- orders events even with no clock
  float    vbatt;     // battery voltage at the moment it fired
  uint8_t  src;       // 0 = auto (low voltage), 1 = manual (dashboard button),
                      // 2 = external (key/FOB -- detected from alternator voltage)
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

// Bound how long a SINGLE chunked write may block.
//
// NetworkClient::write() retries up to WIFI_CLIENT_MAX_WRITE_RETRY (10) times,
// each doing a 1 s select() plus a send() bounded by SO_SNDTIMEO -- and
// WebServer sets that from HTTP_MAX_SEND_WAIT, which is 5000 ms. So one write
// against a stalled client can block ~10 x (1 + 5) = 60 s, TWICE the 30 s task
// watchdog.
//
// This is why fw 4.24 did not fix it. 4.24 fed the WDT *between* chunks, which
// helps a merely-slow client, but the feed sits AFTER the write -- and a write
// that never returns is never followed. Confirmed in the field on 4.26:
// "WDT stall: loop was '/history'". Bounding the socket to 1000 ms caps one
// write at ~10 x (1 + 1) = 20 s, comfortably inside the watchdog, after which
// the per-chunk feed does its job.
static void boundSendStall() { server.client().setTimeout(1000); }

// Wait until the socket will actually accept a write, WITHOUT ever entering a
// blocking write, and WITHOUT weakening the watchdog.
//
// Why the two previous attempts failed. NetworkClient::write() retries up to
// WIFI_CLIENT_MAX_WRITE_RETRY (10) times, each with a HARDCODED 1 s select()
// (WIFI_CLIENT_SELECT_TIMEOUT_US) -- so a single write has a ~10 s floor no
// matter what SO_SNDTIMEO is, and sendContent() issues several writes per chunk.
// fw 4.24 fed the WDT *between* chunks (never reached, the feed is after the
// write) and fw 4.27 shrank SO_SNDTIMEO (cannot touch the hardcoded select
// floor). Both still tripped the 30 s watchdog on /history.
//
// So: poll the socket ourselves in short slices and only send when it is ready.
// The WDT is reset each slice because WE own the wait -- it stays fully armed,
// and a genuine hang anywhere else still reboots the board as before. A client
// that never drains gets the response aborted rather than hanging the loop.
static bool waitWritable(uint32_t budget_ms) {
  int fd = server.client().fd();
  if (fd < 0) return false;
  uint32_t t0 = millis();
  while ((uint32_t)(millis() - t0) < budget_ms) {
    if (!server.client().connected()) return false;
    fd_set w; FD_ZERO(&w); FD_SET(fd, &w);
    struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 50 * 1000;   // 50 ms slice
    int r = ::select(fd + 1, nullptr, &w, nullptr, &tv);
    if (r > 0) return true;
    if (r < 0) return false;
    esp_task_wdt_reset();          // our wait, our feed -- watchdog stays armed
  }
  return false;                    // stalled client -> abort, do not block the loop
}

void trackReq() {
  String u = server.uri();
  g_in_total += u.length() + 120;   // approx request size
  // Finer WDT breadcrumb: record WHICH endpoint, not a generic 'http', so that
  // if a stall happens inside this handler the next boot names it. loopMark()
  // strncpy's into a fixed RTC buffer, so the temporary String is safe here.
  loopMark(u.c_str());
}

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

// ---------- battery drain rate ----------
// Least-squares fit of voltage against time over the most recent *parked*
// stretch of history, so the slope is the actual discharge rate.
//
// The point of this is the parasitic-drain hunt: pull one fuse, let it sit,
// and compare mV/h against the previous run. That turns "does this trace look
// flatter?" into a number, which is the difference between a diagnosis and a
// hunch. r2 is reported because over a short window the slope is mostly noise
// -- a low r2 means "don't trust this yet, leave it longer".
DrainFit computeDrain() {
  DrainFit f; f.ok = false; f.mvph = 0; f.r2 = 0; f.n = 0; f.win_s = 0; f.days = -1;
  if (!hist || histCount < 1) return f;
  // NB: don't early-return on histCount < DRAIN_MIN_N here -- that would report a
  // bare n=0 and the UI reads "0 of 30 min" (looks broken) whenever total history
  // is short, e.g. right after a reboot or a storage-format reset. Run Pass 1 so
  // f.n reflects the real partial window; the n < DRAIN_MIN_N guard below still
  // withholds the fitted slope until there's enough data.
  int oldest = (histCount < HIST_N) ? 0 : histHead;

  // Pass 1 -- find the most recent contiguous parked window (newest backwards).
  // Two passes rather than buffering, to keep ~11 KB of arrays off the heap.
  int n = 0; uint32_t prevTs = 0, oldTs = 0, newTs = 0; bool useTs = true; float vNow = 0;
  for (int k = histCount - 1; k >= 0; k--) {
    Sample& s = hist[(oldest + k) % HIST_N];
    if (s.vbatt >= AS_ALT_V) break;       // alternator was running -- window ends
    if (s.vbatt < 1.0f)      break;       // implausible / no source
    uint32_t t = (s.ts > 1700000000UL) ? s.ts : 0;
    if (!t) useTs = false;
    else if (prevTs && (prevTs - t) > DRAIN_GAP_S) break;   // gap: board was off
    if (n == 0) { vNow = s.vbatt; newTs = t; }              // newest end of the window
    if (t) prevTs = t;
    oldTs = t;
    n++;
  }
  // Report the partial window even when it's too short to fit, so the UI can
  // show honest progress ("need 10 more minutes") instead of a bare 0 that
  // reads as broken. A reboot ends the window, so this counts up from the
  // last power cycle, not from the start of history.
  f.n = n;
  if (n < DRAIN_MIN_N) return f;

  // Pass 2 -- two-variable least squares: V = a*x + b*T + c, x = time (s),
  // T = chip temperature. Temperature explains roughly half the parked voltage
  // wobble (diurnal warming/cooling), so a plain V-vs-time slope is noisy. The a
  // term here is the true discharge rate with that thermal swing regressed out;
  // b is the temperature coefficient (mV/degC). Falls back to a plain V-vs-time
  // fit when the window shows no temperature variation.
  double sx = 0, sy = 0, st = 0, sxx = 0, stt = 0, sxt = 0, sxy = 0, sty = 0, syy = 0;
  int i = 0;
  for (int k = histCount - 1; k >= 0 && i < n; k--, i++) {
    Sample& s = hist[(oldest + k) % HIST_N];
    double x = useTs ? (double)(s.ts - oldTs)
                     : (double)((n - 1 - i) * (SAMPLE_MS / 1000));
    double y = s.vbatt;
    double T = s.temp;
    sx += x; sy += y; st += T;
    sxx += x * x; stt += T * T; sxt += x * T;
    sxy += x * y; sty += T * y; syy += y * y;
  }
  double N = (double)n;
  double Sxx = sxx - sx * sx / N;      // centered cross-products
  double Stt = stt - st * st / N;
  double Sxt = sxt - sx * st / N;
  double Sxy = sxy - sx * sy / N;
  double Sty = sty - st * sy / N;
  double Syy = syy - sy * sy / N;
  if (Sxx <= 0 || Syy <= 0) return f;
  double slope, ssr, tempCoef = 0.0;
  double D = Sxx * Stt - Sxt * Sxt;
  if (Stt > 1e-9 && D > 1e-9) {                 // temperature varies -> 2-var fit
    slope    = (Sxy * Stt - Sty * Sxt) / D;     // V/s, thermal effect removed
    tempCoef = (Sty * Sxx - Sxy * Sxt) / D;     // V/degC
    ssr      = slope * Sxy + tempCoef * Sty;    // regression sum of squares
  } else {                                       // flat temp -> plain V-vs-time
    slope = Sxy / Sxx;
    ssr   = slope * Sxy;
  }
  f.r2 = (float)(ssr / Syy);
  if (f.r2 < 0) f.r2 = 0;
  if (f.r2 > 1) f.r2 = 1;
  f.mvph     = (float)(slope * 1000.0 * 3600.0);
  f.mv_per_c = (float)(tempCoef * 1000.0);
  f.n        = n;
  f.win_s    = (useTs && newTs > oldTs) ? (uint32_t)(newTs - oldTs)
                                        : (uint32_t)((n - 1) * (SAMPLE_MS / 1000));
  f.ok       = true;
  if (slope < 0) {                               // project time to flat at the true rate
    double perDay = -slope * 86400.0;
    if (perDay > 1e-9) f.days = (float)((vNow - DRAIN_FLAT_V) / perDay);
  }
  return f;
}

// Is the parked-drain fit trustworthy enough to GRAPH and to project an ETA from?
// computeDrain() reports ok as soon as there are 30 samples, but a just-settled
// or post-gap window can show a steep, spurious slope -- that is the "spike" that
// misrepresents the graph and produces a bogus short ETA ("4 h" when it's really
// days). Requiring a decent fit quality AND at least an hour of continuous parked
// data rejects those transients; until then drain reads "settling" (0 on the
// graph, no ETA) instead of a wrong number.
static const float    DRAIN_TRUST_R2  = 0.6f;
static const uint32_t DRAIN_TRUST_WIN = 3600;    // >= 1 h continuous parked window
static const float    DRAIN_MAX_MVPH  = 40.0f;   // |rate| above this isn't real parked drain --
                                                 // it's a confound (reboot-spanning fit, a CPU-clock
                                                 // voltage step, or post-drive surface-charge settling).
                                                 // The trigger is voltage-threshold based, so capping
                                                 // the *projection* here is safe; it just reads
                                                 // "settling" until a believable slow slope emerges.
static inline bool drainTrusted() {
  return g_drain.ok && g_drain.r2 >= DRAIN_TRUST_R2 && g_drain.win_s >= DRAIN_TRUST_WIN
      && fabsf(g_drain.mvph) <= DRAIN_MAX_MVPH;
}

// Estimated seconds until low-voltage auto-start would fire: project the current
// parked drain rate (g_drain) down to the trigger threshold, then add the
// sustain hold. Same math as the "projected to 11.8 V" countdown, but the target
// is the user's g_as_volts and it includes g_as_hold. Returns -1 when it doesn't
// apply -- auto-start disarmed or locked, no trustworthy discharging fit yet, or
// the battery is holding/charging so it won't reach the threshold at this rate.
// If voltage is already below the threshold, returns the sustain time remaining.
long autoStartEtaS(float v) {
  if (!g_as_en || g_asLock) return -1;
  if (v < g_as_volts) {                                 // already low: hold remaining
    if (g_lowSince == 0) return (long)g_as_hold;
    long rem = (long)g_as_hold - (long)((millis() - g_lowSince) / 1000);
    return rem > 0 ? rem : 0;
  }
  // Prefer the long-term anchored estimate wherever one exists. It spans days
  // from a fixed point at the SAME phase of the settling curve, so it is not
  // distorted by the diurnal thermal swing -- which on this car is ~130 mV
  // against a ~20 mV daily trend, i.e. the noise is several times the signal.
  // It also survives reboots. The 24 h least-squares fit below is only a
  // fallback for when no anchor exists yet: it is a short window, it restarts on
  // every reboot, and it reads day/night cycling as depletion.
  long lt = longTermEtaS(v);
  if (lt >= 0) return lt + (long)g_as_hold;

  if (!drainTrusted() || g_drain.mvph >= 0) return -1;  // no trustworthy fit, or not discharging
  double vps = (-(double)g_drain.mvph) / 3600000.0;     // mV/h -> V/s (positive)
  if (vps < 1e-12) return -1;
  double toThresh = ((double)v - (double)g_as_volts) / vps;
  return (long)(toThresh + (double)g_as_hold);
}


void recordSample() {
  if (!hist) return;
  Sample s;
  s.ts      = timeIsValid() ? (uint32_t)time(nullptr) : 0;
  s.vbatt   = readBatteryVolts();
  s.temp    = g_lastTemp;            // refreshed by the safety task just before this call
  s.heap_kb = (uint16_t)(ESP.getFreeHeap() / 1024);
  s.disk_kb = (uint16_t)(LittleFS.usedBytes() / 1024);
  s.net_in  = g_in_total  - lastInSnap;   lastInSnap  = g_in_total;
  s.net_out = g_out_total - lastOutSnap;  lastOutSnap = g_out_total;
  s.rssi    = apMode ? 0 : (int16_t)WiFi.RSSI();
  s.cpu0    = g_cpuN ? (uint8_t)(g_cpuAcc0 / g_cpuN + 0.5f) : (uint8_t)(g_cpu0 + 0.5f);
  s.cpu1    = g_cpuN ? (uint8_t)(g_cpuAcc1 / g_cpuN + 0.5f) : (uint8_t)(g_cpu1 + 0.5f);
  // Stored one cycle behind (computeDrain runs at the end of this function), which
  // is a 60 s lag on a metric that moves over hours -- fine. 0 until the fit is
  // trustworthy, so a settling/post-gap transient can't spike the graph.
  float mv = drainTrusted() ? g_drain.mvph : 0.0f;
  if (mv >  32000.0f) mv =  32000.0f;
  if (mv < -32000.0f) mv = -32000.0f;
  s.drain   = (int16_t)lroundf(mv);
  s.link_mbps = staLinkMbps();
  g_cpuAcc0 = 0; g_cpuAcc1 = 0; g_cpuN = 0;      // start a fresh interval average
  hist[histHead] = s;
  histHead = (histHead + 1) % HIST_N;
  if (histCount < HIST_N) histCount++;
  g_drain = computeDrain();     // refresh once per sample, not per HTTP poll

  // Board-health warning: log if free heap crosses below a floor (a leak or
  // memory pressure would show here) -- latched so it fires once per crossing.
  static bool heapWarned = false;
  uint32_t freeHeap = ESP.getFreeHeap();
  if (!heapWarned && freeHeap < 40000)      { logLine("WARNING: low heap %lu bytes", (unsigned long)freeHeap); heapWarned = true; }
  else if (heapWarned && freeHeap > 60000)  { logLine("heap recovered (%lu bytes)", (unsigned long)freeHeap); heapWarned = false; }
}

void saveHistory() {
  if (!hist) return;
  File f = LittleFS.open("/history.bin", FILE_WRITE);
  if (!f) return;
  uint32_t magic = 0x564F4C36;                 // "VOL6" (adds per-sample WiFi link rate)
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
  if (magic == 0x564F4C36) {           // older formats are discarded (layout changed)
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

// ---------- event-log persistence (LittleFS, survives reboot/brownout) ----------
// The event-log ring lives in PSRAM and is wiped on reboot -- useless for
// answering "what happened right before it rebooted". So we mirror it to a small
// rolling file: append new lines to /log.txt, and once it passes LOG_FILE_CAP
// rotate it to /log.old (one prior generation kept), bounding disk to ~2x the
// cap. On boot the tail of both files is replayed back into the ring. Every
// flash write happens from the loop via flushLogToFlash(); logLine() itself
// never touches the filesystem, so file I/O stays off the critical section and
// off the safety-task core.
static const char*  LOG_FILE     = "/log.txt";
static const char*  LOG_OLD      = "/log.old";
static const size_t LOG_FILE_CAP = 49152;      // 48 KB -> rotate; ~96 KB max on disk (of ~10 MB)

// Push a pre-formatted line straight into the ring, keeping its original
// timestamp and WITHOUT re-persisting it. Used only to replay flash on boot.
static void logRingPush(const char* line) {
  if (!g_log) return;
  strncpy(g_log[g_logHead], line, LOG_LEN - 1); g_log[g_logHead][LOG_LEN - 1] = 0;
  g_logHead = (g_logHead + 1) % LOG_LINES;
  if (g_logCount < LOG_LINES) g_logCount++;
}

// Append any ring lines not yet on flash. LOOP-CORE ONLY (single-threaded file
// I/O). Cheap when idle -- the fast path returns before taking the lock.
void flushLogToFlash() {
  if (g_logSeq == g_logPersistedSeq) return;         // nothing pending (unlocked read, benign)
  static char pend[16][LOG_LEN];                      // loop-only -> static is safe, keeps it off the stack
  int np = 0;
  portENTER_CRITICAL(&g_logMux);
  uint32_t behind = g_logSeq - g_logPersistedSeq;
  if (behind && g_log) {
    if (behind > 16) { g_logPersistedSeq = g_logSeq - 16; behind = 16; }  // fell behind: keep newest 16
    int idx = (g_logHead - (int)behind + LOG_LINES) % LOG_LINES;
    for (uint32_t k = 0; k < behind; k++) {
      strncpy(pend[np++], g_log[idx], LOG_LEN); idx = (idx + 1) % LOG_LINES;
    }
    g_logPersistedSeq = g_logSeq;
  }
  portEXIT_CRITICAL(&g_logMux);
  if (!np) return;

  File f = LittleFS.open(LOG_FILE, FILE_APPEND);
  if (!f) return;
  for (int k = 0; k < np; k++) { f.print(pend[k]); f.print('\n'); }
  size_t sz = f.size();
  f.close();
  if (sz >= LOG_FILE_CAP) {                    // keep exactly one prior generation
    LittleFS.remove(LOG_OLD);
    LittleFS.rename(LOG_FILE, LOG_OLD);
  }
}

// Replay the persisted tail (old generation first, then current) into the ring
// so /logtext shows history from before the reboot. Only the newest LOG_LINES
// survive the ring -- a naturally bounded tail. Everything read is already on
// flash, so the persist cursor is set caught-up.
void loadLogFromFlash() {
  const char* files[2] = { LOG_OLD, LOG_FILE };
  for (int i = 0; i < 2; i++) {
    if (!LittleFS.exists(files[i])) continue;
    File f = LittleFS.open(files[i], FILE_READ);
    if (!f) continue;
    while (f.available()) {
      String ln = f.readStringUntil('\n');
      ln.trim();                                // drop a trailing \r if present
      if (ln.length()) logRingPush(ln.c_str());
    }
    f.close();
  }
  portENTER_CRITICAL(&g_logMux);
  g_logSeq = g_logCount; g_logPersistedSeq = g_logSeq;   // loaded lines are already on flash
  portEXIT_CRITICAL(&g_logMux);
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
  saveStarts();                                 // write through -- don't risk losing it
  logLine("ENGINE START (%s) at %.2f V, tx=%s",
          src ? "manual" : "AUTO", v, ok ? "ok" : "FAILED");
  return idx;
}

// Begin watching for the alternator to come up, which is the only real proof
// the engine caught. Used for manual presses too -- it answers "did that work?"
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
// the cooldown SURVIVES A REBOOT -- otherwise a brownout right after a start
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

  // A reading outside this band isn't a car battery at all -- bench rig, an
  // unplugged sense wire, or a glitch. Treat it as "no information", never "low".
  bool valid = (v >= AS_V_FLOOR && v <= AS_V_CEIL);

  // --- accumulators, maintained regardless of arm state ---
  // Parked = the alternator is not running. Requiring a stretch of this before
  // arming means the starter can't fire while you're driving (or in the minutes
  // right after shutdown, when surface charge makes voltage untrustworthy).
  if (valid && v < AS_ALT_V) { if (g_parkS < 1000000UL) g_parkS += dt; }
  else                         g_parkS = 0;

  // Engine on/off from voltage -- ground truth regardless of who started it (key,
  // FOB, or the board). Alternator charging (>= AS_ALT_V) == running. Log both
  // edges and remember the last run, so "Last charge" is real even when the board
  // never issued a start. 0.3 V hysteresis so it can't chatter at the threshold.
  static bool     engRunning = false;
  static uint32_t engOnMs    = 0;
  bool nowRun = engRunning ? (v >= AS_ALT_V - 0.3f) : (valid && v >= AS_ALT_V);
  if (nowRun && !engRunning) {
    engOnMs     = millis();
    g_lastRunTs = timeIsValid() ? (uint32_t)time(nullptr) : 0;
    if (g_lastRunTs) prefs.putUInt("last_run", g_lastRunTs);
    logLine("ENGINE ON: alternator charging at %.2f V", v);
    // The engine is running but WE did not ask for it -- key, FOB or someone
    // else. Record it in the start history so the log is a complete account of
    // every run, not just board-fired ones. Guarded on g_verifying so a start
    // the board DID fire is not double-counted here.
    if (!g_verifying) {
      recordStart(v, 2, true);                    // src 2 = external
      if (g_startCount) {                         // it is confirmed by definition
        int i = (g_startHead - 1 + START_N) % START_N;
        g_starts[i].ver = 1; saveStarts();
      }
    }
    // A run invalidates the long-term reference; a fresh one is taken 12 h after
    // this run ends.
    g_ltRefTs = 0; g_ltRefV = 0; g_ltDue = 0;
    prefs.putUInt("lt_ref_ts", 0); prefs.putUInt("lt_due", 0);
  } else if (!nowRun && engRunning) {
    uint32_t ran = (millis() - engOnMs) / 1000;
    logLine("ENGINE OFF: charging ended at %.2f V after %lum %lus",
            v, (unsigned long)(ran / 60), (unsigned long)(ran % 60));
    if (timeIsValid()) {                          // arm the settled reference
      g_ltDue = (uint32_t)time(nullptr) + LT_SETTLE_S;
      prefs.putUInt("lt_due", g_ltDue);
      logLine("drain baseline: will anchor in %luh (after settling)",
              (unsigned long)(LT_SETTLE_S / 3600));
    }
  }
  engRunning = nowRun;

  // Take the settled reference once the 12 h wait is up. Also bootstrap one if
  // the engine last ran long ago and we simply have no anchor yet (e.g. this
  // firmware is new) -- the baseline then starts now rather than never.
  if (valid && timeIsValid() && !g_ltRefTs && !nowRun) {
    uint32_t nowS = (uint32_t)time(nullptr);
    if (g_ltDue && nowS >= g_ltDue) {          // exactly 12 h after the run ended
      g_ltRefTs = nowS; g_ltRefV = v; g_ltDue = 0;
      prefs.putUInt ("lt_ref_ts", g_ltRefTs);
      prefs.putFloat("lt_ref_v",  g_ltRefV);
      prefs.putUInt ("lt_due", 0);
      logLine("drain baseline anchored at %.2f V (settled, 12h after run)", v);
    } else if (!g_ltDue) {
      seedLongTermFromHistory();               // back-date it from the last run
    }
  }

  // Re-arm hysteresis. Without this, a battery that recovers to just above the
  // trigger re-fires every cooldown, forever, until the tank is empty.
  if (valid && v >= (g_as_volts + AS_REARM_MARGIN)) { if (g_rearmS < 1000000UL) g_rearmS += dt; }
  else                                                g_rearmS = 0;
  if (g_needRearm && g_rearmS >= AS_REARM_S) {
    g_needRearm = false;
    Serial.println("auto-start: re-armed (battery recovered and held)");
  }
  // Escape hatch -- a battery too tired to reach the re-arm bar must not wedge
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
      if (!g_verifyAuto) {                      // manual press -- record it, but never latch
        Serial.printf("manual start: no charging after %lu s (not counted toward lockout)\n",
                      (unsigned long)AS_VERIFY_S);
      } else {
        if (g_asFails < 255) g_asFails++;
        prefs.putUChar("as_fails", g_asFails);
        Serial.printf("auto-start: no charging after %lu s -- fail %u of %u\n",
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

  if (g_lowSince == 0) {                        // first low reading -- start the clock
    g_lowSince = now;
    Serial.printf("auto-start: %.2f V below %.2f V, counting %lu s\n",
                  v, g_as_volts, (unsigned long)g_as_hold);
    return;
  }
  if ((now - g_lowSince) / 1000 < g_as_hold) return;    // still sustaining
  if (autoStartCooldownLeft() > 0) return;              // too soon after the last one

  // ---- every guard passed: fire the starter ----
  // Serialize with any manual /transmit on the loop task: the CC1101 SPI and the
  // start log must never be touched by both tasks at once (prevents a double-fire).
  xSemaphoreTake(g_rfMutex, portMAX_DELAY);
  bool sent = radio.transmitButtonWakeup(COMPUSTAR_START, RF_WAKEUP_MS,
                                         RF_TRAIN_CELLS, RF_START_DATAREPS,
                                         RF_START_BURSTS, RF_GUARD_MS,
                                         RF_START_PKT_GAP_MS, RF_TAIL_CARRIER_MS);
  int idx = recordStart(v, 0, sent);
  xSemaphoreGive(g_rfMutex);
  g_lastStartMs = now;
  g_lastStartTs = timeIsValid() ? (uint32_t)time(nullptr) : 0;
  prefs.putUInt("as_last", g_lastStartTs);
  g_lowSince   = 0;
  g_needRearm  = true;   g_rearmS = 0;
  if (g_fires24 < 255) g_fires24++;
  beginVerify(idx, true);
  Serial.printf("*** AUTO-START FIRED at %.2f V (tx %s) -- cooldown %lu s, fire %u today (cap %s) ***\n",
                v, sent ? "ok" : "FAILED", (unsigned long)g_as_cool, g_fires24,
                g_as_max24 > 0 ? String(g_as_max24).c_str() : "none");
}

// ---------- safety task + watchdog ----------
// The battery sampling and the low-voltage auto-start decision run in their OWN
// FreeRTOS task on core 0, independent of the loop() on core 1 that services
// WiFi/HTTP/SNMP. A weak-signal WiFi reassociation (or any HTTP stall) can no
// longer starve the safety check -- the whole reason the board is in the car.
// This task OWNS the ADC (readBatteryVolts): everything else reads the cached
// g_lastV, so there is no cross-core ADC contention. RF transmits are serialized
// with the loop's manual /transmit via g_rfMutex. (Globals declared near g_lastV.)
void safetyTaskFn(void*) {
  esp_task_wdt_add(nullptr);               // this task is watched too
  uint32_t lastSample = 0, lastEval = 0;
  for (;;) {
    esp_task_wdt_reset();
    uint32_t now = millis();
    safetyMark("adc");   readBatteryVolts();       // ADC owner -> refreshes g_lastV / g_last_mv (I2C to ADS1115)
    safetyMark("temp");  g_lastTemp = temperatureRead();
    if (now - lastEval   >= 1000)      { safetyMark("autostart"); lastEval   = now; evalAutoStart(g_lastV); }
    if (now - lastSample >= SAMPLE_MS) { safetyMark("sample");    lastSample = now; recordSample(); }
    safetyMark("idle");                            // if starved off core 0, this is the frozen mark
    vTaskDelay(pdMS_TO_TICKS(250));
  }
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
static void oidTempDc(SnmpValue& v)   { v.type = SNMP_INT;       v.i = (int32_t)(g_lastTemp * 10.0f); }
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
// Drain: microvolts/hour keeps full precision in a plain signed integer, so
// Cacti needs no scaling gymnastics. Negative = discharging.
static void oidDrainUvph(SnmpValue& v){ v.type = SNMP_INT;     v.i = (int32_t)(g_drain.mvph * 1000.0f); }
static void oidDrainFit(SnmpValue& v) { v.type = SNMP_GAUGE32; v.u = (uint32_t)(g_drain.r2 * 100.0f + 0.5f); }
static void oidDrainWin(SnmpValue& v) { v.type = SNMP_GAUGE32; v.u = g_drain.win_s; }
static void oidDrainHrs(SnmpValue& v) { v.type = SNMP_GAUGE32;
                                        v.u = (g_drain.days > 0) ? (uint32_t)(g_drain.days * 24.0f) : 0; }
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
static void oidAsEtaS(SnmpValue& v)   { v.type = SNMP_GAUGE32;   long e = autoStartEtaS(g_lastV); v.u = (e > 0) ? (uint32_t)e : 0; }

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
  { 26, oidDrainUvph, "drainMicrovoltsPerHour" },
  { 27, oidDrainFit,  "drainFitPct"      },
  { 28, oidDrainWin,  "drainWindowSec"   },
  { 29, oidDrainHrs,  "drainHoursToFlat" },
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
  { 44, oidAsEtaS,    "autoStartEtaSec"  },
};

const char APP_CSS[] PROGMEM = R"CSS(
.wrow{display:flex;gap:12px;flex-wrap:wrap;align-items:flex-end}
select.inp{background:#0d1117;color:#c9d1d9;border:1px solid #30363d;border-radius:6px;padding:5px 6px}

:root{--bg:#0d1117;--card:#161b22;--fg:#e6edf3;--mut:#8b949e}
*{box-sizing:border-box}
body{margin:0;font-family:system-ui,Segoe UI,Roboto,sans-serif;background:var(--bg);color:var(--fg)}
header{padding:12px 20px;border-bottom:1px solid #21262d;display:flex;justify-content:space-between;align-items:center;flex-wrap:wrap;gap:8px}
h1{font-size:15px;margin:0;font-weight:600}
#dot{width:10px;height:10px;border-radius:50%;background:var(--mut);display:inline-block;margin-right:6px}
#status{font-size:13px;color:var(--mut)}
nav.tabs{display:flex;gap:2px;flex-wrap:wrap;background:var(--card);border-bottom:1px solid #21262d;padding:0 8px;position:sticky;top:0;z-index:20}
nav.tabs a{color:var(--mut);text-decoration:none;padding:11px 13px;font-size:14px;border-bottom:2px solid transparent;white-space:nowrap}
nav.tabs a:hover{color:var(--fg)}
nav.tabs a.on{color:var(--fg);border-bottom-color:#1f6feb;font-weight:600}
.wrap{max-width:820px;margin:0 auto;padding:18px 20px}
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
.card .v{font-size:18px;font-weight:600;margin-top:4px;word-break:break-word}
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
.pill.ext{background:#8957e5;color:#fff}
)CSS";
const char APP_JS[] PROGMEM = R"JS(
// Shared engine for all vroom dashboard tabs. Each page sets window.PAGE
// (its graph config) before loading this; every DOM update is null-guarded so
// the same poll()/handlers run on any page regardless of which ids it contains.
function $(i){return document.getElementById(i)}
function T(id,v){var e=$(id);if(e!=null)e.textContent=v}                 // safe textContent
function C(id,c){var e=$(id);if(e!=null)e.style.color=c}                 // safe color
function H(id,h){var e=$(id);if(e!=null)e.innerHTML=h}                   // safe innerHTML
function fmtUp(s){var h=Math.floor(s/3600),m=Math.floor(s%3600/60),x=s%60;return h?h+"h "+m+"m":m?m+"m "+x+"s":x+"s"}
function fmtB(b){if(b===undefined||b===null)return "--";if(b<1024)return b+" B";if(b<1048576)return (b/1024).toFixed(1)+" KB";return (b/1048576).toFixed(2)+" MB"}
function fmtDate(ts){if(!ts)return "--";var x=new Date(ts*1000);
  return x.toLocaleDateString(undefined,{weekday:"short",month:"short",day:"numeric"})+" "+
         x.toLocaleTimeString(undefined,{hour:"2-digit",minute:"2-digit"});}
function fmtEta(s){if(s===undefined||s===null||s<0)return null;if(s<3600)return "~"+Math.max(1,Math.round(s/60))+" min";if(s<86400)return "~"+(s/3600).toFixed(1)+" h";return "~"+(s/86400).toFixed(1)+" days"}

// ---- graph engine (mirrors the original single-page one; min/max lines on all) ----
var PAD=8, CHARTS={}, TS=[], hoverIdx=-1;
var IDS=(window.PAGE&&window.PAGE.charts||[]).map(function(c){return c.id});
function fmtWhen(i){var t=TS[i];if(t&&t>1700000000){return new Date(t*1000).toLocaleString();}
  var m=(TS.length-1-i);return m>=60?(Math.floor(m/60)+"h "+(m%60)+"m ago"):(m+"m ago");}
function drawChart(id){
  var ch=CHARTS[id];if(!ch)return;var c=$(id);if(!c)return;
  var x=c.getContext("2d"),W=c.width,Ht=c.height;x.clearRect(0,0,W,Ht);
  var data=ch.data,N=data.length;if(N<2)return;var lo=ch.lo,hi=ch.hi;
  function gx(i){return PAD+i*(W-2*PAD)/(N-1)}function gy(v){return Ht-PAD-(v-lo)/(hi-lo)*(Ht-2*PAD)}
  if(ch.minmax){x.setLineDash([4,3]);x.lineWidth=1;x.font="11px system-ui";
    [["max",ch.mx,-4],["min",ch.mn,12]].forEach(function(m){var yy=gy(m[1]);
      x.strokeStyle="#6e7681";x.beginPath();x.moveTo(PAD,yy);x.lineTo(W-PAD,yy);x.stroke();
      x.fillStyle="#8b949e";x.fillText(m[0]+" "+m[1].toFixed(ch.dec)+" "+ch.unit,W-118,yy+m[2])});
    x.setLineDash([])}
  x.strokeStyle=ch.color;x.lineWidth=1.5;x.beginPath();
  data.forEach(function(v,i){i?x.lineTo(gx(i),gy(v)):x.moveTo(gx(i),gy(v))});x.stroke();
  if(hoverIdx>=0&&hoverIdx<N){var hx=gx(hoverIdx),hy=gy(data[hoverIdx]);
    x.strokeStyle="#6e7681";x.lineWidth=1;x.beginPath();x.moveTo(hx,PAD);x.lineTo(hx,Ht-PAD);x.stroke();
    x.fillStyle=ch.color;x.beginPath();x.arc(hx,hy,3.5,0,6.2832);x.fill();}
}
function drawAll(){IDS.forEach(drawChart)}
function idxFromEvent(ev,id){var ch=CHARTS[id];if(!ch||ch.data.length<2)return -1;
  var c=$(id),r=c.getBoundingClientRect(),W=c.width,N=ch.data.length;
  var xint=(ev.clientX-r.left)/r.width*W;var i=Math.round((xint-PAD)/((W-2*PAD)/(N-1)));
  return Math.max(0,Math.min(N-1,i));}
function showTip(ev,id){var ch=CHARTS[id];if(!ch||hoverIdx<0)return;
  H("tip","<b>"+ch.data[hoverIdx].toFixed(ch.dec)+" "+ch.unit+"</b><br>"+fmtWhen(hoverIdx));
  var tp=$("tip");if(!tp)return;tp.style.display="block";
  var tx=ev.clientX+12;if(tx+170>window.innerWidth)tx=ev.clientX-160;
  tp.style.left=tx+"px";tp.style.top=(ev.clientY+12)+"px";}
function setupHover(){IDS.forEach(function(id){var c=$(id);if(!c)return;
  c.addEventListener("mousemove",function(ev){hoverIdx=idxFromEvent(ev,id);drawAll();showTip(ev,id)});
  c.addEventListener("mouseleave",function(){hoverIdx=-1;drawAll();var tp=$("tip");if(tp)tp.style.display="none"});})}
function loadHistory(){
  if(!window.PAGE||!window.PAGE.charts||!window.PAGE.charts.length)return;
  fetch("/history?cols="+window.PAGE.cols.join(","),{cache:"no-store"}).then(function(r){return r.text()}).then(function(t){
    var ln=t.trim().split("\n");if(ln.length<2)return;
    var hdr=ln[0].split(","),idx={};hdr.forEach(function(h,i){idx[h]=i});
    var rows=[];for(var i=1;i<ln.length;i++){rows.push(ln[i].split(",").map(Number))}
    if(!rows.length)return;TS=rows.map(function(r){return r[idx.ts]});
    window.PAGE.charts.forEach(function(cfg){var ci=idx[cfg.col];if(ci===undefined)return;
      var data=rows.map(function(r){return r[ci]});
      var mn=Math.min.apply(null,data),mx=Math.max.apply(null,data);
      var lo=mn,hi=mx;if(hi-lo<1e-6){hi+=1;lo-=1}
      if(cfg.anchor0){lo=0;hi=Math.max(cfg.floor||10,hi)}
      if(cfg.keep0){lo=Math.min(0,lo);hi=Math.max(0,hi);if(hi-lo<1e-6){hi+=1;lo-=1}}
      var pad=Math.max((hi-lo)*0.15,0.02);lo-=pad;hi+=pad;
      CHARTS[cfg.id]={data:data,lo:lo,hi:hi,color:cfg.color,dec:cfg.dec,unit:cfg.unit,mn:mn,mx:mx,minmax:true};});
    drawAll();
  }).catch(function(e){});
}

// ---- live values (every assignment guarded; a page only has some of these) ----
function poll(){fetch("/json",{cache:"no-store"}).then(function(r){return r.json()}).then(function(d){
  var vcol={red:"#f85149",blue:"#58a6ff",green:"#3fb950"};
  T("vbatt",d.vbatt.toFixed(2));C("vbatt",vcol[d.led]||"#e6edf3");
  T("temp",d.temp_c.toFixed(1));T("adc",d.adc_mv);
  T("rssi",d.rssi);T("up",fmtUp(d.uptime_s));
  T("ssid",d.ssid||"--");T("bssid",d.bssid||"--");T("ch",d.ch);
  T("phy",d.phy||"--");T("txpwr",(d.txpwr_dbm!==undefined?d.txpwr_dbm.toFixed(1):"--"));T("proto",d.proto||"--");
  T("heap",Math.floor(d.heap_free/1024)+"/"+Math.floor(d.heap_total/1024)+" KB");
  T("psram",(d.psram_free/1048576).toFixed(2)+"/"+(d.psram_total/1048576).toFixed(2)+" MB");
  T("disk",Math.floor(d.disk_used/1024)+"/"+Math.floor(d.disk_total/1024)+" KB");
  T("vstat",d.led);C("vstat",vcol[d.led]||"#e6edf3");
  T("ntp",d.time_ok?"synced":"not synced");C("ntp",d.time_ok?"#3fb950":"#d29922");
  T("nin",fmtB(d.net_in));T("nout",fmtB(d.net_out));
  if(!d.drain_ok){T("drate","--");C("drate","#8b949e");T("dproj","--");
    T("dmeta","settling: "+(d.drain_n||0)+" of 30 min needed. The window restarts at every reboot and at any gap in the log, so a slope is never fitted across a hole.");
  }else{var mv=d.drain_mvph,r2=d.drain_r2,hrs=(d.drain_win_s/3600);
    T("drate",(mv>0?"+":"")+mv.toFixed(1)+" mV/h");
    C("drate",(mv>=0)?"#3fb950":(mv>-3?"#e6edf3":(mv>-10?"#d29922":"#f85149")));
    T("dproj",(d.drain_days>0)?(d.drain_days<1?(Math.round(d.drain_days*24)+" h"):(d.drain_days.toFixed(1)+" days")):"n/a");
    var trust=r2>=0.9?"solid":(r2>=0.6?"usable":"too noisy to trust yet");
    T("dmeta","fit r^2="+r2.toFixed(2)+" ("+trust+") | "+hrs.toFixed(1)+" h window | "+d.drain_n+" samples"
      +((d.drain_mvpc!==undefined&&d.drain_mvpc!=0)?" | temp-comp "+d.drain_mvpc.toFixed(1)+" mV/degC":"")
      +(r2<0.6?" -- leave it sitting longer before comparing":""));
    C("dmeta",(r2<0.6)?"#d29922":"#8b949e");}
  T("sub","divider x"+d.divider+" | cal "+d.cal+" | ADC "+d.adc_mv+" mV | 1 sample/"+d.interval_s+"s");
  T("net",(d.mode?d.mode.toUpperCase():"")+" | "+(d.ip||""));T("ns",d.samples);T("fw",d.fw||"?");
  T("clk",d.time_ok?new Date(d.epoch*1000).toLocaleTimeString():"no NTP");
  var RF={armed:"armed",blocked:"present, no patterns",absent:"not detected",off:"disabled"};
  T("rfstat",RF[d.rf]||d.rf||"?");C("rfstat",(d.rf=="armed")?"#3fb950":(d.rf=="blocked"?"#d29922":"#8b949e"));
  T("cpu",d.cpu_mhz);
  if(d.cpu0!==undefined){T("cpu0",d.cpu0.toFixed(1));T("cpu1",d.cpu1.toFixed(1));T("cpuavg",Math.round((d.cpu0+d.cpu1)/2));}
  document.querySelectorAll("button.seg").forEach(function(b){b.classList.toggle("on",+b.getAttribute("data-mhz")===d.cpu_mhz)});
  var ps=!!d.wifi_ps;H("psbadge",'<span class="badge '+(ps?"on":"off")+'">'+(ps?"ON":"OFF")+'</span>');
  var pb=$("psbtn");if(pb){pb.textContent=ps?"Turn OFF":"Turn ON";pb.setAttribute("data-next",ps?"0":"1");}
  // long-term drain (Voltage tab)
  if(d.lt_ref_ts!==undefined){
    var lte=fmtEta(d.lt_eta_s);
    T("lteta", d.lt_ref_ts?(lte||(d.lt_mvph>=-0.05?"holding":"settling")):"no baseline yet");
    C("lteta", !d.lt_ref_ts?"#8b949e":(d.lt_eta_s<0?"#3fb950":"#d29922"));
    T("ltmv", d.lt_ref_ts?d.lt_mvph.toFixed(2):"--");
    T("ltref", d.lt_ref_ts?new Date(d.lt_ref_ts*1000).toLocaleString():"waiting 12 h after last run");
    T("ltrefv", d.lt_ref_ts?d.lt_ref_v.toFixed(2):"--");
    // Actual calendar dates, not countdowns. as_eta_s is the canonical time to
    // auto-start (it respects enable/lockout and now sources the long-term
    // anchor), so the projected date is simply now + that.
    var nextTs = (d.as_eta_s>0&&d.epoch) ? (d.epoch+d.as_eta_s) : 0;
    T("ltnext", nextTs?fmtDate(nextTs):(d.as_en?"not projected":"auto-start off"));
    C("ltnext", nextTs?"":"#8b949e");
    // as_last is written only by the auto-start fire path, so it IS the last
    // automatic start -- never a manual button press or a key/FOB start.
    T("ltlastauto", d.as_last?fmtDate(d.as_last):"never");
    C("ltlastauto", d.as_last?"":"#8b949e");
    // Whole projected cycle: the duration AND both endpoints.
    if(d.last_run&&nextTs){
      T("ltcycle",(fmtEta(nextTs-d.last_run)||"--").replace("~",""));
      T("ltcycledates", fmtDate(d.last_run)+"  \u2192  "+fmtDate(nextTs));
      // How far through this discharge cycle we are: elapsed / total span.
      var pct = nextTs>d.last_run ? (d.epoch-d.last_run)/(nextTs-d.last_run)*100 : -1;
      pct = Math.max(0, Math.min(100, pct));
      T("ltpct", pct.toFixed(0)+"% elapsed");
      var b=$("ltbar");
      if(b){ b.style.width = pct+"%";
             b.style.background = pct<60?"#3fb950":(pct<85?"#d29922":"#f85149"); }
    }else{
      T("ltpct","");
      var b0=$("ltbar"); if(b0) b0.style.width="0%";
      T("ltcycle", d.last_run?"--":"no run recorded");
      T("ltcycledates", d.last_run?("last run "+fmtDate(d.last_run)+"  \u2192  next not projected"):"--");
    }
  }
  var ae=!!d.as_en,eta=fmtEta(d.as_eta_s);
  T("aseta",ae?(eta||(d.drain_ok?"holding":"settling")):"--");
  C("aseta",(!ae||d.as_eta_s<0)?"#8b949e":(d.drain_r2<0.6?"#d29922":"#3fb950"));
  H("asbadge",'<span class="badge '+(ae?"arm":"off")+'">'+(ae?"ARMED":"OFF")+'</span>');
  var ab=$("asbtn");if(ab){ab.textContent=ae?"Disable":"Enable";ab.setAttribute("data-next",ae?"0":"1");}
  var ST={off:"disabled -- the car will not start itself",
    lockout:"LOCKED OUT -- "+d.as_fails+" starts in a row drew no charge. Check the car, then clear the lockout.",
    "not-armed":"cannot fire -- RF not armed (check CC1101 / patterns)",
    warmup:"warming up after boot -- holding off",
    "no-battery":"sensor is not across a battery ("+d.vbatt.toFixed(2)+" V) -- will not fire",
    verifying:"just started -- watching for the alternator to come up",
    recovering:"waiting for the battery to recover and hold before it may fire again",
    "daily-cap":"24 h cap reached ("+d.as_f24+" of "+d.as_max24+") -- holding off",
    "park-wait":"confirming the car is parked ("+d.as_park_s+"s of "+d.as_park_need+"s below 13.2 V)",
    watching:"armed | watching -- above "+(d.as_volts||0).toFixed(1)+" V"+(fmtEta(d.as_eta_s)?" ("+fmtEta(d.as_eta_s)+" to auto-start)":"")};
  var s=ST[d.as_state]||d.as_state;
  if(d.as_state=="counting")s="voltage low "+d.as_low_s+"s of "+d.as_hold+"s -- starts in "+Math.max(0,d.as_hold-d.as_low_s)+"s";
  if(d.as_state=="cooldown")s="cooldown -- "+fmtUp(d.as_cool_s)+" before it can fire again";
  T("asstate",s);C("asstate",(d.as_state=="counting"||d.as_state=="lockout")?"#f85149":(ae?"#3fb950":"#8b949e"));
  var au=$("asunlock");if(au)au.style.display=d.as_lock?"inline-block":"none";
  T("aslow",d.as_low_s?(d.as_low_s+" / "+d.as_hold+" s"):"--");C("aslow",d.as_low_s?"#f85149":"#e6edf3");
  T("ascool",d.as_cool_s?fmtUp(d.as_cool_s):"clear");
  T("aspark",d.as_park_s>=d.as_park_need?"confirmed":(d.as_park_s+" / "+d.as_park_need+" s"));
  C("aspark",(d.as_park_s>=d.as_park_need)?"#3fb950":"#8b949e");
  T("asf24",d.as_f24+(d.as_max24>0?(" / "+d.as_max24):" (no cap)"));
  T("asfail",d.as_fails+" / 2");C("asfail",d.as_fails?"#d29922":"#e6edf3");
  T("aslock",d.as_lock?"LOCKED":"no");C("aslock",d.as_lock?"#f85149":"#3fb950");
  T("asn",d.as_n);
  T("aslast",(d.as_last>1700000000)?new Date(d.as_last*1000).toLocaleString():"never");
  T("assince",(d.as_last>1700000000&&d.epoch>d.as_last)?(fmtUp(d.epoch-d.as_last)+" ago"):"--");
  T("lastrun",(d.last_run>1700000000)?new Date(d.last_run*1000).toLocaleString():"never");
  T("runsince",(d.last_run>1700000000&&d.epoch>d.last_run)?(fmtUp(d.epoch-d.last_run)+" ago"):"--");
  var af=document.activeElement?document.activeElement.id:"";
  if(af!="asv"&&$("asv"))$("asv").value=(d.as_volts||0).toFixed(1);
  if(af!="ash"&&$("ash"))$("ash").value=d.as_hold;
  if(af!="asc"&&$("asc"))$("asc").value=d.as_cool;
  if(af!="asm"&&$("asm"))$("asm").value=d.as_max24;
  C("dot","#3fb950");T("stxt","live");
}).catch(function(e){C("dot","#d29922");T("stxt","reconnecting...")})}

function loadStarts(){if(!$("startbox"))return;
  fetch("/starts",{cache:"no-store"}).then(function(r){return r.json()}).then(function(a){
    if(!a||!a.length){H("startbox",'<div class="k">no starts recorded</div>');return}
    var h='<table class="st"><tr><th>When</th><th>Voltage</th><th>Trigger</th><th>TX</th><th>Engine</th></tr>';
    var VER=['<span style="color:#8b949e">unknown</span>','<span style="color:#3fb950">ran</span>','<span style="color:#f85149">no charge</span>'];
    a.forEach(function(e){var w=(e.ts>1700000000)?new Date(e.ts*1000).toLocaleString():("uptime "+fmtUp(e.up_s)+" | no clock");
      h+='<tr><td>'+w+'</td><td>'+e.v.toFixed(2)+' V</td><td><span class="pill '+(e.src=="auto"?"auto":(e.src=="external"?"ext":"man"))+'">'+e.src+'</span></td><td>'+(e.ok?"sent":"<span style=\"color:#f85149\">failed</span>")+'</td><td>'+(VER[e.ver]||VER[0])+'</td></tr>';});
    H("startbox",h+'</table>');
  }).catch(function(e){})}

// ---- controls (all on the Main page; guarded so other pages skip them) ----
function attachHandlers(){
  document.querySelectorAll("button.tx[data-b]").forEach(function(btn){btn.addEventListener("click",function(){
    var b=btn.getAttribute("data-b");
    if(b=="START"&&!confirm("Start the engine now? This transmits the real remote-start code and CRANKS THE ENGINE if the car is in range."))return;
    T("rfmsg",b+" ...");
    fetch("/transmit?button="+b,{method:"POST"}).then(function(r){return r.json()}).then(function(d){
      T("rfmsg",d.ok?(b+" sent x"+d.repeats):(b+" failed: "+(d.detail||"error")));
    }).catch(function(e){T("rfmsg",b+" request error")})})});
  document.querySelectorAll("button.seg").forEach(function(b){b.addEventListener("click",function(){
    var m=b.getAttribute("data-mhz");T("pwrmsg","setting CPU to "+m+" MHz...");
    fetch("/cpu?mhz="+m,{method:"POST"}).then(function(r){return r.json()}).then(function(d){
      T("pwrmsg",d.ok?("CPU now "+d.cpu_mhz+" MHz"):("CPU change failed: "+(d.detail||"error")));poll();
    }).catch(function(e){T("pwrmsg","CPU request error")})})});
  var psb=$("psbtn");if(psb)psb.addEventListener("click",function(){
    var nx=psb.getAttribute("data-next")||"0";T("pwrmsg","updating WiFi power saving...");
    fetch("/wifips?on="+nx,{method:"POST"}).then(function(r){return r.json()}).then(function(d){
      T("pwrmsg","WiFi power saving "+(d.wifi_ps?"ON":"OFF"));poll();
    }).catch(function(e){T("pwrmsg","WiFi power-save request error")})});
  var asb=$("asbtn");if(asb)asb.addEventListener("click",function(){
    var nx=asb.getAttribute("data-next")||"0";
    if(nx=="1"&&!confirm("ARM low-voltage auto-start?\n\nThe car will CRANK BY ITSELF, unattended, whenever battery voltage stays at or below the threshold for the hold time.\n\nNever leave this armed while the car is parked indoors or in an attached garage -- engine exhaust in an enclosed space is lethal."))return;
    T("asmsg","updating...");
    fetch("/autostart?en="+nx,{method:"POST"}).then(function(r){return r.json()}).then(function(d){
      T("asmsg","auto-start "+(d.as_en?"ARMED":"disabled"));poll();
    }).catch(function(e){T("asmsg","request error")})});
  var ass=$("assave");if(ass)ass.addEventListener("click",function(){
    var q="volts="+$("asv").value+"&hold="+$("ash").value+"&cool="+$("asc").value+"&max24="+$("asm").value;
    T("asmsg","saving...");
    fetch("/autostart?"+q,{method:"POST"}).then(function(r){return r.json()}).then(function(d){
      T("asmsg",d.ok?("saved -- start below "+d.as_volts.toFixed(1)+" V held "+d.as_hold+" s"):("save failed: "+(d.detail||"error")));poll();
    }).catch(function(e){T("asmsg","save error")})});
  var asu=$("asunlock");if(asu)asu.addEventListener("click",function(){
    if(!confirm("Clear the auto-start lockout?\n\nIt latched because starts drew no charge -- the engine did not catch. Make sure you know why before re-enabling."))return;
    fetch("/autostart?unlock=1",{method:"POST"}).then(function(r){return r.json()}).then(function(d){
      T("asmsg","lockout cleared");poll();}).catch(function(e){T("asmsg","unlock error")})});
  var asc=$("asclear");if(asc)asc.addEventListener("click",function(){
    if(!confirm("Clear the entire start history?"))return;
    fetch("/starts",{method:"POST"}).then(function(){loadStarts();T("asmsg","start log cleared")})
    .catch(function(e){T("asmsg","clear error")})});
  var rb=$("rebootbtn");if(rb)rb.addEventListener("click",function(e){e.preventDefault();
    if(!confirm("Reboot the board now?\n\nIt drops off WiFi for a few seconds, then the dashboard reconnects. Auto-start protection resumes on boot."))return;
    C("dot","#d29922");T("stxt","rebooting...");
    fetch("/reboot",{method:"POST"}).catch(function(){});setTimeout(poll,9000);});
  // ---- WiFi configuration (WiFi tab only; all null-guarded) ----
  function wcSet(id,v){var e=$(id);if(e!=null&&v!==undefined&&v!==null)e.value=v}
  function wcHint(){var a=+($("wAfter")||{}).value||0,r=+($("wRetry")||{}).value||0;
    T("wTimerHint","= raise the AP after "+(a/60).toFixed(a%60?1:0)+" min, then retry home every "+(r/60).toFixed(r%60?1:0)+" min");}
  function wcLoad(){
    fetch("/wificfg",{cache:"no-store"}).then(function(r){return r.json()}).then(function(d){
      wcSet("wSsid",d.sta_ssid); wcSet("wMinsec",d.sta_minsec);
      wcSet("wApSsid",d.ap_ssid); wcSet("wApAuth",d.ap_auth); wcSet("wApChan",d.ap_chan);
      var h=$("wApHid"); if(h)h.checked=!!d.ap_hidden;
      wcSet("wAfter",d.ap_after_s); wcSet("wRetry",d.ap_retry_s);
      wcSet("wWait",d.ap_wait_s); wcSet("wBoot",d.boot_s);
      wcSet("wTx",d.tx_dbm); wcSet("wProto",d.proto); wcSet("wHost",d.hostname);
      var p=$("wPass"),ap=$("wApPass");
      if(p)p.placeholder=d.sta_pass_set?"unchanged (set)":"not set";
      if(ap)ap.placeholder=d.ap_pass_set?"unchanged (set)":"not set";
      wcHint();
    }).catch(function(){T("wMsg","could not load current settings")});
  }
  if($("wSsid")){
    wcLoad();
    ["wAfter","wRetry"].forEach(function(i){var e=$(i);if(e)e.addEventListener("input",wcHint)});
    var sb=$("wScanBtn");if(sb)sb.addEventListener("click",function(){
      T("wScanMsg","scanning (a few seconds; the link may blip)...");
      fetch("/scan",{cache:"no-store"}).then(function(r){return r.json()}).then(function(d){
        var dl=$("wScan");if(dl){dl.innerHTML="";
          var seen={};d.aps.forEach(function(a){if(a.ssid&&!seen[a.ssid]){seen[a.ssid]=1;
            var o=document.createElement("option");o.value=a.ssid;dl.appendChild(o)}})}
        var top=d.aps.slice(0,6).map(function(a){return (a.ssid||"(hidden)")+" "+a.rssi+"dBm ch"+a.ch}).join(" | ");
        T("wScanMsg",d.n+" networks, own link "+d.self_rssi+" dBm -- "+top);
      }).catch(function(){T("wScanMsg","scan failed (the link may have dropped during it)")});
    });
    var sv=$("wSave");if(sv)sv.addEventListener("click",function(){
      var ss=$("wSsid").value, pw2=$("wPass").value;
      if(!confirm("Save WiFi settings?\n\nSSID: "+ss+(pw2?"\nPassword: (changing)":"\nPassword: (unchanged)")+
        "\n\nIf the SSID or password changed the board switches now and this page will drop for a few seconds. "+
        "If the new network does not come up it reverts to the old one automatically. Check the Log tab for the result."))return;
      var kv=[["sta_ssid",ss],["sta_pass",pw2],["sta_minsec",$("wMinsec").value],
        ["ap_ssid",$("wApSsid").value],["ap_pass",$("wApPass").value],["ap_auth",$("wApAuth").value],
        ["ap_chan",$("wApChan").value],["ap_hidden",$("wApHid").checked?"1":"0"],
        ["ap_after_s",$("wAfter").value],["ap_retry_s",$("wRetry").value],
        ["ap_wait_s",$("wWait").value],["boot_s",$("wBoot").value],
        ["tx_dbm",$("wTx").value],["proto",$("wProto").value],["hostname",$("wHost").value]];
      var body=kv.map(function(x){return encodeURIComponent(x[0])+"="+encodeURIComponent(x[1])}).join("&");
      T("wMsg","saving...");
      fetch("/wificfg",{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded"},body:body})
        .then(function(r){return r.json()}).then(function(d){
          T("wMsg",d.ok?d.detail:("save failed: "+(d.detail||"error")));
          if(d.ok){var p=$("wPass");if(p)p.value="";var a=$("wApPass");if(a)a.value="";
            if(d.pending)setTimeout(wcLoad,12000); else wcLoad();}
        }).catch(function(){T("wMsg","request error -- if the SSID changed this is expected; check the Log tab")});
    });
  }

  var pw=$("powerbtn");if(pw)pw.addEventListener("click",function(e){e.preventDefault();
    if(!confirm("Power-up mode?\n\nDisables WiFi power-save and sets the CPU to 240 MHz (both persist across reboot). Snappier + more reliable link, at a bit more current draw."))return;
    fetch("/powerup",{method:"POST"}).then(function(r){return r.json()}).then(function(d){
      T("pwrmsg","power-up: CPU "+d.cpu_mhz+" MHz, WiFi PS "+(d.wifi_ps?"on":"off"));poll();})
    .catch(function(){T("pwrmsg","power-up request error")})});
}

// ---- init ----
(function(){
  // highlight the active tab by pathname
  var p=location.pathname;document.querySelectorAll("nav.tabs a").forEach(function(a){
    if(a.getAttribute("data-p")===p)a.classList.add("on");});
  attachHandlers();setupHover();
  poll();setInterval(poll,2000);
  if(window.PAGE&&window.PAGE.charts&&window.PAGE.charts.length){loadHistory();setInterval(loadHistory,30000);}
  if($("startbox")){loadStarts();setInterval(loadStarts,15000);}
})();
)JS";
const char MAIN_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>vroom &middot; Main</title><link rel="stylesheet" href="/app.css?v=431"></head><body>
<div id="tip"></div>
<header><h1>&#9889; ESP32-S3 Voltage Monitor</h1>
<span id="status"><span id="dot"></span><span id="stxt">connecting&hellip;</span></span></header>
<nav class="tabs">
<a href="/" data-p="/">Main</a>
<a href="/wifi" data-p="/wifi">WiFi / Net</a>
<a href="/voltage" data-p="/voltage">Voltage</a>
<a href="/cpu" data-p="/cpu">CPU</a>
<a href="/memdisk" data-p="/memdisk">Mem / Disk</a>
<a href="/logs" data-p="/logs">Log</a>
<a href="/update" data-p="/update">Update</a>
</nav>
<div class="wrap">
<div class="hero">
<div class="metric"><div class="lbl">Voltage</div><span class="big" id="vbatt">--</span><span class="u"> V</span></div>
<div class="metric"><div class="lbl">Chip temp</div><span class="big" id="temp">--</span><span class="u"> &deg;C</span></div>
<div class="metric"><div class="lbl">Uptime</div><span class="big" id="up" style="font-size:26px">--</span></div>
</div>
<div class="sub" id="sub">waiting for data&hellip;</div>

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
still cranks easily. Below about <b>11.8 V</b> it likely won't crank at all. The hold time ignores the brief dip
while the engine is <i>actually cranking</i>. It won't fire while you're driving and won't re-fire until the
battery recovers and holds. <b>Max per 24 h</b> is off by default (<b>0</b>/<b>&minus;1</b> = unlimited). The
real runaway guard is the lockout: if <b>2</b> starts in a row draw no charge, it latches off until you clear it.
</div>
<div class="k" id="asmsg" style="margin-top:8px">&nbsp;</div>
</div>
<div class="grid" style="margin-top:0">
<div class="card"><div class="k">Low for</div><div class="v"><span id="aslow">--</span></div></div>
<div class="card"><div class="k">Est. to auto-start</div><div class="v"><span id="aseta">--</span></div></div>
<div class="card"><div class="k">Cooldown left</div><div class="v"><span id="ascool">--</span></div></div>
<div class="card"><div class="k">Park confirm</div><div class="v"><span id="aspark">--</span></div></div>
<div class="card"><div class="k">Auto-starts 24 h</div><div class="v"><span id="asf24">--</span></div></div>
<div class="card"><div class="k">Fail streak</div><div class="v"><span id="asfail">--</span></div></div>
<div class="card"><div class="k">Lockout</div><div class="v"><span id="aslock">--</span></div></div>
<div class="card"><div class="k">Starts logged</div><div class="v"><span id="asn">--</span></div></div>
<div class="card"><div class="k">Last start (cmd)</div><div class="v" style="font-size:13px" id="aslast">--</div></div>
<div class="card"><div class="k">Since last start</div><div class="v"><span id="assince">--</span></div></div>
<div class="card"><div class="k">Last charge (ran)</div><div class="v" style="font-size:13px" id="lastrun">--</div></div>
<div class="card"><div class="k">Since last charge</div><div class="v"><span id="runsince">--</span></div></div>
</div>

<div class="clbl">Start history</div>
<div class="card" style="margin-bottom:10px">
<div id="startbox"><div class="k">no starts recorded</div></div>
<div style="margin-top:10px"><button class="tx" id="asclear">Clear log</button></div>
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
<div class="pwrrow" style="margin-top:12px;padding-top:12px;border-top:1px solid #21262d">
<div><div class="k">Board</div><div class="v" style="font-size:13px">power-up = 240 MHz + PS off</div></div>
<div style="display:flex;gap:8px"><button class="tx" id="powerbtn">Power-up</button><button class="tx" id="rebootbtn" style="border-color:#8957e5">Reboot</button></div>
</div>
<div class="k" id="pwrmsg" style="margin-top:10px">&nbsp;</div>
</div>
</div>
<footer><span id="net">&hellip;</span> &middot; fw <span id="fw">?</span> &middot; samples <span id="ns">0</span>/1440 &middot; <span id="clk">--</span></footer>
<script src="/app.js?v=431"></script>
</body></html>
)HTML";
const char WIFI_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>vroom &middot; WiFi / Net</title><link rel="stylesheet" href="/app.css?v=431"></head><body>
<div id="tip"></div>
<header><h1>&#9889; ESP32-S3 &middot; WiFi / Network</h1>
<span id="status"><span id="dot"></span><span id="stxt">connecting&hellip;</span></span></header>
<nav class="tabs">
<a href="/" data-p="/">Main</a>
<a href="/wifi" data-p="/wifi">WiFi / Net</a>
<a href="/voltage" data-p="/voltage">Voltage</a>
<a href="/cpu" data-p="/cpu">CPU</a>
<a href="/memdisk" data-p="/memdisk">Mem / Disk</a>
<a href="/logs" data-p="/logs">Log</a>
<a href="/update" data-p="/update">Update</a>
</nav>
<div class="wrap">
<div class="hero">
<div class="metric"><div class="lbl">RSSI</div><span class="big" id="rssi">--</span><span class="u"> dBm</span></div>
<div class="metric"><div class="lbl">PHY / link</div><span class="big" id="phy" style="font-size:26px">--</span></div>
<div class="metric"><div class="lbl">TX power</div><span class="big" id="txpwr" style="font-size:30px">--</span><span class="u"> dBm</span></div>
</div>
<div class="grid">
<div class="card"><div class="k">SSID</div><div class="v" id="ssid">--</div></div>
<div class="card"><div class="k">BSSID (AP)</div><div class="v" style="font-size:14px" id="bssid">--</div></div>
<div class="card"><div class="k">Channel</div><div class="v"><span id="ch">--</span></div></div>
<div class="card"><div class="k">Protocol</div><div class="v" id="proto">--</div></div>
<div class="card"><div class="k">Mode / IP</div><div class="v" style="font-size:13px" id="net">--</div></div>
<div class="card"><div class="k">Clock (NTP)</div><div class="v" id="ntp">--</div></div>
<div class="card"><div class="k">HTTP in total</div><div class="v"><span id="nin">--</span></div></div>
<div class="card"><div class="k">HTTP out total</div><div class="v"><span id="nout">--</span></div></div>
</div>
<div class="clbl">WiFi configuration</div>
<div class="card" style="margin-bottom:10px">

<div class="k" style="margin-bottom:6px">Home network &mdash; what it connects to</div>
<div class="wrow">
<div><div class="k">SSID</div><input id="wSsid" class="inp" style="width:190px" list="wScan" autocomplete="off"><datalist id="wScan"></datalist></div>
<div><div class="k">Password</div><input id="wPass" class="inp" type="password" style="width:170px" placeholder="unchanged" autocomplete="new-password"></div>
<div><div class="k">Min security</div><select id="wMinsec" class="inp"><option value="0">Any / open</option><option value="1">WPA or better</option><option value="2">WPA2 or better</option><option value="3">WPA3 only</option></select></div>
<button class="tx" id="wScanBtn">Scan for networks</button>
</div>
<div class="k" id="wScanMsg" style="margin-top:6px">&nbsp;</div>

<div class="k" style="margin:14px 0 6px;padding-top:12px;border-top:1px solid #21262d">Fallback access point &mdash; what it becomes when it can't connect</div>
<div class="wrow">
<div><div class="k">AP SSID</div><input id="wApSsid" class="inp" style="width:190px" autocomplete="off"></div>
<div><div class="k">AP password</div><input id="wApPass" class="inp" type="password" style="width:170px" placeholder="unchanged" autocomplete="new-password"></div>
<div><div class="k">AP security</div><select id="wApAuth" class="inp"><option value="0">Open</option><option value="1">WPA2-PSK</option><option value="2">WPA / WPA2</option><option value="3">WPA2 / WPA3</option></select></div>
<div><div class="k">AP channel</div><input id="wApChan" class="inp" type="number" min="1" max="13" style="width:70px"></div>
<div><div class="k">Hidden</div><input id="wApHid" type="checkbox" style="transform:scale(1.4);margin-top:8px"></div>
</div>

<div class="k" style="margin:14px 0 6px;padding-top:12px;border-top:1px solid #21262d">Timers</div>
<div class="wrow">
<div><div class="k">Give up &amp; raise AP after</div><input id="wAfter" class="inp" type="number" min="30" max="3600" style="width:90px"> <span class="k">s (30&ndash;3600)</span></div>
<div><div class="k">Retry home every</div><input id="wRetry" class="inp" type="number" min="60" max="7200" style="width:90px"> <span class="k">s (60&ndash;7200)</span></div>
<div><div class="k">Each retry waits</div><input id="wWait" class="inp" type="number" min="5" max="120" style="width:80px"> <span class="k">s (5&ndash;120)</span></div>
<div><div class="k">Boot connect window</div><input id="wBoot" class="inp" type="number" min="5" max="300" style="width:80px"> <span class="k">s (5&ndash;300)</span></div>
</div>
<div class="k" id="wTimerHint" style="margin-top:6px">&nbsp;</div>

<div class="k" style="margin:14px 0 6px;padding-top:12px;border-top:1px solid #21262d">Radio</div>
<div class="wrow">
<div><div class="k">TX power</div><select id="wTx" class="inp"><option value="19.5">19.5 dBm (max)</option><option value="19">19 dBm</option><option value="18.5">18.5 dBm</option><option value="17">17 dBm</option><option value="15">15 dBm</option><option value="13">13 dBm</option><option value="11">11 dBm</option><option value="8.5">8.5 dBm</option><option value="7">7 dBm</option><option value="5">5 dBm</option><option value="2">2 dBm</option><option value="-1">-1 dBm</option></select></div>
<div><div class="k">Protocol</div><select id="wProto" class="inp"><option value="7">b/g/n (default)</option><option value="3">b/g</option><option value="1">b only (see warning)</option><option value="15">b/g/n + LR</option></select></div>
<div><div class="k">Hostname</div><input id="wHost" class="inp" style="width:150px" autocomplete="off"></div>
<button class="tx" id="wSave" style="border-color:#8957e5">Save WiFi settings</button>
</div>
<div class="k" id="wMsg" style="margin-top:10px">&nbsp;</div>

<div class="k" style="margin-top:12px;line-height:1.7;text-transform:none;letter-spacing:0">
Changing the <b>SSID or password</b> is applied live and <b>verified</b>: if the new network does not come up
within the boot-connect window, the board <b>reverts to the previous one by itself</b>. You will lose this page
for a few seconds either way &mdash; check the <b>Log</b> tab for the result, which is written to flash and
survives a reboot. <b>b only</b> is kept for completeness but was a disaster against this AP in fw 4.18
(latency ~1.7 s, 20&nbsp;% loss, watchdog reboot loop) &mdash; leave it on b/g/n unless testing. Every setting
here is stored in NVS and survives reboots.
</div>
</div>

<div class="clbl">WiFi RSSI dBm (24 h)</div><canvas id="g_rssi" width="800" height="104"></canvas>
<div class="clbl">WiFi link rate Mbps (24 h) &mdash; from negotiated PHY</div><canvas id="g_link" width="800" height="104"></canvas>
<div class="clbl">Network in B/min (24 h)</div><canvas id="g_nin" width="800" height="104"></canvas>
<div class="clbl">Network out B/min (24 h)</div><canvas id="g_nout" width="800" height="104"></canvas>
</div>
<footer>fw <span id="fw">?</span> &middot; samples <span id="ns">0</span>/1440 &middot; <span id="clk">--</span></footer>
<script>window.PAGE={cols:["rssi","link","net_in","net_out"],charts:[
{id:"g_rssi",col:"rssi",dec:0,unit:"dBm",color:"#f778ba"},
{id:"g_link",col:"link",dec:0,unit:"Mbps",color:"#39c5cf",anchor0:true,floor:20},
{id:"g_nin",col:"net_in",dec:0,unit:"B/min",color:"#ffa657"},
{id:"g_nout",col:"net_out",dec:0,unit:"B/min",color:"#7ee787"}]};</script>
<script src="/app.js?v=431"></script>
</body></html>
)HTML";
const char VOLT_HTML[] PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>vroom &middot; Voltage</title><link rel="stylesheet" href="/app.css?v=431"></head><body>
<div id="tip"></div>
<header><h1>&#9889; ESP32-S3 &middot; Voltage</h1>
<span id="status"><span id="dot"></span><span id="stxt">connecting&hellip;</span></span></header>
<nav class="tabs">
<a href="/" data-p="/">Main</a>
<a href="/wifi" data-p="/wifi">WiFi / Net</a>
<a href="/voltage" data-p="/voltage">Voltage</a>
<a href="/cpu" data-p="/cpu">CPU</a>
<a href="/memdisk" data-p="/memdisk">Mem / Disk</a>
<a href="/logs" data-p="/logs">Log</a>
<a href="/update" data-p="/update">Update</a>
</nav>
<div class="wrap">
<div class="hero">
<div class="metric"><div class="lbl">Voltage</div><span class="big" id="vbatt">--</span><span class="u"> V</span></div>
<div class="metric"><div class="lbl">Status</div><span class="big" id="vstat" style="font-size:26px">--</span></div>
<div class="metric"><div class="lbl">Chip temp</div><span class="big" id="temp">--</span><span class="u"> &deg;C</span></div>
</div>
<div class="sub" id="sub">waiting for data&hellip;</div>
<div class="card" style="margin:6px 0 8px">
<div class="pwrrow">
<div><div class="k">Battery drain rate</div><div class="v" style="font-size:24px"><span id="drate">--</span></div></div>
<div style="text-align:right"><div class="k">Projected to 11.8 V</div><div class="v" id="dproj">--</div></div>
</div>
<div class="k" id="dmeta" style="margin-top:8px;text-transform:none;letter-spacing:0">&nbsp;</div>
</div>
<div class="clbl">Long-term drain &mdash; the days-to-weeks view</div>
<div class="card" style="margin-bottom:10px">
<div class="grid" style="margin-top:0">
<div class="card"><div class="k">Est. time to auto-start</div><div class="v"><span id="lteta">--</span></div></div>
<div class="card"><div class="k">Long-term rate</div><div class="v"><span id="ltmv">--</span> mV/h</div></div>
<div class="card"><div class="k">Baseline anchored</div><div class="v" style="font-size:13px" id="ltref">--</div></div>
<div class="card"><div class="k">Baseline voltage</div><div class="v"><span id="ltrefv">--</span> V</div></div>
<div class="card"><div class="k">Next auto-start</div><div class="v" style="font-size:15px" id="ltnext">--</div></div>
<div class="card"><div class="k">Last auto-start</div><div class="v" style="font-size:15px" id="ltlastauto">--</div></div>
<div class="card" style="grid-column:span 2"><div class="k">Last run &rarr; next auto-start</div>
<div class="v"><span id="ltcycle">--</span> <span id="ltpct" style="font-size:15px;color:#8b949e"></span></div>
<div style="margin-top:8px;height:10px;background:#21262d;border-radius:5px;overflow:hidden">
<div id="ltbar" style="height:100%;width:0%;background:#3fb950;border-radius:5px;transition:width .4s"></div></div>
<div class="k" style="text-transform:none;letter-spacing:0;margin-top:6px;font-size:12px" id="ltcycledates">--</div></div>
</div>
<div class="k" style="margin-top:10px;line-height:1.7;text-transform:none;letter-spacing:0">
The 24&nbsp;h graphs below cannot see a drain that plays out over days, and their window restarts on every
reboot. This estimate instead anchors a single reference point &mdash; time and voltage &mdash; <b>12&nbsp;hours
after the engine last stopped</b>, skipping the fast, fluctuating settling phase while surface charge
dissipates, and measures the slope from there to now. Both ends live in flash, so <b>it survives reboots</b>
and keeps extending for as long as the car sits. It needs 6&nbsp;h of baseline before it will report.
</div>
</div>
<div class="clbl">Voltage (24 h)</div><canvas id="g_v" width="800" height="104"></canvas>
<div class="clbl">Temperature &deg;C (24 h)</div><canvas id="g_t" width="800" height="104"></canvas>
<div class="clbl">Battery drain rate mV/h (24 h) &mdash; below 0 = discharging</div><canvas id="g_d" width="800" height="104"></canvas>
<div class="grid">
<div class="card"><div class="k">ADC node</div><div class="v"><span id="adc">--</span> mV</div></div>
<div class="card"><div class="k">Voltage status</div><div class="v" id="vstat2">--</div></div>
</div>
</div>
<footer><span id="net">&hellip;</span> &middot; fw <span id="fw">?</span> &middot; samples <span id="ns">0</span>/1440 &middot; <span id="clk">--</span></footer>
<script>window.PAGE={cols:["vbatt","temp","drain"],charts:[
{id:"g_v",col:"vbatt",dec:2,unit:"V",color:"#3fb950"},
{id:"g_t",col:"temp",dec:1,unit:"degC",color:"#d29922"},
{id:"g_d",col:"drain",dec:0,unit:"mV/h",color:"#ff7b72",keep0:true}]};</script>
<script src="/app.js?v=431"></script>
</body></html>
)HTML";
const char CPU_HTML[]  PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>vroom &middot; CPU</title><link rel="stylesheet" href="/app.css?v=431"></head><body>
<div id="tip"></div>
<header><h1>&#9889; ESP32-S3 &middot; CPU</h1>
<span id="status"><span id="dot"></span><span id="stxt">connecting&hellip;</span></span></header>
<nav class="tabs">
<a href="/" data-p="/">Main</a>
<a href="/wifi" data-p="/wifi">WiFi / Net</a>
<a href="/voltage" data-p="/voltage">Voltage</a>
<a href="/cpu" data-p="/cpu">CPU</a>
<a href="/memdisk" data-p="/memdisk">Mem / Disk</a>
<a href="/logs" data-p="/logs">Log</a>
<a href="/update" data-p="/update">Update</a>
</nav>
<div class="wrap">
<div class="hero">
<div class="metric"><div class="lbl">CPU load (avg)</div><span class="big" id="cpuavg">--</span><span class="u"> %</span></div>
<div class="metric"><div class="lbl">Clock</div><span class="big" id="cpu">--</span><span class="u"> MHz</span></div>
<div class="metric"><div class="lbl">Uptime</div><span class="big" id="up" style="font-size:26px">--</span></div>
</div>
<div class="grid">
<div class="card"><div class="k">CPU core 0</div><div class="v"><span id="cpu0">--</span> %</div></div>
<div class="card"><div class="k">CPU core 1</div><div class="v"><span id="cpu1">--</span> %</div></div>
<div class="card"><div class="k">Chip temp</div><div class="v"><span id="temp">--</span> &deg;C</div></div>
<div class="card"><div class="k">Clock (NTP)</div><div class="v" id="ntp">--</div></div>
</div>
<div class="clbl">CPU core 0 load % (24 h)</div><canvas id="g_c0" width="800" height="104"></canvas>
<div class="clbl">CPU core 1 load % (24 h)</div><canvas id="g_c1" width="800" height="104"></canvas>
</div>
<footer><span id="net">&hellip;</span> &middot; fw <span id="fw">?</span> &middot; samples <span id="ns">0</span>/1440 &middot; <span id="clk">--</span></footer>
<script>window.PAGE={cols:["cpu0","cpu1"],charts:[
{id:"g_c0",col:"cpu0",dec:0,unit:"%",color:"#7ee787",anchor0:true},
{id:"g_c1",col:"cpu1",dec:0,unit:"%",color:"#e3b341",anchor0:true}]};</script>
<script src="/app.js?v=431"></script>
</body></html>
)HTML";
const char MEM_HTML[]  PROGMEM = R"HTML(
<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>vroom &middot; Mem / Disk</title><link rel="stylesheet" href="/app.css?v=431"></head><body>
<div id="tip"></div>
<header><h1>&#9889; ESP32-S3 &middot; Memory / Disk</h1>
<span id="status"><span id="dot"></span><span id="stxt">connecting&hellip;</span></span></header>
<nav class="tabs">
<a href="/" data-p="/">Main</a>
<a href="/wifi" data-p="/wifi">WiFi / Net</a>
<a href="/voltage" data-p="/voltage">Voltage</a>
<a href="/cpu" data-p="/cpu">CPU</a>
<a href="/memdisk" data-p="/memdisk">Mem / Disk</a>
<a href="/logs" data-p="/logs">Log</a>
<a href="/update" data-p="/update">Update</a>
</nav>
<div class="wrap">
<div class="grid">
<div class="card"><div class="k">Free heap</div><div class="v"><span id="heap">--</span></div></div>
<div class="card"><div class="k">Free PSRAM</div><div class="v"><span id="psram">--</span></div></div>
<div class="card"><div class="k">Disk used</div><div class="v"><span id="disk">--</span></div></div>
</div>
<div class="clbl">Free memory KB (24 h)</div><canvas id="g_heap" width="800" height="104"></canvas>
<div class="clbl">Disk used KB (24 h)</div><canvas id="g_disk" width="800" height="104"></canvas>
</div>
<footer><span id="net">&hellip;</span> &middot; fw <span id="fw">?</span> &middot; samples <span id="ns">0</span>/1440 &middot; <span id="clk">--</span></footer>
<script>window.PAGE={cols:["heap_kb","disk_kb"],charts:[
{id:"g_heap",col:"heap_kb",dec:0,unit:"KB",color:"#58a6ff"},
{id:"g_disk",col:"disk_kb",dec:0,unit:"KB",color:"#bc8cff"}]};</script>
<script src="/app.js?v=431"></script>
</body></html>
)HTML";

// Shared assets are cacheable (versioned via ?v= in each page's tags) so tab
// switches over a weak link don't re-download the CSS/JS every time.
static void sendCached(const char* ctype, const char* body) {
  server.sendHeader("Cache-Control", "max-age=86400");
  g_out_total += strlen_P(body);
  server.send_P(200, ctype, body);
}
static void sendPage(const char* body) {
  g_out_total += strlen_P(body);
  server.send_P(200, "text/html; charset=utf-8", body);
}
void handleAppCss()      { trackReq(); sendCached("text/css; charset=utf-8", APP_CSS); }
void handleAppJs()       { trackReq(); sendCached("application/javascript; charset=utf-8", APP_JS); }
void handleDash()        { trackReq(); sendPage(MAIN_HTML); }   // "/" = Main tab
void handleWifiPage()    { trackReq(); sendPage(WIFI_HTML); }
void handleVoltagePage() { trackReq(); sendPage(VOLT_HTML); }
void handleCpuPage()     { trackReq(); sendPage(CPU_HTML); }
void handleMemPage()     { trackReq(); sendPage(MEM_HTML); }


void handleJson() {
  trackReq();
  float v  = g_lastV;                // cached by the safety task (it owns the ADC)
  float tC = g_lastTemp;             // cached by the safety task (it owns the temp sensor)
  String ip = apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
  int    rssi = apMode ? 0 : (int)WiFi.RSSI();
  // WiFi link detail (kept in locals so the String temporaries survive snprintf).
  String ssid  = apMode ? g_ap_ssid : WiFi.SSID();
  String bssid = apMode ? String("")      : WiFi.BSSIDstr();
  int    chan  = WiFi.channel();
  int8_t txq = 0; esp_wifi_get_max_tx_power(&txq);        // 0.25 dBm units
  uint8_t proto = 0; esp_wifi_get_protocol(WIFI_IF_STA, &proto);
  char pbuf[8]; int pi = 0;
  if (proto & WIFI_PROTOCOL_11B) pbuf[pi++] = 'b';
  if (proto & WIFI_PROTOCOL_11G) pbuf[pi++] = 'g';
  if (proto & WIFI_PROTOCOL_11N) pbuf[pi++] = 'n';
  if (proto & WIFI_PROTOCOL_LR)  pbuf[pi++] = 'L';
  pbuf[pi] = 0;
  char json[1650];
  snprintf(json, sizeof(json),
    "{\"vbatt\":%.2f,\"temp_c\":%.1f,\"adc_mv\":%d,\"divider\":%.3f,\"cal\":%.3f,"
    "\"rssi\":%d,\"uptime_s\":%lu,\"heap_free\":%u,\"heap_total\":%u,"
    "\"psram_free\":%u,\"psram_total\":%u,\"disk_used\":%u,\"disk_total\":%u,"
    "\"mode\":\"%s\",\"ip\":\"%s\",\"interval_s\":%d,\"samples\":%d,\"led\":\"%s\",\"fw\":\"%s\",\"rf\":\"%s\","
    "\"ssid\":\"%s\",\"bssid\":\"%s\",\"ch\":%d,\"phy\":\"%s\",\"txpwr_dbm\":%.2f,\"proto\":\"%s\","
    "\"epoch\":%lu,\"time_ok\":%s,\"cpu_mhz\":%u,\"wifi_ps\":%s,"
    "\"as_en\":%s,\"as_volts\":%.2f,\"as_hold\":%lu,\"as_cool\":%lu,"
    "\"as_state\":\"%s\",\"as_low_s\":%lu,\"as_cool_s\":%lu,\"as_n\":%d,"
    "\"as_lock\":%s,\"as_fails\":%u,\"as_park_s\":%lu,\"as_park_need\":%lu,\"as_f24\":%u,"
    "\"as_max24\":%d,\"as_eta_s\":%ld,\"cpu0\":%.1f,\"cpu1\":%.1f,"
    "\"net_in\":%lu,\"net_out\":%lu,\"as_last\":%lu,\"last_run\":%lu,"
    "\"drain_ok\":%s,\"drain_mvph\":%.2f,\"drain_r2\":%.3f,\"drain_n\":%d,"
    "\"drain_win_s\":%lu,\"drain_days\":%.2f,\"drain_mvpc\":%.1f,"
    "\"lt_ref_ts\":%lu,\"lt_ref_v\":%.2f,\"lt_mvph\":%.2f,\"lt_eta_s\":%ld}",
    v, tC, g_last_mv, DIVIDER, CAL, rssi, (unsigned long)(millis() / 1000),
    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getHeapSize(),
    (unsigned)ESP.getFreePsram(), (unsigned)ESP.getPsramSize(),
    (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes(),
    apMode ? "ap" : "sta", ip.c_str(), (int)(SAMPLE_MS / 1000), histCount, voltStatus(v), FW_VERSION,
    rfStatusStr(),
    ssid.c_str(), bssid.c_str(), chan, apMode ? "AP" : staPhyMode(), txq / 4.0f, pbuf,
    (unsigned long)(timeIsValid() ? (uint32_t)time(nullptr) : 0), timeIsValid() ? "true" : "false",
    (unsigned)getCpuFrequencyMhz(), WiFi.getSleep() ? "true" : "false",
    g_as_en ? "true" : "false", g_as_volts,
    (unsigned long)g_as_hold, (unsigned long)g_as_cool, autoStartState(),
    (unsigned long)(g_lowSince ? (millis() - g_lowSince) / 1000 : 0),
    (unsigned long)autoStartCooldownLeft(), g_startCount,
    g_asLock ? "true" : "false", g_asFails,
    (unsigned long)g_parkS, (unsigned long)AS_PARK_S, g_fires24,
    g_as_max24, autoStartEtaS(v), g_cpu0, g_cpu1,
    (unsigned long)g_in_total, (unsigned long)g_out_total,
    (unsigned long)g_lastStartTs, (unsigned long)g_lastRunTs,
    g_drain.ok ? "true" : "false", g_drain.mvph, g_drain.r2, g_drain.n,
    (unsigned long)g_drain.win_s, g_drain.days, g_drain.mv_per_c,
    (unsigned long)g_ltRefTs, g_ltRefV, longTermMvph(v), longTermEtaS(v));
  g_out_total += strlen(json);
  server.send(200, "application/json", json);
}

// Streams the ring buffer oldest->newest as CSV (one line per sample).
// GET /history[?cols=a,b,c] -- the ring buffer oldest->newest as CSV. ts is
// always the first column; ?cols= selects a subset by name so each dashboard
// page downloads only the columns it graphs (a weak link isn't asked to move
// every column). No cols= returns all. Column names:
//   vbatt temp heap_kb disk_kb net_in net_out rssi cpu0 cpu1 drain link
void handleHistory() {
  trackReq();
  boundSendStall();
  static const char* NM[] = {"vbatt","temp","heap_kb","disk_kb","net_in",
                             "net_out","rssi","cpu0","cpu1","drain","link"};
  const int NC = 11;
  bool sel[NC];
  String want = server.arg("cols");
  for (int i = 0; i < NC; i++) sel[i] = want.length() ? (want.indexOf(NM[i]) >= 0) : true;

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");
  String hdr = "ts";
  for (int i = 0; i < NC; i++) if (sel[i]) { hdr += ','; hdr += NM[i]; }
  hdr += '\n';
  g_out_total += hdr.length(); server.sendContent(hdr);

  if (hist) {
    int oldest = (histCount < HIST_N) ? 0 : histHead;
    String chunk; chunk.reserve(2048);
    for (int n = 0; n < histCount; n++) {
      Sample& s = hist[(oldest + n) % HIST_N];
      chunk += s.ts;
      if (sel[0])  { chunk += ','; chunk += String(s.vbatt, 2); }
      if (sel[1])  { chunk += ','; chunk += String(s.temp, 1); }
      if (sel[2])  { chunk += ','; chunk += s.heap_kb; }
      if (sel[3])  { chunk += ','; chunk += s.disk_kb; }
      if (sel[4])  { chunk += ','; chunk += s.net_in; }
      if (sel[5])  { chunk += ','; chunk += s.net_out; }
      if (sel[6])  { chunk += ','; chunk += s.rssi; }
      if (sel[7])  { chunk += ','; chunk += s.cpu0; }
      if (sel[8])  { chunk += ','; chunk += s.cpu1; }
      if (sel[9])  { chunk += ','; chunk += s.drain; }
      if (sel[10]) { chunk += ','; chunk += s.link_mbps; }
      chunk += '\n';
      if (chunk.length() > 1500) {
        if (!waitWritable(4000)) break;               // stalled client -> abort rather than block
        g_out_total += chunk.length(); server.sendContent(chunk); chunk = ""; esp_task_wdt_reset(); }
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

  // START needs the wake-up carrier (duty-cycled receiver); lock/unlock/trunk
  // are short taps the receiver catches without it.
  bool sent;
  xSemaphoreTake(g_rfMutex, portMAX_DELAY);   // serialize with the safety task's auto-fire
  if (btn == "START") {
    sent = radio.transmitButtonWakeup(pattern, RF_WAKEUP_MS, RF_TRAIN_CELLS,
                                      RF_START_DATAREPS, RF_START_BURSTS, RF_GUARD_MS,
                                      RF_START_PKT_GAP_MS, RF_TAIL_CARRIER_MS);
    beginVerify(recordStart(g_lastV, 1, sent), false);   // log the attempt
  } else {
    sent = radio.transmitButton(pattern, RF_REPEATS, RF_GUARD_MS);
  }
  xSemaphoreGive(g_rfMutex);
  if (!sent) { fail(500, "bad pattern or TX failed"); return; }

  char j[128];
  snprintf(j, sizeof(j), "{\"ok\":true,\"button\":\"%s\",\"bursts\":%d}",
           btn.c_str(), (btn == "START") ? RF_START_BURSTS : RF_REPEATS);
  g_out_total += strlen(j);
  server.send(200, "application/json", j);
  Serial.printf("RF TX: %s x%d\n", btn.c_str(), RF_REPEATS);
}

// GET /xtaltest -- measure the CC1101 crystal (no SDR). Our frequency words
// assume a 26 MHz xtal to hit 433.92 MHz; a 27 MHz part actually radiates near
// 450 MHz and the car can't hear it, while every register readback still passes.
void handleXtalTest() {
  trackReq();
  if (!RF_ENABLED || !rfReady) {
    const char* m = "{\"ok\":false,\"detail\":\"RF disabled or CC1101 absent\"}";
    g_out_total += strlen(m); server.send(503, "application/json", m); return;
  }
  uint32_t xtal = radio.measureXtalHz();
  double actualMHz = 433.92 * ((double)xtal / 26.0e6);
  const char* verdict;
  if      (xtal >= 25.7e6 && xtal <= 26.3e6) verdict = "26MHz-ok";
  else if (xtal >= 26.7e6 && xtal <= 27.3e6) verdict = "27MHz-OFFBAND";
  else                                       verdict = "unexpected";
  char j[256];
  snprintf(j, sizeof(j),
    "{\"ok\":true,\"xtal_hz\":%lu,\"xtal_mhz\":%.3f,\"actual_tx_mhz\":%.2f,\"verdict\":\"%s\"}",
    (unsigned long)xtal, xtal / 1.0e6, actualMHz, verdict);
  g_out_total += strlen(j);
  server.send(200, "application/json", j);
  Serial.printf("xtal test: %.3f MHz -> actual TX ~%.2f MHz (%s)\n",
                xtal / 1.0e6, actualMHz, verdict);
}

// POST /rftune?khz=<signed> -- shift the carrier by N kHz (runtime, not saved).
// Used to match the real FOB frequency measured on an SDR.
void handleRfTune() {
  trackReq();
  if (!RF_ENABLED || !rfReady) {
    const char* m = "{\"ok\":false,\"detail\":\"RF disabled or CC1101 absent\"}";
    g_out_total += strlen(m); server.send(503, "application/json", m); return;
  }
  long khz = server.arg("khz").toInt();
  if (khz < -500 || khz > 500) {
    const char* m = "{\"ok\":false,\"detail\":\"khz must be -500..500\"}";
    g_out_total += strlen(m); server.send(400, "application/json", m); return;
  }
  radio.nudgeFreqHz((int32_t)khz * 1000);
  uint32_t w = radio.freqWord();
  double mhz = (double)w * 26.0 / 65536.0;   // nominal, assuming 26 MHz xtal
  char j[160];
  snprintf(j, sizeof(j), "{\"ok\":true,\"khz\":%ld,\"freq_word\":%lu,\"nominal_mhz\":%.4f}",
           khz, (unsigned long)w, mhz);
  g_out_total += strlen(j);
  server.send(200, "application/json", j);
  Serial.printf("RF tune %+ld kHz -> word 0x%06lX (~%.4f MHz nominal)\n", khz, (unsigned long)w, mhz);
}

// GET /rfregs -- dump the CC1101 config registers the self-test doesn't check
// (IOCFG0 = async data-pin config; PATABLE = actual PA output level). A wrong
// PATABLE[1] or IOCFG0 means MARCSTATE reaches TX but little/no RF is modulated.
void handleRfRegs() {
  trackReq();
  if (!RF_ENABLED || !rfReady) {
    const char* m = "{\"ok\":false,\"detail\":\"RF disabled or CC1101 absent\"}";
    g_out_total += strlen(m); server.send(503, "application/json", m); return;
  }
  uint8_t pat[8];
  radio.readPatable(pat);
  char j[420];
  snprintf(j, sizeof(j),
    "{\"ok\":true,"
    "\"IOCFG2\":\"0x%02X\",\"IOCFG0\":\"0x%02X\",\"PKTCTRL0\":\"0x%02X\","
    "\"FREQ2\":\"0x%02X\",\"FREQ1\":\"0x%02X\",\"FREQ0\":\"0x%02X\","
    "\"MDMCFG2\":\"0x%02X\",\"DEVIATN\":\"0x%02X\",\"MCSM0\":\"0x%02X\",\"FREND0\":\"0x%02X\","
    "\"PATABLE0\":\"0x%02X\",\"PATABLE1\":\"0x%02X\","
    "\"expect\":\"IOCFG0=2D PKTCTRL0=32 FREND0=11 PATABLE0=00 PATABLE1=C0\"}",
    radio.peekReg(0x00), radio.peekReg(0x02), radio.peekReg(0x08),
    radio.peekReg(0x0D), radio.peekReg(0x0E), radio.peekReg(0x0F),
    radio.peekReg(0x12), radio.peekReg(0x15), radio.peekReg(0x18), radio.peekReg(0x22),
    pat[0], pat[1]);
  g_out_total += strlen(j);
  server.send(200, "application/json", j);
}

// GET /scan -- WiFi survey, used as an ANTENNA HEALTH CHECK.
//
// Why a scan rather than a single RSSI: one RSSI number only means something if
// you already know the distance and the AP's transmit power. A scan compares
// THIS receiver against many transmitters at once, so it can be checked against
// a phone or laptop standing in the same spot. A healthy front end sees roughly
// the same AP list at roughly the same levels. A disconnected, pinched or
// metal-buried antenna shows far fewer APs AND a uniform deficit (~20-30 dB for
// an unseated U.FL) across ALL of them -- and that pattern cannot be explained
// away by distance or AP power, which is what makes it conclusive.
//
// Costs: scanNetworks() blocks for a few seconds because it visits every
// channel, and it briefly takes the radio off the home channel, so an already
// marginal STA link may drop and re-associate. On-demand only, never periodic.
// The WDT is fed either side; the safety task on core 0 is untouched by this.
void handleScan() {
  trackReq();
  boundSendStall();
  // Persist a "starting" marker BEFORE blocking. logLine() only writes the RAM
  // ring; flash persistence normally happens in loop(), which cannot run while
  // the scan blocks -- so without an explicit flush a brownout or non-WDT reset
  // mid-scan would leave no trace that a scan was ever in flight. Same trick the
  // OTA handler uses before it reboots. (A WDT reset is already covered: 4.24's
  // trackReq() stamps "/scan" into the RTC breadcrumb, so the next boot names it.)
  logLine("scan: starting (blocks a few seconds, may drop the link)");
  flushLogToFlash();
  esp_task_wdt_reset();
  int n = WiFi.scanNetworks(false, true);   // synchronous, include hidden SSIDs
  esp_task_wdt_reset();
  if (n < 0) n = 0;                         // -1 running / -2 failed -> report none

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "application/json", "");
  String chunk; chunk.reserve(1024);
  chunk += "{\"ok\":true,\"n\":"; chunk += n;
  chunk += ",\"self_rssi\":";
  chunk += (WiFi.status() == WL_CONNECTED) ? (int)WiFi.RSSI() : 0;
  chunk += ",\"aps\":[";
  for (int i = 0; i < n; i++) {
    if (i) chunk += ',';
    chunk += "{\"ssid\":\"";
    String ss = WiFi.SSID(i);               // hidden APs return "" -- expected
    for (unsigned k = 0; k < ss.length(); k++) {
      char c = ss[k];                       // minimal JSON escaping
      if (c == '"' || c == '\\') { chunk += '\\'; chunk += c; }
      else if ((uint8_t)c < 0x20)   { chunk += ' '; }
      else                          { chunk += c; }
    }
    chunk += "\",\"bssid\":\""; chunk += WiFi.BSSIDstr(i);
    chunk += "\",\"rssi\":";     chunk += (int)WiFi.RSSI(i);
    chunk += ",\"ch\":";          chunk += (int)WiFi.channel(i);
    chunk += '}';
    if (chunk.length() > 1200) { g_out_total += chunk.length(); server.sendContent(chunk); chunk = ""; esp_task_wdt_reset(); }
  }
  chunk += "]}";
  g_out_total += chunk.length(); server.sendContent(chunk);
  server.sendContent("");
  WiFi.scanDelete();                        // release the driver's result buffer
  logLine("scan: %d APs visible, own link %d dBm", n,
          (WiFi.status() == WL_CONNECTED) ? (int)WiFi.RSSI() : 0);
}

// ---- WiFi configuration ---------------------------------------------------
// GET /wificfg  -> current settings. Passwords are NEVER returned, only whether
// one is set, so a saved password cannot be read back out of the device.
static void jsonEscTo(String& out, const String& in) {
  for (unsigned i = 0; i < in.length(); i++) {
    char c = in[i];
    if (c == '"' || c == '\\') { out += '\\'; out += c; }
    else if ((uint8_t)c < 0x20)  { out += ' '; }
    else                          { out += c; }
  }
}

void handleWifiCfgGet() {
  trackReq();
  String j; j.reserve(640);
  j += "{\"ok\":true,\"sta_ssid\":\"";      jsonEscTo(j, g_sta_ssid);
  j += "\",\"sta_pass_set\":";                j += g_sta_pass.length() ? "true" : "false";
  j += ",\"ap_ssid\":\"";                     jsonEscTo(j, g_ap_ssid);
  j += "\",\"ap_pass_set\":";                 j += g_ap_pass.length() ? "true" : "false";
  j += ",\"hostname\":\"";                    jsonEscTo(j, g_hostname);
  j += "\",\"ap_auth\":";                     j += g_ap_auth;
  j += ",\"ap_chan\":";                        j += g_ap_chan;
  j += ",\"ap_hidden\":";                      j += g_ap_hidden ? "true" : "false";
  j += ",\"sta_minsec\":";                     j += g_sta_minsec;
  j += ",\"ap_after_s\":";                     j += g_ap_after_s;
  j += ",\"ap_retry_s\":";                     j += g_ap_retry_s;
  j += ",\"ap_wait_s\":";                      j += g_ap_wait_s;
  j += ",\"boot_s\":";                         j += g_boot_s;
  j += ",\"tx_dbm\":";                         j += String(g_tx_dbm, 1);
  j += ",\"proto\":";                          j += g_proto;
  j += ",\"pending\":";                        j += g_wifiPend ? "true" : "false";
  j += "}";
  g_out_total += j.length();
  server.send(200, "application/json", j);
}

// POST /wificfg -- validate, persist, apply. Credentials are read from the POST
// BODY (not the query string) so they never land in a URL. Non-credential
// settings apply immediately; a changed SSID/password is handed to
// applyPendingWifi() so this reply gets out before the radio drops.
void handleWifiCfgSave() {
  trackReq();
  String err;
  auto num = [&](const char* k, long lo, long hi, uint32_t cur) -> uint32_t {
    if (!server.hasArg(k)) return cur;
    long v = server.arg(k).toInt();
    if (v < lo || v > hi) { err = String(k) + " out of range"; return cur; }
    return (uint32_t)v;
  };

  uint32_t ap_after = num("ap_after_s", 30, 3600, g_ap_after_s);
  uint32_t ap_retry = num("ap_retry_s", 60, 7200, g_ap_retry_s);
  uint32_t ap_wait  = num("ap_wait_s",   5,  120, g_ap_wait_s);
  uint32_t boot_s   = num("boot_s",      5,  300, g_boot_s);
  uint32_t ap_chan  = num("ap_chan",     1,   13, g_ap_chan);
  uint32_t ap_auth  = num("ap_auth",     0,    3, g_ap_auth);
  uint32_t minsec   = num("sta_minsec",  0,    3, g_sta_minsec);
  uint32_t proto    = num("proto",       1,   15, g_proto);
  float    tx       = server.hasArg("tx_dbm") ? server.arg("tx_dbm").toFloat() : g_tx_dbm;
  if (tx < -1.0f || tx > 19.5f) err = "tx_dbm out of range";

  String staSsid = server.hasArg("sta_ssid") ? server.arg("sta_ssid") : g_sta_ssid;
  String apSsid  = server.hasArg("ap_ssid")  ? server.arg("ap_ssid")  : g_ap_ssid;
  String host    = server.hasArg("hostname") ? server.arg("hostname") : g_hostname;
  // Blank password field means "leave it alone" -- otherwise the UI could not
  // show the form without either leaking or clearing the stored password.
  String staPass = (server.hasArg("sta_pass") && server.arg("sta_pass").length()) ? server.arg("sta_pass") : g_sta_pass;
  String apPass  = (server.hasArg("ap_pass")  && server.arg("ap_pass").length())  ? server.arg("ap_pass")  : g_ap_pass;

  if (!staSsid.length() || staSsid.length() > 32) err = "STA SSID must be 1-32 chars";
  if (!apSsid.length()  || apSsid.length()  > 32) err = "AP SSID must be 1-32 chars";
  if (!host.length()    || host.length()    > 31) err = "hostname must be 1-31 chars";
  // A secured AP with a short password silently fails to start -- that would
  // strand the only recovery path, so refuse it here.
  if (ap_auth != 0 && apPass.length() < 8) err = "AP password must be 8+ chars (or set security to Open)";
  if (staPass.length() && staPass.length() < 8) err = "STA password must be 8+ chars";

  if (err.length()) {
    String j = "{\"ok\":false,\"detail\":\""; jsonEscTo(j, err); j += "\"}";
    g_out_total += j.length(); server.send(400, "application/json", j); return;
  }

  bool credsChanged = (staSsid != g_sta_ssid) || (staPass != g_sta_pass);

  // Persist + apply everything that cannot strand us.
  g_ap_after_s = ap_after; g_ap_retry_s = ap_retry; g_ap_wait_s = ap_wait;
  g_boot_s = boot_s; g_ap_chan = ap_chan; g_ap_auth = ap_auth;
  g_sta_minsec = minsec; g_proto = proto; g_tx_dbm = tx;
  g_ap_ssid = apSsid; g_ap_pass = apPass; g_hostname = host;
  g_ap_hidden = server.hasArg("ap_hidden") ? (server.arg("ap_hidden") == "1") : g_ap_hidden;

  prefs.putUInt ("ap_after_s", g_ap_after_s); prefs.putUInt ("ap_retry_s", g_ap_retry_s);
  prefs.putUInt ("ap_wait_s",  g_ap_wait_s);  prefs.putUInt ("boot_s",     g_boot_s);
  prefs.putUChar("ap_chan",    g_ap_chan);    prefs.putUChar("ap_auth",    g_ap_auth);
  prefs.putUChar("sta_minsec", g_sta_minsec); prefs.putUChar("proto",      g_proto);
  prefs.putFloat("tx_dbm",     g_tx_dbm);     prefs.putBool ("ap_hidden",  g_ap_hidden);
  prefs.putString("ap_ssid",   g_ap_ssid);    prefs.putString("ap_pass",   g_ap_pass);
  prefs.putString("hostname",  g_hostname);

  if (!apMode) { applyWifiRangeProfile(); WiFi.setMinSecurity(staMinAuth()); }  // live radio settings

  if (credsChanged) {                 // hand off; loop() does the risky part
    g_pendSsid = staSsid; g_pendPass = staPass; g_wifiPend = true;
  }
  logLine("wificfg saved: AP '%s' ch%u auth%u, fallback %lus/retry %lus/wait %lus, boot %lus, TX %.1f, proto 0x%02X%s",
          g_ap_ssid.c_str(), (unsigned)g_ap_chan, (unsigned)g_ap_auth,
          (unsigned long)g_ap_after_s, (unsigned long)g_ap_retry_s, (unsigned long)g_ap_wait_s,
          (unsigned long)g_boot_s, g_tx_dbm, protoBits(),
          credsChanged ? " [STA change pending]" : "");

  String j = "{\"ok\":true,\"pending\":";
  j += credsChanged ? "true" : "false";
  j += ",\"detail\":\"";
  j += credsChanged ? "settings saved; switching network now -- if it fails the board reverts automatically"
                    : "settings saved";
  j += "\"}";
  g_out_total += j.length();
  server.send(200, "application/json", j);
}

// GET /rftest -- non-transmitting CC1101 health check (see CC1101::selfTest).
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

// POST /cpu?mhz=80|240 -- set the CPU clock. 80 MHz is the floor that still
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
  logLine("CPU clock -> %u MHz", (unsigned)getCpuFrequencyMhz());
}

// POST /wifips?on=0|1 -- enable/disable WiFi modem-sleep power saving.
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
  logLine("WiFi power-save %s", WiFi.getSleep() ? "ON" : "OFF");
}

// POST /powerup -- one-shot "max performance": WiFi power-save OFF + CPU 240 MHz,
// both persisted to NVS. Same net effect as /wifips?on=0 then /cpu?mhz=240, but
// parameterless so it can be fired automatically (like /reboot). Trades a bit
// of current draw for lowest latency + fastest response.
void handlePowerup() {
  trackReq();
  WiFi.setSleep(false);                          // radio always on -- lowest latency
  g_wifi_ps = WiFi.getSleep();
  prefs.putBool("wifi_ps", g_wifi_ps);
  setCpuFrequencyMhz(240);                        // max clock
  g_cpu_mhz = getCpuFrequencyMhz();
  prefs.putUInt("cpu_mhz", g_cpu_mhz);
  char j[96];
  snprintf(j, sizeof(j), "{\"ok\":true,\"cpu_mhz\":%u,\"wifi_ps\":%s}",
           (unsigned)g_cpu_mhz, g_wifi_ps ? "true" : "false");
  g_out_total += strlen(j);
  server.send(200, "application/json", j);
  Serial.printf("POWERUP: CPU %u MHz, WiFi power-save %s\n",
                (unsigned)g_cpu_mhz, g_wifi_ps ? "ON" : "OFF");
  logLine("POWERUP: CPU 240 MHz, WiFi power-save OFF");
}

// GET /logtext -- the rolling event log as plain text, oldest line first.
void handleLogText() {
  trackReq();
  boundSendStall();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/plain", "");
  int cnt = g_logCount;
  int start = (g_logCount < LOG_LINES) ? 0 : g_logHead;
  String chunk; chunk.reserve(1600);
  for (int k = 0; k < cnt; k++) {
    chunk += g_log[(start + k) % LOG_LINES]; chunk += '\n';
    if (chunk.length() > 1400) {
      if (!waitWritable(4000)) break;                // stalled client -> abort rather than block
      g_out_total += chunk.length(); server.sendContent(chunk); chunk = ""; esp_task_wdt_reset(); }
  }
  if (chunk.length()) { g_out_total += chunk.length(); server.sendContent(chunk); }
  server.sendContent("");
}

// GET /logpage?p=N -- server-side pagination for the /logs viewer: transfers
// ONLY the requested page of the newest-first log (25 lines), never the whole
// ring -- so a quick look is cheap on a flaky link. First response line is the
// total line count (so the UI can show "page X / Y"); the rest are the page's
// lines, newest first. /logtext stays the full raw (oldest-first) dump.
static const int LOG_PAGE_SZ = 25;
void handleLogPage() {
  trackReq();
  boundSendStall();
  int p = server.arg("p").toInt();
  if (p < 0) p = 0;
  portENTER_CRITICAL(&g_logMux);                 // snapshot a consistent (head,count)
  int head = g_logHead, total = g_logCount;
  portEXIT_CRITICAL(&g_logMux);

  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/plain", "");
  char hd[24]; int hn = snprintf(hd, sizeof(hd), "%d\n", total);
  g_out_total += hn; server.sendContent(hd);

  String chunk; chunk.reserve(1600);
  int startK = p * LOG_PAGE_SZ;                   // k=0 is the newest line
  for (int k = startK; k < startK + LOG_PAGE_SZ && k < total; k++) {
    int idx = (head - 1 - k + 2 * LOG_LINES) % LOG_LINES;
    chunk += g_log[idx]; chunk += '\n';
    if (chunk.length() > 1400) {
      if (!waitWritable(4000)) break;                // stalled client -> abort rather than block
      g_out_total += chunk.length(); server.sendContent(chunk); chunk = ""; esp_task_wdt_reset(); }
  }
  if (chunk.length()) { g_out_total += chunk.length(); server.sendContent(chunk); }
  server.sendContent("");
}

// GET /logs -- the event log as a tab: newest-first, paginated 25 lines/page,
// with a settable auto-refresh. Reads /logtext (oldest-first) and reverses it
// client-side, so /logtext stays raw-chronological for any tooling.
void handleLogsPage() {
  trackReq();
  static const char PAGE[] PROGMEM = R"HTML(<!DOCTYPE html><html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>vroom &middot; Log</title><link rel="stylesheet" href="/app.css?v=431">
<style>
#log{background:var(--card);border:1px solid #21262d;border-radius:10px;padding:12px;font-family:ui-monospace,Menlo,Consolas,monospace;font-size:12.5px;line-height:1.55;white-space:pre-wrap;word-break:break-word;min-height:200px}
#log div{padding:1px 0;border-bottom:1px solid #12161c}
.pg{display:flex;gap:10px;align-items:center;flex-wrap:wrap;margin:12px 0}
.pg button{background:#21262d;color:var(--fg);border:1px solid #30363d;border-radius:8px;padding:8px 14px;font-size:14px;cursor:pointer}
.pg button:disabled{opacity:.4;cursor:default}
.pg .k{color:var(--mut);font-size:13px}
select.inp{width:auto}
</style></head><body>
<header><h1>&#9889; ESP32-S3 &middot; Event Log</h1>
<span id="status"><span id="dot"></span><span id="stxt">connecting&hellip;</span></span></header>
<nav class="tabs">
<a href="/" data-p="/">Main</a>
<a href="/wifi" data-p="/wifi">WiFi / Net</a>
<a href="/voltage" data-p="/voltage">Voltage</a>
<a href="/cpu" data-p="/cpu">CPU</a>
<a href="/memdisk" data-p="/memdisk">Mem / Disk</a>
<a href="/logs" data-p="/logs">Log</a>
<a href="/update" data-p="/update">Update</a>
</nav>
<div class="wrap">
<div class="pg">
<button id="prev">&larr; Newer</button>
<span class="k" id="pginfo">--</span>
<button id="next">Older &rarr;</button>
<span class="k">refresh</span>
<select id="iv" class="inp">
<option value="0">off</option><option value="5">5 s</option><option value="10">10 s</option>
<option value="30">30 s</option><option value="60">60 s</option>
</select>
<button id="refresh">Refresh now</button>
<a href="/logtext" style="color:#58a6ff;margin-left:auto">raw</a>
</div>
<div id="log">loading&hellip;</div>
<div class="pg">
<button id="prev2">&larr; Newer</button>
<span class="k" id="pginfo2">--</span>
<button id="next2">Older &rarr;</button>
</div>
</div>
<footer>newest first &middot; 25 lines/page &middot; persisted to flash, survives reboot</footer>
<script>
function $(i){return document.getElementById(i)}
var PS=25, page=0, total=0, timer=null;
function render(lines){
  var pages=Math.max(1,Math.ceil(total/PS));
  if(page>=pages)page=pages-1; if(page<0)page=0;
  $("log").innerHTML = lines.length? lines.map(function(l){
    return "<div>"+l.replace(/&/g,"&amp;").replace(/</g,"&lt;")+"</div>";}).join("") : '<div class="k">no log lines</div>';
  var info="page "+(page+1)+" / "+pages+" &middot; "+total+" lines";
  $("pginfo").innerHTML=info; $("pginfo2").innerHTML=info;
  var atNew=page<=0, atOld=page>=pages-1;
  $("prev").disabled=atNew; $("prev2").disabled=atNew;
  $("next").disabled=atOld; $("next2").disabled=atOld;
}
// Fetches ONLY the current page (25 lines + a total count) -- never the whole
// log -- so a quick look stays cheap on a flaky link. Server returns the total
// on the first line, then the page's lines already newest-first.
function load(){
  fetch("/logpage?p="+page,{cache:"no-store"}).then(function(r){return r.text()}).then(function(t){
    var a=t.replace(/\s+$/,"").split("\n");
    total=parseInt(a.shift(),10)||0;
    var pages=Math.max(1,Math.ceil(total/PS));
    if(page>pages-1&&page>0){page=pages-1;load();return;}   // rolled past the end -> clamp + refetch
    $("dot").style.background="#3fb950";$("stxt").textContent="live";
    render(a.filter(function(s){return s.length}));
  }).catch(function(e){$("dot").style.background="#d29922";$("stxt").textContent="reconnecting..."});
}
function setIv(v){ if(timer){clearInterval(timer);timer=null;} if(v>0)timer=setInterval(load,v*1000);
  try{localStorage.vroomLogIv=v;}catch(e){} }
$("prev").onclick=$("prev2").onclick=function(){if(page>0){page--;load();window.scrollTo(0,0)}};
$("next").onclick=$("next2").onclick=function(){page++;load();window.scrollTo(0,0)};
$("refresh").onclick=load;
$("iv").onchange=function(){setIv(+this.value)};
document.querySelectorAll("nav.tabs a").forEach(function(a){if(a.getAttribute("data-p")===location.pathname)a.classList.add("on")});
var iv=5; try{if(localStorage.vroomLogIv!==undefined)iv=+localStorage.vroomLogIv;}catch(e){}
$("iv").value=iv; setIv(iv); load();
</script>
</body></html>)HTML";
  g_out_total += strlen_P(PAGE);
  server.send_P(200, "text/html; charset=utf-8", PAGE);
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
  // Clearing the latched lockout after a run of failed starts is deliberate --
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

// GET /starts -- the engine-start log, newest first, as a JSON array.
void handleStarts() {
  trackReq();
  boundSendStall();
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
      e.src == 0 ? "auto" : (e.src == 1 ? "manual" : "external"),
      e.ok ? "true" : "false", e.ver);
    if (!waitWritable(4000)) break;       // stalled client -> abort rather than block
    g_out_total += strlen(rec);
    server.sendContent(rec);
    esp_task_wdt_reset();
  }
  server.sendContent("]");
  server.sendContent("");
}

// POST /starts -- clear the start log.
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
<!DOCTYPE html><html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>vroom &middot; Update</title><link rel="stylesheet" href="/app.css?v=431"></head><body>
<header><h1>&#9889; ESP32-S3 &middot; Firmware Update</h1></header>
<nav class="tabs">
<a href="/" data-p="/">Main</a>
<a href="/wifi" data-p="/wifi">WiFi / Net</a>
<a href="/voltage" data-p="/voltage">Voltage</a>
<a href="/cpu" data-p="/cpu">CPU</a>
<a href="/memdisk" data-p="/memdisk">Mem / Disk</a>
<a href="/logs" data-p="/logs">Log</a>
<a href="/update" data-p="/update">Update</a>
</nav>
<div class="wrap">
<div class="card" style="max-width:460px;margin:10px auto">
<div class="k">Upload a compiled .bin &mdash; the board flashes it and reboots</div>
<input type="file" id="fw" accept=".bin" style="margin:14px 0;color:#e6edf3"><br>
<button class="tx" style="background:#238636;border-color:#2ea043" onclick="up()">Upload &amp; flash</button>
<progress id="pb" value="0" max="100" style="width:100%;height:16px;margin-top:16px"></progress>
<div id="m" class="k" style="margin-top:12px;min-height:1.2em">pick the compiled .bin</div>
</div>
</div>
<script>
document.querySelectorAll("nav.tabs a").forEach(function(a){if(a.getAttribute("data-p")===location.pathname)a.classList.add("on")});
function up(){var f=document.getElementById('fw').files[0];if(!f){return}
var x=new XMLHttpRequest(),fd=new FormData();fd.append('firmware',f);
x.upload.onprogress=function(e){if(e.lengthComputable){document.getElementById('pb').value=100*e.loaded/e.total}};
x.onload=function(){document.getElementById('m').textContent=x.responseText+'  (reconnect in ~5 s)'};
x.onerror=function(){document.getElementById('m').textContent='upload error'};
document.getElementById('m').textContent='uploading... do not power off...';
x.open('POST','/update');x.send(fd)}
</script></body></html>
)HTML";

// STA link profile, applied whenever the STA (re)starts.
//   TX power -> max the core exposes (~19.5 dBm): boosts our uplink; safe for the
//   S3 and well under 2.4 GHz regulatory limits with the small whip.
// NOTE ON PROTOCOL: 4.18 briefly forced 802.11b-only for range. Against THIS AP
// that was a disaster -- latency jumped to ~1.7 s / 20% loss and a watchdog-fed
// task starved into a TASK-WATCHDOG *reboot loop*. esp_wifi stores the protocol
// in NVS, so we must explicitly set it back to the default b/g/n to undo the
// persisted b-only -- doing nothing would leave the bad setting in flash.
// esp_wifi_set_protocol() must run after the WiFi driver has started (WiFi.mode
// does that), so this is always called after a mode change, before begin().
// TX power is an enum of discrete steps, not a continuous value -- snap to the
// nearest one the radio actually supports so the UI can offer plain dBm numbers.
static wifi_power_t txEnumFor(float dbm) {
  struct { float d; wifi_power_t e; } T[] = {
    {-1.0f, WIFI_POWER_MINUS_1dBm}, {2.0f, WIFI_POWER_2dBm},   {5.0f, WIFI_POWER_5dBm},
    {7.0f,  WIFI_POWER_7dBm},       {8.5f, WIFI_POWER_8_5dBm}, {11.0f, WIFI_POWER_11dBm},
    {13.0f, WIFI_POWER_13dBm},      {15.0f, WIFI_POWER_15dBm}, {17.0f, WIFI_POWER_17dBm},
    {18.5f, WIFI_POWER_18_5dBm},    {19.0f, WIFI_POWER_19dBm}, {19.5f, WIFI_POWER_19_5dBm} };
  int best = 11; float bd = 1e9f;
  for (int i = 0; i < 12; i++) { float d = fabsf(T[i].d - dbm); if (d < bd) { bd = d; best = i; } }
  return T[best].e;
}

static uint8_t protoBits() {
  uint8_t p = 0;
  if (g_proto & 1) p |= WIFI_PROTOCOL_11B;
  if (g_proto & 2) p |= WIFI_PROTOCOL_11G;
  if (g_proto & 4) p |= WIFI_PROTOCOL_11N;
  if (g_proto & 8) p |= WIFI_PROTOCOL_LR;
  if (!p) p = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N;   // never leave it empty
  return p;
}

// Minimum AP security the STA will associate to. Refusing weak crypto is a real
// setting, but set it too high and the board silently cannot join -- so the
// default is "accept anything the AP offers".
static wifi_auth_mode_t staMinAuth() {
  switch (g_sta_minsec) {
    case 1:  return WIFI_AUTH_WPA_PSK;
    case 2:  return WIFI_AUTH_WPA2_PSK;
    case 3:  return WIFI_AUTH_WPA3_PSK;
    default: return WIFI_AUTH_OPEN;
  }
}

// Seed the long-term anchor by BACK-DATING it to the vehicle's last run, not to
// whenever this board happened to boot.
//
// The target is g_lastRunTs + LT_SETTLE_S -- 12 h after the engine last stopped,
// past the fast fluctuating settle. If that moment has already passed we do not
// need to wait for a new one: the 24 h history ring is written through to flash
// (see saveHistory/loadHistory) and therefore SURVIVES REBOOTS, so the voltage
// at that time is usually still on record. Take the earliest stored sample at or
// after the target; if the target predates the ring, the oldest sample we still
// hold is used, which is the longest baseline the data actually supports.
//
// This is the whole point: the measurement must be anchored to the CAR's last
// run, so a board reboot cannot reset it. An anchor taken at boot would restart
// the baseline every time the watchdog fired, which is exactly the failure this
// feature exists to escape.
void seedLongTermFromHistory() {
  if (g_ltRefTs || !hist || histCount < 2 || !g_lastRunTs || !timeIsValid()) return;
  uint32_t target = g_lastRunTs + LT_SETTLE_S;
  uint32_t nowS   = (uint32_t)time(nullptr);
  if (nowS <= target + LT_MIN_S) return;            // baseline still too short
  int oldest = (histCount < HIST_N) ? 0 : histHead;
  for (int k = 0; k < histCount; k++) {
    Sample& s = hist[(oldest + k) % HIST_N];
    if (s.ts >= target && s.vbatt > 8.0f && s.vbatt < 16.0f) {
      g_ltRefTs = s.ts; g_ltRefV = s.vbatt;
      prefs.putUInt ("lt_ref_ts", g_ltRefTs);
      prefs.putFloat("lt_ref_v",  g_ltRefV);
      logLine("drain baseline back-dated to %.2f V at %lu (%.1f h after the last run, %.1f h of baseline)",
              g_ltRefV, (unsigned long)g_ltRefTs,
              (g_ltRefTs - g_lastRunTs) / 3600.0f, (nowS - g_ltRefTs) / 3600.0f);
      return;
    }
  }
}

// Long-term drain rate in mV/h from the settled anchor to now. Returns 0 if
// there is no anchor yet or too little baseline to be meaningful.
float longTermMvph(float vNow) {
  if (!g_ltRefTs || !timeIsValid()) return 0.0f;
  uint32_t nowS = (uint32_t)time(nullptr);
  if (nowS <= g_ltRefTs + LT_MIN_S)  return 0.0f;
  float hrs = (nowS - g_ltRefTs) / 3600.0f;
  return (vNow - g_ltRefV) * 1000.0f / hrs;
}

// Seconds until the trigger at the long-term rate. -1 = unknown/holding.
long longTermEtaS(float vNow) {
  float r = longTermMvph(vNow);
  if (r >= -0.05f) return -1;                    // flat or rising -> no estimate
  float mv = (vNow - g_as_volts) * 1000.0f;
  if (mv <= 0) return 0;
  return (long)(mv / (-r) * 3600.0f);
}

static void loadWifiCfg() {
  // fw 4.28 anchored at boot time, which restarted the baseline on every reboot.
  // Drop any anchor written by that scheme once, so it is re-seeded from history.
  if (prefs.getUChar("lt_schema", 1) < 2) {
    prefs.putUInt("lt_ref_ts", 0); prefs.putFloat("lt_ref_v", 0.0f);
    prefs.putUChar("lt_schema", 2);
  }
  g_ltRefTs = prefs.getUInt ("lt_ref_ts", 0);
  g_ltRefV  = prefs.getFloat("lt_ref_v",  0.0f);
  g_ltDue   = prefs.getUInt ("lt_due",    0);
  g_sta_ssid   = prefs.getString("sta_ssid", WIFI_SSID);
  g_sta_pass   = prefs.getString("sta_pass", WIFI_PASS);
  g_ap_ssid    = prefs.getString("ap_ssid",  AP_SSID);
  g_ap_pass    = prefs.getString("ap_pass",  AP_PASS);
  g_hostname   = prefs.getString("hostname", HOSTNAME);
  g_ap_auth    = prefs.getUChar("ap_auth",   1);
  g_ap_chan    = prefs.getUChar("ap_chan",   1);
  g_ap_hidden  = prefs.getBool ("ap_hidden", false);
  g_sta_minsec = prefs.getUChar("sta_minsec",2);
  g_ap_after_s = prefs.getUInt ("ap_after_s", AP_AFTER_DOWN_MS / 1000);
  g_ap_retry_s = prefs.getUInt ("ap_retry_s", AP_RETRY_STA_MS / 1000);
  g_ap_wait_s  = prefs.getUInt ("ap_wait_s",  AP_RETRY_WAIT_MS / 1000);
  g_boot_s     = prefs.getUInt ("boot_s",     20);
  g_tx_dbm     = prefs.getFloat("tx_dbm",     19.5f);
  g_proto      = prefs.getUChar("proto",      7);
  // Corrupt or out-of-range values must fall back to something that still
  // connects -- never to something that strands the board.
  if (!g_sta_ssid.length())                       g_sta_ssid = WIFI_SSID;
  if (!g_ap_ssid.length())                        g_ap_ssid  = AP_SSID;
  if (!g_hostname.length())                       g_hostname = HOSTNAME;
  if (g_ap_chan < 1 || g_ap_chan > 13)            g_ap_chan  = 1;
  if (g_ap_auth > 3)                              g_ap_auth  = 1;
  if (g_sta_minsec > 3)                           g_sta_minsec = 0;
  if (g_ap_after_s < 30  || g_ap_after_s > 3600)  g_ap_after_s = 300;
  if (g_ap_retry_s < 60  || g_ap_retry_s > 7200)  g_ap_retry_s = 600;
  if (g_ap_wait_s  < 5   || g_ap_wait_s  > 120)   g_ap_wait_s  = 10;
  if (g_boot_s     < 5   || g_boot_s     > 300)   g_boot_s     = 20;
  if (g_tx_dbm < -1.0f   || g_tx_dbm > 19.5f)     g_tx_dbm     = 19.5f;
  if (!g_proto || g_proto > 15)                   g_proto      = 7;
}

// Applies protocol + TX power to the STA interface. esp_wifi_set_protocol
// PERSISTS TO NVS in the driver, which is why fw 4.18's 11b-only setting
// survived a reflash -- so this always writes explicitly rather than assuming
// a default. Called after every mode change, before begin().
static void applyWifiRangeProfile() {
  esp_err_t e = esp_wifi_set_protocol(WIFI_IF_STA, protoBits());
  WiFi.setTxPower(txEnumFor(g_tx_dbm));
  Serial.printf("WiFi profile: proto 0x%02X (%s), TX %.1f dBm\n",
                protoBits(), e == ESP_OK ? "ok" : "set FAILED", g_tx_dbm);
}

void startAP() {
  loopMark("wifi-startAP");
  apMode = true;
  WiFi.mode(WIFI_AP);
  WiFi.setTxPower(txEnumFor(g_tx_dbm));  // strong fallback AP too, so it's reachable from the house
  IPAddress apIP(192, 168, 4, 1), gw(192, 168, 4, 1), mask(255, 255, 255, 0);
  WiFi.softAPConfig(apIP, gw, mask);
  bool secured = (g_ap_auth != 0) && g_ap_pass.length() >= 8;   // WPA needs 8+ chars
  WiFi.softAP(g_ap_ssid.c_str(), secured ? g_ap_pass.c_str() : nullptr,
              g_ap_chan, g_ap_hidden ? 1 : 0);
  if (secured && g_ap_auth > 1) {          // softAP() only gives WPA2-PSK; widen it if asked
    wifi_config_t c;
    if (esp_wifi_get_config(WIFI_IF_AP, &c) == ESP_OK) {
      c.ap.authmode = (g_ap_auth == 2) ? WIFI_AUTH_WPA_WPA2_PSK : WIFI_AUTH_WPA2_WPA3_PSK;
      esp_wifi_set_config(WIFI_IF_AP, &c);
    }
  }
  dnsServer.start(53, "*", apIP);
  Serial.println("---- starting fallback ACCESS POINT ----");
  Serial.printf("  Join WiFi \"%s\" %s, then browse http://192.168.4.1/\n",
                g_ap_ssid.c_str(), secured ? "(password set)" : "(OPEN)");
  logLine("home WiFi lost >%lus -> fallback AP '%s' ch%u%s (192.168.4.1)",
          (unsigned long)g_ap_after_s, g_ap_ssid.c_str(), (unsigned)g_ap_chan,
          g_ap_hidden ? " hidden" : "");
}

void setup() {
  Serial.begin(115200);
  delay(300);

  // Restore persisted power/perf settings from NVS and apply the CPU clock
  // now (before WiFi). Falls back to the compiled defaults on first boot or
  // a bad/garbage value.
  prefs.begin("vroom", false);
  loadWifiCfg();                     // WiFi config before any radio call uses it
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
  g_lastRunTs   = prefs.getUInt("last_run", 0);
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
  g_log = (char(*)[LOG_LEN]) ps_malloc((size_t)LOG_LINES * LOG_LEN);   // event-log ring in PSRAM
  Serial.printf("Event log: %d lines (%u bytes) in %s\n", LOG_LINES,
                (unsigned)((size_t)LOG_LINES * LOG_LEN), g_log ? "PSRAM" : "FAILED");
  loadLogFromFlash();                              // replay pre-reboot log tail into the ring
  Serial.printf("Restored %d prior log lines from flash.\n", g_logCount);
  loadHistory();
  Serial.printf("Loaded %d prior samples from flash.\n", histCount);
  loadStarts();
  Serial.printf("Loaded %d prior start events from flash.\n", g_startCount);

  setenv("TZ", TZ_INFO, 1); tzset();               // local time for logs even before NTP replies
  sntp_set_time_sync_notification_cb(onNtpSync);   // log each NTP sync
  WiFi.onEvent(onWiFiEvent);                        // verbose WiFi diagnostics -> event log
  logLine("boot: fw %s, CPU %u MHz, reset=%s",
          FW_VERSION, (unsigned)getCpuFrequencyMhz(), resetReasonName());
  // If the last reset was the task watchdog, the breadcrumbs in RTC memory say
  // what each watched task was doing when it hung -- the stuck one names the
  // blocking op. (Guarded by a magic so a cold power-on doesn't print garbage.)
  if (esp_reset_reason() == ESP_RST_TASK_WDT && g_markMagic == MARK_MAGIC)
    logLine("  ^ WDT stall: loop was '%s', safety was '%s'",
            g_loopMark[0] ? g_loopMark : "?", g_safetyMark[0] ? g_safetyMark : "?");
  g_markMagic = MARK_MAGIC;                         // (re)validate for this session
  loopMark("boot"); safetyMark("boot");

  Serial.printf("Connecting to WiFi '%s' ", g_sta_ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.setHostname(g_hostname.c_str());
  applyWifiRangeProfile();             // protocol + TX power before we associate
  WiFi.setMinSecurity(staMinAuth());   // always explicit: the library default is WPA2
  WiFi.begin(g_sta_ssid.c_str(), g_sta_pass.c_str());
  WiFi.setSleep(g_wifi_ps);            // apply persisted WiFi power-save state
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < g_boot_s * 1000UL) { delay(400); Serial.print("."); }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("WiFi OK in %lu ms. IP = %s   RSSI = %d dBm\n",
                  (unsigned long)(millis() - t0), WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
    if (MDNS.begin(g_hostname.c_str())) { MDNS.addService("http", "tcp", 80);
      Serial.printf("Reachable at: http://%s.local/  or  http://%s/\n", g_hostname.c_str(), WiFi.localIP().toString().c_str()); }
    syncTimeNow();   // kick off NTP now that STA is up
    Serial.printf("NTP sync requested (%s / %s)\n", NTP_SERVER1, NTP_SERVER2);
  } else {
    startAP();   // boot-time failure -> AP immediately (no NTP in AP mode)
  }

  // Page routes are GET-only: /cpu also has an HTTP_POST handler (set the clock),
  // and a method-less registration here would shadow it -> POST /cpu would serve
  // the CPU page HTML instead of changing frequency ("CPU request error").
  server.on("/", HTTP_GET, handleDash);        // Main tab
  server.on("/wifi", HTTP_GET, handleWifiPage);        // WiFi / Network tab
  server.on("/voltage", HTTP_GET, handleVoltagePage);  // Voltage tab
  server.on("/cpu", HTTP_GET, handleCpuPage);          // CPU tab (POST /cpu below sets the clock)
  server.on("/memdisk", HTTP_GET, handleMemPage);      // Memory / Disk tab
  server.on("/app.css", HTTP_GET, handleAppCss);       // shared cached stylesheet
  server.on("/app.js", HTTP_GET, handleAppJs);         // shared cached engine
  server.on("/json", handleJson);
  server.on("/history", handleHistory);
  server.on("/update", HTTP_GET, []() { trackReq(); server.send_P(200, "text/html; charset=utf-8", UPDATE_HTML); });
  server.on("/update", HTTP_POST,
    []() {                                    // runs after the upload finishes
      bool ok = !Update.hasError();
      server.send(200, "text/plain", ok ? "OK - flashed" : "FAILED");
      flushLogToFlash();                          // persist pending lines before the OTA reboot
      delay(800);
      if (ok) ESP.restart();
    },
    []() {                                    // streams the uploaded .bin into the spare OTA slot
      HTTPUpload& u = server.upload();
      if (u.status == UPLOAD_FILE_START) {
        Serial.printf("OTA start: %s\n", u.filename.c_str());
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) Update.printError(Serial);
      } else if (u.status == UPLOAD_FILE_WRITE) {
        loopMark("ota");
        esp_task_wdt_reset();   // a 1.1 MB OTA over weak WiFi can span many seconds; keep the WDT fed
        if (Update.write(u.buf, u.currentSize) != u.currentSize) Update.printError(Serial);
      } else if (u.status == UPLOAD_FILE_END) {
        if (Update.end(true)) Serial.printf("OTA done: %u bytes, rebooting\n", (unsigned)u.totalSize);
        else Update.printError(Serial);
      }
    });
  server.on("/transmit", HTTP_POST, handleTransmit);
  server.on("/rftest", HTTP_GET, handleRfTest);
  server.on("/xtaltest", HTTP_GET, handleXtalTest);
  server.on("/rfregs", HTTP_GET, handleRfRegs);
  server.on("/scan", HTTP_GET, handleScan);      // WiFi survey / antenna health check
  server.on("/wificfg", HTTP_GET,  handleWifiCfgGet);
  server.on("/wificfg", HTTP_POST, handleWifiCfgSave);
  server.on("/rftune", HTTP_POST, handleRfTune);
  server.on("/cpu", HTTP_POST, handleCpu);
  server.on("/wifips", HTTP_POST, handleWifiPs);
  server.on("/powerup", HTTP_POST, handlePowerup);
  server.on("/logs", HTTP_GET, handleLogsPage);
  server.on("/logtext", HTTP_GET, handleLogText);
  server.on("/logpage", HTTP_GET, handleLogPage);   // server-side paged log (25/req)
  server.on("/autostart", HTTP_POST, handleAutoStart);
  server.on("/starts", HTTP_GET, handleStarts);
  server.on("/starts", HTTP_POST, handleStartsClear);
  server.on("/reboot", HTTP_POST, []() {          // manual reboot from the dashboard
    trackReq();
    logLine("reboot requested via /reboot");
    flushLogToFlash();                            // get that last line onto flash before we go
    server.send(200, "application/json", "{\"ok\":true,\"rebooting\":true}");
    delay(300);
    ESP.restart();
  });
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
  // SNMP agent -- read-only, so an NMS can poll the board directly with no Pi.
  if (SNMP_ENABLED) {
    size_t nOids = sizeof(SNMP_OIDS) / sizeof(SNMP_OIDS[0]);
    if (snmp.begin(SNMP_PORT, SNMP_COMMUNITY, SNMP_OIDS, nOids))
      Serial.printf("SNMP up on udp/%u, %u OIDs at 1.3.6.1.4.1.99999.8.x.0\n",
                    SNMP_PORT, (unsigned)nOids);
    else
      Serial.printf("SNMP failed to bind udp/%u\n", SNMP_PORT);
  }

  g_lastTemp = temperatureRead();    // seed the caches before the safety task exists
  recordSample();                    // seed one sample now
  updateLed(readBatteryVolts());     // set the LED immediately

  // Watchdog + safety task. Create the RF mutex first (both fire paths need it),
  // arm the task watchdog (reboot if the loop or the safety task stalls > 30 s),
  // subscribe the loop task, then launch the safety task on core 0 (loop is core 1).
  g_rfMutex = xSemaphoreCreateMutex();
  esp_task_wdt_config_t wdtc = { .timeout_ms = WDT_TIMEOUT_MS, .idle_core_mask = 0, .trigger_panic = true };
  if (esp_task_wdt_init(&wdtc) == ESP_ERR_INVALID_STATE) esp_task_wdt_reconfigure(&wdtc);  // Arduino may have pre-inited it
  esp_task_wdt_add(nullptr);         // watch loopTask
  xTaskCreatePinnedToCore(safetyTaskFn, "safety", 8192, nullptr, 3, &g_safetyTask, 0);
  Serial.println("safety task started (core 0); task watchdog armed @30s");

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
uint32_t lastApRetry = 0;      // millis() of the last AP->STA reconnect attempt

// Switch the STA to new credentials, verify, and REVERT if they do not come up.
// Runs from loop() (never from the HTTP handler) so the reply reaches the
// browser before the radio drops. This is what makes remote credential edits
// safe on a board that is bolted behind a dash: a typo costs one connection
// cycle, not a disassembly.
void applyPendingWifi() {
  if (!g_wifiPend) return;
  g_wifiPend = false;
  loopMark("wifi-switch");
  String oldS = g_sta_ssid, oldP = g_sta_pass;
  String newS = g_pendSsid, newP = g_pendPass;

  logLine("wifi: trying '%s' (auto-revert to '%s' after %lus if it fails)",
          newS.c_str(), oldS.c_str(), (unsigned long)g_boot_s);
  flushLogToFlash();                       // must survive even if this goes wrong

  WiFi.disconnect(false, true);            // drop and erase the stored AP entry
  delay(200);
  if (!apMode) WiFi.mode(WIFI_STA);
  applyWifiRangeProfile();
  WiFi.setMinSecurity(staMinAuth());
  WiFi.begin(newS.c_str(), newP.c_str());
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < g_boot_s * 1000UL) {
    esp_task_wdt_reset();                  // a legitimate wait, not a stall
    delay(100);
  }

  if (WiFi.status() == WL_CONNECTED) {
    g_sta_ssid = newS; g_sta_pass = newP;
    prefs.putString("sta_ssid", g_sta_ssid);
    prefs.putString("sta_pass", g_sta_pass);
    if (MDNS.begin(g_hostname.c_str())) MDNS.addService("http", "tcp", 80);
    syncTimeNow();
    logLine("wifi: joined '%s' as %s @ %d dBm -- saved",
            newS.c_str(), WiFi.localIP().toString().c_str(), (int)WiFi.RSSI());
  } else {
    logLine("wifi: '%s' did NOT come up -- reverting to '%s'", newS.c_str(), oldS.c_str());
    WiFi.disconnect(false, true);
    delay(200);
    WiFi.begin(oldS.c_str(), oldP.c_str());
    t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < g_boot_s * 1000UL) {
      esp_task_wdt_reset();
      delay(100);
    }
    logLine(WiFi.status() == WL_CONNECTED
              ? "wifi: reverted to '%s' OK"
              : "wifi: revert to '%s' failed too -- AP fallback will take over",
            oldS.c_str());
  }
  downSince = 0;                            // the switch itself is not "home down"
  flushLogToFlash();
}
void loop() {
  esp_task_wdt_reset();                            // loop serviced a pass -> healthy
  if (apMode) dnsServer.processNextRequest();
  loopMark("http");   server.handleClient();       // top suspect: a slow client on a weak link
  if (SNMP_ENABLED) { loopMark("snmp"); snmp.poll(); }
  loopMark("logflush"); flushLogToFlash();          // persist any new event-log lines (idle-cheap)
  loopMark("loop");
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

  applyPendingWifi();                              // pending credential switch (safe: reverts on failure)

  if (!apMode) {                                   // configurable downtime -> AP
    if (WiFi.status() == WL_CONNECTED) downSince = 0;
    else { if (downSince == 0) downSince = now;
           else if (now - downSince >= g_ap_after_s * 1000UL) { startAP(); lastApRetry = now; } }
  } else if (now - lastApRetry >= g_ap_retry_s * 1000UL) {
    // In AP mode: periodically try to get back onto the home network. Runs the
    // AP and STA together during the attempt so anyone connected to the
    // fallback AP isn't kicked off just because the retry failed.
    lastApRetry = now;
    Serial.printf("AP mode: retrying home WiFi '%s'...\n", g_sta_ssid.c_str());
    loopMark("wifi-ap-retry");                     // driver calls below can block if the stack wedges
    WiFi.mode(WIFI_AP_STA);
    applyWifiRangeProfile();                     // re-assert proto + TX on the retry
    WiFi.begin(g_sta_ssid.c_str(), g_sta_pass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < g_ap_wait_s * 1000UL) {
      server.handleClient();                       // stay responsive while waiting
      esp_task_wdt_reset();                         // legit retry -- don't let the WDT trip
      delay(100);
    }
    if (WiFi.status() == WL_CONNECTED) {
      loopMark("wifi-rejoin");                    // teardown mode-flips below can also block
      dnsServer.stop();
      WiFi.softAPdisconnect(true);                 // tear the AP down, back to plain STA
      WiFi.mode(WIFI_STA);
      applyWifiRangeProfile();                     // re-assert 11b + max TX after the mode flip
      WiFi.setSleep(g_wifi_ps);                    // re-apply the persisted power-save
      apMode    = false;
      downSince = 0;
      if (MDNS.begin(g_hostname.c_str())) MDNS.addService("http", "tcp", 80);
      syncTimeNow();
      Serial.printf("rejoined '%s' as %s - AP torn down\n",
                    g_sta_ssid.c_str(), WiFi.localIP().toString().c_str());
      logLine("WiFi rejoined home as %s (AP torn down)", WiFi.localIP().toString().c_str());
    } else {
      WiFi.mode(WIFI_AP);                          // give up for now; AP-only draws less
      Serial.println("home WiFi still unavailable; staying in AP mode");
    }
  }

  // NTP time: only meaningful with a live STA connection.
  if (!apMode && WiFi.status() == WL_CONNECTED) {
    if (!g_timeSynced) {
      // Not synced yet -- retry every 30 s after the boot kick until the
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

  // Sampling + the low-voltage auto-start decision now run in safetyTaskFn on
  // core 0, so a WiFi/HTTP stall in this loop can no longer starve them. The loop
  // keeps history persistence, the LED, CPU-load sampling, and the heartbeat.
  if (now - lastSave  >= SAVE_MS) { lastSave = now; saveHistory(); }
  if (now - lastPrint >= 2000) {
    lastPrint = now;
    float v = g_lastV;         // sampled by the safety task (ADC owner)
    updateLed(v);
    sampleCpuLoad();           // per-core utilisation over the last 2 s
    Serial.printf("Vbatt=%6.2f V  temp=%4.1f C  heap=%uKB  disk=%uKB  samples=%d  %s\n",
                  v, g_lastTemp, (unsigned)(ESP.getFreeHeap() / 1024),
                  (unsigned)(LittleFS.usedBytes() / 1024), histCount, apMode ? "[AP]" : "[STA]");
  }
}
