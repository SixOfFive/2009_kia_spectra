"""Light unit tests for cc1101 driver — CPython only (no hardware)."""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "src"))

from lib import cc1101  # noqa: E402


class CaptureSPI:
    """Mock SPI that records every write/read for later assertions."""
    def __init__(self):
        self.writes = []
        self.read_values = []

    def write(self, data):
        self.writes.append(("write", bytes(data)))

    def write_readinto(self, tx, rx):
        self.writes.append(("write_readinto", bytes(tx)))
        for i in range(len(rx)):
            rx[i] = self.read_values.pop(0) if self.read_values else 0

    def deinit(self):
        pass


class CapturePin:
    """Mock Pin that records every on/off/value call."""
    OUT = 0
    IN = 1

    def __init__(self, *args, **kwargs):
        self.history = []
        self._v = 0

    def on(self):
        self._v = 1
        self.history.append("on")

    def off(self):
        self._v = 0
        self.history.append("off")

    def value(self, v=None):
        if v is None:
            return self._v
        self._v = int(v)
        self.history.append("value=" + str(v))


def _make_radio():
    radio = cc1101.CC1101.__new__(cc1101.CC1101)
    radio.cs = CapturePin()
    radio.gdo0 = CapturePin()
    radio.spi = CaptureSPI()
    return radio


def test_strobe_writes_one_byte():
    radio = _make_radio()
    radio.spi.read_values = [0x0F]   # mocked status byte
    status = radio._strobe(cc1101.STX)
    assert status == 0x0F
    op, data = radio.spi.writes[0]
    assert op == "write_readinto" and data == bytes([cc1101.STX])


def test_write_reg_format():
    radio = _make_radio()
    radio._write_reg(cc1101.FREQ2, 0x10)
    op, data = radio.spi.writes[0]
    assert op == "write" and data == bytes([cc1101.FREQ2, 0x10])


def test_read_reg_uses_burst_bit_for_status_registers():
    radio = _make_radio()
    radio.spi.read_values = [0x00, 0x14]   # version byte returned as 2nd byte
    value = radio._read_reg(cc1101.VERSION)
    assert value == 0x14
    op, data = radio.spi.writes[0]
    # VERSION = 0x31 >= 0x30 so burst+read bits should be set: 0xC0 | 0x31 = 0xF1
    assert data[0] == (cc1101.VERSION | 0x80 | 0x40)


def test_write_burst_writes_header_plus_values():
    radio = _make_radio()
    radio._write_burst(cc1101.PATABLE, [0x00, 0xC0])
    op, data = radio.spi.writes[0]
    assert op == "write"
    assert data[0] == (cc1101.PATABLE | 0x40)
    assert data[1:] == bytes([0x00, 0xC0])


def test_transmit_pulses_toggles_gdo0_per_pulse():
    radio = _make_radio()
    pulses = [(400, 800), (800, 400), (0, 4000)]   # bit0, bit1, gap
    radio.transmit_pulses(pulses)

    # gdo0 should see on/off pairs for the first two pulses, only off for the gap
    # plus a final off after the last pulse and the SIDLE strobe sequencing.
    ons = [h for h in radio.gdo0.history if h == "on"]
    offs = [h for h in radio.gdo0.history if h == "off"]
    assert len(ons) == 2  # only pulses with high_us > 0 turn on
    assert len(offs) >= 3  # at least one per pulse with low_us > 0, plus final off


def test_init_writes_all_expected_registers():
    radio = _make_radio()
    # Pre-load read values for the reset path (sleep_ms is a no-op in CPython)
    radio.init_433mhz_ook()
    write_ops = [w for w in radio.spi.writes if w[0] == "write"]
    # At least the SRES strobe + many register writes + PATABLE burst
    # Count register writes (2-byte writes)
    register_writes = [w for w in write_ops if len(w[1]) == 2]
    assert len(register_writes) >= 20  # ~30 registers expected

    # Confirm FREQ registers match 433.92 MHz values
    freq_writes = {w[1][0]: w[1][1] for w in register_writes if w[1][0] in (cc1101.FREQ2, cc1101.FREQ1, cc1101.FREQ0)}
    assert freq_writes[cc1101.FREQ2] == 0x10
    assert freq_writes[cc1101.FREQ1] == 0xB0
    assert freq_writes[cc1101.FREQ0] == 0x71


def run():
    tests = [v for k, v in globals().items() if k.startswith("test_") and callable(v)]
    passed = 0
    for t in tests:
        t()
        passed += 1
        print(f"PASS {t.__name__}")
    print(f"\n{passed}/{len(tests)} cc1101 tests passed")


if __name__ == "__main__":
    run()
