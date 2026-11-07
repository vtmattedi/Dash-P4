#pragma once
#include "lvgl.h"

// The screen manager: status bar on top, navigation rail on the left, one page at a
// time in the remaining 928x556, and a banner above the page for live conditions.

enum UiPage
{
    UI_PAGE_HOME = 0,
    UI_PAGE_NETWORK,
    UI_PAGE_MONITOR,
#if DASH_PC_SCREEN
    UI_PAGE_PC, // the PC's display: opening the tab connects and shows it (PcLink_show)
#endif
#if DASH_CAMERA
    UI_PAGE_CAMERA, // the board's camera live, its picture controls, and the USB webcam switch
#endif
    UI_PAGE_SETTINGS,
    UI_PAGE_COUNT
};

/// Build the whole UI. Call once with the LVGL lock held, after board_init() and App_begin().
void Ui_begin(lv_display_t *display);

void Ui_show(UiPage page);
UiPage Ui_current();

/// The container a page draws into (all pages are built at boot and hidden when not shown).
lv_obj_t *Ui_pageContainer(UiPage page);
