# 20 — SNMP integration

The Pi runs a small **read-only SNMPv2c responder** alongside the
dashboard daemon, so an existing NMS (LibreNMS, Cacti, observium, even
ad-hoc `snmpwalk` from your laptop) can pull vroom-specific telemetry
and graph it next to whatever else you monitor.

The responder is pure stdlib Python (~250 LoC of BER + PDU handling)
and lives at `pi/app/snmp_responder.py`. It runs as a **thread inside
the main `vroom.service` daemon** (sibling to the UART listener and
MQTT publisher) so it shares the in-process `STATE` dict and returns
live telemetry, not snapshot-on-startup defaults. It serves a small
custom OID tree and intentionally **does not implement standard host
MIBs** like sysUptime, ifTable, or hrStorageTable — the host where the
Pi runs should already be polled by your NMS for those.

## Why a custom responder instead of `snmpd`

Three reasons, all aligned with the rest of this project's stack:

1. **Footprint** — not actually the bottleneck (snmpd RSS is ~10 MB on
   ARM Debian, well within the Pi Zero 2 W's 512 MB), but the custom
   responder slot of the unified `vroom.service` adds essentially zero
   RSS (no second process — same `python3` interpreter, same imports,
   one extra background thread).
2. **Single source of truth** — the responder reads
   `pi.app.state.snapshot()` from the same in-process dict the UART
   listener writes to. No IPC, no `snmpd.conf` to keep in sync with
   the OID layout.
3. **Pattern consistency** — same thread-in-daemon shape as
   `uart_listener`, `mqtt_publisher`, and `mqtt_subscriber`. One
   config surface (`pi/app/config.py` + `pi/app/secrets.py`), one log
   destination (journal under `vroom.service`).

The port, community string, and bind host live in `pi/app/secrets.py`
(copy `secrets.py.example` first). The on/off toggle lives in
`pi/app/config.py` (`SNMP_ENABLED = True` by default). No separate
systemd unit — the responder thread is started by `pi.app.daemon`
at boot.

## OID reference

All OIDs are read-only, scalars (each has a trailing `.0` instance
suffix per SMI convention). Base prefix: `1.3.6.1.4.1.99999.7.`

| OID suffix | Type | Description |
|---|---|---|
| `.1.0` | INTEGER | `run_state` — `0`=BOOT, `1`=MONITORING, `2`=STARTING, `3`=RUNNING, `4`=STOPPING, `5`=COOLDOWN, `99`=unknown |
| `.2.0` | Gauge32 | `battery_mv` — battery voltage in millivolts (avoids float-encoding hassle) |
| `.3.0` | INTEGER | `engine_running` — `0` or `1` |
| `.4.0` | OCTET STRING | `state_name` — free-form ESP32 state string (canonical source of truth; the .1.0 mapping above is a convenience) |
| `.5.0` | TimeTicks | `esp32_uptime_ticks` — hundredths of a second since the ESP32 booted |
| `.6.0` | INTEGER | `pi_wifi_rssi_dbm` — signed dBm (typically -30 to -90) |
| `.7.0` | Counter32 | `event_count` — number of events currently retained in the dashboard ring buffer |
| `.8.0` | OCTET STRING | `last_event_name` — most recent event (empty string if none yet) |
| `.9.0` | TimeTicks | `ticks_since_last_engine_start` — hundredths of a second since the last engine start, `0` if never started |
| `.10.0` | Counter32 | `esp32_message_counter` — UART frame sequence number from the ESP32 |

Add new OIDs by appending entries to `OID_TABLE` in
`pi/app/snmp_responder.py` — the GETNEXT walker re-sorts on each
request, so insertion order doesn't matter.

## About the `.99999` prefix

`1.3.6.1.4.1.99999` is not a registered IANA Private Enterprise
Number. It's the "squatter" prefix the vault uses for personal
projects that don't need to interoperate with external SNMP toolkits.
This is the same pattern documented in [[asus2snmp Asus router SNMP
project]] and [[ddwrt router telnet-to-monitoring bridges]].

To renumber later (e.g. if vroom ever gets an IANA-registered PEN),
change one constant — `VROOM_ENTERPRISE` in
`pi/app/snmp_responder.py` — and the entire OID tree shifts.

## Verifying with snmpwalk

From any machine with `snmp-utils` (Debian package: `snmp`):

```bash
# Walk the whole vroom tree
snmpwalk -v2c -c public 192.168.1.42:1161 1.3.6.1.4.1.99999.7

# Or, with MIB names disabled and numeric OIDs (cleanest output):
snmpwalk -v2c -c public -On 192.168.1.42:1161 1.3.6.1.4.1.99999.7

# Get one OID
snmpget -v2c -c public 192.168.1.42:1161 1.3.6.1.4.1.99999.7.2.0
```

Expected output of `snmpwalk` against a healthy unit:

```
.1.3.6.1.4.1.99999.7.1.0 = INTEGER: 1
.1.3.6.1.4.1.99999.7.2.0 = Gauge32: 12450
.1.3.6.1.4.1.99999.7.3.0 = INTEGER: 0
.1.3.6.1.4.1.99999.7.4.0 = STRING: "MONITORING"
.1.3.6.1.4.1.99999.7.5.0 = Timeticks: (1234500) 3:25:45.00
.1.3.6.1.4.1.99999.7.6.0 = INTEGER: -55
.1.3.6.1.4.1.99999.7.7.0 = Counter32: 4
.1.3.6.1.4.1.99999.7.8.0 = STRING: "engine_stopped"
.1.3.6.1.4.1.99999.7.9.0 = Timeticks: (84000) 0:14:00.00
.1.3.6.1.4.1.99999.7.10.0 = Counter32: 173
```

If the walk hangs and your NMS is on a different subnet, double-check
that port 1161/udp is open through the router and that
`SNMP_BIND_HOST` in `secrets.py` is `0.0.0.0` rather than `127.0.0.1`.

## LibreNMS / observium integration

LibreNMS doesn't ship a vroom MIB, so the cleanest path is the
`unix-agent`-style approach: add a custom application that polls the
known OIDs and graphs them. Place a snippet like this in your LibreNMS
configuration:

```yaml
# config.php (or equivalent in includes/definitions/)
$config['os']['vroom']['mib_dir'] = []
$config['os']['vroom']['poller'] = 'wmi'   # not real wmi; placeholder
# ... or, simpler: use the "snmp-walks" custom-OID feature with the
# OID table above pasted into a vroom.yaml under includes/definitions/
```

(LibreNMS-specific integration is left as an exercise — the OIDs above
are stable enough to script against directly with `snmpget` in a cron
job + Telegraf if you don't want to build a full module.)

## Cacti integration

Cacti's "data input methods" can call `snmpget` directly. Create one
data input method per OID you want to graph, set the OID field to one
of the entries from the table above, and tie it to a CDEF for unit
conversion if needed (e.g. Gauge32 `battery_mv` → divide by 1000 for
volts on the graph).

The cleanest pattern, lifted from
[[ddwrt router telnet-to-monitoring bridges]]:

1. One Cacti **data template** per metric family (battery, state, events)
2. One Cacti **graph template** that combines them
3. A single Cacti host bound to the Pi's IP with the custom
   `snmpget`-based data inputs

## Standalone process mode (debugging only)

`python -m pi.app.snmp_responder` will spawn the responder as its own
process for ad-hoc wire-format sanity-checking. It works at the
protocol level (BER round-trips, GET/GETNEXT/walk all do the right
thing) but serves **boot-time defaults forever** for every OID,
because module-level dicts are per-process in CPython: the standalone
process imports its own `pi.app.state` that no UART listener ever
updates. The standalone main() logs a WARNING at startup as a
reminder. For production, always run via `pi.app.daemon`.

## Security note

The responder is **read-only** — no SET requests are honored, so
exposing it on the LAN doesn't add a new control surface. The
SNMPv2c community string is the only access gate; treat it as
read-level credentials and don't reuse it across systems where one
might be compromised.

If you need stronger auth, the upgrade path is to swap SNMPv2c for
SNMPv3 (USM with authPriv). That would mean either:

- Reimplementing the auth/priv layer in `snmp_responder.py` (real work
  — adds ~400 LoC for AES + HMAC-SHA + key derivation), or
- Switching to `net-snmp` (`snmpd`) with a `pass_persist` Python
  extension that calls into the vroom state. The footprint argument
  (a few MB extra RSS) loses meaning if you've already committed to
  the extra complexity.

For a personal-LAN install the v2c-with-community-string baseline is
fine.

## Operations

The responder is a thread inside `vroom.service` — no separate unit
to start/stop. To check its state, look at the parent service:

```bash
# Service status (covers dashboard + UART + MQTT + SNMP threads)
sudo systemctl status vroom

# Logs — the responder logs malformed requests at DEBUG and binding
# errors at WARNING. Thread name "vroom-snmp" makes them grep-friendly.
sudo journalctl -u vroom -f | grep -E 'vroom\.snmp|vroom-snmp'

# Disable SNMP without touching the dashboard
sudoedit /opt/vroom/pi/app/config.py        # set SNMP_ENABLED = False
sudo systemctl restart vroom

# Restart on a different port
sudoedit /opt/vroom/pi/app/secrets.py       # change SNMP_PORT
sudo systemctl restart vroom
```

If SNMP fails to bind (port already in use, etc.), the responder
thread logs the failure and dies; the rest of `vroom.service`
(dashboard + UART + MQTT) keeps running. There is no auto-retry —
restart the service after fixing the conflict.
