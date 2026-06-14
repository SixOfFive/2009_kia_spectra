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

const char* FW_VERSION = "2.2";             // 2.2 = CPU clock + WiFi power-save persist across reboot (NVS)

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

float readBatteryVolts() {
  uint32_t acc = 0;
  for (int i = 0; i < SAMPLES; i++) acc += analogReadMilliVolts(VSENSE_PIN);
  g_last_mv = (int)(acc / SAMPLES);
  return (g_last_mv / 1000.0f) * DIVIDER * CAL;
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
  hist[histHead] = s;
  histHead = (histHead + 1) % HIST_N;
  if (histCount < HIST_N) histCount++;
}

void saveHistory() {
  if (!hist) return;
  File f = LittleFS.open("/history.bin", FILE_WRITE);
  if (!f) return;
  uint32_t magic = 0x564F4C33;                 // "VOL3" (added per-sample ts)
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
  if (magic == 0x564F4C33) {           // older formats (no ts) are discarded
    f.read((uint8_t*)&histCount, 4);
    f.read((uint8_t*)&histHead, 4);
    f.read((uint8_t*)hist, sizeof(Sample) * HIST_N);
    if (histCount < 0 || histCount > HIST_N) { histCount = 0; histHead = 0; }
  }
  f.close();
}

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
<div class="grid">
<div class="card"><div class="k">ADC node</div><div class="v"><span id="adc">--</span> mV</div></div>
<div class="card"><div class="k">WiFi RSSI</div><div class="v"><span id="rssi">--</span> dBm</div></div>
<div class="card"><div class="k">Uptime</div><div class="v" id="up">--</div></div>
<div class="card"><div class="k">Disk</div><div class="v"><span id="disk">--</span></div></div>
<div class="card"><div class="k">Free heap</div><div class="v"><span id="heap">--</span> KB</div></div>
<div class="card"><div class="k">Free PSRAM</div><div class="v"><span id="psram">--</span> MB</div></div>
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
</div>
<footer><span id="net">&hellip;</span> &middot; fw <span id="fw">?</span> &middot; samples <span id="ns">0</span>/1440 &middot; <span id="clk">--</span> &middot; <a href="/update">update</a></footer>
<script>
function $(i){return document.getElementById(i)}
function fmtUp(s){var h=Math.floor(s/3600),m=Math.floor(s%3600/60),x=s%60;return h?h+"h "+m+"m":m?m+"m "+x+"s":x+"s"}
// 7 graphs, in canvas order c0..c6, mapped to /history data columns 1..7.
var COLS=["#3fb950","#d29922","#58a6ff","#bc8cff","#39c5cf","#f778ba","#ffa657"];
var DEC=[2,1,0,0,0,0,0];
var UNITS=["V","°C","KB","KB","B/min","B/min","dBm"];
var PAD=8;
var CHARTS={};      // id -> {data, lo, hi, color, dec, unit}
var TS=[];          // per-sample epoch seconds (0 if NTP not synced), parallel to data
var hoverIdx=-1;
var IDS=["c0","c1","c2","c3","c4","c5","c6"];
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
for(var col=0;col<7;col++){(function(col){
  var data=rows.map(function(r){return r[col+1]});           // data cols 1..7
  var lo=Math.min.apply(null,data),hi=Math.max.apply(null,data);if(hi-lo<1e-6){hi+=1;lo-=1}
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
document.querySelectorAll("button.seg").forEach(function(b){b.classList.toggle("on",+b.getAttribute("data-mhz")===d.cpu_mhz)});
var ps=!!d.wifi_ps;
$("psbadge").innerHTML='<span class="badge '+(ps?"on":"off")+'">'+(ps?"ON":"OFF")+'</span>';
$("psbtn").textContent=ps?"Turn OFF":"Turn ON";$("psbtn").setAttribute("data-next",ps?"0":"1");
$("dot").style.background="#3fb950";$("stxt").textContent="live";
}).catch(function(e){$("dot").style.background="#d29922";$("stxt").textContent="reconnecting…"})}
document.querySelectorAll("button.tx").forEach(function(btn){btn.addEventListener("click",function(){
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
setupHover();setInterval(poll,2000);setInterval(loadHistory,30000);poll();loadHistory();
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
  char json[640];
  snprintf(json, sizeof(json),
    "{\"vbatt\":%.2f,\"temp_c\":%.1f,\"adc_mv\":%d,\"divider\":%.3f,\"cal\":%.3f,"
    "\"rssi\":%d,\"uptime_s\":%lu,\"heap_free\":%u,\"heap_total\":%u,"
    "\"psram_free\":%u,\"psram_total\":%u,\"disk_used\":%u,\"disk_total\":%u,"
    "\"mode\":\"%s\",\"ip\":\"%s\",\"interval_s\":%d,\"samples\":%d,\"led\":\"%s\",\"fw\":\"%s\",\"rf\":\"%s\","
    "\"epoch\":%lu,\"time_ok\":%s,\"cpu_mhz\":%u,\"wifi_ps\":%s}",
    v, tC, g_last_mv, DIVIDER, CAL, rssi, (unsigned long)(millis() / 1000),
    (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getHeapSize(),
    (unsigned)ESP.getFreePsram(), (unsigned)ESP.getPsramSize(),
    (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes(),
    apMode ? "ap" : "sta", ip.c_str(), (int)(SAMPLE_MS / 1000), histCount, voltStatus(v), FW_VERSION,
    rfStatusStr(),
    (unsigned long)(timeIsValid() ? (uint32_t)time(nullptr) : 0), timeIsValid() ? "true" : "false",
    (unsigned)getCpuFrequencyMhz(), WiFi.getSleep() ? "true" : "false");
  g_out_total += strlen(json);
  server.send(200, "application/json", json);
}

// Streams the ring buffer oldest->newest as CSV (one line per sample).
void handleHistory() {
  trackReq();
  server.setContentLength(CONTENT_LENGTH_UNKNOWN);
  server.send(200, "text/csv", "");
  const char* hdr = "ts,vbatt,temp,heap_kb,disk_kb,net_in,net_out,rssi\n";
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
      chunk += s.rssi;    chunk += '\n';
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

  analogSetPinAttenuation(VSENSE_PIN, ADC_11db);

  if (LittleFS.begin(true)) Serial.printf("LittleFS mounted: %u / %u bytes used\n",
                                      (unsigned)LittleFS.usedBytes(), (unsigned)LittleFS.totalBytes());
  else Serial.println("LittleFS mount FAILED");

  hist = (Sample*) ps_malloc(sizeof(Sample) * HIST_N);   // history lives in PSRAM
  Serial.printf("History buffer: %u bytes in %s\n", (unsigned)(sizeof(Sample) * HIST_N),
                hist ? "PSRAM" : "FAILED");
  loadHistory();
  Serial.printf("Loaded %d prior samples from flash.\n", histCount);
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
    Serial.printf("Vbatt=%6.2f V  temp=%4.1f C  heap=%uKB  disk=%uKB  samples=%d  %s\n",
                  v, temperatureRead(), (unsigned)(ESP.getFreeHeap() / 1024),
                  (unsigned)(LittleFS.usedBytes() / 1024), histCount, apMode ? "[AP]" : "[STA]");
  }
}
