#pragma once
#include "lvgl.h"

// Hardware self-test page: colour bars (panel/colour order/DSI stability), a touch
// pad that plots the contact point (touch mapping), a backlight slider, and a
// live memory readout. Reachable from Settings -> Diagnostics once the UI exists.

/// Build the page into `parent`. Call with the LVGL lock held.
lv_obj_t *BoardTest_create(lv_obj_t *parent);
