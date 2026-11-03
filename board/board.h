#pragma once

// Umbrella header for the JC1060P470C board support.
//
//   board_init()  powers the panel, brings up the shared I2C bus, the DSI panel,
//                 the backlight (left OFF), the touch controller and LVGL.
//
// After it returns, LVGL is running in its own task: every LVGL call from any other
// task must sit between board_lvgl_lock() and board_lvgl_unlock().

#include "board_pins.h"
#include "board_display.h"
#include "board_touch.h"
#include "board_lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Bring up everything above, in dependency order. Idempotent.
esp_err_t board_init(void);

#ifdef __cplusplus
}
#endif
