# 06 — Capture per-button packet patterns

## Goal

Take the `.bits` files from step 05 and harvest the four values the
ESP32 firmware actually needs:

1. **Remote ID** — 16 bits identical across every transmission from your FOB
2. **Per-button bit pattern** — 35 bits captured once per button (Start /
   Lock / Unlock / Trunk)
3. **Pulse-width parameters** — measured directly by `demod-compustar.py`
   (printed in the `.bits` file's header comments) and cross-checked
   against rtl_433's `-A` if you want extra confidence

These four button patterns + the pulse widths are everything `secrets.py`
needs. There is no rolling code, no encryption key, no counter, no serial
to extract separately from the patterns — for a Compustar 1WG3R-family
FOB, the patterns ARE the secret material and they don't change.

## Background — why this is so much shorter than the HCS-KeeLoq case

Earlier drafts of this walkthrough assumed the FOB used the standard
Microchip HCS300/301 layout — `[preamble][hopping code(32)][serial(28)][function(4)][status(2)]` — and described diff'ing
Start-vs-Start to isolate the 32 bits that change (the encrypted
hopping code) from the ones that don't.

The Compustar 1WG3R protocol family is different and simpler:

- 35 bits per packet between sync triplets (no separate hopping/serial
  fields you need to diff out — the whole 35-bit pattern is what you
  transmit)
- Same press = identical bits on the wire (fixed code, no rolling
  counter)
- First 16 bits are the Remote ID; the remaining 19 bits encode the
  button. You don't need to fully reverse the layout because we replay
  verbatim — but knowing the Remote ID extracts cleanly is useful for
  logs.

If you're working with an HCS-KeeLoq FOB instead (the diff-based path),
see [the appendix below](#appendix---hcs-keeloq-discovery-path).

## Prerequisites

- [05 — demodulation](05-demodulation.md) complete
- At least one `.bits` file per button (Start / Lock / Unlock / Trunk),
  produced by `demod-compustar.py`
- Multiple Start `.bits` files if you want to confirm fixed-code
  behavior (they should all be byte-identical)

## Step 1 — Confirm fixed-code behavior

Open three Start `.bits` files:

```powershell
type sdr\captures\fob-start-001-b1.bits
type sdr\captures\fob-start-002-b1.bits
type sdr\captures\fob-start-003-b1.bits
```

The bit lines (after the `#` comment header) should be **identical**.
If they aren't, either:

- One file contains a different button (mis-labeled capture)
- `demod-compustar.py` is mis-aligning the sync triplet on one of
  them (rare; re-run with `--verbose` and check the sync-point output)
- The FOB is genuinely transmitting differently per press, which would
  mean it's not a 1WG3R-family FOB — drop to the HCS-KeeLoq appendix.

For our project's 1WSHR-PRO, all 10 Start presses captured at `-g 20`
produced byte-identical 35-bit patterns. That's the fingerprint of a
fixed-code FOB.

## Step 2 — Read the Remote ID

The first 16 bits of any packet are the Remote ID (the FOB's
"serial number"). Run `demod-compustar.py --verbose` on any clean
capture:

```powershell
python sdr\scripts\demod-compustar.py sdr\captures\fob-start-001-b1.bin --verbose
```

It prints `ID=0x____` for each decoded packet. The Remote ID should be
identical across every packet from your FOB — across buttons, across
presses, across captures. If it isn't, something's wrong with the
demodulator's alignment.

Record the Remote ID as `COMPUSTAR_REMOTE_ID` in `framing.local.md`
(see step 4 below).

## Step 3 — Capture each button's 35-bit pattern

Read out the bit line from one `.bits` file per button:

```powershell
type sdr\captures\fob-start-001-b1.bits
type sdr\captures\fob-lock-001-b1.bits
type sdr\captures\fob-unlock-001-b1.bits
type sdr\captures\fob-trunk-001-b1.bits
```

Each one ends in a 35-character `0`/`1` string. Copy that string for
each button.

Quick sanity check — the first 16 characters (Remote ID) should be
identical across all four buttons. If they aren't, you mixed up captures.

The remaining 19 characters per packet encode the button. You don't
need to parse those further — `secrets.py` stores the whole 35-bit
pattern as a string and the ESP32 transmits it verbatim.

## Step 4 — Record findings

**Two files, two scopes:**

- `sdr/analysis/framing.md` (committed, public) — pulse widths and
  protocol structure (universal to 1WG3R-family FOBs), session-level
  observations.
- `sdr/analysis/framing.local.md` (**gitignored, never commit**) — your
  Remote ID, your per-button 35-bit patterns, any captured Lock /
  Unlock / Trunk patterns you don't want public.

If you don't have `framing.local.md` yet, copy the template:

```powershell
copy sdr\analysis\framing.local.md.example sdr\analysis\framing.local.md
```

Fill it in. Minimum content:

```markdown
### Remote ID: 0x____

### Per-button 35-bit data

```
Start  : <35-character string from your demod output>
Lock   : <35-character string from your demod output>
Unlock : <35-character string from your demod output>
Trunk  : <35-character string from your demod output>
```
```

(All four entries share the same first 16 bits — the Remote ID — and
differ only in the trailing 19 bits per button. Don't post your real
captured patterns publicly: combined with the Remote ID they're
sufficient material to operate your car remotely.)

Pulse-width measurements (from `demod-compustar.py` or rtl_433 `-A`)
can go in the committed `framing.md` if you want public reference
data — they're protocol-family parameters, not FOB-identifying.

## Step 5 — Populate `secrets.py.example` style config

Once `framing.local.md` has your values, you're ready to fill in
`esp32/src/secrets.py` (when bench-testing in step 12). The structure
is:

```python
COMPUSTAR_REMOTE_ID = 0x____            # from step 2 (16-bit)
COMPUSTAR_PACKETS = {
    "START":  "<35 chars from your demod output>",   # from step 3
    "LOCK":   "<35 chars from your demod output>",
    "UNLOCK": "<35 chars from your demod output>",
    "TRUNK":  "<35 chars from your demod output>",
}
```

Whitespace, `_`, and `-` inside the patterns are ignored, so you can
format them as e.g. `"0010 1101_11010 110000 1000 010011111101"` for
readability if you want.

## What you should have when done

- One known Remote ID
- Four 35-bit patterns (one per button), recorded in `framing.local.md`
- (Optional) Multiple Start captures confirming the patterns are stable
  across presses
- (Optional) rtl_433 `-A` cross-check confirming the pulse widths match
  what `demod-compustar.py` reported

## What you don't need (vs the original HCS-KeeLoq plan)

The original plan required ALL of these to replicate the FOB:

- 28-bit FOB serial — **don't need** (Remote ID is 16-bit and embedded
  in the pattern; no need to extract it separately)
- 4-bit function code per button — **don't need** (the whole 35-bit
  pattern encodes the button)
- 32-bit hopping code per press — **don't need** (no encryption)
- 16-bit rolling counter — **don't need** (no counter)
- 64-bit device key — **don't need** (no encryption key — see [`sdr/07`](07-key-recovery.md))

Skipping all of that is what makes the firmware ~150 LOC simpler than
the HCS-KeeLoq path would have been.

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| Different Remote ID across captures | Either your demod is misaligning sync, or you have captures from multiple FOBs mixed up. Re-run `demod-compustar.py --verbose` and check the sync points. |
| Two Start captures' 35-bit patterns differ | Either it's not really a 1WG3R-family FOB (drop to the HCS appendix), or sync alignment is off, or the captures are mislabeled. |
| 4 button patterns where the first 16 bits don't match | One of the captures is from a different FOB, or one capture's sync alignment is shifted. |
| Pulse widths in `framing.md` don't match `demod-compustar.py` output | Use `rtl_433.exe -A` as a tiebreaker; the rtl_433 measurements come from established baseband DSP and are the most trustworthy reference. |

## Appendix — HCS-KeeLoq discovery path

If your FOB is NOT a Compustar 1WG3R-family device (i.e., the bits
genuinely vary between consecutive presses of the same button), then
your FOB uses some HCS-KeeLoq variant and the diff-based discovery
applies:

1. Diff Start vs Start to find the ~32 bits that flip (= hopping code
   position).
2. Diff Start vs Lock to find the 4 bits outside the hopping code that
   flip (= function code position).
3. Everything else in the data payload is your 28-bit FOB serial + 2-bit
   status flags.
4. Update `compustar.Function` placeholder values with your measured
   function codes.
5. Proceed to [`07-key-recovery.md`](07-key-recovery.md) to recover the
   64-bit device key (KeeLoq cipher inversion via manufacturer-key
   database, PICkit EEPROM dump, or cryptanalytic attack).

The original helper script `sdr/scripts/diff-bits.py` supports this
workflow and is still in tree, even though it's not used by the
1WG3R-family path.

## Next

[`12-bench-validation.md`](../docs/12-bench-validation.md) — populate
`secrets.py` with the values from this step and bench-test the
synthesized transmission against your real car.

If you're on the HCS-KeeLoq path: [`07-key-recovery.md`](07-key-recovery.md)
first, then doc 12.
