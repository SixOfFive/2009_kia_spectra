# 2009 Kia Spectra — voltage-triggered remote start + telematics

A pair of in-car ESP32 + Raspberry Pi Zero 2 W units that:

- Monitor battery voltage continuously at sub-mA average draw
- Trigger the factory-installed Compustar remote start when voltage drops below a configurable threshold
- Run the engine for 15 minutes, then shut it off
- Stream live OBD-II data (RPM, speed, coolant, fuel, voltage, etc.) over CAN
- Display gauges + a map on a 5" touchscreen
- Expose state to the home network via WiFi (web UI + MQTT)
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

## Build status

Pre-prototype. Parts on order.

## License

Personal project. Not licensed for redistribution yet — to be decided once it works.
