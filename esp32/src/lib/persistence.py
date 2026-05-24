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

# Sanity ceiling for the loaded streak count. The legitimate maximum is
# bounded by LOW_V_SUSTAIN_S / WAKE_INTERVAL_S which in any realistic
# config sits in the single-digits-to-low-hundreds. If we read a value
# above this, treat it as RTC corruption (e.g. partial magic match with
# garbage data after a brown-out, all-0xFF flash, etc.) and reset to 0
# rather than trigger the engine immediately on next sample.
_MAX_REASONABLE_STREAK = 10000


try:
    from machine import RTC, reset_cause, DEEPSLEEP_RESET
    _HAS_RTC = True
except ImportError:
    _HAS_RTC = False
    DEEPSLEEP_RESET = 4      # ESP32 constant; useful for CPython mock comparison
    reset_cause = None       # noqa — signals CPython fallback to reset_cause_string


# MicroPython machine.* reset_cause() return codes. The exact ints aren't part
# of MicroPython's public API but they've been stable on ESP32 for years.
# Mapping captured here so the controller can emit a human-readable label
# at boot — useful post-mortem ("did the WDT actually fire?").
_RESET_CAUSE_NAMES = {
    0: "PWRON_RESET",
    1: "HARD_RESET",
    2: "WDT_RESET",
    3: "DEEPSLEEP_RESET",  # historical alias on some ports
    4: "DEEPSLEEP_RESET",
    5: "SOFT_RESET",
    6: "BROWNOUT_RESET",
}


def reset_cause_string():
    """Return a stable human-readable name for the most recent reset cause.

    Returns "cpython" on the CPython host (no machine.reset_cause available)
    and "unknown(<n>)" if MicroPython returns a code we don't have a label for.
    Never raises.
    """
    if reset_cause is None:
        return "cpython"
    try:
        code = reset_cause()
    except Exception:
        return "cpython"
    name = _RESET_CAUSE_NAMES.get(code)
    if name is not None:
        return name
    return "unknown(%d)" % code


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
    except Exception:
        return 0
    # Defend against partial-magic-match-with-garbage scenarios (low-voltage
    # glitch, all-0xFF flash, etc.). 0xFFFF would otherwise look like a huge
    # streak and trigger the engine on the very next sample.
    if count > _MAX_REASONABLE_STREAK:
        return 0
    return count


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
