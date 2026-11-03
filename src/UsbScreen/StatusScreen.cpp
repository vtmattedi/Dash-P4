#include "StatusScreen.h"

#include <cstdio>

#include "lvgl.h"
#include "PcLink/PcLink.h"
#include "usb_screen.h"

namespace
{
constexpr uint32_t kBg = 0x101418;
constexpr uint32_t kText = 0xE8EAED;
constexpr uint32_t kDim = 0x8A9099;
constexpr uint32_t kAccent = 0xFF9849;
constexpr uint32_t kOk = 0x4CC38A;

lv_obj_t *g_state;
lv_obj_t *g_hint;
lv_obj_t *g_stats;
lv_obj_t *g_button;
lv_obj_t *g_buttonText;
lv_obj_t *g_close;

lv_obj_t *label(lv_obj_t *parent, const lv_font_t *font, uint32_t colour)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(colour), 0);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return l;
}

void refresh()
{
    const usb_screen_link_t link = PcLink_link();
    const bool closed = !PcLink_connected();
    if (closed)
    {
        lv_label_set_text(g_state, "Connection closed");
        lv_obj_set_style_text_color(g_state, lv_color_hex(kDim), 0);
        lv_label_set_text(g_hint, "The PC no longer has this panel as a display. Connect to bring it back.");
    }
    else switch (link)
    {
    case USB_SCREEN_DETACHED:
        lv_label_set_text(g_state, "Waiting for the PC");
        lv_obj_set_style_text_color(g_state, lv_color_hex(kAccent), 0);
        lv_label_set_text(g_hint, "Connect the PC to the USB-C port marked high-speed (number 9 in Guition's "
                                  "spec sheet).");
        break;
    case USB_SCREEN_MOUNTED:
        lv_label_set_text(g_state, "Connected - no picture yet");
        lv_obj_set_style_text_color(g_state, lv_color_hex(kAccent), 0);
        lv_label_set_text(g_hint, "Windows sees the device but sends no frames. Install Espressif's USB Extended "
                                  "Display driver (xfz1986_usb_graphic, signed), then turn the display on under "
                                  "Settings > System > Display (Extend these displays).");
        break;
    case USB_SCREEN_STREAMING:
        lv_label_set_text(g_state, "PC display ready");
        lv_obj_set_style_text_color(g_state, lv_color_hex(kOk), 0);
        lv_label_set_text(g_hint, "The PC is streaming. Tap the button to show it; while it is shown, tap the tab at "
                                  "the left edge (or hold three fingers for two seconds) to come back here and close "
                                  "the connection.");
        break;
    }
    // One primary button: connect when closed, show when the PC streams. Close is there whenever connected.
    if (closed || link == USB_SCREEN_STREAMING)
    {
        lv_label_set_text(g_buttonText, closed ? "Connect to the PC" : "Show the PC screen");
        lv_obj_remove_flag(g_button, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(g_button, LV_OBJ_FLAG_HIDDEN);
    }
    if (closed)
    {
        lv_obj_add_flag(g_close, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_remove_flag(g_close, LV_OBJ_FLAG_HIDDEN);
    }

    usb_screen_stats_t s;
    usb_screen_get_stats(&s);
    char text[160];
    snprintf(text, sizeof(text), "Frames received %lu   decoded %lu   drawn %lu   dropped %lu   errors %lu",
             static_cast<unsigned long>(s.received), static_cast<unsigned long>(s.decoded),
             static_cast<unsigned long>(s.drawn), static_cast<unsigned long>(s.dropped),
             static_cast<unsigned long>(s.errors));
    lv_label_set_text(g_stats, text);
}

void onTimer(lv_timer_t *) { refresh(); }

void onShow(lv_event_t *) { PcLink_show(); }

void onClose(lv_event_t *) { PcLink_close(); }
} // namespace

void StatusScreen_begin()
{
    lv_obj_t *screen = lv_screen_active();
    lv_obj_set_style_bg_color(screen, lv_color_hex(kBg), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(screen, 48, 0);
    lv_obj_set_flex_flow(screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(screen, 18, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_label_set_text(label(screen, &lv_font_montserrat_20, kDim), "USB SCREEN");
    g_state = label(screen, &lv_font_montserrat_48, kAccent);
    g_hint = label(screen, &lv_font_montserrat_24, kText);

    g_button = lv_button_create(screen);
    lv_obj_set_style_bg_color(g_button, lv_color_hex(kAccent), 0);
    lv_obj_set_style_pad_hor(g_button, 32, 0);
    lv_obj_set_style_pad_ver(g_button, 18, 0);
    lv_obj_add_event_cb(g_button, onShow, LV_EVENT_CLICKED, nullptr);
    g_buttonText = lv_label_create(g_button);
    lv_obj_set_style_text_font(g_buttonText, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(g_buttonText, lv_color_hex(kBg), 0);

    g_close = lv_button_create(screen);
    lv_obj_set_style_bg_color(g_close, lv_color_hex(0x2A3038), 0);
    lv_obj_set_style_pad_hor(g_close, 32, 0);
    lv_obj_set_style_pad_ver(g_close, 18, 0);
    lv_obj_add_event_cb(g_close, onClose, LV_EVENT_CLICKED, nullptr);
    lv_obj_t *closeText = lv_label_create(g_close);
    lv_obj_set_style_text_font(closeText, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(closeText, lv_color_hex(kText), 0);
    lv_label_set_text(closeText, "Close the connection");

    lv_obj_t *spacer = lv_obj_create(screen);
    lv_obj_remove_style_all(spacer);
    lv_obj_set_flex_grow(spacer, 1);

    g_stats = label(screen, &lv_font_montserrat_16, kDim);

    PcLink_onChange(refresh);
    lv_timer_create(onTimer, 1000, nullptr);
    refresh();
}
