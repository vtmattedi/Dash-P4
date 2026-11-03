#pragma once
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

/// The one I2C bus on GPIO7/8. Touch, the audio codec and the camera's SCCB all share
/// it, so it is created once here and handed out. NULL until board_i2c_bus_init() or
/// board_touch_init() has run.
i2c_master_bus_handle_t board_i2c_bus(void);
esp_err_t board_i2c_bus_init(void);

/// Create the GT911 touch controller (RST/INT are not wired on this board).
esp_err_t board_touch_init(void);
esp_lcd_touch_handle_t board_touch(void);

#ifdef __cplusplus
}
#endif
