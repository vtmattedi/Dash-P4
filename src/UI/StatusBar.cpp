#include "StatusBar.h"

#include <stdio.h>
#include <time.h>

#include "App/Notifications.h"
#include "StatusIcons.h"
#include "Theme.h"

namespace
{
constexpr uint32_t kMessageMs = 4000;

StatusIcons g_icons;
lv_obj_t *g_clock;
lv_obj_t *g_message;
uint32_t g_messageUntil = 0; // lv_tick_get() deadline; 0 = none
} // namespace

void Notify(NotifySeverity severity, const std::string &, const std::string &title, const std::string &body)
{
    if (g_message == nullptr)
    {
        return;
    }
    const lv_color_t colour = severity == NotifySeverity::Crit   ? UI_COL_DANGER
                              : severity == NotifySeverity::Warn ? UI_COL_WARN
                              : severity == NotifySeverity::Ok   ? UI_COL_OK
                                                                 : UI_COL_TEXT;
    std::string text = title;
    if (!body.empty())
    {
        text += " - " + body;
    }
    Theme_textColor(g_message, colour);
    Theme_setText(g_message, text.c_str());
    g_messageUntil = lv_tick_get() + kMessageMs;
    if (g_messageUntil == 0)
    {
        g_messageUntil = 1;
    }
}

lv_obj_t *StatusBar_create(lv_obj_t *parent)
{
    lv_obj_t *bar = lv_obj_create(parent);
    Theme_plain(bar);
    lv_obj_set_size(bar, BOARD_LCD_H_RES, UI_STATUSBAR_H);
    lv_obj_set_pos(bar, 0, 0);
    Theme_bgColor(bar, UI_COL_SURFACE);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    Theme_borderColor(bar, UI_COL_BORDER);
    lv_obj_set_style_pad_hor(bar, UI_GUTTER, 0);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(bar, 14, 0);

    g_clock = Theme_label(bar, "--:--", UI_FONT_L, UI_COL_TEXT);
    g_message = Theme_label(bar, "", UI_FONT_M, UI_COL_TEXT);
    lv_label_set_long_mode(g_message, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(g_message, 1);
    StatusIcons_create(g_icons, bar, UI_STATUSBAR_H - 2);

    StatusBar_refresh();
    return bar;
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


    StatusIcons_refresh(g_icons);

    if (g_messageUntil != 0 && static_cast<int32_t>(lv_tick_get() - g_messageUntil) >= 0)
    {
        g_messageUntil = 0;
        Theme_setText(g_message, "");
    }
}
