# 06 — Extract framing: serial, function codes, hopping code structure

## Goal

Take the `.bits` files from step 05 and identify:

1. **FOB serial** — 28 bits that are identical across every transmission from your FOB
2. **Function codes** — 4 bits that differ between Start, Lock, Unlock, Trunk
3. **Hopping code position** — 32 bits that change between every press (because the counter incremented and got re-encrypted)
4. **Status bits** — 2 bits (V_LOW and repeat flag)
5. **TE (bit element time)** — measured from URH

These are the values you'll plug into `secrets.py` and `config.py` so the ESP32 can synthesize valid packets matching your FOB.

## Prerequisites

- [05 — URH analysis](05-urh-analysis.md) complete
- At least 3 Start captures and 1 each of Lock / Unlock decoded to `.bits` files

## Step 1 — Diff Start vs Start to find hopping bits

Two captures of the same button (Start) should differ **only** in:

- The hopping code (32 bits, encrypted with the rolling counter)
- The repeat flag (1 bit, usually 0 on first packet of burst, 1 on retransmits)
- The V_LOW flag (1 bit, usually 0 unless battery is low)

Run the helper script (created in this step — see below):

```powershell
python sdr/scripts/diff-bits.py sdr/analysis/fob-start-001.bits sdr/analysis/fob-start-002.bits
```

Expected output: ~32-34 bits flip. If you see 60+ bits flipping, something's wrong with your demodulation in step 05.

The 32 contiguous bits that flip = your **hopping code position** in the packet.

## Step 2 — Diff Start vs Lock to find function code

Two captures of *different* buttons differ in:

- The hopping code (because the counter still incremented)
- The 4-bit function code (the entire reason different buttons exist)
- Possibly the discrimination bits inside the encrypted hopping plaintext

```powershell
python sdr/scripts/diff-bits.py sdr/analysis/fob-start-001.bits sdr/analysis/fob-lock-001.bits
```

Expected: ~36 bits flip (the 32 hopping bits, plus 4 in the function code position).

The 4 flipped bits **outside** the hopping code = your **function code position**.

## Step 3 — Extract the FOB serial

After steps 1-2, you know where the 32-bit hopping code lives and where the 4-bit function code lives. Everything else in the data payload is either:

- 28 bits of FOB serial (constant for your FOB across all transmissions)
- 2 bits of status flags (V_LOW + repeat)

The serial is the 28 bits adjacent to the function code (the HCS layout puts them together as a 32-bit "fixed code" field: 28-bit serial in the high bits + 4-bit function in the low bits).

To read your serial: take any one of your `.bits` files, locate the fixed-code section, and read out 28 bits as an integer.

If your fixed-code section contains the bits `0000 1010 1011 1100 1101 1110 1111 0100` (just as an example), then:

- Serial (top 28 bits): `0000 1010 1011 1100 1101 1110 1111` = `0x0ABCDEF`
- Function code (bottom 4 bits): `0100` = `0x4` (this would be Start)

## Step 4 — Record findings

Update `sdr/analysis/framing.md`:

```markdown
## Framing extracted on YYYY-MM-DD

### Packet layout (66 bits, MSB first as transmitted)

| Field          | Bits  | Position  | Notes                             |
|----------------|-------|-----------|-----------------------------------|
| Hopping code   | 32    | 0..31     | Encrypted, changes every press    |
| FOB serial     | 28    | 32..59    | Constant for this FOB             |
| Function code  | 4     | 60..63    | Identifies which button           |
| V_LOW          | 1     | 64        | Low-battery indicator             |
| Repeat         | 1     | 65        | 0 = first packet, 1 = repeat      |

### Timing

- TE (bit element time): _____ µs (measured from URH bit length / sample rate)
- Preamble: _____ half-bit cycles
- Header gap: _____ TE periods of silence
- Inter-packet guard: _____ ms

### FOB-specific values

- **Serial**: 0x_______ (28 bits)
- **Function codes** (compare button by button):
  - Start: 0x_
  - Lock: 0x_
  - Unlock: 0x_
  - Trunk: 0x_ (if applicable)

### Hopping code observations

- Across 10 Start presses, the 32-bit hopping value changed all 32 bits each time
  (as expected — encryption with an incrementing counter scrambles the whole block)
- The counter increment between consecutive presses (when we decrypt with the
  recovered key in step 07) should be +1 per press
```

## Step 5 — Update the code constants

Once you have function codes confirmed, update `esp32/src/lib/compustar.py`:

```python
class Function:
    START = 0x_   # (your measured value)
    LOCK = 0x_
    UNLOCK = 0x_
    TRUNK = 0x_
```

And update TE timing in `esp32/src/config.py`:

```python
RF_TE_US = _____   # measured µs per bit
PREAMBLE_HALF_BITS = _____
HEADER_GAP_TE = _____
```

Commit these updates with a message like `Update Compustar function codes from SDR captures`.

## Helper script: diff-bits.py

If `sdr/scripts/diff-bits.py` doesn't exist yet, create it:

```python
"""Compare two .bits files and report positions where they differ."""
import sys

def load_bits(path):
    bits = []
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or line.startswith("["):
                continue
            for ch in line:
                if ch in "01":
                    bits.append(int(ch))
    return bits

a = load_bits(sys.argv[1])
b = load_bits(sys.argv[2])
assert len(a) == len(b), f"length mismatch: {len(a)} vs {len(b)}"

diffs = [i for i in range(len(a)) if a[i] != b[i]]
print(f"{len(diffs)} bits differ out of {len(a)}")
print(f"Positions: {diffs}")
```

Save to `sdr/scripts/diff-bits.py` and `chmod +x` if you want.

## What you should have when done

- Locked-in packet layout in `sdr/analysis/framing.md`
- Known FOB serial number
- Known function codes for each button
- Measured TE timing
- Updated `esp32/src/lib/compustar.py` and `esp32/src/config.py` with real values

## Where artifacts go

- `sdr/analysis/framing.md` — committed, authoritative findings
- `sdr/scripts/diff-bits.py` — committed, helper script

## Troubleshooting

| Symptom | Fix |
|---|---|
| 60+ bits differ between two Start captures | Demodulation is wrong — re-do step 05 with corrected bit length |
| 0 bits differ between two Start captures | FOB counter isn't incrementing (battery dying? hardware issue?) OR you accidentally saved the same file twice |
| Function code is in a different position than expected | Layout assumption is wrong for your specific HCS variant — manually inspect bit-by-bit instead of using the helper script |

## Next

[`07-key-recovery.md`](07-key-recovery.md) — now the hardest part: recovering the 64-bit KeeLoq device key that turns the counter into the hopping code we see on the wire. This is what lets the ESP32 generate new valid packets.
