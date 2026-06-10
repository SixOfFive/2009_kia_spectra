#!/usr/bin/env python3
"""Poll the ESP32-S3 voltage monitor over WiFi and print readings.

The ESP32 (running sketches/voltage_monitor) serves JSON at  http://<host>/json  like:
    {"vbatt":12.34,"adc_mv":901,"divider":5.545,"cal":1.0,"rssi":-52,"uptime_s":123}

Usage:
    python voltage_client.py [host] [count]
        host   IP address or mDNS name   (default: esp32-volt.local)
        count  number of polls, 0 = forever (default: 0)

Examples:
    python voltage_client.py 192.168.x.x         # poll forever
    python voltage_client.py esp32-volt.local 5  # five readings then stop

Uses only the Python standard library -- no pip install needed.
"""
import sys
import time
import json
import urllib.request

host = sys.argv[1] if len(sys.argv) > 1 else "esp32-volt.local"
count = int(sys.argv[2]) if len(sys.argv) > 2 else 0
url = f"http://{host}/json"
interval = 2.0

print(f"Polling {url}  (Ctrl-C to stop)")
i = 0
while count == 0 or i < count:
    stamp = time.strftime("%H:%M:%S")
    try:
        with urllib.request.urlopen(url, timeout=4) as resp:
            d = json.loads(resp.read().decode())
        print(f"{stamp}  Vbatt = {d['vbatt']:6.2f} V   "
              f"(ADC {d['adc_mv']:>4} mV | RSSI {d['rssi']} dBm | up {d['uptime_s']}s)")
    except Exception as e:
        print(f"{stamp}  -- no response ({e})")
    i += 1
    if count == 0 or i < count:
        time.sleep(interval)
