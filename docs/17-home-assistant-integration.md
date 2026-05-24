# 17 — Home Assistant integration

## Goal

Surface battery voltage, engine state, trigger events, and last-trip
metadata in Home Assistant, with a manual-transmit button and an
automation that notifies you when an unexpected auto-trigger fires.

The Pi already publishes MQTT messages as soon as `MQTT_BROKER` is
set in `secrets.py`. This step is about consuming them on the Home
Assistant side: declaring entities, building a Lovelace card, and
wiring up one or two automations.

## Prerequisites

- [16 — First auto-start](16-first-auto-start.md) passed
- Home Assistant instance reachable on your LAN (Home Assistant OS,
  Container, or Supervised — any flavor works)
- MQTT broker running. Easiest: the official **Mosquitto broker**
  add-on inside Home Assistant. Already-installed brokers (separate
  host, mqtt.foo.lan, etc.) work identically.
- Home Assistant's **MQTT integration** configured to point at that
  broker.
- Pi's `secrets.py` has matching `MQTT_BROKER`, `MQTT_PORT`,
  `MQTT_USERNAME`, `MQTT_PASSWORD`, and `MQTT_TOPIC_PREFIX` (default
  `vroom/spectra`)

## Topic structure recap

The Pi's `vroom.service` publishes and subscribes on these topics
(prefix `vroom/spectra` by default):

| Topic | Direction | Payload | Cadence |
|---|---|---|---|
| `vroom/spectra/state` | Pi → broker | JSON snapshot of full state (battery V, RPM, run state, last-event, etc.) | every `MQTT_PUBLISH_INTERVAL_S` (default 10s, see `pi/app/config.py`), retained |
| `vroom/spectra/event` | Pi → broker | JSON of a discrete event (`low_voltage_trigger`, `engine_started`, `engine_stopped`, `shutdown_ack`, etc.) | on event, **not** retained |
| `vroom/spectra/cmd` | broker → Pi | JSON command — see whitelist in [10 — Pi setup](10-pi-setup.md) step 6 | on demand |

You can verify with `mosquitto_sub` from any host:

```powershell
mosquitto_sub -h <broker> -t "vroom/spectra/#" -v
```

You should see one `state` message immediately (retained) and another
every `MQTT_PUBLISH_INTERVAL_S` seconds.

## Step 1 — Declare entities in Home Assistant

Add the following to your Home Assistant `configuration.yaml`, or
into a dedicated `mqtt:` section file if you've split configuration.
Replace `vroom/spectra` with your actual `MQTT_TOPIC_PREFIX` if
different.

```yaml
mqtt:
  sensor:
    - name: "Spectra battery voltage"
      unique_id: spectra_battery_voltage
      state_topic: "vroom/spectra/state"
      value_template: "{{ value_json.v_battery }}"
      unit_of_measurement: "V"
      device_class: voltage
      state_class: measurement

    - name: "Spectra engine RPM"
      unique_id: spectra_engine_rpm
      state_topic: "vroom/spectra/state"
      value_template: "{{ value_json.rpm | default(0) }}"
      unit_of_measurement: "rpm"
      state_class: measurement

    - name: "Spectra coolant temp"
      unique_id: spectra_coolant_temp
      state_topic: "vroom/spectra/state"
      value_template: "{{ value_json.coolant_c | default(0) }}"
      unit_of_measurement: "°C"
      device_class: temperature
      state_class: measurement

    - name: "Spectra last event"
      unique_id: spectra_last_event
      state_topic: "vroom/spectra/event"
      value_template: "{{ value_json.kind }}"
      json_attributes_topic: "vroom/spectra/event"

    - name: "Spectra ESP32 temp"
      unique_id: spectra_esp32_temp
      state_topic: "vroom/spectra/state"
      value_template: "{{ value_json.esp32_temp_c | default(0) }}"
      unit_of_measurement: "°C"
      device_class: temperature
      state_class: measurement

  binary_sensor:
    - name: "Spectra engine running"
      unique_id: spectra_engine_running
      state_topic: "vroom/spectra/state"
      value_template: "{{ 'ON' if value_json.run_state == 'running' else 'OFF' }}"
      device_class: running

  button:
    - name: "Spectra start engine"
      unique_id: spectra_start_engine
      command_topic: "vroom/spectra/cmd"
      payload_press: '{"cmd": "start_engine"}'

    - name: "Spectra stop engine"
      unique_id: spectra_stop_engine
      command_topic: "vroom/spectra/cmd"
      payload_press: '{"cmd": "stop_engine"}'

    - name: "Spectra ping"
      unique_id: spectra_ping
      command_topic: "vroom/spectra/cmd"
      payload_press: '{"cmd": "ping"}'
```

Reload via **Developer Tools → YAML → Manually configured MQTT
entities** (or restart Home Assistant). The new entities should
appear in `sensor.spectra_battery_voltage`, `button.spectra_start_engine`,
etc.

If a sensor reads `unavailable`, the retained `state` message hasn't
landed yet — give it `MQTT_PUBLISH_INTERVAL_S` and refresh.

## Step 2 — Last-trip metadata (optional)

If you want a "last trip" rollup that shows up cleanly in the
dashboard, expose the run-end summary as its own entity. The Pi
publishes `vroom/spectra/event` with `kind: engine_stopped` and
attributes like `run_duration_s`, `final_v_battery`, `peak_v_battery`,
`peak_coolant_c`:

```yaml
mqtt:
  sensor:
    - name: "Spectra last trip duration"
      unique_id: spectra_last_trip_duration_s
      state_topic: "vroom/spectra/event"
      value_template: >-
        {% if value_json.kind == 'engine_stopped' %}
          {{ value_json.run_duration_s | int }}
        {% else %}
          {{ states('sensor.spectra_last_trip_duration') | default(0) }}
        {% endif %}
      unit_of_measurement: "s"
      state_class: measurement
```

That pattern (the `{% if value_json.kind == X %}` guard + the
`else: keep previous`) is how you build a sticky-value sensor on
top of a non-retained event topic.

## Step 3 — Lovelace card

A minimal card showing the four most useful values + the three
buttons:

```yaml
type: vertical-stack
cards:
  - type: entities
    title: Spectra
    entities:
      - entity: binary_sensor.spectra_engine_running
        name: Engine
      - entity: sensor.spectra_battery_voltage
        name: Battery
      - entity: sensor.spectra_engine_rpm
        name: RPM
      - entity: sensor.spectra_coolant_temp
        name: Coolant
      - entity: sensor.spectra_esp32_temp
        name: ESP32 temp
      - entity: sensor.spectra_last_event
        name: Last event
  - type: horizontal-stack
    cards:
      - type: button
        entity: button.spectra_start_engine
        name: Start
        icon: mdi:car-electric
      - type: button
        entity: button.spectra_stop_engine
        name: Stop
        icon: mdi:car-off
      - type: button
        entity: button.spectra_ping
        name: Ping
        icon: mdi:radio-tower
```

For a richer view, swap `entities` for a `mini-graph-card` (HACS) on
the battery voltage entity — long-term voltage trends are the most
useful long-term data this build produces.

## Step 4 — Automation: notify on unexpected auto-trigger

The trigger you *expect* is the one you initiated from the
dashboard. The one you want to know about is an unattended trigger
in the middle of the night — that means the car decided on its own
that voltage was low, which is either working-as-intended (cold
snap, normal parasitic drain) or warning-as-intended (alternator
not charging, parasitic load you didn't know about).

```yaml
automation:
  - alias: "Spectra unexpected auto-trigger notification"
    description: >-
      Notify when the car auto-starts itself, since we want to know
      about every voltage-trigger event even if it's working as
      designed.
    trigger:
      - platform: mqtt
        topic: vroom/spectra/event
        value_template: "{{ value_json.kind }}"
        payload: low_voltage_trigger
    action:
      - service: notify.mobile_app   # change to your notify target
        data:
          title: "Spectra auto-started"
          message: >-
            Battery hit {{ trigger.payload_json.v_battery }} V at
            {{ now().strftime('%H:%M') }}. Engine running for
            {{ states('sensor.spectra_last_trip_duration') }} s.
```

Variations:

- **Notify only at unusual times** — add a condition with `now().hour
  not in [22,23,0,1,2,3,4,5]` to skip overnight triggers (if those
  are normal for you).
- **Notify on engine_stopped too**, to confirm the run completed
  cleanly. Useful early in the install when you're still building
  confidence.
- **Notify if `v_battery` < 12.0 V for 5 minutes**, independent of
  the controller's own logic. Belt-and-suspenders alerting in case
  the controller itself is misbehaving.

## Step 5 — Long-term logging

Home Assistant's recorder defaults work for short-term troubleshooting
but the project is most useful with months of voltage data:

- Install **InfluxDB** add-on, set up a `vroom` bucket
- Add the **influxdb:** integration in `configuration.yaml`, include
  `sensor.spectra_battery_voltage` and `sensor.spectra_esp32_temp` in
  the whitelist
- Add **Grafana** add-on, build a dashboard with weekly/monthly
  voltage trend, trigger frequency, run durations

The voltage trend is the single most useful piece of long-term data
the build produces — it tells you when the battery is dying
(declining baseline voltage), when parasitic drain has crept up
(faster discharge between triggers), and whether your
`LOW_V_TRIGGER` value needs seasonal tweaking.

## What you should have when done

- 5+ entities in Home Assistant tracking battery V, RPM, coolant,
  engine running, last event, ESP32 temp
- 3 button entities for Start, Stop, Ping
- One Lovelace card showing the above
- One automation that notifies on `low_voltage_trigger` events
- (Optional) InfluxDB + Grafana set up for long-term voltage trend

## Where artifacts go

- HA configuration changes typically live in `configuration.yaml`
  or `packages/spectra.yaml` if you split by package. **Not in this
  repo** — it's your HA's config.
- A redacted copy of the Lovelace YAML can go to
  `docs/examples/lovelace-spectra-card.yaml` if you want to share
  it with others reproducing this build — but only after stripping
  any topic prefixes that identify your specific deployment.

## Troubleshooting

| Symptom | Fix |
|---|---|
| Sensors show `unavailable` | No retained `state` message on the broker yet. Wait `MQTT_PUBLISH_INTERVAL_S` or push manually from the Pi (`systemctl restart vroom.service`). |
| Button presses don't fire the engine | Check broker logs for the `cmd` message landing. Then check `journalctl -u vroom.service -f` on the Pi for `forwarding command to ESP32`. If the command lands but the engine doesn't crank, see [step 15 troubleshooting](15-first-trigger.md). |
| Sensors update but the engine_running binary stays OFF | Pi side isn't tagging `run_state` correctly. Check `pi/app/state.py` — should set `running` when an `engine_started` event is observed and clear it on `engine_stopped`. |
| Automation triggers twice per event | The Pi may publish the same event from both UART listener and MQTT bridge. Add a `for: '5s'` condition on the trigger, or fix the duplicate publish on the Pi side. |
| Engine RPM stays 0 even while running | OBD CAN decoder not finding RPM PID, or `machine.CAN` isn't compiled into the ESP32 firmware. RPM gauge is OBD-derived; it's an enhancement, not the core trigger path. |
| MQTT messages stop overnight then resume in morning | Pi WiFi is dropping. Check `/var/log/syslog` for `wpa_supplicant` reconnects. Antenna placement may be marginal — see [14](14-case-mounting.md). |

## Next

[18 — Long-term stability](18-long-term-stability.md) — what to watch
for over the first month of installed operation.
