"""
CC1101 sub-GHz transceiver driver — minimal subset for 433.92 MHz OOK transmit.

The CC1101 has its own modulator but we use **async serial mode**: the chip
acts as a transparent OOK modulator and we bit-bang the GDO0 pin with the
exact PWM-encoded HCS pulses that ``compustar.packet_to_pulses`` emits.

That gives us microsecond-precise control over the on-air waveform without
having to fight the chip's packet engine, which doesn't natively understand
HCS's PWM bit encoding.

Wiring (CC1101 → ESP32):

    SCK   → SPI clock          (default GPIO 18)
    MISO  → SPI MISO           (default GPIO 19)
    MOSI  → SPI MOSI           (default GPIO 23)
    CSn   → CS / chip-select   (any free GPIO; we use GPIO 5 — adjust)
    GDO0  → data line (TX in)  (any free GPIO; we use GPIO 22)
    VCC   → 3.3V               (NOT 5V; CC1101 is 3.3V only)
    GND   → GND

Usage:

    from lib.cc1101 import CC1101
    radio = CC1101(spi_id=2, cs_pin=5, gdo0_pin=22)
    radio.init_433mhz_ook()
    radio.transmit_pulses(pulses, te_us=400)   # pulses from compustar.packet_to_pulses

Notes:

- Runs under MicroPython on ESP32. CPython smoke-test mode is provided for
  development on a desktop (no hardware required).
- Bit-bang timing precision depends on disabling GC during transmit and
  using ``time.sleep_us``. At TE=400µs the per-bit window is enormous in
  MicroPython terms (~2000+ cycles of overhead headroom), so this works
  reliably. For TE < 50µs you'd need ``esp32.RMT`` hardware timing instead.
"""

try:
    from machine import Pin, SPI
    import time
    import gc
    _MICROPYTHON = True
except ImportError:
    # CPython fallback for development / unit tests
    _MICROPYTHON = False

    class Pin:  # noqa
        OUT = 0
        IN = 1
        def __init__(self, *a, **kw): self._v = 0
        def value(self, v=None):
            if v is None:
                return self._v
            self._v = int(v)
        def on(self): self._v = 1
        def off(self): self._v = 0

    class SPI:  # noqa
        MSB = 0
        def __init__(self, *a, **kw): pass
        def write(self, data): pass
        def write_readinto(self, tx, rx):
            for i in range(len(rx)):
                rx[i] = 0
        def deinit(self): pass

    class _Time:
        def sleep_us(self, n): pass
        def sleep_ms(self, n): pass
        def ticks_us(self): return 0
        def ticks_diff(self, a, b): return 0
    time = _Time()

    class _Gc:
        def collect(self): pass
        def disable(self): pass
        def enable(self): pass
    gc = _Gc()


# ----- CC1101 register addresses -----

IOCFG2 = 0x00
IOCFG1 = 0x01
IOCFG0 = 0x02
FIFOTHR = 0x03
SYNC1 = 0x04
SYNC0 = 0x05
PKTLEN = 0x06
PKTCTRL1 = 0x07
PKTCTRL0 = 0x08
ADDR = 0x09
CHANNR = 0x0A
FSCTRL1 = 0x0B
FSCTRL0 = 0x0C
FREQ2 = 0x0D
FREQ1 = 0x0E
FREQ0 = 0x0F
MDMCFG4 = 0x10
MDMCFG3 = 0x11
MDMCFG2 = 0x12
MDMCFG1 = 0x13
MDMCFG0 = 0x14
DEVIATN = 0x15
MCSM2 = 0x16
MCSM1 = 0x17
MCSM0 = 0x18
FOCCFG = 0x19
BSCFG = 0x1A
AGCCTRL2 = 0x1B
AGCCTRL1 = 0x1C
AGCCTRL0 = 0x1D
FREND1 = 0x21
FREND0 = 0x22
FSCAL3 = 0x23
FSCAL2 = 0x24
FSCAL1 = 0x25
FSCAL0 = 0x26
TEST2 = 0x2C
TEST1 = 0x2D
TEST0 = 0x2E

PARTNUM = 0x30   # read with burst bit
VERSION = 0x31
MARCSTATE = 0x35
PATABLE = 0x3E

# Command strobes (single-byte writes that trigger state changes)
SRES = 0x30      # software reset
SCAL = 0x33      # calibrate
SRX = 0x34       # enable RX
STX = 0x35       # enable TX
SIDLE = 0x36     # exit RX/TX, go to IDLE
SFRX = 0x3A      # flush RX FIFO
SFTX = 0x3B      # flush TX FIFO
SNOP = 0x3D      # no-op


# Bit masks for SPI header byte
_WRITE = 0x00
_READ = 0x80
_BURST = 0x40


class CC1101:
    def __init__(self, spi_id=2, cs_pin=5, gdo0_pin=22,
                 sck=18, miso=19, mosi=23, baudrate=5_000_000):
        """Initialize the SPI bus and control pins; does NOT touch the chip yet."""
        self.cs = Pin(cs_pin, Pin.OUT)
        self.cs.on()  # CS active-low; idle high
        self.gdo0 = Pin(gdo0_pin, Pin.OUT)
        self.gdo0.off()

        if _MICROPYTHON:
            self.spi = SPI(spi_id, baudrate=baudrate, polarity=0, phase=0,
                           sck=Pin(sck), miso=Pin(miso), mosi=Pin(mosi))
        else:
            self.spi = SPI()

    # ----- Low-level SPI helpers -----

    def _strobe(self, cmd):
        """Issue a single-byte command strobe; returns the status byte."""
        self.cs.off()
        rx = bytearray(1)
        self.spi.write_readinto(bytes([cmd]), rx)
        self.cs.on()
        return rx[0]

    def _write_reg(self, addr, value):
        self.cs.off()
        self.spi.write(bytes([_WRITE | (addr & 0x3F), value & 0xFF]))
        self.cs.on()

    def _read_reg(self, addr):
        self.cs.off()
        rx = bytearray(2)
        # Status registers (0x30-0x3D) require burst bit set per datasheet table 32
        header = _READ | (_BURST if addr >= 0x30 else 0) | (addr & 0x3F)
        self.spi.write_readinto(bytes([header, 0]), rx)
        self.cs.on()
        return rx[1]

    def _write_burst(self, addr, values):
        self.cs.off()
        self.spi.write(bytes([_WRITE | _BURST | (addr & 0x3F)]) + bytes(values))
        self.cs.on()

    # ----- High-level operations -----

    def reset(self):
        """Software-reset the chip and wait for it to come ready."""
        self._strobe(SRES)
        if _MICROPYTHON:
            time.sleep_ms(10)

    def part_info(self):
        """Return (partnum, version). For CC1101 partnum=0x00, version=0x14 (or 0x04 on clones)."""
        return self._read_reg(PARTNUM), self._read_reg(VERSION)

    def init_433mhz_ook(self, tx_power=0xC0):
        """
        Configure the chip for 433.92 MHz async OOK transmit. Register values
        derived from TI SmartRF Studio for OOK, 5 kBaud, with async serial
        mode enabled so we can drive GDO0 directly.

        :param tx_power: PATABLE[1] value for OOK '1' bit power.
            0xC0 ≈ +10 dBm (max), 0x60 ≈ 0 dBm, 0x50 ≈ -3 dBm.
        """
        self.reset()

        config = [
            (IOCFG2, 0x0B),     # GDO2 = serial clock (unused here but safe)
            (IOCFG0, 0x2D),     # GDO0 = async serial data input from us
            (FIFOTHR, 0x47),
            (PKTLEN, 0xFF),
            (PKTCTRL1, 0x04),   # no addr filtering, append status
            (PKTCTRL0, 0x32),   # async serial mode, infinite packet length
            (FSCTRL1, 0x06),
            # 433.920 MHz with 26 MHz crystal: f = freq * fxosc / 2^16
            # 0x10B071 / 65536 * 26e6 ≈ 433.92e6
            (FREQ2, 0x10),
            (FREQ1, 0xB0),
            (FREQ0, 0x71),
            (MDMCFG4, 0xF8),    # mostly RX bandwidth — non-critical for async TX
            (MDMCFG3, 0x83),
            (MDMCFG2, 0x30),    # OOK / ASK, no preamble, no sync (async)
            (MDMCFG1, 0x22),
            (MDMCFG0, 0xF8),
            (DEVIATN, 0x15),
            (MCSM0, 0x18),      # auto-calibrate from IDLE→TX
            (FOCCFG, 0x14),
            (AGCCTRL2, 0x03),
            (AGCCTRL1, 0x00),
            (AGCCTRL0, 0x91),
            (FREND0, 0x11),     # use PATABLE[1] for OOK '1'
            (FSCAL3, 0xE9),
            (FSCAL2, 0x2A),
            (FSCAL1, 0x00),
            (FSCAL0, 0x1F),
            (TEST2, 0x81),
            (TEST1, 0x35),
            (TEST0, 0x09),
        ]
        for addr, value in config:
            self._write_reg(addr, value)

        # PATABLE controls OOK power: index 0 = '0' bit (silence), index 1 = '1' bit
        self._write_burst(PATABLE, [0x00, tx_power & 0xFF])

    def idle(self):
        self._strobe(SIDLE)

    def transmit_pulses(self, pulses, te_us=None):
        """
        Transmit a list of (high_us, low_us) pulse pairs via async OOK.

        Drives GDO0 high for ``high_us`` then low for ``low_us`` per pulse.
        The CC1101 must already be in TX mode (init_433mhz_ook + STX).

        :param pulses: list of (high_us, low_us) tuples
        :param te_us:  unused (kept for API symmetry with compustar module)
        """
        del te_us  # consumed at packet_to_pulses time
        self._strobe(STX)

        gdo0 = self.gdo0
        sleep_us = time.sleep_us

        # Disable GC for the duration of transmit so a collect cycle doesn't
        # stretch a low-pulse by milliseconds and break the receiver's PWM
        # window. Re-enable in finally.
        gc.collect()
        gc.disable()
        try:
            for high_us, low_us in pulses:
                if high_us:
                    gdo0.on()
                    sleep_us(high_us)
                if low_us:
                    gdo0.off()
                    sleep_us(low_us)
            gdo0.off()
        finally:
            gc.enable()
            self._strobe(SIDLE)

    def transmit_burst(self, pulses, repeats, guard_ms):
        """
        Transmit ``pulses`` ``repeats`` times with ``guard_ms`` of silence
        between each (HCS-style multi-press burst).
        """
        for i in range(repeats):
            self.transmit_pulses(pulses)
            if i < repeats - 1 and _MICROPYTHON:
                time.sleep_ms(guard_ms)
