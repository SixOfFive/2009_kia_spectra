"""
MicroPython boot entrypoint for the ESP32.

Wires the hardware drivers together and hands control to VroomController.

To test the controller logic on a desktop, import controller.py directly
and pass mock hardware — don't run this file.
"""
import config

from machine import Pin, UART

from lib.cc1101 import CC1101
from lib.ads1115 import ADS1115
from lib.pi_link import UartLink

from controller import VroomController


def main():
    # ----- Hardware init -----
    adc = ADS1115(i2c_id=0, sda=21, scl=22)
    radio = CC1101(spi_id=2, cs_pin=5, gdo0_pin=22)
    radio.init_433mhz_ook()

    # CAN may be unavailable depending on MicroPython build; degrade gracefully
    can = None
    try:
        from lib.twai_can import Can
        can = Can(
            tx_pin=config.CAN_TX_PIN,
            rx_pin=config.CAN_RX_PIN,
            baudrate=config.CAN_BAUDRATE,
        )
    except ImportError as e:
        print("[main] CAN unavailable, OBD polling disabled: %s" % e)

    uart_hw = UART(
        2,
        baudrate=config.UART_BAUDRATE,
        tx=config.UART_TX_PIN,
        rx=config.UART_RX_PIN,
    )
    uart = UartLink(uart_hw)

    pi_power = Pin(config.PI_POWER_GPIO, Pin.OUT)

    controller = VroomController(adc, radio, can, uart, pi_power, config)
    controller.run()


if __name__ == "__main__":
    main()
