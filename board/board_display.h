#pragma once
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Create the DSI bus, DBI control channel and the JD9165 panel (one DPI frame buffer
/// in PSRAM, cleared to black), reset it and switch it on. The backlight stays off.
esp_err_t board_display_init(void);

esp_lcd_panel_handle_t board_display_panel(void);
esp_lcd_panel_io_handle_t board_display_io(void);

/// Set the backlight, 0 (off) to 255 (full). Safe from any task.
void board_set_backlight(uint8_t level);
uint8_t board_get_backlight(void);

// --- A second producer of whole frames (the USB screen) ----------------------
// The panel copies a draw buffer into its frame buffer with DMA2D, asynchronously, and refuses a new draw
// while a copy is running. These two let a producer other than LVGL draw whole frames safely. Use them only
// while LVGL is paused (board_lvgl_pause()), so the two never draw at the same time.

/// Copy a packed full-screen RGB565 image (BOARD_LCD_H_RES x BOARD_LCD_V_RES) into the panel. Waits for the
/// previous copy first; returns once this one has started (the buffer is read until the next call or
/// board_display_wait_idle() returns).
esp_err_t board_display_draw_frame(const void *rgb565, uint16_t width, uint16_t height);
/// Wait until no copy is running.
esp_err_t board_display_wait_idle(void);

/// Called (from the copy-done interrupt) after every completed copy, LVGL's or a frame's. board_lvgl uses it
/// to tell LVGL its flush is done, the job esp_lvgl_port's own callback did.
typedef void (*board_display_done_hook_t)(void *ctx);
/// Install the panel's copy-done callback (replacing whatever esp_lvgl_port registered) with `hook` chained.
esp_err_t board_display_claim_done_callback(board_display_done_hook_t hook, void *ctx);

#ifdef __cplusplus
}
#endif
