"""
Compustar / HCS-family rolling-code packet builder.

The Compustar 1WSHR-PRO FOB transmits 433.92 MHz OOK packets in the standard
Microchip HCS300/301/361 format. Each packet contains:

    [preamble][header gap][32-bit hopping code][28-bit fixed code][2-bit status]

  - Preamble: ~12 cycles of alternating bits at TE rate
  - Header gap: ~10 * TE silence
  - Hopping code: 32-bit KeeLoq encryption of [counter(16) | discrimination(4) | function(4) | OVR(2) | nothing(6)]
    using the FOB's device key (64-bit, secret)
  - Fixed code: 28-bit FOB serial + 4-bit function code (sent in the clear)
  - Status: 1-bit V-LOW + 1-bit repeat indicator

Each bit on the wire uses PWM-style encoding:
    binary 0 = short high pulse (~TE) + long low (~2*TE)
    binary 1 = long high (~2*TE) + short low (~TE)

Constants below for function codes and exact TE timing are PLACEHOLDERS until
we confirm them via SDR capture of the actual 1WSHR-PRO FOB. Pin them down
in `sdr/analysis/framing.md` once captures are processed.

This module runs in both CPython and MicroPython.
"""

from lib import keeloq


# ----- Placeholders to verify from SDR captures -----

# Element time (basic bit period) for HCS series. Datasheet default is
# ~400 us but each chip can be configured 25-1600 us. Confirm by measuring
# the preamble cycle width on an SDR capture.
TE_MICROSECONDS = 400

# Preamble length in TE cycles (HCS300/301 default is 23 half-bit cycles,
# producing ~11.5 high/low transitions before the header gap).
PREAMBLE_HALF_BITS = 23

# Header gap is silence after the preamble. Datasheet default = 10 * TE.
HEADER_GAP_TE = 10

# Guard time between repeat packets in a multi-press burst.
# Datasheet default = 39 ms but varies.
GUARD_TIME_MS = 39


# ----- Function codes (placeholder; verify with SDR per button) -----
# These are the 4-bit codes the FOB transmits in the fixed-code portion
# AND that get hashed into the hopping code's discrimination bits.
# The mapping is manufacturer-specific. Compustar values to be confirmed.
class Function:
    START = 0x4   # placeholder
    LOCK = 0x2    # placeholder
    UNLOCK = 0x1  # placeholder
    TRUNK = 0x8   # placeholder


# ----- Packet builder -----

def build_hopping_code(counter, function_code, discrimination, device_key):
    """
    Build the 32-bit hopping code by KeeLoq-encrypting the 32-bit
    [counter | discrimination | function | overflow] plaintext.

    Plaintext layout (LSB-first per HCS301 datasheet, AN642 figure 3-3):
      bits  0..15  = counter (16 bits)
      bits 16..19  = discrimination (4 bits — typically low nibble of serial)
      bits 20..23  = function code (4 bits)
      bits 24..25  = overflow flags (2 bits; set 0 for normal transmit)
      bits 26..31  = reserved / 0

    :param counter:        16-bit rolling counter
    :param function_code:  4-bit function (see Function class)
    :param discrimination: 4-bit discrimination value
    :param device_key:     64-bit per-device secret
    :return:               32-bit hopping code
    """
    plaintext = (
        (counter & 0xFFFF)
        | ((discrimination & 0xF) << 16)
        | ((function_code & 0xF) << 20)
        # overflow bits and reserved stay 0 for normal transmissions
    )
    return keeloq.encrypt(plaintext, device_key)


def build_packet(serial, function_code, counter, device_key, v_low=0, repeat=0):
    """
    Assemble a complete 66-bit Compustar transmission packet.

    :param serial:        28-bit FOB serial number
    :param function_code: 4-bit function code (see Function class)
    :param counter:       16-bit rolling counter (incremented each press)
    :param device_key:    64-bit per-device secret KeeLoq key
    :param v_low:         1-bit low-battery indicator (default 0)
    :param repeat:        1-bit repeat indicator (default 0; set 1 for repeats in burst)
    :return: dict with keys:
        - hopping_code: 32-bit encrypted block
        - fixed_code:   32-bit (28-bit serial | 4-bit function)
        - status:       2-bit (v_low | repeat)
        - bits:         list of 66 individual bits in transmit order (MSB first)
    """
    serial &= 0x0FFFFFFF  # 28 bits
    function_code &= 0xF
    # Discrimination is typically the low nibble of the serial.
    discrimination = serial & 0xF

    hopping = build_hopping_code(counter, function_code, discrimination, device_key)
    fixed = (serial << 4) | function_code  # 32 bits = 28-bit serial + 4-bit fn
    status = ((v_low & 1) << 1) | (repeat & 1)

    bits = _pack_bits(hopping, 32) + _pack_bits(fixed, 32) + _pack_bits(status, 2)
    assert len(bits) == 66, "packet must be exactly 66 bits"

    return {
        "hopping_code": hopping,
        "fixed_code": fixed,
        "status": status,
        "bits": bits,
    }


def _pack_bits(value, width):
    """Return ``value`` as a list of ``width`` bits, MSB first."""
    return [(value >> (width - 1 - i)) & 1 for i in range(width)]


# ----- OOK waveform generation -----

def packet_to_pulses(packet_bits, te_us=TE_MICROSECONDS,
                     preamble_half_bits=PREAMBLE_HALF_BITS,
                     header_gap_te=HEADER_GAP_TE):
    """
    Render a packet as a list of (high_us, low_us) pulse pairs suitable for
    feeding into a CC1101 GPIO toggle or a microcontroller PWM-style output.

    PWM encoding (HCS series):
      bit 0 = high TE,  low 2*TE
      bit 1 = high 2*TE, low TE

    Preamble:
      alternating high/low half-bits of TE each, ``preamble_half_bits`` of them.

    Header gap:
      silence of ``header_gap_te * TE`` after the preamble.

    Returns a list of (high_microseconds, low_microseconds) tuples representing
    the full transmission in chronological order.
    """
    pulses = []

    # Preamble: alternating TE high / TE low half-bits
    for _ in range(preamble_half_bits):
        pulses.append((te_us, te_us))

    # Header gap: one final silence before data begins.
    # Append zero high + long low to represent the gap as a pulse pair.
    pulses.append((0, header_gap_te * te_us))

    # Data bits in PWM encoding
    for bit in packet_bits:
        if bit:
            pulses.append((2 * te_us, te_us))
        else:
            pulses.append((te_us, 2 * te_us))

    return pulses
