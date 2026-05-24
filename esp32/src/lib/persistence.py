"""
Persistent state helpers for the ESP32 controller.

Two tiers of persistence:

1. **RTC memory** (machine.RTC().memory()) — survives deep sleep but NOT
   power loss. Up to 2 KB on ESP32. We use a tiny binary blob (8 bytes)
   to track the consecutive-low-voltage sample count across deep sleeps.

2. **Flash file** — survives power loss. Used for the Compustar rolling
   counter (in controller.py directly, not here).

RTC memory layout (8 bytes total):
    bytes 0..3  : magic b"VRM2" (version marker; bump if layout changes)
    bytes 4..5  : low_v_count, unsigned 16-bit little-endian
    bytes 6..7  : reserved / padding (zero)

Why a sample count and not a timestamp?
    time.time() resets to zero on each deep-sleep wake (no RTC time source
    on most ESP32 boards). So we can't compare "now" to "streak start ts".
    Counting samples is robust: N consecutive low samples × WAKE_INTERVAL_S
    seconds per sample = sustained low duration.
"""

import struct


RTC_MAGIC = b"VRM2"
_RTC_FORMAT = "<H"           # uint16 little-endian
_RTC_BLOB_SIZE = 8           # magic(4) + count(2) + reserved(2)


try:
    from machine import RTC, reset_cause, DEEPSLEEP_RESET
    _HAS_RTC = True
except ImportError:
    _HAS_RTC = False
    DEEPSLEEP_RESET = 4      # ESP32 constant; useful for CPython mock comparison
    def reset_cause():       # noqa
        return 0


def save_streak(low_v_count):
    """Persist the consecutive-low-voltage sample count to RTC memory."""
    if not _HAS_RTC:
        return
    try:
        blob = RTC_MAGIC + struct.pack(_RTC_FORMAT, low_v_count & 0xFFFF) + b"\x00\x00"
        RTC().memory(blob)
    except Exception as e:
        print("[persistence] WARN save_streak: %s" % e)


def load_streak():
    """Load the saved sample count from RTC memory. Returns 0 if absent or invalid."""
    if not _HAS_RTC:
        return 0
    try:
        blob = RTC().memory()
    except Exception:
        return 0
    if not blob or len(blob) < _RTC_BLOB_SIZE or blob[:4] != RTC_MAGIC:
        return 0
    try:
        (count,) = struct.unpack(_RTC_FORMAT, blob[4:6])
        return count
    except Exception:
        return 0


def clear_streak():
    """Reset RTC streak to 0."""
    save_streak(0)


def was_deep_sleep_wake():
    """True iff this boot came from a deep-sleep wake (vs cold power-on or reset)."""
    if not _HAS_RTC:
        return False
    try:
        return reset_cause() == DEEPSLEEP_RESET
    except Exception:
        return False
