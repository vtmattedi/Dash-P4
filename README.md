# Dash-P4

NightMare Network wall dashboard for the **Guition JC1060P470C** (ESP32-P4, 16 MB flash, 32 MB PSRAM, 7" 1024x600 MIPI-DSI panel, GT911 touch, ESP32-C6 Wi-Fi co-processor via ESP-Hosted). Built on native ESP-IDF (no Arduino) with PlatformIO and LVGL.

A wall display for the day: the clock, the calendar (events and deadlines), tasks (Linear and WOI), Stackport's health,
and the NightMare devices (AC, light, door). A small companion app on the PC (`pc/`) lets the panel open links on it.

## Environments

| Env | What it builds |
| --- | --- |
| `jc1060p470c` | The dashboard |
| `jc1060p470c_espnow` | The dashboard + ESP-NOW through the C6 |

The Windows USB extended display and the board's USB camera were proofs of concept. They are not in the tree; the git tag
`usb-proofs` has them.

## Build and flash

```sh
pio run -e jc1060p470c              # build
pio run -e jc1060p470c -t upload --upload-port COM6    # flash
pio device monitor                  # serial monitor, 115200
```

On Windows, set `PYTHONUTF8=1` when the output of `pio run -t upload` is redirected or piped: esptool's progress characters
crash PlatformIO's output on the default code page, and the upload then looks hung.

## Setup

- Copy `include/creds.example.h` to `include/creds.h` and fill in your credentials (Wi-Fi, MQTT, the notification feed token).
- The ESP-NOW env needs the C6 running the sister project's firmware (`nightmare/devices/dash-p4-c6`). Install it from Settings > Firmware > Update C6, or with the `C6OTA` console command.
- If the boot log reports chip revision v3.x, switch the board to `esp32-p4_r3` in `platformio.ini` and flip the revision in `board/sdkconfig.board`.

## Layout

- `src/` - application and UI; `src/Link/` is the server side of the PC link
- `board/` - everything specific to this board (display, touch, power); see [board/NOTES.md](board/NOTES.md)
- `pc/` - `dashlink`, the PC companion app and the link protocol ([pc/README.md](pc/README.md))
- `espnow_hosted/` - ESP-NOW over ESP-Hosted
- `test/host`, `tools/host_test.sh` - host-side tests
- `partitions.csv`, `sdkconfig.defaults` - flash layout and Kconfig defaults (`sdkconfig.<env>` is generated)

The NightMareNetwork library is pulled from its GitHub `main` branch; don't edit the copy under `.pio/libdeps`.

## Windows build notes

`python tools/repair_managed.py` checks `managed_components/` against the component manager's cache and restores damaged
components (the configure step can fail with "Path ... does not exist" after the component manager is interrupted).
