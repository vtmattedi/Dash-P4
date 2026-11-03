# Dash-P4

NightMare Network wall dashboard for the **Guition JC1060P470C** (ESP32-P4, 16 MB flash, 32 MB PSRAM, 7" 1024x600 MIPI-DSI panel, GT911 touch, ESP32-C6 Wi-Fi co-processor via ESP-Hosted). Built on native ESP-IDF (no Arduino) with PlatformIO and LVGL.

## Environments

| Env | What it builds |
| --- | --- |
| `jc1060p470c` | The dashboard |
| `jc1060p470c_usbscreen` | The panel as a Windows USB extended display only (stage 1) |
| `jc1060p470c_usbscreen_nm` | Dashboard + Windows display when the PC streams (stage 2) |
| `jc1060p470c_usbscreen_nm_espnow` | Stage 2 + ESP-NOW through the C6 (stage 3) |

## Build and flash

```sh
pio run -e jc1060p470c              # build
pio run -e jc1060p470c -t upload    # flash
pio device monitor                  # serial monitor, 115200
```

## Setup

- Copy `include/creds.example.h` to `include/creds.h` and fill in your credentials.
- The USB display needs Espressif's USB Extended Display driver on the PC (see [usb_screen/NOTES.md](usb_screen/NOTES.md)).
- Stage 3 needs the C6 running the sister project's firmware (`nightmare/devices/dash-p4-c6`). Install it from Settings > Firmware > Update C6, or with the `C6OTA` console command.
- If the boot log reports chip revision v3.x, switch the board to `esp32-p4_r3` in `platformio.ini` and flip the revision in `board/sdkconfig.board`.

## Layout

- `src/` - application, UI, USB screen, PC link
- `board/` - everything specific to this board (display, touch, power); see [board/NOTES.md](board/NOTES.md)
- `usb_screen/` - Windows USB display component
- `espnow_hosted/` - ESP-NOW over ESP-Hosted
- `test/host`, `tools/host_test.sh` - host-side tests
- `partitions.csv`, `sdkconfig.*` - flash layout and per-env configs

The NightMareNetwork library is pulled from its GitHub `main` branch; don't edit the copy under `.pio/libdeps`.
