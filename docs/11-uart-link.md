# 11 — Wire ESP32 ↔ Pi UART link

## Goal

Connect the ESP32's UART2 pins to the Pi's primary UART, confirm the JSON
line protocol round-trips, and see real STATUS messages appear in the
dashboard.

## Prerequisites

- [09 — bench smoke tests](09-bench-smoke-tests.md) — ESP32 talks via Thonny
- [10 — Pi setup](10-pi-setup.md) — `vroom.service` running on the Pi
- Three jumper wires (TX, RX, GND)

## Step 1 — Identify the pins

**ESP32** (defaults in `esp32/src/config.py`):
- TX = GPIO 17
- RX = GPIO 16
- GND = any GND pin

**Pi Zero 2 W** (40-pin header):
- TX = GPIO 14 = pin 8
- RX = GPIO 15 = pin 10
- GND = pin 6 (or any other GND pin)

## Step 2 — Wire it up

The link is crossed: each side's TX goes to the other side's RX.

| ESP32 pin | wire | Pi header pin |
|---|---|---|
| GPIO 17 (TX) | → | pin 10 (RX / GPIO 15) |
| GPIO 16 (RX) | → | pin 8 (TX / GPIO 14) |
| GND          | → | pin 6 (GND) |

**Both boards must share a common ground.** Skip the GND wire and the
levels float — you'll see garbage chars on both ends.

## Step 3 — Voltage compatibility check

Both the ESP32 and the Pi Zero 2 W operate at 3.3V logic levels — no
level-shifter needed. **Do not connect the Pi to a 5V-logic ESP32 clone
without a level-shifter** — the Pi's RX pin is not 5V-tolerant.

If you're unsure, measure the ESP32's TX with a multimeter (idle should
be 3.3V). 5V boards exist and they'll fry the Pi's GPIO.

## Step 4 — Confirm both sides see the line

On the Pi:

```bash
sudo systemctl stop vroom.service   # release /dev/serial0
stty -F /dev/serial0 115200 cs8 -cstopb -parenb
cat /dev/serial0
```

Leave that running. Now from Thonny on the ESP32:

```python
>>> from machine import UART
>>> uart = UART(2, baudrate=115200, tx=17, rx=16)
>>> uart.write(b"hello pi\n")
```

You should see `hello pi` printed on the Pi terminal. If not:
- Swap the TX/RX wires (crossed wrong is the #1 cause of "no traffic")
- Confirm both at 115200
- Check GND is connected

Reverse direction — type on the Pi:

```bash
echo "hello esp32" > /dev/serial0
```

And on the ESP32:

```python
>>> uart.read()
b'hello esp32\n'
```

If both directions work, the physical link is good.

## Step 5 — Validate the JSON protocol

Restart the daemon:

```bash
sudo systemctl start vroom.service
```

On the ESP32 (Thonny REPL — first close any other UART references):

```python
>>> from machine import UART
>>> from lib.pi_link import UartLink, status
>>> uart = UART(2, baudrate=115200, tx=17, rx=16)
>>> link = UartLink(uart)
>>> link.send(status(12.6, "test_from_esp32"))
```

On the Pi, watch the daemon's log:

```bash
journalctl -u vroom.service -f
```

You should see a line from `uart-listener` confirming a STATUS dispatch
with v_battery=12.6.

Refresh the dashboard in your browser — the battery gauge should now
show 12.6V (replacing the mocked initial value).

## Step 6 — Round-trip a command

In the dashboard, click the **Ping** button.

In Thonny, drain incoming messages:

```python
>>> while True:
...     msg = link.recv()
...     if msg:
...         print(msg)
...         break
```

You should see something like:

```python
{'type': 'COMMAND', 'ts': 1234567, 'cmd': 'ping'}
```

Send an ACK back:

```python
>>> from lib.pi_link import ack
>>> link.send(ack("ping", detail="manual_test"))
```

The dashboard's last-button-press indicator (or recent events panel) should
show the ack arrived.

## Step 7 — Run the controller for real

Stop the manual REPL session and have the ESP32 run the full controller:

In Thonny: **Run → Run current script (F5)** on `/main.py`.

The controller will:
1. Print `[controller] starting in state=monitoring`
2. Sample the battery voltage (right now via your ADS1115; this is what
   it actually reads, possibly garbage if no divider is wired yet)
3. Send a STATUS message every WAKE_INTERVAL_S
4. Deep-sleep between samples

Watch the Pi's `journalctl -u vroom.service -f`. You should see STATUS
messages flowing every minute, and the dashboard updates accordingly.

Soft-stop with `Ctrl-C` in Thonny when you've confirmed the link.

## What you should have when done

- 3-wire UART (TX, RX, GND) between ESP32 and Pi
- Bidirectional JSON line traffic confirmed
- Dashboard's battery gauge updates from real STATUS messages
- Ping round-trip works (Pi → ESP32 → ACK back)
- Controller can be left running and stream messages indefinitely

## Troubleshooting

| Symptom | Fix |
|---|---|
| Garbage characters | Baud mismatch or GND missing |
| One-way traffic only | TX/RX crossed wrong or one side's TX is dead |
| Daemon doesn't see messages | `vroom.service` not running, or `/dev/serial0` permissions; `journalctl -u vroom.service -e` |
| `usb_claim_interface error -6` on ESP32 | Thonny still holds the USB port; close Thonny before starting `cat /dev/serial0` |
| Dashboard never updates | Browser cached old `/api/state`; hard-refresh with Ctrl-F5 |

## Next

[`12-keeloq-bench-validation.md`](12-keeloq-bench-validation.md) — final
bench step before the car: synthesize a real Keeloq packet with the
device key from step 07, validate it against the captured FOB pattern,
do a Lock-cycle test in the car.
