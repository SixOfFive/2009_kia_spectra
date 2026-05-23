# Bill of Materials

Two complete units. Prices in CAD. Sourced from Amazon.ca where shipping speed matters, AliExpress where cost matters more than time.

## Per-unit hardware (×2)

| # | Part | CAD | Purpose |
|---|---|-----|---|
| 1 | Raspberry Pi Zero 2 W | $25 | UI brain, WiFi, map display |
| 2 | SanDisk 32GB microSD (U3/A1) | $12 | Pi OS + user storage |
| 3 | ESP32-WROOM-32U DevKit (external antenna) | $12 | Always-on monitor, RF, CAN |
| 4 | IPEX-to-SMA pigtail × 2 | $6 | ESP32 WiFi + 433 MHz external antennas |
| 5 | 433 MHz SMA whip antenna | $3 | Compustar RF transmit |
| 6 | 2.4 GHz SMA antenna | $4 | ESP32 WiFi reach to house |
| 7 | CC1101 433 MHz module | $5 | Sub-GHz transceiver for Keeloq |
| 8 | ADS1115 16-bit ADC breakout | $5 | Accurate battery voltage sample |
| 9 | SN65HVD230 CAN transceiver module | $3 | OBD-II CAN interface |
| 10 | 5" HDMI capacitive touchscreen (800×480) | $50 | Cabin display |
| 11 | mini-HDMI to HDMI flat cable, 50cm | $8 | Pi Zero video out |
| 12 | USB micro-to-A short cable | $3 | Display touch controller power/data |
| 13 | 12V→5V buck converter, 5A | $10 | Main power conversion |
| 14 | AO3401A P-MOSFET (logic-level) | $1 | Pi power gating (ESP32-controlled) |
| 15 | Voltage divider parts + TVS SMBJ24CA + 10µF tantalum | $7 | ADC scaling + load-dump protection |
| 16 | 1A blade fuse + inline fuse holder | $3 | Device-side overcurrent protection |
| 17 | OBD-II passive Y-splitter cable (dual-female) | $20 | Pass-through so scan tools still work |
| 18 | OBD-II to bare-wire pigtail | $8 | Case-side connector with bare leads |
| 19 | Hammond 1591ESBK ABS case, 150×80×50mm | $18 | Project enclosure |
| 20 | Solderless breadboard, full-size 830-tie | $5 | Prototyping phase |
| 21 | Perfboard 70×90mm × 2 | $5 | Permanent assembly |
| 22 | Rubber grommet | $2 | Cable pass-through in case |
| 23 | 3M VHB tape strip | $3 | Display dash mount |

**Per unit: $218 CAD**
**Two units: $436 CAD**

## Shared consumables (one purchase covers both builds)

| Item | CAD |
|---|---|
| Dupont jumper wire assortment (M-M, M-F, F-F, 65 pcs each) | $12 |
| Female header sockets (assortment) | $6 |
| Male header pin strips (assortment) | $4 |
| Silicone hookup wire kit, 22/26 AWG, 6 colors | $12 |
| Heat-shrink tubing assortment | $8 |
| **Subtotal** | **$42** |

## Tools (already owned, listed for reference)

| Tool | Status |
|---|---|
| Variable-temp soldering iron + 60/40 solder | ✓ |
| Drill | ✓ |
| Multimeter | ✓ |
| RTL-SDR (Vomeko 100kHz–1.7GHz) | ✓ |
| Precision wire strippers (22-28 AWG) | TBD |
| Liquid flux pen | TBD |
| Step drill bit (4-30mm) | TBD |
| Coping saw + fine blades (case window cut) | TBD |
| Small precision needle-nose pliers | TBD |

## Total project cost

| | CAD |
|---|---|
| Two units of hardware | $436 |
| Shared consumables | $42 |
| **Total to build both** | **$478** |

## Deferred — buy only if needed

- **PICkit-style programmer** ($30) — only if SDR-only Keeloq extraction fails and we need to read the FOB chip's EEPROM directly
- **Logic analyzer** (Saleae clone, $15) — useful for SPI/CAN debugging if integration goes sideways
- **Cellular modem module + SIM** ($50 + monthly) — if you ever want remote alerts when car is parked away from home WiFi
