"""
ADS1115 16-bit ADC driver — minimal single-shot single-ended read.

The ADS1115 is a 4-channel 16-bit I2C ADC. We use it to measure the car
battery voltage at OBD-II pin 16, scaled through a voltage divider:

    +12V battery ─[ 30kΩ ]──┬── ADS1115 AIN0
                            │
                          [ 10kΩ ]
                            │
                           GND

The divider produces ~3V at the ADC for a 12V battery, well inside the
ADS1115's input range with the default PGA (±4.096V on 3.3V rail).

Wiring (ADS1115 → ESP32):

    VDD → 3.3V
    GND → GND
    SCL → I2C SCL  (default GPIO 22)
    SDA → I2C SDA  (default GPIO 21)
    ADDR → GND     (= I2C address 0x48)
    AIN0 → divider midpoint (post 30k / 10k from +12V)

Usage:

    from lib.ads1115 import ADS1115
    adc = ADS1115(i2c_id=0, sda=21, scl=22)
    raw = adc.read_single_ended(channel=0)        # signed 16-bit
    voltage = adc.raw_to_volts(raw, pga=1)        # apply PGA scaling
    battery_v = voltage * 4.0                      # undo voltage divider

Runs in MicroPython on ESP32; CPython fallback for desktop development.
"""

try:
    from machine import I2C, Pin
    import time
    _MICROPYTHON = True
except ImportError:
    _MICROPYTHON = False

    class Pin:  # noqa
        def __init__(self, *a, **kw): pass

    class I2C:  # noqa
        def __init__(self, *a, **kw): pass
        def writeto_mem(self, addr, reg, data): pass
        def readfrom_mem(self, addr, reg, n):
            return bytes(n)

    class _Time:
        def sleep_ms(self, n): pass
    time = _Time()


# ----- ADS1115 register addresses -----

_REG_CONVERSION = 0x00
_REG_CONFIG = 0x01

# Default I2C address (ADDR pin tied to GND)
DEFAULT_ADDRESS = 0x48

# Config register bit fields (MSB first when written as 2 bytes)
# Bit 15:    OS — operational status / single-shot start
# Bits 14:12 MUX — input multiplexer
# Bits 11:9  PGA — programmable gain amplifier (full-scale range)
# Bit 8:     MODE — 1 = single-shot, 0 = continuous
# Bits 7:5   DR — data rate
# Bits 4:0   COMP_* — comparator (we disable it)

# MUX values for single-ended reads from AINx vs GND
MUX_AIN0 = 0b100
MUX_AIN1 = 0b101
MUX_AIN2 = 0b110
MUX_AIN3 = 0b111

# PGA values (full-scale range, ±V)
PGA_6_144V = (0, 6.144)
PGA_4_096V = (1, 4.096)
PGA_2_048V = (2, 2.048)
PGA_1_024V = (3, 1.024)
PGA_0_512V = (4, 0.512)
PGA_0_256V = (5, 0.256)

# Data rate (samples per second)
DR_8 = 0
DR_16 = 1
DR_32 = 2
DR_64 = 3
DR_128 = 4   # default
DR_250 = 5
DR_475 = 6
DR_860 = 7

# Conversion time per data rate in ms (approximate, includes overhead)
_DR_CONVERSION_MS = {0: 130, 1: 70, 2: 35, 3: 20, 4: 10, 5: 6, 6: 4, 7: 3}


class ADS1115:
    def __init__(self, i2c_id=0, address=DEFAULT_ADDRESS, sda=21, scl=22, freq=400_000):
        self.address = address
        if _MICROPYTHON:
            self.i2c = I2C(i2c_id, sda=Pin(sda), scl=Pin(scl), freq=freq)
        else:
            self.i2c = I2C()

    def read_single_ended(self, channel=0, pga=PGA_4_096V, data_rate=DR_128):
        """
        Take a single-shot reading from AIN<channel> vs GND.

        :param channel:   0-3, which AIN pin to read
        :param pga:       one of the PGA_* tuples (config_value, fs_volts)
        :param data_rate: one of DR_8 .. DR_860 (DR_128 is a good default)
        :return: signed 16-bit raw ADC reading
        """
        if channel not in (0, 1, 2, 3):
            raise ValueError("channel must be 0-3")

        mux = MUX_AIN0 + channel  # MUX_AIN0..MUX_AIN3 are consecutive
        pga_value = pga[0]

        # Build 16-bit config word
        config = (
            (1 << 15)              # OS = start single-shot conversion
            | (mux << 12)
            | (pga_value << 9)
            | (1 << 8)             # MODE = single-shot
            | (data_rate << 5)
            | 0b11                 # COMP_QUE = disable comparator
        )
        config_bytes = bytes([(config >> 8) & 0xFF, config & 0xFF])
        self.i2c.writeto_mem(self.address, _REG_CONFIG, config_bytes)

        # Wait long enough for the conversion to complete
        time.sleep_ms(_DR_CONVERSION_MS.get(data_rate, 10))

        raw_bytes = self.i2c.readfrom_mem(self.address, _REG_CONVERSION, 2)
        raw = (raw_bytes[0] << 8) | raw_bytes[1]
        # Two's complement 16-bit
        if raw & 0x8000:
            raw -= 0x10000
        return raw

    @staticmethod
    def raw_to_volts(raw, pga=PGA_4_096V):
        """Convert a signed 16-bit ADC reading to volts at the input pin."""
        fs_volts = pga[1]
        return raw * fs_volts / 32768.0

    def read_voltage(self, channel=0, pga=PGA_4_096V, data_rate=DR_128):
        """One-call helper: raw read + PGA scaling. Returns volts."""
        raw = self.read_single_ended(channel, pga, data_rate)
        return self.raw_to_volts(raw, pga)
