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

Pre-built Windows binaries (NOT on the osmocom GitHub mirror — that
repo has no Releases. Use one of these instead):

- **Official osmocom builds** (fresh, weekly updates):
  https://ftp.osmocom.org/binaries/windows/rtl-sdr/

  Download `rtl-sdr-64bit-YYYYMMDD.zip` (most recent date). The 32-bit
  variant is also there if you're on a 32-bit Windows install (rare).

- **RTL-SDR Blog fork releases on GitHub** (signed Windows release,
  includes Zadig, slightly older but well-tested):
  https://github.com/rtlsdrblog/rtl-sdr-blog/releases

  Pick the latest `Release.zip`. Works fine with generic R820T2 dongles
  (Vomeko, NESDR, etc.) — not just the Blog V4 hardware.

Either way: extract to e.g. `C:\rtl-sdr\` and add the bin path (or `x64\`
subfolder, depending on which fork) to your PATH.

Verify with PowerShell:

```powershell
rtl_test
```

(Don't use `-t` — that's an E4000-tuner-specific test and always
errors out with `No E4000 tuner found, aborting.` on R820T2 / R828D
which is what's actually inside these dongles.)

Expected output: model info + tuner type + supported sample rates, then it
will start continuously sampling and print throughput stats. Press Ctrl-C
to stop.

If you get "No supported devices found", redo the Zadig step.

If tuner detects but you get a flood of `rtlsdr_demod_read/write_reg failed
with -9` (USB pipe stalls) and especially `r82xx_write: i2c wr failed=-9`,
that's a USB-power / USB-signal-integrity symptom — **NOT a software
issue**. Three things to try in order:

1. Plug the dongle into a **USB 2.0 port** (black, usually on the back of
   the PC). USB 3.x host controllers negotiate power differently and
   R828D-tuner dongles are picky about it.
2. Switch to the [RTL-SDR Blog fork](https://github.com/rtlsdrblog/rtl-sdr-blog/releases)
   driver bundle if you grabbed the osmocom build first. The Blog fork
   has R828D-specific tuning workarounds.
3. Try a different USB cable (cheap ones drop bits on i2c traffic).

### Tuner variants you might see

The cheap "RTL-SDR" sticks ship with one of two tuners depending on year /
batch / clone source:
- **R820T2** — older, slightly less sensitive, very well supported
- **R828D** — newer, the tuner on RTL-SDR Blog V4, more USB-power-sensitive

Both work for our use case. `rtl_test` prints which one you have on the
`Found Rafael Micro ... tuner` line.

### Install Universal Radio Hacker

```powershell
pip install --user urh
```

**Python version gotcha (Windows)**: as of URH 2.10.0, the ONLY Windows
wheel on PyPI is `cp313-win_amd64` — i.e. Python **3.13** only. Trying
to install URH on Windows under Python 3.10, 3.11, 3.12, or 3.14 falls
back to a source build that requires both Cython and MSVC Build Tools;
even with both installed it often still fails.

The clean fix is to install Python 3.13 alongside your existing Python
(other tooling — Thonny, our test suite — can stay on whatever version
you already use):

1. Install [Python 3.13](https://www.python.org/downloads/release/python-3137/).
   **Uncheck** "Add to PATH" during install so it doesn't shadow your
   default Python.
2. Use the Python launcher to target 3.13 explicitly:

   ```powershell
   py -3.13 -m pip install --user urh
   ```

   This grabs the prebuilt `urh-2.10.0-cp313-cp313-win_amd64.whl` —
   no compilation needed.

3. Launch URH. `pip install --user` puts script wrappers in your per-user
   Scripts directory which is NOT on PATH by default. Either run by
   full path:

   ```powershell
   C:\Users\<you>\AppData\Roaming\Python\Python313\Scripts\urh.exe
   ```

   …or add that Scripts directory to PATH once (admin PowerShell):

   ```powershell
   [Environment]::SetEnvironmentVariable("Path", $env:Path + ";C:\Users\$env:USERNAME\AppData\Roaming\Python\Python313\Scripts", "User")
   ```

   Then in a new terminal, plain `urh` works.

   **Note:** `py -3.13 -m urh` does NOT work — URH is a setuptools script
   entry point, not a runnable module. Trying it fails with
   `'urh' is a package and cannot be directly executed`.

(Linux ships wheels for Python 3.10-3.14 — no Python-version juggling
required; just `pip install --user urh` then `urh` will be on `~/.local/bin`.)

Launch URH once to confirm — a GUI window should open. Close it; we'll
come back to it in step 05.

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
