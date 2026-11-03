# board/ — Guition JC1060P470C (ESP32-P4, 7" 1024x600)

Copy this folder into another project that runs on the same board. It is an IDF component
and carries no application code.

## To reuse
1. Copy `board/` into the new project root.
2. Root `CMakeLists.txt` (before `include(project.cmake)`):
   ```cmake
   list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/board")
   set(SDKCONFIG_DEFAULTS "${CMAKE_CURRENT_LIST_DIR}/board/sdkconfig.board;${CMAKE_CURRENT_LIST_DIR}/sdkconfig.defaults")
   ```
3. Give the app component `REQUIRES board`; call `board_init()` once from `app_main`, then
   `board_set_backlight()`.
4. PlatformIO: `board = esp32-p4`, `framework = espidf`, 16MB partition table with offset 0xa000
   (see `sdkconfig.board`). See this project's `platformio.ini` and `partitions.csv`.
5. Edits to `sdkconfig.board` are only picked up when PlatformIO re-merges: touch the project's
   `sdkconfig.defaults` or delete `sdkconfig.<env>`.

## Files
| File | What |
| --- | --- |
| `board_pins.h` | every pin and panel constant; only `<stdint.h>`, so anything may include it |
| `board_power.*` | DSI PHY LDO (channel 3, 2.5 V) |
| `board_jd9165.*` | the JD9165 panel driver: the component's source with **the vendor's New_Panel init table** (the component's default does not configure the panel) and no blocking ID read |
| `board_display.*` | DSI bus (2 lanes, 750 Mbps), JD9165 panel with one PSRAM frame buffer (cleared to black), reset on GPIO0, LEDC backlight |
| `board_touch.*` | the shared I2C bus (GPIO7/8) and the GT911 |
| `board_lvgl.*` | esp_lvgl_port setup: partial rendering in 2 x 40-line strips in PSRAM, two draw units (both cores), touch read every 10 ms, `board_lvgl_lock/unlock` |
| `board.h/.c` | `board_init()` umbrella |
| `sdkconfig.board` | chip revision, 360 MHz, 16 MB flash, hex PSRAM 200 MHz + XIP, LVGL 9 memory/fonts |
| `idf_component.yml` | lvgl 9.2, esp_lvgl_port **~2.7** (2.9 needs a newer IDF/LVGL), jd9165, gt911 |

## Still to come (P8)
`board_audio.*` (ES8311 + PA on GPIO11) and `board_sd.*` (SDMMC 4-bit). Pins are already in `board_pins.h`
(marked `[E]`: taken from the Espressif EV-board BSP the vendor reuses, not yet confirmed on this PCB).
Wi-Fi is not here: the ESP32-C6 is reached through ESP-Hosted, configured by the application
(pins in `board_pins.h`, Kconfig in the app's `sdkconfig.defaults`).

## Official data
Guition's own download area: https://pan.jczn1688.com/directlink/1/HMI%20display/ (the JC1060P470C_I_W_Y package has schematic, spec, burn images and the New_Panel / Old_Panel demos).
Community mirrors on GitHub are older and only carry the old-panel set.

## Panel generations (read this before blaming the hardware)
Two panels were sold as JC1060P470C. The one this folder drives is the newer one: the back-of-shell label has "V2" after the
material number. Its init table and 136-pixel horizontal back porch are the vendor's "New_Panel" ones. The older panel needs
the vendor's "Old_Panel" table (different gamma and gate-driver registers) and a 160-pixel back porch. Using the wrong table
on a panel shows **random vertical stripes** that follow the content; it does no permanent damage.
Image sticking: faint column ghosts of any picture that stays fixed for hours (bars, a stripe pattern) can linger and fade
after an hour or two of changing content. Do not show a fixed test pattern on every boot.

## Gotchas
- The panel reset is wired to GPIO0 (`BOARD_LCD_RESET_GPIO`), so a warm reset or flash is fine. Without that pulse the panel keeps
  its state and the DSI command FIFO never drains (the init hangs).
- Adding a `.c` file to `board/` needs `touch CMakeLists.txt` (the file list is globbed at configure time).
- Supply >= 600 mA or the panel browns out and resets.
- Chip rev < v3 is selected (this board reports v1.3); a v3.x board needs `ESP32P4_SELECTS_REV_V3` and PlatformIO board `esp32-p4_r3`.
- LVGL calls from any task but LVGL's own need `board_lvgl_lock()`.
- Internal RAM is tight with Wi-Fi/TLS/MQTT (about 40 KB was left with the strips in internal RAM): keep the LVGL strips in PSRAM.
- Anything that runs Wi-Fi handlers on the default event loop needs `CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=8192` (a scan result handler
  with a 32-entry record array overflowed the 4 KB default).
