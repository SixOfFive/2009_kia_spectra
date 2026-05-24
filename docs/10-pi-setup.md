# 10 — Set up the Raspberry Pi Zero 2 W

## Goal

Boot the Pi from a fresh SD card, run `provision.sh`, see the dashboard on
the 5" touchscreen, confirm it's reachable from any device on your LAN.

## Prerequisites

- Pi Zero 2 W + SanDisk Ultra 32GB SD card from the BOM
- Elecrow 5" HDMI capacitive touchscreen (800x480, integrated USB-A)
- Mini-HDMI to HDMI cable (0.5m)
- Micro-USB power supply (5V 2A) for first-boot setup on a desk
- Another computer with an SD card reader and the Raspberry Pi Imager

## Step 1 — Flash Raspberry Pi OS to the SD card

1. Install the [Raspberry Pi Imager](https://www.raspberrypi.com/software/)
   on your dev machine.
2. Insert the SD card.
3. In the Imager:
   - **Choose Device**: Raspberry Pi Zero 2 W
   - **Choose OS**: **Raspberry Pi OS Lite (64-bit)** — we don't need the
     desktop, chromium kiosk is the only UI
   - **Choose Storage**: your SD card
   - Click the gear icon ⚙ for advanced options:
     - Set hostname: `vroom-spectra`
     - Enable SSH with password auth (set a real password, not the
       default `raspberry`)
     - Set username: `pi`
     - Configure WiFi: your home SSID + password (so it joins on boot)
     - Set locale: your timezone, keyboard layout
4. Click **Write**. Eject when done.

## Step 2 — First boot on a desk (not in the car)

1. Insert the SD card into the Pi
2. Plug in the HDMI cable (mini-HDMI side into the Pi) and connect to
   any HDMI monitor temporarily
3. Power the Pi via micro-USB (5V 2A supply)
4. Wait ~30 seconds for first-boot expansion + resize
5. Pi should boot to a login prompt with your hostname visible

## Step 3 — SSH in from your dev machine

From your dev machine:

```powershell
ssh pi@vroom-spectra.local
```

Enter the password you set during imaging. If `.local` mDNS doesn't
resolve, find the Pi's IP from your router and `ssh pi@<ip>`.

## Step 4 — Pull the repo + run provision.sh

```bash
git clone https://github.com/SixOfFive/2009_kia_spectra.git /tmp/vroom-clone
sudo bash /tmp/vroom-clone/pi/setup/provision.sh
```

What this does (idempotent — safe to re-run):
- Installs apt packages: flask, pyserial, chromium, unclutter, etc.
- Installs paho-mqtt via pip
- Enables UART hardware (disables serial console login)
- Creates `/var/lib/vroom` and `/var/log/vroom`
- Clones the project to `/opt/vroom` (or pulls latest if already there)
- Writes `/etc/sudoers.d/vroom` so the daemon can `shutdown -h` without
  password
- Installs and enables `vroom.service` systemd unit
- Configures chromium kiosk autostart pointing at `http://localhost:8000`

Watch the output for any errors. Common ones:

| Symptom | Fix |
|---|---|
| `apt-get` fails | `sudo apt update` first; check internet |
| `git clone` fails | DNS issue; check `cat /etc/resolv.conf` |
| `raspi-config` fails | Already-set serial settings; safe to ignore |

## Step 5 — Configure secrets

```bash
sudo -u pi cp /opt/vroom/pi/app/secrets.py.example /opt/vroom/pi/app/secrets.py
sudoedit /opt/vroom/pi/app/secrets.py
```

Fill in:
- WiFi creds (only used if you want the Pi to also re-configure its WiFi
  — usually not, since you already set them during imaging)
- MQTT broker / username / password / topic prefix (or leave `MQTT_BROKER
  = None` to disable MQTT)
- COMPUSTAR_DEVICE_KEY / COMPUSTAR_SERIAL / COMPUSTAR_COUNTER — these
  come from the SDR walkthrough (step 07). For initial dashboard testing
  before the SDR phase is complete, set placeholder values; the actual
  start path will refuse to transmit until real values are in place.

## Step 6 — Start the daemon

```bash
sudo systemctl start vroom.service
sudo systemctl status vroom.service
```

Expected status: `active (running)`. If it fails to start:

```bash
journalctl -u vroom.service -e
```

Common issues:
- `ModuleNotFoundError: No module named 'flask'` — `provision.sh` didn't
  finish; re-run it
- Permission denied on `/dev/serial0` — `pi` user not in `dialout` group:
  `sudo usermod -aG dialout pi && reboot`

### MQTT topics published / subscribed

If `MQTT_BROKER` is set in secrets, the daemon also:

- Publishes `<MQTT_TOPIC_PREFIX>/state` every `MQTT_PUBLISH_INTERVAL_S` (retained, full snapshot JSON)
- Subscribes to `<MQTT_TOPIC_PREFIX>/cmd` and forwards allowed commands to the ESP32

Example Home Assistant automation to trigger a start from anywhere:

```yaml
service: mqtt.publish
data:
  topic: vroom/spectra/cmd
  payload: '{"cmd": "start_engine"}'
```

Allowed `cmd` values: `start_engine`, `stop_engine`, `ping`, `set_threshold` (with `value`). Other commands are silently dropped (whitelist defense).

## Step 7 — Open the dashboard

In a browser on your dev machine:

```
http://vroom-spectra.local:8000
```

You should see the dark dashboard with 6 gauges. Initial values are
mocked (12.6V battery, 0 RPM, etc.) until the UART listener receives
real STATUS messages from the ESP32.

The Start / Stop / Ping buttons return 503 ("esp32 link not initialized")
until the UART link comes up — that's expected at this point. We wire
the ESP32 in step 11.

## Step 8 — Attach the touchscreen + test chromium kiosk

1. Power off the Pi: `sudo shutdown -h now`
2. Unplug the mini-HDMI from the test monitor; plug into the Elecrow
   5" touchscreen
3. Plug the Elecrow's integrated USB-A connector into the Pi's USB-A
   port (some Pi Zero 2 W adapters needed if you're using the
   micro-USB OTG port — for the in-cab install we'll likely use a USB
   hub since the Pi Zero only has one data USB port)
4. Power up
5. Pi should boot to chromium kiosk after ~25-30 seconds, displaying
   the dashboard fullscreen at 800x480

Touch the dashboard buttons to confirm touch input works (clicks should
produce visible button-press visual feedback even though the commands
still return 503).

## What you should have when done

- Pi flashed with Raspberry Pi OS Lite 64-bit
- `vroom.service` running on boot, auto-restart on failure
- Dashboard reachable at `http://vroom-spectra.local:8000` from your LAN
- Chromium kiosk renders the dashboard fullscreen at 800x480 on the
  Elecrow display on boot
- Touch input works
- Awareness that buttons return 503 until UART is wired in step 11

## Where artifacts go

- Pi's filesystem at `/opt/vroom` (committed code, kept in sync via
  `git pull` whenever you push from your dev machine)
- Pi's secrets at `/opt/vroom/pi/app/secrets.py` (NOT committed — local
  to that Pi)

## Next

[`11-uart-link.md`](11-uart-link.md) — physically wire the ESP32 UART to
the Pi UART, validate the JSON line protocol round-trips.
