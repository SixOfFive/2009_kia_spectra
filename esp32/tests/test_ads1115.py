"""Light unit tests for ads1115 driver — CPython only (no hardware)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "src"))

from lib import ads1115  # noqa: E402


class FakeI2C:
    def __init__(self, response_bytes=b"\x00\x00"):
        self.writes = []   # list of (addr, reg, data)
        self.response = response_bytes

    def writeto_mem(self, addr, reg, data):
        self.writes.append((addr, reg, bytes(data)))

    def readfrom_mem(self, addr, reg, n):
        return self.response[:n]


def _make_adc():
    adc = ads1115.ADS1115.__new__(ads1115.ADS1115)
    adc.address = ads1115.DEFAULT_ADDRESS
    adc.i2c = FakeI2C()
    return adc


def test_raw_to_volts_positive_full_scale():
    # +FS at PGA=4.096V is 32767 → ~+4.096V (off by one LSB)
    v = ads1115.ADS1115.raw_to_volts(32767, pga=ads1115.PGA_4_096V)
    assert 4.09 < v < 4.10


def test_raw_to_volts_negative():
    v = ads1115.ADS1115.raw_to_volts(-32768, pga=ads1115.PGA_4_096V)
    assert v == -4.096


def test_raw_to_volts_zero():
    assert ads1115.ADS1115.raw_to_volts(0) == 0.0


def test_raw_to_volts_typical_battery_reading():
    # 12.6V battery through 4:1 divider = 3.15V at ADC
    # At PGA=4.096V full scale, raw = 3.15 * 32768 / 4.096 ≈ 25200
    v = ads1115.ADS1115.raw_to_volts(25200, pga=ads1115.PGA_4_096V)
    assert 3.14 < v < 3.16


def test_read_single_ended_writes_correct_config():
    adc = _make_adc()
    adc.i2c.response = bytes([0x62, 0x70])    # ~25200 raw
    raw = adc.read_single_ended(channel=0, pga=ads1115.PGA_4_096V, data_rate=ads1115.DR_128)
    # Decoded: 0x6270 = 25200
    assert raw == 25200

    # Confirm config write happened with correct shape
    addr, reg, data = adc.i2c.writes[0]
    assert addr == ads1115.DEFAULT_ADDRESS
    assert reg == ads1115._REG_CONFIG
    assert len(data) == 2

    config = (data[0] << 8) | data[1]
    assert (config >> 15) & 1 == 1            # OS = start
    assert (config >> 12) & 0b111 == 0b100    # MUX = AIN0 vs GND
    assert (config >> 9) & 0b111 == 1         # PGA = ±4.096V
    assert (config >> 8) & 1 == 1             # MODE = single-shot
    assert (config >> 5) & 0b111 == 4         # DR = 128 SPS
    assert config & 0b11 == 0b11              # comparator disabled


def test_read_single_ended_decodes_negative_two_complement():
    adc = _make_adc()
    adc.i2c.response = bytes([0xFF, 0xFF])  # -1
    raw = adc.read_single_ended()
    assert raw == -1


def test_read_voltage_combines_read_and_scaling():
    adc = _make_adc()
    adc.i2c.response = bytes([0x40, 0x00])  # 16384 = half full-scale
    v = adc.read_voltage(channel=0, pga=ads1115.PGA_4_096V)
    assert 2.04 < v < 2.06   # 4.096 / 2


def test_invalid_channel_raises():
    adc = _make_adc()
    try:
        adc.read_single_ended(channel=4)
    except ValueError:
        return
    raise AssertionError("expected ValueError for channel=4")


def run():
    tests = [v for k, v in globals().items() if k.startswith("test_") and callable(v)]
    passed = 0
    for t in tests:
        t()
        passed += 1
        print(f"PASS {t.__name__}")
    print(f"\n{passed}/{len(tests)} ads1115 tests passed")


if __name__ == "__main__":
    run()
