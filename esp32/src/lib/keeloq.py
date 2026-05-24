"""
KeeLoq block cipher — pure Python implementation.

KeeLoq is a 32-bit block cipher with a 64-bit key and 528 rounds. Designed by
Nanotronics (acquired by Microchip in 2012) for use in HCS3xx-series rolling
code remote keyless entry encoders, garage door openers, and aftermarket
remote start systems.

Algorithm (from Microchip HCS301 datasheet and Wikipedia):

    y = plaintext
    for i in 0..527:
        bit_new = y[0] XOR y[16] XOR key[i mod 64] XOR NLF(y[1], y[9], y[20], y[26], y[31])
        y = (y >> 1) | (bit_new << 31)

The NLF (non-linear function) is implemented as a 32-bit lookup constant
0x3A5C742E indexed by 5 bits taken from specific positions in y.

This module runs unmodified in both CPython 3.x and MicroPython on ESP32.

Security note: the cipher has academic weaknesses (Eisentrager et al.,
Eurocrypt 2008) allowing key recovery from ~2^16 chosen plaintexts in
practical time. For our use case (synthesizing transmissions on behalf
of a remote we physically own) we just need correct encryption matching
what the receiver expects.
"""

# Non-linear function lookup table — 5-bit input indexes into this 32-bit
# constant. The bit at the indexed position is the NLF output.
# Equivalent boolean expression: NLF(a,b,c,d,e) is described in the
# Microchip patent as a particular Boolean function of 5 inputs; the
# 0x3A5C742E constant encodes the truth table.
_NLF = 0x3A5C742E


def _bit(value, position):
    """Return bit at ``position`` (0-indexed from LSB) as 0 or 1."""
    return (value >> position) & 1


def _nlf(y):
    """Compute the non-linear feedback bit for state ``y``."""
    idx = (
        _bit(y, 1)
        | (_bit(y, 9) << 1)
        | (_bit(y, 20) << 2)
        | (_bit(y, 26) << 3)
        | (_bit(y, 31) << 4)
    )
    return (_NLF >> idx) & 1


def encrypt(plaintext, key):
    """
    Encrypt a 32-bit plaintext block with a 64-bit key.

    :param plaintext: 32-bit integer (will be masked to 32 bits)
    :param key:       64-bit integer (will be masked to 64 bits)
    :return:          32-bit integer ciphertext
    """
    y = plaintext & 0xFFFFFFFF
    key &= 0xFFFFFFFFFFFFFFFF
    for round_idx in range(528):
        key_bit = (key >> (round_idx & 0x3F)) & 1
        new_top = _bit(y, 0) ^ _bit(y, 16) ^ key_bit ^ _nlf(y)
        y = ((y >> 1) | (new_top << 31)) & 0xFFFFFFFF
    return y


def decrypt(ciphertext, key):
    """
    Decrypt a 32-bit ciphertext block with a 64-bit key.

    :param ciphertext: 32-bit integer (will be masked to 32 bits)
    :param key:        64-bit integer (will be masked to 64 bits)
    :return:           32-bit integer plaintext

    Decryption reverses the round structure: at each step we shift y left
    by one and recover the bit that was originally shifted out (which is
    ``y_pre[0]``), using the fact that ``y_post[31]`` was computed during
    encryption as ``y_pre[0] ^ y_pre[16] ^ key_bit ^ NLF(y_pre[1,9,20,26,31])``.
    After shifting, ``y_pre[k+1] == y_post[k]``, so the NLF inputs in
    decryption read from ``y_post[0, 8, 19, 25, 30]``.
    """
    y = ciphertext & 0xFFFFFFFF
    key &= 0xFFFFFFFFFFFFFFFF
    for round_idx in range(527, -1, -1):
        key_bit = (key >> (round_idx & 0x3F)) & 1
        # NLF inputs during decrypt: post-state bits 0, 8, 19, 25, 30
        # (these are the pre-state bits 1, 9, 20, 26, 31 after the right shift)
        idx = (
            _bit(y, 0)
            | (_bit(y, 8) << 1)
            | (_bit(y, 19) << 2)
            | (_bit(y, 25) << 3)
            | (_bit(y, 30) << 4)
        )
        nlf_out = (_NLF >> idx) & 1
        # Solve for the bit that was shifted out:
        #   new_top = y_pre[0] ^ y_pre[16] ^ key_bit ^ nlf_out
        # where new_top is currently at y_post[31], and y_pre[16] is at y_post[15].
        old_bit_0 = _bit(y, 31) ^ _bit(y, 15) ^ key_bit ^ nlf_out
        y = ((y << 1) | old_bit_0) & 0xFFFFFFFF
    return y
