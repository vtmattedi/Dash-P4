#pragma once
#include "lvgl.h"

/// The 44 px bar across the top: the clock at the top left (always), a line of feedback in the middle (see Notify), the
/// status icons on the right. Swiping down from it opens the quick-actions drawer.
lv_obj_t *StatusBar_create(lv_obj_t *parent);
/// Update the clock, the icons and the message timeout. Cheap; call a few times a second.
void StatusBar_refresh();
