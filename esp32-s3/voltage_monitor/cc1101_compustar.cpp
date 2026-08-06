// cc1101_compustar.cpp -- implementation. See cc1101_compustar.h.

#include "cc1101_compustar.h"
#include "driver/pulse_cnt.h"   // ESP32 hardware pulse counter (crystal measurement)
#include <math.h>               // lround

// ---- CC1101 register addresses ----
enum {
  REG_IOCFG2 = 0x00, REG_IOCFG0 = 0x02, REG_FIFOTHR = 0x03,
  REG_PKTLEN = 0x06, REG_PKTCTRL1 = 0x07, REG_PKTCTRL0 = 0x08,
  REG_FSCTRL1 = 0x0B,
  REG_FREQ2 = 0x0D, REG_FREQ1 = 0x0E, REG_FREQ0 = 0x0F,
  REG_MDMCFG4 = 0x10, REG_MDMCFG3 = 0x11, REG_MDMCFG2 = 0x12,
  REG_MDMCFG1 = 0x13, REG_MDMCFG0 = 0x14, REG_DEVIATN = 0x15,
  REG_MCSM0 = 0x18, REG_FOCCFG = 0x19,
  REG_AGCCTRL2 = 0x1B, REG_AGCCTRL1 = 0x1C, REG_AGCCTRL0 = 0x1D,
  REG_FREND0 = 0x22,
  REG_FSCAL3 = 0x23, REG_FSCAL2 = 0x24, REG_FSCAL1 = 0x25, REG_FSCAL0 = 0x26,
  REG_TEST2 = 0x2C, REG_TEST1 = 0x2D, REG_TEST0 = 0x2E,
  REG_PARTNUM = 0x30, REG_VERSION = 0x31, REG_MARCSTATE = 0x35,
  REG_PATABLE = 0x3E,
};

// Command strobes
enum {
  STR_SRES = 0x30, STR_STX = 0x35, STR_SIDLE = 0x36, STR_SFTX = 0x3B,
  MARC_TX = 0x13,        // MARCSTATE value meaning the PA is actually keyed
};

// SPI header bits
static const uint8_t HDR_WRITE = 0x00;
static const uint8_t HDR_READ  = 0x80;
static const uint8_t HDR_BURST = 0x40;

CC1101Compustar::CC1101Compustar(SPIClass* spi, int sckPin, int misoPin,
                                 int mosiPin, int csPin, int gdo0Pin)
    : _spi(spi), _sck(sckPin), _miso(misoPin), _mosi(mosiPin),
      _cs(csPin), _gdo0(gdo0Pin),
      _spiSettings(4000000, MSBFIRST, SPI_MODE0) {}

void CC1101Compustar::csLow()  { digitalWrite(_cs, LOW); }
void CC1101Compustar::csHigh() { digitalWrite(_cs, HIGH); }

uint8_t CC1101Compustar::strobe(uint8_t cmd) {
  _spi->beginTransaction(_spiSettings);
  csLow();
  uint8_t status = _spi->transfer(cmd);
  csHigh();
  _spi->endTransaction();
  return status;
}

void CC1101Compustar::writeReg(uint8_t addr, uint8_t value) {
  _spi->beginTransaction(_spiSettings);
  csLow();
  _spi->transfer(HDR_WRITE | (addr & 0x3F));
  _spi->transfer(value);
  csHigh();
  _spi->endTransaction();
}

uint8_t CC1101Compustar::readReg(uint8_t addr) {
  // Status registers (>= 0x30) require the burst bit set, per the
  // datasheet -- otherwise the read targets a command strobe.
  uint8_t header = HDR_READ | (addr >= 0x30 ? HDR_BURST : 0) | (addr & 0x3F);
  _spi->beginTransaction(_spiSettings);
  csLow();
  _spi->transfer(header);
  uint8_t value = _spi->transfer(0x00);
  csHigh();
  _spi->endTransaction();
  return value;
}

void CC1101Compustar::writeBurst(uint8_t addr, const uint8_t* values, size_t n) {
  _spi->beginTransaction(_spiSettings);
  csLow();
  _spi->transfer(HDR_WRITE | HDR_BURST | (addr & 0x3F));
  for (size_t i = 0; i < n; i++) _spi->transfer(values[i]);
  csHigh();
  _spi->endTransaction();
}

void CC1101Compustar::reset() {
  // Manual power-up reset sequence (datasheet 19.1): toggle CS, then SRES.
  csHigh(); delayMicroseconds(5);
  csLow();  delayMicroseconds(10);
  csHigh(); delayMicroseconds(45);
  strobe(STR_SRES);
  delay(10);
}

void CC1101Compustar::configure433Ook(uint8_t txPower) {
  reset();

  // Register values: TI SmartRF Studio, 433.92 MHz, OOK/ASK, async serial
  // mode so we drive GDO0 ourselves. Identical to the MicroPython driver.
  struct Reg { uint8_t addr; uint8_t val; };
  static const Reg cfg[] = {
    {REG_IOCFG2, 0x0B},    // GDO2 = serial clock (unused, safe)
    {REG_IOCFG0, 0x2D},    // GDO0 = async serial data IN (from us)
    {REG_FIFOTHR, 0x47},
    {REG_PKTLEN, 0xFF},
    {REG_PKTCTRL1, 0x04},
    {REG_PKTCTRL0, 0x32},  // async serial, infinite packet length
    {REG_FSCTRL1, 0x06},
    // 0x10B11C. The textbook word for 433.92 MHz @ 26 MHz is 0x10B071, but this
    // module's crystal measures 25.991 MHz, which pulled the actual carrier down
    // to ~433.874 MHz. Verified on an SDR against the genuine FOB (433.9415 MHz)
    // and trimmed +68 kHz so our carrier lands at 433.9418 MHz -- on top of the FOB.
    {REG_FREQ2, 0x10},
    {REG_FREQ1, 0xB1},
    {REG_FREQ0, 0x1C},
    {REG_MDMCFG4, 0xF8},
    {REG_MDMCFG3, 0x83},
    {REG_MDMCFG2, 0x30},   // OOK/ASK, no preamble, no sync (async)
    {REG_MDMCFG1, 0x22},
    {REG_MDMCFG0, 0xF8},
    {REG_DEVIATN, 0x15},
    {REG_MCSM0, 0x18},     // auto-calibrate IDLE->TX
    {REG_FOCCFG, 0x14},
    {REG_AGCCTRL2, 0x03},
    {REG_AGCCTRL1, 0x00},
    {REG_AGCCTRL0, 0x91},
    {REG_FREND0, 0x11},    // use PATABLE[1] for the OOK '1' level
    {REG_FSCAL3, 0xE9},
    {REG_FSCAL2, 0x2A},
    {REG_FSCAL1, 0x00},
    {REG_FSCAL0, 0x1F},
    {REG_TEST2, 0x81},
    {REG_TEST1, 0x35},
    {REG_TEST0, 0x09},
  };
  for (const Reg& r : cfg) writeReg(r.addr, r.val);

  // PATABLE[0] = '0' bit (silence), [1] = '1' bit power.
  // 0xC0 ~ +10 dBm (max), 0x60 ~ 0 dBm, 0x50 ~ -3 dBm.
  uint8_t pa[2] = {0x00, txPower};
  writeBurst(REG_PATABLE, pa, 2);
}

bool CC1101Compustar::begin(uint8_t txPower) {
  pinMode(_cs, OUTPUT);
  digitalWrite(_cs, HIGH);
  pinMode(_gdo0, OUTPUT);
  digitalWrite(_gdo0, LOW);

  _spi->begin(_sck, _miso, _mosi, -1);   // CS handled manually

  configure433Ook(txPower);
  _ready = present();
  return _ready;
}

uint8_t CC1101Compustar::peekReg(uint8_t addr) { return readReg(addr); }

uint32_t CC1101Compustar::freqWord() {
  return ((uint32_t)readReg(REG_FREQ2) << 16) |
         ((uint32_t)readReg(REG_FREQ1) << 8) | readReg(REG_FREQ0);
}

void CC1101Compustar::nudgeFreqHz(int32_t hz) {
  // FREQ step = f_xtal / 2^16 ~= 396.7 Hz at 26 MHz.
  int32_t dw = (int32_t)lround((double)hz * 65536.0 / 26000000.0);
  uint32_t w = (uint32_t)((int32_t)freqWord() + dw);
  strobe(STR_SIDLE);
  writeReg(REG_FREQ2, (w >> 16) & 0xFF);
  writeReg(REG_FREQ1, (w >> 8) & 0xFF);
  writeReg(REG_FREQ0, w & 0xFF);
  // next STX auto-recalibrates the synth to the new frequency (MCSM0=0x18)
}

void CC1101Compustar::readPatable(uint8_t out[8]) {
  _spi->beginTransaction(_spiSettings);
  csLow();
  _spi->transfer(HDR_READ | HDR_BURST | 0x3E);   // PATABLE burst read
  for (int i = 0; i < 8; i++) out[i] = _spi->transfer(0x00);
  csHigh();
  _spi->endTransaction();
}

uint8_t CC1101Compustar::partnum() { return readReg(REG_PARTNUM); }
uint8_t CC1101Compustar::version() { return readReg(REG_VERSION); }

bool CC1101Compustar::present() {
  uint8_t pn = partnum();
  uint8_t ver = version();
  // Genuine CC1101: PARTNUM 0x00, VERSION 0x14. Clones report 0x04/0x07.
  // A floating bus reads 0x00/0x00 or 0xFF/0xFF, so require a non-trivial
  // version and the expected partnum.
  if (pn != 0x00) return false;
  if (ver == 0x00 || ver == 0xFF) return false;
  return true;
}

CC1101SelfTest CC1101Compustar::selfTest() {
  CC1101SelfTest t = {};
  t.partnum = partnum();
  t.version = version();
  t.spiOk = (t.partnum == 0x00) && (t.version != 0x00) && (t.version != 0xFF);

  // Read back a handful of the values configure433Ook() wrote. If SPI
  // read+write are sound and the chip held its config, these match.
  struct Expect { uint8_t addr; uint8_t val; };
  static const Expect expect[] = {
    {REG_FREQ2, 0x10}, {REG_FREQ1, 0xB0}, {REG_FREQ0, 0x71},
    {REG_PKTCTRL0, 0x32}, {REG_MDMCFG2, 0x30}, {REG_FREND0, 0x11},
  };
  t.regsOk = true;
  for (const Expect& e : expect) {
    if (readReg(e.addr) != e.val) { t.regsOk = false; break; }
  }

  // Confirm the chip enters the TX path on STX. GDO0 is held LOW the whole
  // time = OOK '0' = carrier OFF, so this radiates nothing meaningful.
  digitalWrite(_gdo0, LOW);
  strobe(STR_SFTX);
  strobe(STR_STX);
  t.txEntered = false;
  for (int i = 0; i < 50; i++) {           // poll up to ~5 ms
    t.marcstate = readReg(REG_MARCSTATE) & 0x1F;
    // 0x08-0x0B = calibrate/settling (en route to TX), 0x12 = FSTXON,
    // 0x13 = TX. Any of these means the TX path engaged.
    if (t.marcstate == 0x12 || t.marcstate == 0x13 ||
        (t.marcstate >= 0x08 && t.marcstate <= 0x0B)) {
      t.txEntered = true;
      break;
    }
    delayMicroseconds(100);
  }
  strobe(STR_SIDLE);                         // back to IDLE, transmitter off
  digitalWrite(_gdo0, LOW);

  t.gdo0Ok = gdo0Continuity();

  t.ok = t.spiOk && t.regsOk && t.txEntered && t.gdo0Ok;
  return t;
}

// --- crystal frequency measurement (no SDR needed) ---
// Uses the ESP32 PCNT hardware counter, NOT a software interrupt: at 135 kHz a
// software ISR loses edges whenever a WiFi ISR preempts it, which reads ~1.7 %
// low and would frame a real 26 MHz part as a bogus ~25.5 MHz. PCNT counts in
// silicon with zero misses.
uint32_t CC1101Compustar::measureXtalHz() {
  if (!_ready) return 0;
  strobe(STR_SIDLE);
  uint8_t savedIocfg0 = readReg(REG_IOCFG0);

  pinMode(_gdo0, INPUT);
  writeReg(REG_IOCFG0, 0x3F);        // GDO0 = CLK_XOSC / 192 (~135 kHz @ 26 MHz)
  delay(2);                          // settle

  uint32_t xtal = 0;
  pcnt_unit_config_t uc = {};
  uc.high_limit = 32000; uc.low_limit = -1;
  pcnt_unit_handle_t unit = nullptr;
  if (pcnt_new_unit(&uc, &unit) == ESP_OK) {
    pcnt_chan_config_t cc = {};
    cc.edge_gpio_num = _gdo0; cc.level_gpio_num = -1;
    pcnt_channel_handle_t ch = nullptr;
    if (pcnt_new_channel(unit, &cc, &ch) == ESP_OK) {
      pcnt_channel_set_edge_action(ch, PCNT_CHANNEL_EDGE_ACTION_INCREASE,
                                       PCNT_CHANNEL_EDGE_ACTION_HOLD);   // count rising edges
      pcnt_unit_enable(unit);
      pcnt_unit_clear_count(unit);
      uint32_t t0 = micros();
      pcnt_unit_start(unit);
      delay(100);                    // ~13.5k edges @ 26 MHz -> under the 32k limit
      pcnt_unit_stop(unit);
      uint32_t dt = micros() - t0;
      int cnt = 0;
      pcnt_unit_get_count(unit, &cnt);
      pcnt_unit_disable(unit);
      pcnt_del_channel(ch);
      if (dt > 0 && cnt > 0) {
        double gdo0Hz = (double)cnt * 1e6 / (double)dt;   // edges/sec
        xtal = (uint32_t)(gdo0Hz * 192.0 + 0.5);          // xtal = GDO0 * 192
      }
    }
    pcnt_del_unit(unit);
  }

  writeReg(REG_IOCFG0, savedIocfg0);
  pinMode(_gdo0, OUTPUT);
  digitalWrite(_gdo0, LOW);
  return xtal;
}

bool CC1101Compustar::gdo0Continuity() {
  // Confirm the GDO0<->GPIO wire is actually connected, with no extra
  // hardware and no RF: have the CC1101 drive a clock onto its GDO0 pin and
  // read it back on our GPIO. A connected, actively-driven pin overrides the
  // ESP32's internal pull both ways; a floating (disconnected) pin just
  // follows the pull. Chip stays in IDLE -- nothing is radiated.
  strobe(STR_SIDLE);

  // Release the ESP32's drive BEFORE the CC1101 starts driving (avoid
  // momentary output-vs-output contention).
  pinMode(_gdo0, INPUT_PULLDOWN);
  uint8_t savedIocfg0 = readReg(REG_IOCFG0);
  writeReg(REG_IOCFG0, 0x3F);     // GDO0 = CLK_XOSC/192 (~135 kHz)
  delayMicroseconds(300);

  // Pulldown: a driven clock still shows HIGHs; a floating pin stays low.
  bool sawHigh = false;
  for (int i = 0; i < 4000; i++) { if (digitalRead(_gdo0)) { sawHigh = true; break; } }

  // Pullup: a driven clock still shows LOWs; a floating pin stays high.
  pinMode(_gdo0, INPUT_PULLUP);
  delayMicroseconds(300);
  bool sawLow = false;
  for (int i = 0; i < 4000; i++) { if (!digitalRead(_gdo0)) { sawLow = true; break; } }

  // Restore: CC1101 GDO0 back to async-data-in FIRST, then re-take the line
  // as our output (again, no contention).
  writeReg(REG_IOCFG0, savedIocfg0);   // 0x2D
  pinMode(_gdo0, OUTPUT);
  digitalWrite(_gdo0, LOW);

  // Connected => the pin was actively driven to BOTH levels against the pull.
  return sawHigh && sawLow;
}

// Enter TX and wait until the PA is genuinely up.
//
// This wait is not optional. MCSM0 = 0x18 means FS_AUTOCAL = "calibrate on
// every IDLE->TX", and that calibration takes ~721 us at 26 MHz during which
// the chip radiates NOTHING. The old code strobed STX and started toggling
// GDO0 immediately, so the ~1476 us leading sync pulse was half-swallowed --
// and because each repeat dropped back to IDLE, all 8 repeats lost their
// sync. The packet went out and the receiver never framed it: transmitter
// healthy, self-test green, car silent. (The boot self-test even recorded the
// symptom -- MARCSTATE read 0x08 = CALIBRATE right after STX, never 0x13 = TX.)
bool CC1101Compustar::enterTxAndWait(uint32_t timeoutUs) {
  strobe(STR_SIDLE);
  strobe(STR_SFTX);
  strobe(STR_STX);
  uint32_t t0 = micros();
  while ((uint32_t)(micros() - t0) < timeoutUs) {
    if ((readReg(REG_MARCSTATE) & 0x1F) == MARC_TX) return true;
    delayMicroseconds(20);
  }
  return false;
}

void CC1101Compustar::transmitPulses(const CompustarPulse* pulses, size_t n) {
  // Caller has already put the chip in TX and confirmed it settled; we only
  // modulate here. Staying in TX across the whole burst is correct for OOK --
  // PATABLE[0] is 0x00, so GDO0 LOW is genuinely zero output power, i.e. the
  // inter-repeat guard is real silence without a re-calibration each time.

  // Bit-bang GDO0. Pulse widths are 700-1500 us; delayMicroseconds on the
  // S3 is cycle-accurate and the few-us of WiFi-ISR jitter is far inside
  // the Compustar receiver's +/-10-15% window for these widths, so we keep
  // interrupts enabled (disabling them for the ~70 ms burst would stall
  // WiFi).
  for (size_t i = 0; i < n; i++) {
    if (pulses[i].high_us) {
      digitalWrite(_gdo0, HIGH);
      delayMicroseconds(pulses[i].high_us);
    }
    if (pulses[i].low_us) {
      digitalWrite(_gdo0, LOW);
      delayMicroseconds(pulses[i].low_us);
    }
  }
  digitalWrite(_gdo0, LOW);   // leave the carrier off; caller strobes IDLE
}

bool CC1101Compustar::transmitButton(const char* pattern, uint8_t repeats,
                                     uint16_t guardMs) {
  if (!_ready) return false;
  if (!pattern) return false;

  // Validate: exactly CMP_PACKET_BITS chars, all '0'/'1'.
  size_t len = strlen(pattern);
  if (len != CMP_PACKET_BITS) return false;
  for (size_t i = 0; i < len; i++) {
    if (pattern[i] != '0' && pattern[i] != '1') return false;
  }

  // Render: sync triplet, then one pulse pair per data bit.
  CompustarPulse pulses[CMP_SYNC_COUNT + CMP_PACKET_BITS];
  size_t k = 0;
  for (uint8_t s = 0; s < CMP_SYNC_COUNT; s++) {
    pulses[k].high_us = CMP_SYNC_HIGH_US;
    pulses[k].low_us  = CMP_SYNC_LOW_US;
    k++;
  }
  for (size_t i = 0; i < len; i++) {
    if (pattern[i] == '1') {
      pulses[k].high_us = CMP_LONG_HIGH_US;
      pulses[k].low_us  = CMP_LONG_LOW_US;
    } else {
      pulses[k].high_us = CMP_SHORT_HIGH_US;
      pulses[k].low_us  = CMP_SHORT_LOW_US;
    }
    k++;
  }

  // Key the PA ONCE for the whole burst and confirm it actually reached TX
  // before the first edge, then hold TX across all repeats.
  if (!enterTxAndWait(5000)) {
    strobe(STR_SIDLE);
    return false;                       // never got to TX -- report failure
  }
  for (uint8_t r = 0; r < repeats; r++) {
    transmitPulses(pulses, k);
    if (r < repeats - 1) delay(guardMs);  // GDO0 low = carrier off = real silence
  }
  digitalWrite(_gdo0, LOW);
  strobe(STR_SIDLE);
  return true;
}

bool CC1101Compustar::transmitButtonWakeup(const char* pattern, uint16_t wakeupMs,
                                           uint8_t trainCells, uint8_t dataReps,
                                           uint8_t bursts, uint16_t guardMs) {
  if (!_ready || !pattern) return false;
  size_t len = strlen(pattern);
  if (len != CMP_PACKET_BITS) return false;
  for (size_t i = 0; i < len; i++)
    if (pattern[i] != '0' && pattern[i] != '1') return false;

  // Render the sync triplet + data bits, same as transmitButton.
  CompustarPulse pulses[CMP_SYNC_COUNT + CMP_PACKET_BITS];
  size_t k = 0;
  for (uint8_t s = 0; s < CMP_SYNC_COUNT; s++) {
    pulses[k].high_us = CMP_SYNC_HIGH_US; pulses[k].low_us = CMP_SYNC_LOW_US; k++;
  }
  for (size_t i = 0; i < len; i++) {
    if (pattern[i] == '1') { pulses[k].high_us = CMP_LONG_HIGH_US;  pulses[k].low_us = CMP_LONG_LOW_US; }
    else                   { pulses[k].high_us = CMP_SHORT_HIGH_US; pulses[k].low_us = CMP_SHORT_LOW_US; }
    k++;
  }

  if (!enterTxAndWait(5000)) { strobe(STR_SIDLE); return false; }

  for (uint8_t b = 0; b < bursts; b++) {
    // (1) Wake-up carrier: hold the carrier ON continuously so a duty-cycled
    //     receiver catches it during one of its listen windows. delay() yields
    //     to the RTOS, so WiFi survives; GDO0 stays HIGH the whole time.
    digitalWrite(_gdo0, HIGH);
    delay(wakeupMs);
    // (2) Training run: a few ~750 us equal on/off cells, as the FOB sends,
    //     to let the receiver's data slicer settle its threshold and clock.
    for (uint8_t t = 0; t < trainCells; t++) {
      digitalWrite(_gdo0, LOW);  delayMicroseconds(760);
      digitalWrite(_gdo0, HIGH); delayMicroseconds(745);
    }
    digitalWrite(_gdo0, LOW);
    // (3) The sync + data packet, repeated dataReps times so the just-woken
    //     receiver reliably catches it (the FOB sends ~8).
    for (uint8_t d = 0; d < dataReps; d++) {
      transmitPulses(pulses, k);
      if (d < dataReps - 1) delay(guardMs);
    }
    if (b < bursts - 1) delay(guardMs);
  }
  digitalWrite(_gdo0, LOW);
  strobe(STR_SIDLE);
  return true;
}
