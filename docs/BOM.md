# Bill of Materials

Single unit. Prices in CAD as of 2026-05. Sourced from Amazon.ca where shipping speed matters, AliExpress where cost matters more than time.

## Hardware

| # | Part | CAD | Purpose |
|---|---|-----|---|
| 1 | Raspberry Pi Zero 2 W | $25 | UI brain, WiFi, map display |
| 2 | SanDisk 32GB microSD (U3/A1) | $12 | Pi OS + user storage |
| 3 | ESP32-WROOM-32U DevKit (external antenna connector) | $12 | Always-on monitor, RF, CAN |
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

**Hardware subtotal: $218**

## Consumables

These you'll use in any electronics build — buy once, save the rest for future projects.

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
| Coping saw + fine blades (case window cut) | $15 |
| Small precision needle-nose pliers | $10 |

## Total project cost

**$260 CAD** for one complete build (hardware + consumables), assuming you already own the tools. Add $155 if buying every tool above from scratch.

## Deferred — buy only if needed

- **PICkit-style programmer** ($30) — only if SDR-only Keeloq extraction fails and we need to read the FOB chip's EEPROM directly
- **Logic analyzer** (Saleae clone, $15) — useful for SPI/CAN debugging if integration goes sideways
- **Cellular modem module + SIM** ($50 + monthly) — only if you want remote alerts when car is parked away from home WiFi
