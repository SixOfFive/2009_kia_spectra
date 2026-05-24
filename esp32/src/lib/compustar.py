"""
Compustar 1-way 1WSHR-PRO / 1WG3R-family FIXED-CODE remote packet builder.

The 1WSHR-PRO FOB transmits 433.92 MHz OOK packets in a fixed-code
PWM-encoded protocol that's a structural sub-variant of the canonical
rtl_433 ``compustar_1wg3r`` decoder (which covers 1WG3R-SH and
1WAMR-1900). Verified via SDR + rtl_433 ``-A`` pulse-analyzer on a
known-good FOB. See ``sdr/analysis/framing.md`` (public) and
``sdr/analysis/framing.local.md`` (gitignored) for the underlying
measurements.

PROTOCOL SUMMARY
----------------

Modulation: OOK, **symmetric** PWM (HIGH and LOW both vary per bit).
Each bit is encoded as a (HIGH_us, LOW_us) pulse pair:

    bit 0 = HIGH(~732 us)  + LOW(~1136 us)   (HIGH short, LOW long)
    bit 1 = HIGH(~1100 us) + LOW(~756 us)    (HIGH long,  LOW short)
    sync  = HIGH(~1476 us) + LOW(~1500 us)   (both long, ~equal)

Pulse-pair sum is approximately constant at ~1860 us, so the bit
period is ~1860 us; the asymmetry between HIGH and LOW encodes the
bit value.

Per-packet structure (1WSHR-PRO sub-variant):

    [3 x sync pulse]  +  [35 data bits]

The canonical rtl_433 1WG3R protocol is documented as ``1 sync + 36
bits`` with the field layout ``[16-bit Remote ID][3-bit 0b111][8-bit
~button][8-bit button][1-bit 0]``. Our specific FOB transmits 3 sync
pulses and 35 data bits whose middle field doesn't quite match that
layout. Since we replay captured bit patterns verbatim rather than
re-encode from a button code, we don't need to fully reverse the
field structure. The capture-then-replay approach works on every
1WG3R-family FOB regardless of sub-variant.

NO KeeLoq, NO rolling counter, NO device key are needed. Same press =
identical packet on the wire. Different button = different bit pattern.

USAGE
-----

::

    from lib import compustar

    pulses = compustar.build_pulses_for_button(
        compustar.Button.START,
        packets=cfg.COMPUSTAR_PACKETS,
    )
    radio.transmit_burst(pulses, repeats=cfg.RF_BURST_REPEATS,
                         guard_ms=cfg.RF_GUARD_MS)

The 35-bit patterns in ``cfg.COMPUSTAR_PACKETS`` come from SDR
captures of the FOB (see ``sdr/06-framing-extraction.md``).

Runs in both CPython (for unit tests) and MicroPython (for the
on-device firmware).
"""

# ----- Pulse widths in microseconds (rtl_433 -A measured on our FOB) -----
# These match the 1WG3R rtl_433 spec to within ~3% and are well inside
# Compustar receiver tolerance. Override per-call if a different sub-variant
# needs slightly different timing.

SHORT_HIGH_US = 732
SHORT_LOW_US = 1136
LONG_HIGH_US = 1100
LONG_LOW_US = 756
SYNC_HIGH_US = 1476
SYNC_LOW_US = 1500

# Each packet starts with this many sync pulses (3 for 1WSHR-PRO; canonical
# 1WG3R uses 1). Adjust if your FOB sub-variant differs.
SYNC_COUNT = 3

# Number of data bits per packet (35 for 1WSHR-PRO; canonical 1WG3R uses 36).
PACKET_BITS = 35


class Button:
    """Canonical button identifiers. Used as keys into the
    per-FOB packets dictionary in ``secrets.py``."""
    START = "START"
    LOCK = "LOCK"
    UNLOCK = "UNLOCK"
    TRUNK = "TRUNK"
    ALL = (START, LOCK, UNLOCK, TRUNK)


# ----- Packet parsing -----

def parse_bit_pattern(pattern, expected_bits=PACKET_BITS):
    """
    Convert a string of ``"0"`` and ``"1"`` characters into a list of ints.

    Whitespace is silently stripped (so you can store patterns in
    secrets.py with spaces / underscores for readability if you want).

    :param pattern: str like ``"01101001011010010110100101101001011"`` (35 chars)
    :param expected_bits: enforce this length after stripping whitespace
    :return: list of 0/1 ints, length ``expected_bits``
    :raises ValueError: on any non-bit character or wrong length
    """
    bits = []
    for c in pattern:
        if c in ("0", "1"):
            bits.append(int(c))
        elif c in (" ", "\t", "\n", "\r", "_", "-"):
            continue
        else:
            raise ValueError("invalid bit char %r in pattern" % (c,))
    if len(bits) != expected_bits:
        raise ValueError(
            "pattern has %d bits, expected %d" % (len(bits), expected_bits)
        )
    return bits


# ----- OOK waveform generation -----

def packet_to_pulses(bits,
                     sync_count=SYNC_COUNT,
                     short_high_us=SHORT_HIGH_US, short_low_us=SHORT_LOW_US,
                     long_high_us=LONG_HIGH_US, long_low_us=LONG_LOW_US,
                     sync_high_us=SYNC_HIGH_US, sync_low_us=SYNC_LOW_US):
    """
    Render a list of bits into a chronological list of OOK pulse pairs.

    Output ordering: ``sync_count`` sync pulses first, then one pulse pair
    per data bit. Each pulse pair is ``(high_us, low_us)`` so the CC1101
    driver toggles GDO0 high for ``high_us`` then low for ``low_us``.

    :param bits: list of 0/1 ints, typically the output of
        ``parse_bit_pattern(pattern)``
    :return: list of (high_us, low_us) tuples, length
        ``sync_count + len(bits)``
    """
    pulses = []
    for _ in range(sync_count):
        pulses.append((sync_high_us, sync_low_us))
    for b in bits:
        if b:
            pulses.append((long_high_us, long_low_us))
        else:
            pulses.append((short_high_us, short_low_us))
    return pulses


def build_pulses_for_button(button_name, packets):
    """
    Look up the captured bit pattern for ``button_name`` in ``packets``
    and render it to a pulse list.

    :param button_name: one of ``Button.START`` / ``LOCK`` / ``UNLOCK``
        / ``TRUNK`` (or any custom button key present in ``packets``)
    :param packets: dict mapping button name → bit-pattern string
    :return: list of (high_us, low_us) pulse pairs ready for
        ``cc1101.transmit_burst``
    :raises KeyError: if ``button_name`` isn't a key in ``packets``
    :raises ValueError: if the stored pattern is malformed
    """
    pattern = packets[button_name]
    bits = parse_bit_pattern(pattern)
    return packet_to_pulses(bits)


def validate_packets(packets):
    """
    Sanity-check a packets dict at startup. Returns a list of error
    strings; empty list means all good.

    :param packets: dict (or None) of button name → pattern str
    """
    errors = []
    if packets is None:
        return ["COMPUSTAR_PACKETS is None"]
    if not isinstance(packets, dict):
        return ["COMPUSTAR_PACKETS is not a dict: %r" % type(packets)]
    for name in Button.ALL:
        if name not in packets:
            errors.append("missing button %r in COMPUSTAR_PACKETS" % name)
            continue
        try:
            parse_bit_pattern(packets[name])
        except ValueError as e:
            errors.append("button %r: %s" % (name, e))
    return errors
