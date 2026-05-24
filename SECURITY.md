# Security model

This project is published openly on GitHub. This page describes what is and isn't safe to share, and what the realistic threat model is for an in-cab device that can start the engine.

## What this project is

A voltage-triggered remote starter for a 2009 Kia Spectra. It rides on a factory-installed Compustar 1WSHR-PRO aftermarket remote start. The novel hardware piece is an ESP32 + CC1101 that synthesizes the same 433.92 MHz KeeLoq packets the FOB transmits, allowing a Pi-based controller to trigger the engine when battery voltage drops below a configurable threshold.

## What's in the repo, what's not

**In the repo (public)**:
- All source code, drivers, state machine
- Architecture, BOM, reproduction docs
- KeeLoq cipher implementation (academic since 2007)
- Compustar HCS packet format (documented in the Microchip HCS300/301 datasheets)
- SDR analysis scripts and walkthrough

**Never in the repo (gitignored)**:
- `esp32/src/secrets.py` — the FOB's device key + serial + counter, WiFi creds, MQTT creds
- `pi/app/secrets.py` — same on the Pi side
- Raw IQ captures in `sdr/captures/` (4MB each + contain timing patterns that could fingerprint a specific FOB)

The gitignore was set up on day one and is verified by `git status` before every commit.

## Threat model

### What an attacker physically near the car can already do

Anyone with a $40 SDR and patience can:
- Capture every transmission the FOB makes (the FOB transmits in the clear-frequency-clear-modulation sense; only the payload is encrypted)
- Replay them — but the receiver tracks the counter and rejects re-used codes (this is the whole point of KeeLoq)
- Run a "rollback" attack to capture two consecutive codes when the user is near the car, then later replay the first to put the receiver in re-sync mode

Cars with 2-way remote start systems (Compustar T-series, 900 MHz) are no harder against these attacks. The Compustar 1WSHR-PRO is no easier or harder than any other consumer KeeLoq-family system.

### What this project does NOT make worse

- The KeeLoq device key never leaves the ESP32. It's loaded from the gitignored `secrets.py` at boot and used internally.
- The Compustar receiver in the car has the same key the FOB has — we don't add a second point where the key lives. We just add an additional transmitter that knows the existing key.
- The ESP32 transmits the same RF packet structure the FOB does. From the receiver's perspective, the ESP32 is indistinguishable from the FOB.

### What this project DOES newly expose

- If someone physically steals the device from the car, they can:
  1. Pull the SD card and read `pi/app/secrets.py` (WiFi creds, MQTT creds)
  2. Read flash from the ESP32 with the right tooling (~$30 + an hour) and recover the device key
- Once the device key is recovered, they could start your car remotely. But they could also do this by physically stealing your FOB, so the marginal risk depends on whether the device is harder or easier to steal than the FOB.

**Mitigation**: physical-security the unit (lockable case, mounted behind dash, tamper-evident screws). Treat it like the FOB itself.

### What this project does NOT defend against

- Someone compromising your MQTT broker can send `start_engine` commands. We whitelist the allowed commands at the subscriber but a compromised broker can still issue allowed ones.
- Someone on your LAN can hit the Flask dashboard (no auth — designed for a trusted home network). If you want auth, put it behind a reverse proxy with HTTP Basic.
- Power-glitch attacks on the ESP32 to dump flash — out of scope for v1.
- Compromising the GitHub repo to inject malicious code that runs after `git pull` — out of scope for any solo OSS project; mitigate with branch protection if you set up CI deployment.

## Reporting issues

If you find an actual vulnerability (e.g. a way for someone NOT in physical proximity to start the engine), open an issue on the GitHub repo. Don't include reproducing steps that would help an attacker — describe the class of issue and we'll coordinate disclosure.

## Why this is OK to publish

Everything novel in this project (the integration, the documentation, the helper scripts) is hardware/software design that's the user's own work. The KeeLoq cipher and HCS protocol are decades-old published standards. The attack literature against KeeLoq is academic and public (Bogdanov 2007, Eisentrager 2008). Open-sourcing the integration doesn't tell attackers anything they don't already know — but it does help anyone else with a similar use case avoid re-doing the same analysis.

The hardest secrets in the system (the per-FOB device key) are uniquely yours and live only on hardware you physically possess.
