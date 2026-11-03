#pragma once
#include "esp_err.h"
#include "esp_lcd_panel_dev.h"
#include "esp_lcd_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/// Same contract as esp_lcd_new_panel_jd9165() (config struct: jd9165_vendor_config_t from the component's
/// header), but with this board's init table and without the ID read that can hang. See board_jd9165.c.
esp_err_t board_lcd_new_panel_jd9165(const esp_lcd_panel_io_handle_t io, const esp_lcd_panel_dev_config_t *panel_dev_config,
                                     esp_lcd_panel_handle_t *ret_panel);

#ifdef __cplusplus
}
#endif
