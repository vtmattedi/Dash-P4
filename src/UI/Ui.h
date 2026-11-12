#pragma once
#include "lvgl.h"

// The screen manager: status bar on top, navigation rail on the left, one page at a
// time in the remaining 928x556.

enum UiPage
{
    UI_PAGE_HOME = 0,
    UI_PAGE_CALENDAR, // the feed's events and deadlines by day
    UI_PAGE_TASKS,    // Linear and WOI tasks, filtered by source and status
    UI_PAGE_NETWORK,
    UI_PAGE_SETTINGS,
    UI_PAGE_COUNT
};

/// Build the whole UI. Call once with the LVGL lock held, after board_init() and App_begin().
void Ui_begin(lv_display_t *display);

void Ui_show(UiPage page);
UiPage Ui_current();

/// LVGL's own FPS / CPU / render-time readout (bottom right of the panel). Shared by Settings > Display > "Show FPS" and
/// the console's PERF command, so they cannot disagree. LVGL task only.
bool Ui_perfShown();
void Ui_setPerf(bool on);

/// The container a page draws into (all pages are built at boot and hidden when not shown).
lv_obj_t *Ui_pageContainer(UiPage page);
