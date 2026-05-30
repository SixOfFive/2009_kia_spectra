"""
Pi daemon — the single-process production entrypoint.

Threads inside one process so they all share STATE without IPC:
    - main thread: Flask development server (threaded=True)
    - uart-listener:  reads ESP32, mutates STATE
    - mqtt-publisher: periodically publishes STATE snapshots to MQTT broker

For a more robust deployment swap Flask's dev server for waitress or
gunicorn — but for an in-cab single-user dashboard the dev server is fine
and gives us easy reload-on-file-change during bench debugging.
"""

from pi.app import config, snmp_responder, state
from pi.app.comms import esp32_link, mqtt_publisher, mqtt_subscriber, uart_listener
from pi.app.display import server


def main():
    print("[daemon] starting")

    # Open the UART link to the ESP32. Lazy-opened by Esp32Link on first
    # send/recv, so this succeeds even if the ESP32 isn't responding yet.
    link = esp32_link.Esp32Link(config.UART_PORT, baudrate=config.UART_BAUDRATE)
    state.set_link(link)

    # Announce ourselves to the ESP32 so it logs the boot event.
    try:
        link.send(esp32_link.event(esp32_link.EVENT_PI_BOOT))
    except Exception as e:
        print(f"[daemon] WARN failed to send PI_BOOT event: {e}")

    uart_listener.start_thread(
        link, poll_interval_s=config.UART_POLL_INTERVAL_S,
    )
    mqtt_publisher.start_thread()
    mqtt_subscriber.start_thread()

    # SNMP responder runs as a thread (not a separate process) so it
    # sees the same STATE dict the UART listener writes to. See
    # docs/20-snmp-integration.md.
    if config.snmp_ready():
        snmp_responder.start_thread(
            config.SNMP_BIND_HOST, config.SNMP_PORT, config.SNMP_COMMUNITY,
            state.snapshot,
        )
        print(f"[daemon] SNMP responder on udp://{config.SNMP_BIND_HOST}:{config.SNMP_PORT}")
    else:
        print("[daemon] SNMP disabled (config.SNMP_ENABLED is False)")

    print(f"[daemon] dashboard on http://{config.DISPLAY_BIND_HOST}:{config.DISPLAY_BIND_PORT}")
    server.app.run(
        host=config.DISPLAY_BIND_HOST,
        port=config.DISPLAY_BIND_PORT,
        debug=False,
        use_reloader=False,
        threaded=True,
    )


if __name__ == "__main__":
    main()
