// cc1101_compustar.h
// Self-contained CC1101 sub-GHz driver + Compustar 1WG3R fixed-code
// transmitter for the ESP32-S3 vroom build.
//
// Ported from the project's MicroPython reference driver
// (esp32/src/lib/cc1101.py + esp32/src/lib/compustar.py). No external
// CC1101 Arduino library — the register init comes straight from TI
// SmartRF Studio for 433.92 MHz async OOK, and we bit-bang the GDO0 pin
// with the exact (HIGH_us, LOW_us) pulse pairs the Compustar 1WG3R
// family expects. See docs/compustar-research.md and
// sdr/analysis/framing.md for the protocol.
//
// Wiring (CC1101 module -> ESP32-S3, all 3.3V logic — the CC1101 is
// NOT 5V tolerant):
//
//     CC1101   ESP32-S3 GPIO (defaults; override in the .ino)
//     ------   -----------------------------------------------
//     SCK   -> 18
//     MISO  -> 17   (SO)
//     MOSI  -> 16   (SI)
//     CSn   -> 15
//     GDO0  ->  4   (async OOK data line we drive)
//     VCC   -> 3V3  (NOT 5V)
//     GND   -> GND
//
// GDO2 is left unconnected. A 433 MHz quarter-wave whip (~17 cm) or the
// module's SMA antenna goes on the CC1101's antenna pad.

#pragma once
#include <Arduino.h>
#include <SPI.h>

// ---- Compustar 1WG3R / 1WSHR-PRO timing (microseconds) ----
// rtl_433 -A measured on the project's known-good FOB; within ~3% of the
// 1WG3R spec and well inside Compustar receiver tolerance. See
// sdr/analysis/framing.md.
static const uint16_t CMP_SHORT_HIGH_US = 732;
static const uint16_t CMP_SHORT_LOW_US  = 1136;
static const uint16_t CMP_LONG_HIGH_US  = 1100;
static const uint16_t CMP_LONG_LOW_US   = 756;
static const uint16_t CMP_SYNC_HIGH_US  = 1476;
static const uint16_t CMP_SYNC_LOW_US   = 1500;

// 1WSHR-PRO sub-variant framing: 3 sync pulses then 35 data bits.
static const uint8_t  CMP_SYNC_COUNT = 3;
static const uint8_t  CMP_PACKET_BITS = 35;

// Burst defaults — mimic how the genuine FOB repeats a packet per press.
static const uint8_t  CMP_DEFAULT_REPEATS  = 8;
static const uint16_t CMP_DEFAULT_GUARD_MS = 39;

struct CompustarPulse {
  uint16_t high_us;
  uint16_t low_us;
};

// Result of a non-transmitting CC1101 health check. See CC1101Compustar::selfTest.
struct CC1101SelfTest {
  bool    spiOk;       // PARTNUM/VERSION are plausible (SPI + power good)
  uint8_t partnum;     // expect 0x00
  uint8_t version;     // expect 0x14 (0x04/0x07 on clones)
  bool    regsOk;      // key 433/OOK config registers read back as written
  bool    txEntered;   // chip reached a TX-path state after STX (GDO0 held low = no carrier)
  uint8_t marcstate;   // last MARCSTATE seen during the TX-entry probe
  bool    ok;          // spiOk && regsOk && txEntered
};

class CC1101Compustar {
 public:
  CC1101Compustar(SPIClass* spi, int sckPin, int misoPin, int mosiPin,
                  int csPin, int gdo0Pin);

  // Init SPI + reset + configure for 433.92 MHz async OOK.
  // Returns true if the chip answers with a plausible PARTNUM/VERSION.
  bool begin(uint8_t txPower = 0xC0);

  // Read PARTNUM (expect 0x00) and VERSION (0x14, or 0x04 on some clones).
  bool present();
  uint8_t partnum();
  uint8_t version();

  // Non-transmitting health check: confirms SPI/power (PARTNUM/VERSION),
  // that the 433/OOK config registers read back as written, and that the
  // chip enters a TX-path state on STX. GDO0 is held LOW throughout, which
  // in OOK is the '0'/off level (PATABLE[0]=0x00) — so NO carrier is
  // radiated and nothing can be triggered. Safe to run any time.
  CC1101SelfTest selfTest();

  // Validate a 35-char "0"/"1" pattern, render it to the OOK pulse train
  // (sync triplet + data bits), and transmit it `repeats` times with
  // `guardMs` of silence between repeats. Returns false if the pattern is
  // malformed (wrong length or a non-bit character).
  bool transmitButton(const char* pattern,
                      uint8_t repeats = CMP_DEFAULT_REPEATS,
                      uint16_t guardMs = CMP_DEFAULT_GUARD_MS);

  // True once begin() has confirmed the chip is present.
  bool ready() const { return _ready; }

 private:
  SPIClass* _spi;
  int _sck, _miso, _mosi, _cs, _gdo0;
  bool _ready = false;
  SPISettings _spiSettings;

  void csLow();
  void csHigh();
  uint8_t strobe(uint8_t cmd);
  void writeReg(uint8_t addr, uint8_t value);
  uint8_t readReg(uint8_t addr);
  void writeBurst(uint8_t addr, const uint8_t* values, size_t n);

  void reset();
  void configure433Ook(uint8_t txPower);

  // Drive GDO0 through one rendered pulse list once.
  void transmitPulses(const CompustarPulse* pulses, size_t n);
};
