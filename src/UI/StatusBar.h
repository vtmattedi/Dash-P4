#pragma once
#include "lvgl.h"

/// The 44 px bar across the top: page title, transport chip, clock, notification bell.
lv_obj_t *StatusBar_create(lv_obj_t *parent);
void StatusBar_setTitle(const char *title);
/// Update the clock, transport chip and unread badge. Cheap; call a few times a second.
void StatusBar_refresh();
