# 01 — Install SDR software

## Goal

Get the rtl-sdr command-line tools and Universal Radio Hacker (URH) running on your machine so you can capture and analyze sub-GHz signals.

## Prerequisites

- An RTL-SDR dongle (don't plug it in yet — Windows needs a special driver installed first)
- Python 3.9+ already installed
- Admin / sudo rights on your machine

## Windows 10 / 11

### Install the RTL-SDR USB driver via Zadig

Windows ships with a default USB driver that **does not** let userspace tools control the dongle. You need to replace it with WinUSB using Zadig.

1. Plug the RTL-SDR dongle into a USB port. Wait for Windows to install its (useless) default driver.
2. Download Zadig from [https://zadig.akeo.ie/](https://zadig.akeo.ie/) — single ~5MB exe, no installer.
3. Run Zadig as administrator.
4. **Options → List All Devices** (check the box).
5. In the dropdown, select **"Bulk-In, Interface (Interface 0)"** — that's the RTL2832U. (If you see two RTL entries, pick Interface 0, not Interface 1.)
6. In the target driver field, select **WinUSB** (should be the default).
7. Click **Replace Driver**. Wait ~30 seconds.
8. Unplug and replug the dongle. Windows shouldn't reinstall its own driver this time.

### Install rtl-sdr command-line tools

```powershell
# Using the prebuilt Windows binaries from the original osmocom build
# Download from:
#   https://github.com/osmocom/rtl-sdr/releases  (release ZIPs include the binaries)
# Extract to e.g. C:\rtl-sdr\ and add C:\rtl-sdr\x64\ to your PATH
```

Verify with PowerShell:

```powershell
rtl_test -t
```

Expected output: model info + tuner type + supported sample rates. If you get "No supported devices found", redo the Zadig step.

### Install Universal Radio Hacker

```powershell
pip install urh
```

Launch it once to make sure it works:

```powershell
urh
```

A GUI window should open. Close it; we'll come back to it in step 05.

## Linux (Debian / Ubuntu / Raspberry Pi OS)

```bash
sudo apt update
sudo apt install rtl-sdr librtlsdr-dev python3-pip
pip3 install --user urh

# Blacklist the kernel's default DVB driver — otherwise the dongle gets
# claimed at boot and rtl-sdr tools can't open it.
sudo tee /etc/modprobe.d/blacklist-rtl.conf > /dev/null <<EOF
blacklist dvb_usb_rtl28xxu
blacklist rtl2832
blacklist rtl2830
EOF
sudo modprobe -r dvb_usb_rtl28xxu rtl2832 rtl2830 2>/dev/null
```

Plug the dongle in (or unplug + replug if already in) and verify:

```bash
rtl_test -t
```

## macOS

```bash
brew install librtlsdr
pip3 install --user urh
```

`brew` handles the driver situation transparently — no equivalent of Zadig needed.

## What you should have when done

- `rtl_test`, `rtl_sdr`, `rtl_power`, `rtl_fm` commands available in your shell
- `urh` command available, opens a GUI window
- Dongle blinks an LED (if it has one) when plugged in

## Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `rtl_test`: "No supported devices found" | Windows: redo Zadig. Linux: blacklist DVB drivers + replug. |
| `rtl_test`: "usb_claim_interface error -6" | Another process owns the dongle. Close GQRX/URH/SDR# and retry. |
| URH won't install with pip | Try `pip install urh --no-build-isolation`. On Linux you may need `sudo apt install python3-pyqt5 python3-numpy python3-psutil` first. |
| Dongle randomly disconnects after 30s | Cheap USB extension cable causing voltage droop. Plug direct or use a quality powered hub. |

## Where artifacts go

Nothing to save in this step — it's environment setup only.

## Next

[`02-hardware-verification.md`](02-hardware-verification.md) — confirm the dongle actually receives a known signal before we go hunting for the FOB.
