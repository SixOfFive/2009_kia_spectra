// cc1101_compustar.cpp — implementation. See cc1101_compustar.h.

#include "cc1101_compustar.h"

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
  REG_PARTNUM = 0x30, REG_VERSION = 0x31,
  REG_PATABLE = 0x3E,
};

// Command strobes
enum {
  STR_SRES = 0x30, STR_STX = 0x35, STR_SIDLE = 0x36, STR_SFTX = 0x3B,
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
  // datasheet — otherwise the read targets a command strobe.
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
    {REG_FREQ2, 0x10},     // 0x10B071 -> ~433.92 MHz with a 26 MHz xtal
    {REG_FREQ1, 0xB0},
    {REG_FREQ0, 0x71},
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

void CC1101Compustar::transmitPulses(const CompustarPulse* pulses, size_t n) {
  strobe(STR_SFTX);   // flush, then enter TX
  strobe(STR_STX);

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
  digitalWrite(_gdo0, LOW);
  strobe(STR_SIDLE);
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

  for (uint8_t r = 0; r < repeats; r++) {
    transmitPulses(pulses, k);
    if (r < repeats - 1) delay(guardMs);
  }
  return true;
}
