# 2009 Kia Spectra — voltage-triggered remote start + telematics

An in-car ESP32 + Raspberry Pi Zero 2 W unit that:

- Monitors battery voltage continuously at sub-mA average draw
- Triggers the factory-installed Compustar remote start when voltage drops below a configurable threshold
- Runs the engine for 15 minutes, then shuts it off
- Streams live OBD-II data (RPM, speed, coolant, fuel, voltage, etc.) over CAN
- Displays gauges + a map on a 5" touchscreen
- Exposes state to the home network via WiFi (web UI + MQTT)
- Optional Bluetooth proximity tricks via phone

The Compustar trigger is performed by software synthesis of the Keeloq rolling-code RF signal — the ESP32 plays the role of the FOB, allowing the in-car system to stay self-contained (FOB remains in the house).

## Hardware architecture

```
                       +12V (OBD-II Pin 16, always-hot)
                                  │
                       [1A inline fuse] + [TVS SMBJ24CA]
                                  │
                   [12V → 5V buck converter, 5A]
                                  │
              ┌───────────────────┼───────────────────┐
              │                   │                   │
            5V to             5V to                5V to
           ESP32           Pi (gated by         5" HDMI
        (always on)        AO3401A MOSFET       touchscreen
                           controlled by
                           ESP32 GPIO)

   ┌────────────────────────┐         ┌─────────────────────────┐
   │ ESP32-WROOM-32U        │ UART    │ Raspberry Pi Zero 2 W   │
   │ - ADC voltage monitor  │◄───────►│ - 5" HDMI touch display │
   │ - CC1101 RF transmit   │         │ - Web UI + maps         │
   │ - SN65HVD230 CAN ↔ OBD │         │ - MQTT to home network  │
   │ - WiFi + BT + deep slp │         │ - Logging to SD card    │
   └────────────────────────┘         └─────────────────────────┘
              ▲                                  ▲
              │ CAN-H, CAN-L, GND, +12V         │ HDMI, USB (touch)
              │                                  │
   OBD-II passive Y-splitter ── pigtail to case
```

## Repository layout

| Directory | Contents |
|---|---|
| `docs/` | BOM, wiring diagrams, architecture notes, Compustar research |
| `esp32/` | MicroPython firmware — voltage monitor, RF, CAN, comms |
| `pi/` | Raspberry Pi Python application — UI, logging, MQTT, web |
| `sdr/` | Tools and notes for SDR capture + Keeloq analysis |
| `logs/` | Day-by-day build journal |

## Build status

Pre-prototype. Parts on order. Follow [`logs/`](logs/) for day-by-day progress.

## Reproducing this build

This documentation describes a **single** unit. Parts list, wiring, and code are all single-unit. Build one and learn from it; scale to multiple if you have more vehicles.

Start here:

1. Read [`docs/BOM.md`](docs/BOM.md) — what to order
2. Read [`docs/architecture.md`](docs/architecture.md) — how the pieces fit together
3. Read [`docs/compustar-research.md`](docs/compustar-research.md) — what was learned about the installed remote start
4. Follow [`sdr/README.md`](sdr/README.md) — the SDR capture + Keeloq reverse-engineering workflow
5. Build out `esp32/` and `pi/` per their READMEs

## License

Personal project. Not licensed for redistribution yet — to be decided once it works.
