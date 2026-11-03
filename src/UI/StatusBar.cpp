#include "StatusBar.h"

#include <stdio.h>
#include <time.h>

#include "App/Net.h"
#include "App/Notifications.h"
#include "NotificationCentre.h"
#include "Theme.h"
#if DASH_PC_SCREEN
#include "PcLink/PcLink.h"
#endif

namespace
{
lv_obj_t *g_title;
lv_obj_t *g_chip;
lv_obj_t *g_clock;
lv_obj_t *g_bell;
lv_obj_t *g_badge;
lv_obj_t *g_badgeLabel;
#if DASH_PC_SCREEN
lv_obj_t *g_pc; // "show the PC" -- only while the PC streams and is not on the panel
#endif
}

lv_obj_t *StatusBar_create(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    Theme_plain(bar);
    lv_obj_set_size(bar, BOARD_LCD_H_RES, UI_STATUSBAR_H);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, UI_COL_SURFACE, 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_border_color(bar, UI_COL_BORDER, 0);
    lv_obj_set_style_pad_hor(bar, UI_GUTTER, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(bar, 14, 0);

    g_title = Theme_label(bar, "", UI_FONT_L, UI_COL_TEXT);
    lv_obj_set_flex_grow(g_title, 1);

#if DASH_PC_SCREEN
    g_pc = Theme_button(bar, LV_SYMBOL_IMAGE "  PC", 96, 36, Tone::Accent);
    lv_obj_add_event_cb(g_pc, [](lv_event_t *) { PcLink_show(); }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(g_pc, LV_OBJ_FLAG_HIDDEN);
#endif

    g_chip = Theme_chip(bar);
    g_clock = Theme_label(bar, "--:--", UI_FONT_L, UI_COL_TEXT);

    // Bell with an unread badge.
    g_bell = Theme_iconButton(bar, LV_SYMBOL_BELL, 38, Tone::Neutral);
    lv_obj_set_style_bg_opa(g_bell, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(g_bell, [](lv_event_t *) { NotificationCentre_open(); }, LV_EVENT_CLICKED, nullptr);
    g_badge = lv_obj_create(g_bell);
    lv_obj_remove_style_all(g_badge);
    lv_obj_set_size(g_badge, 20, 20);
    lv_obj_set_style_radius(g_badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_badge, UI_COL_DANGER, 0);
    lv_obj_set_style_bg_opa(g_badge, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_badge, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(g_badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(g_badge, LV_ALIGN_TOP_RIGHT, 2, -2);
    g_badgeLabel = Theme_label(g_badge, "0", UI_FONT_S, lv_color_white());
    lv_obj_center(g_badgeLabel);
    lv_obj_add_flag(g_badge, LV_OBJ_FLAG_HIDDEN);

    StatusBar_refresh();
    return bar;
}

void StatusBar_setTitle(const char *title)
{
    Theme_setText(g_title, title);
}

void StatusBar_refresh()
{
    // Clock: blank until SNTP has set it; "--:--" is honest, 01:00 on 1 January 1970 is not.
    const time_t now = time(nullptr);
    char text[16] = "--:--";
    if (now > 1700000000)
    {
        struct tm local;
        localtime_r(&now, &local);
        strftime(text, sizeof(text), "%H:%M", &local);
    }
    Theme_setText(g_clock, text);

#if DASH_PC_SCREEN
    if (PcLink_link() == USB_SCREEN_STREAMING && !PcLink_showing())
    {
        lv_obj_remove_flag(g_pc, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(g_pc, LV_OBJ_FLAG_HIDDEN);
    }
#endif

    // Which transport carries traffic.
    const NetState net = Net_state();
    if (net.active == NightMare::ConnectionType::ESP_NOW)
    {
        Theme_chipSet(g_chip, "ESP-NOW", UI_COL_INFO);
    }
    else if (net.active == NightMare::ConnectionType::MQTT)
    {
        Theme_chipSet(g_chip, "MQTT", UI_COL_OK);
    }
    else if (net.wifiIp)
    {
        Theme_chipSet(g_chip, "Wi-Fi, no broker", UI_COL_WARN);
    }
    else
    {
        Theme_chipSet(g_chip, "Offline", UI_COL_DANGER);
    }

    const uint32_t unread = Notifications_unread();
    if (unread == 0)
    {
        lv_obj_add_flag(g_badge, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        char buf[8];
        snprintf(buf, sizeof(buf), "%u", unread > 99 ? 99u : (unsigned)unread);
        Theme_setText(g_badgeLabel, buf);
        lv_obj_remove_flag(g_badge, LV_OBJ_FLAG_HIDDEN);
    }
}
