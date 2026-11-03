#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Start esp_lvgl_port, register the DSI display (tear-free, two PSRAM frame
/// buffers) and the touch pointer. Needs board_display_init() and board_touch_init().
esp_err_t board_lvgl_start(void);

/// Completed LVGL refreshes since start (diagnostics).
uint32_t board_lvgl_frames(void);

lv_display_t *board_lvgl_display(void);
lv_indev_t *board_lvgl_touch(void);

/// Hand the panel and the touch controller to another producer (pause = true), or take them back.
/// Paused, LVGL keeps running (timers, events, app logic) but renders nothing and reads no touches; whatever
/// was already invalidated is drawn first, and the last copy has finished when this returns. Resuming
/// redraws the whole screen and counts as user activity (idle timers restart). Takes the LVGL lock: do not
/// call it with the lock held by another task that waits on you.
esp_err_t board_lvgl_pause(bool pause);
bool board_lvgl_paused(void);

/// Take the LVGL mutex. timeout_ms 0 waits forever. Returns false on timeout.
/// Reentrant; the LVGL task itself already holds it inside its callbacks and timers.
bool board_lvgl_lock(uint32_t timeout_ms);
void board_lvgl_unlock(void);

#ifdef __cplusplus
}
#endif
