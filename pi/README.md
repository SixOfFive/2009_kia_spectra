# Raspberry Pi application

Python 3 application running on Raspberry Pi OS Lite. Handles the 5" touch display, web UI, MQTT integration, persistent logging, and clean shutdown coordination.

## Status

Not started. Awaiting parts.

## Planned components

- `app/`
  - `main.py` — application entry point
  - `display/` — Flask-based dashboard rendered fullscreen in chromium kiosk mode
    - `templates/dashboard.html` — gauges + map + manual controls
    - `static/` — JS, CSS, cached map tiles
  - `comms/`
    - `esp32_link.py` — UART client to talk to ESP32
    - `mqtt_publisher.py` — Push state to home MQTT broker
  - `logger.py` — Append-only CSV/JSON log to SD card
  - `shutdown_listener.py` — systemd-compatible service that listens for ESP32's SHUTDOWN command
- `systemd/`
  - `vroom-display.service` — Auto-start the dashboard on boot
  - `vroom-shutdown.service` — Listen for clean-shutdown signal from ESP32
- `setup/`
  - `provision.sh` — One-shot setup script to install dependencies, configure chromium kiosk, set up systemd services

## Development environment

- Develop on a host machine (Linux/WSL/Mac) with Python 3.11+
- Deploy to Pi via `rsync` over SSH once it's networked
- Headless Pi setup: enable SSH and WiFi via `wpa_supplicant.conf` on the SD card boot partition before first power-on

## Setting up a fresh Pi

1. Download Raspberry Pi Imager
2. Flash Raspberry Pi OS Lite (64-bit) to microSD
3. Pre-configure WiFi + SSH via Imager's advanced options (Ctrl+Shift+X)
4. First boot: takes ~2 minutes, then SSH-able
5. Run `setup/provision.sh`

## Notes

To be expanded as code is written.
