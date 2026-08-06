# Bill of Materials

Single unit. Prices in CAD as of 2026-05. Sourced from Amazon.ca where
shipping speed matters, AliExpress where cost matters more than time.

The BOM is **split into v1 (essential — the shipped ESP32-only build)
and v2 (optional — Pi-side telematics, deferred)**. v1 alone is a
complete voltage-triggered remote-start unit; v2 adds a Pi, a
touchscreen, and the LAN observability layer described in the
[`README.md`](../README.md). Scope decision documented 2026-06-01.

## v1 — Essential (the ESP32-only build)

| # | Part | CAD | Purpose |
|---|---|-----|---|
| 1 | ESP32-WROOM-32U DevKit (external antenna connector) | $12 | The whole brain. Voltage monitor + RF transmitter — **see AliExpress warning below** |
| 2 | CC1101 433 MHz module | $5 | Sub-GHz transceiver, plays Compustar 1WG3R packets |
| 3 | ADS1115 16-bit ADC breakout | $5 | Accurate battery voltage sample (ESP32 onboard ADC is too noisy for cutoff decisions) |
| 4 | 433 MHz SMA whip antenna | $3 | Compustar RF transmit (~17 cm quarter-wave) |
| 5 | IPEX-to-SMA pigtail × 1 | $3 | Routes CC1101 RF out to the case wall (only if your CC1101 board has a U.FL connector; SMA-on-board variants skip this) |
| 6 | 12V→5V buck converter, **1A** (LM2596 module or similar) | $5 | ESP32 alone needs maybe 100 mA peak; 1A is comfortable. (v2 needs 5A — see below) |
| 7 | Voltage divider parts (**1 MΩ + 220 kΩ** + 100 nF cap) + TVS SMBJ24CA + 10 µF tantalum | $7 | ADC scaling + load-dump protection (cars can hit 80 V spikes) |
| 8 | 2A ATM mini blade fuse + inline fuse holder | $3 | Device-side overcurrent protection. (1A would work for v1 alone but 2A leaves headroom if you ever bolt v2 on top) |
| 9 | OBD-II passive Y-splitter cable (dual-female) | $20 | Pass-through so scan tools still work alongside the install |
| 10 | OBD-II to bare-wire pigtail | $8 | Case-side connector — taps Pin 16 (+12V) and Pin 4/5 (GND) |
| 11 | **Hammond 1591BSBK** ABS case, 100×50×25mm | $14 | Project enclosure (smaller than the v2 1591ESBK — v1 has less to fit) |
| 12 | Solderless breadboard, full-size 830-tie | $5 | Prototyping phase |
| 13 | Perfboard 70×90mm | $3 | Permanent assembly |
| 14 | Rubber grommet | $2 | OBD pigtail pass-through in case |

**v1 hardware subtotal: ~$95 CAD**

> **Divider values (row 7).** Earlier revisions of this table listed 30 kΩ + 10 kΩ.
> That is wrong for a 12 V system on this ADC: it divides by 4, so 14.6 V of
> alternator charging lands at 3.65 V on the pin — past the ~3.1 V top of the
> ESP32-S3's 12 dB input range and uncomfortably near the 3.3 V rail. The build
> uses **1 MΩ (high side) + 220 kΩ (low side)**, a ÷5.545 divider that puts
> 14.6 V at 2.63 V with ~2.6 V of battery headroom to spare, and reads usefully
> to about 17 V. That 5.545 is the `DIVIDER` constant in the firmware. The 1 MΩ
> doubles as the GPIO's overvoltage protection (it limits fault current into the
> pin clamp to microamps), and the 100 nF is what makes the ~180 kΩ source
> impedance acceptable to the sampling ADC — keep it physically at the pin.

That's a complete voltage-triggered remote start in one box. Drops
into the OBD-II Y-splitter behind the dash, sleeps until the battery
sags, fires the Compustar packet, runs 15 minutes, sleeps again.

### v1 firmware-side: does the ESP32 even need WiFi?

For a pure-trigger v1 deployment: **no**. The firmware doesn't
initialize Wi-Fi or Bluetooth on the ESP32; the radio stays off
between samples for power. You could substitute the cheaper
**ESP32-WROOM-32** (with the printed PCB antenna and **no** IPEX
connector — ~$8) for line 1 and skip the 2.4 GHz antenna in v2. The
BOM here keeps the `-U` variant only because future-proofing for v2
is cheap.

## v2 — Optional telematics (deferred Pi-side parts)

Add these on top of v1 if you want the Pi-side dashboard / MQTT / map /
SNMP layer described in the README. **None of these are required for
the trigger function.**

| # | Part | CAD | Purpose |
|---|---|-----|---|
| v2.1 | Raspberry Pi Zero 2 W | $25 | UI brain, Wi-Fi, MQTT, map display |
| v2.2 | SanDisk 32GB microSD (U3/A1) | $12 | Pi OS + user storage |
| v2.3 | 5" HDMI capacitive touchscreen (800×480) | $50 | Cabin display |
| v2.4 | mini-HDMI to HDMI flat cable, 50 cm | $8 | Pi Zero video out |
| v2.5 | USB micro-to-A short cable | $3 | Display touch controller power/data |
| v2.6 | SN65HVD230 CAN transceiver module | $3 | OBD-II CAN interface (live RPM, speed, coolant, etc.) |
| v2.7 | IPEX-to-SMA pigtail × 1 (extra) | $3 | ESP32 WiFi antenna routing |
| v2.8 | 2.4 GHz SMA antenna | $4 | ESP32 WiFi reach to house |
| v2.9 | AO3401A P-MOSFET (logic-level) | $1 | Pi power gating — ESP32 turns the Pi on only when needed |
| v2.10 | **Upgrade the buck**: replace v1's 1A with a **5A** (LM2596 5A or Pololu D36V28F5) | +$5 | Pi + touchscreen pull ~1.1 A peak together |
| v2.11 | **Upgrade the enclosure**: 1591BSBK → 1591ESBK (150×80×50mm) | +$4 | More room for the Pi + display ribbon |
| v2.12 | 3M VHB tape strip | $3 | Dash-mount the display |

**v2 incremental: ~$121 CAD** (on top of v1's $95 → ~$216 CAD for the
full system)

## ⚠ Buyer warning — ESP32-WROOM-32U on AliExpress

Several AliExpress listings for "ESP32-WROOM-32U DevKitC" advertise a
**headline price of ~$3 CAD** that looks like the actual development
board. It isn't. The cheapest variant on most of these listings is the
**antenna only** — a small bundled 2.4 GHz whip, not the ESP32 board.
The actual board variant is buried in the variant selector at a much
higher price (often $8-12).

**How to avoid the trap:**
- Read the variant dropdown carefully before adding to cart. Look for
  variants like "Just Antenna" or "Antenna Only" — these are the bait.
- The actual board variant will usually mention "DevKitC", "Board", or
  include a count like "1pc Board"
- If the headline price is dramatically below the going rate (genuine
  ESP32-WROOM-32U boards generally run $5-10 even in bulk), check the
  default variant assumption
- Hold the cart at the variant selector page until you have visually
  confirmed you're buying the board, not the bundled accessory

This bait pattern is common on several "DevKit" SKUs across
AliExpress. The pricing in this BOM ($12 CAD) assumes a legitimate
listing of the actual board.

## Consumables

These you'll use in any electronics build — buy once, save the rest
for future projects.

| Item | CAD |
|---|---|
| Dupont jumper wire assortment (M-M, M-F, F-F, 65 pcs each) | $12 |
| Female header sockets (assortment) | $6 |
| Male header pin strips (assortment) | $4 |
| Silicone hookup wire kit, 22/26 AWG, 6 colors | $12 |
| Heat-shrink tubing assortment | $8 |
| **Subtotal** | **$42** |

## Tools

You probably have some of these. Listed here for completeness.

| Tool | Approx. CAD if buying new |
|---|---|
| Variable-temp soldering iron (Pinecil V2 or TS100) | $40 |
| 60/40 rosin-core solder, 0.6mm | $10 |
| Multimeter | $20 |
| Drill | varies |
| RTL-SDR dongle (any R820T2 + RTL2832U works) | $30 |
| Precision wire strippers (22-28 AWG) | $15 |
| Liquid flux pen | $5 |
| Step drill bit (4-30mm) | $10 |
| Coping saw + fine blades (case window cut — v2 only) | $15 |
| Small precision needle-nose pliers | $10 |

## Total project cost

- **v1 (ESP32-only):** ~$95 hardware + ~$42 consumables = **~$137 CAD**
- **v1 + v2 (full system):** ~$216 hardware + ~$42 consumables = **~$258 CAD**

Add $155 if buying every tool above from scratch.

## Deferred — buy only if needed

- **PICkit-style programmer** ($30) — only if SDR-only Keeloq extraction
  fails on a *different* Compustar FOB and you need to read the chip's
  EEPROM directly. Not needed for the 1WSHR-PRO this project targets
  (fixed-code, no key recovery).
- **Logic analyzer** (Saleae clone, $15) — useful for SPI/CAN debugging
  if integration goes sideways.
- **Cellular modem module + SIM** ($50 + monthly) — only if you want
  remote alerts when car is parked away from home Wi-Fi. v2-extension
  territory.
