#include "Pages.h"

#if DASH_PC_SCREEN

#include "PcLink/PcLink.h"
#include "UI/Theme.h"
#include "usb_screen.h"

// The PC tab: the status of the Windows display and the two things to do about it. Opening the tab already asks
// for the PC (Ui_show -> PcLink_show), so when the PC streams this page is only seen for a moment.

namespace
{
lv_obj_t *g_state;
lv_obj_t *g_hint;
lv_obj_t *g_open;
lv_obj_t *g_close;

void onOpen(lv_event_t *)
{
    PcLink_show();
    PagePc_refresh(true);
}

void onClose(lv_event_t *)
{
    PcLink_close();
    PagePc_refresh(true);
}
} // namespace

lv_obj_t *PagePc_create(lv_obj_t *parent)
{
    lv_obj_t *page = Theme_column(parent, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(page, UI_GUTTER, 0);
    lv_obj_set_style_pad_gap(page, UI_GUTTER, 0);

    lv_obj_t *card = Theme_card(page, lv_pct(100), LV_SIZE_CONTENT);
    Theme_cardHeader(card, "PC screen");
    g_state = Theme_label(card, "", UI_FONT_XL, UI_COL_TEXT);
    g_hint = Theme_label(card, "", UI_FONT_M, UI_COL_TEXT_DIM);
    lv_label_set_long_mode(g_hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_hint, lv_pct(100));

    lv_obj_t *row = Theme_row(card, lv_pct(100), 64);
    g_open = Theme_button(row, "Open PC screen", 260, 52, Tone::Accent);
    lv_obj_add_event_cb(g_open, onOpen, LV_EVENT_CLICKED, nullptr);
    g_close = Theme_button(row, "Close connection", 260, 52, Tone::Danger);
    lv_obj_add_event_cb(g_close, onClose, LV_EVENT_CLICKED, nullptr);

    Theme_label(card,
                "While the PC is shown, tap the tab at the left edge of its picture for Rest, Settings, Disconnect or Cancel. "
                "Disconnecting takes the display away from the PC; this tab connects it again.",
                UI_FONT_S, UI_COL_TEXT_FAINT);
    PagePc_refresh(true);
    return page;
}

void PagePc_refresh(bool)
{
    const char *state;
    const char *hint;
    bool canOpen = true;
    bool canClose = true;
    if (!PcLink_connected())
    {
        state = "Connection closed";
        hint = "The PC no longer has this panel as a display.";
        canClose = false;
    }
    else
    {
        switch (PcLink_link())
        {
        case USB_SCREEN_DETACHED:
            state = "Waiting for the PC";
            hint = "Connect the PC to the USB-C port marked high-speed (9 in Guition's spec sheet). The PC must be awake.";
            canOpen = false;
            break;
        case USB_SCREEN_MOUNTED:
            state = "Connected, no picture yet";
            hint = "Windows sees the device but sends no frames: install Espressif's USB Extended Display driver and "
                   "turn the display on under Settings > System > Display.";
            canOpen = false;
            break;
        default:
            state = "PC ready";
            hint = "The PC is streaming.";
            break;
        }
    }
    Theme_setText(g_state, state);
    Theme_setText(g_hint, hint);
    if (canOpen)
    {
        lv_obj_remove_flag(g_open, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(g_open, LV_OBJ_FLAG_HIDDEN);
    }
    if (canClose)
    {
        lv_obj_remove_flag(g_close, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(g_close, LV_OBJ_FLAG_HIDDEN);
    }
}

#endif
