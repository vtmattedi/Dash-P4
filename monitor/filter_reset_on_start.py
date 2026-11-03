"""PlatformIO monitor filter: hard-reset the board as soon as the serial monitor opens.

Opening a serial port does not reset a P4 board by itself, so the boot log is missed and a
board that hung earlier stays hung. This pulses the auto-reset lines the same way esptool's
"hard reset" does: DTR low (BOOT/IO0 stays high, so the chip boots the app, not the ROM
downloader), RTS high (EN low) for 100 ms, then RTS low.

It only works if the board's USB bridge wires DTR/RTS to BOOT/EN (most do). If nothing happens,
press the Reset button once; the filter is harmless either way.

Enable with `monitor_filters = ..., reset_on_start` in platformio.ini.
"""
import time

from platformio.device.monitor.filters.base import DeviceMonitorFilterBase


class ResetOnStart(DeviceMonitorFilterBase):
    NAME = "reset_on_start"

    def set_running_terminal(self, terminal):
        super().set_running_terminal(terminal)
        serial = getattr(terminal, "serial", None)
        if serial is None or not serial.is_open:
            return
        try:
            serial.dtr = False  # IO0 high: normal boot
            serial.rts = True   # EN low: reset
            time.sleep(0.1)
            serial.rts = False  # EN high: run
            print("--- reset_on_start: board reset via RTS/DTR")
        except (OSError, ValueError, AttributeError) as exc:  # a bridge without modem lines
            print("--- reset_on_start: could not toggle RTS/DTR (%s)" % exc)
