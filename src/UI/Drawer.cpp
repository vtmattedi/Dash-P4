#include "Drawer.h"

#include <stdio.h>

#include "App/AcClient.h"
#include "App/FeedSync.h"
#include "App/LightControl.h"
#include "App/Notifications.h"
#include "App/Prefs.h"
#include "Rest.h"
#include "Theme.h"
#include "Ui.h"
#include "board.h"

namespace
{
constexpr int kPanelW = 580; // three tiles across, anchored under the status icons
constexpr int kPanelH = 398;
constexpr int kTileW = 168;
constexpr int kTileH = 120;
constexpr int kSwipeUpPx = 50;

struct Tile
{
    lv_obj_t *button = nullptr;
    lv_obj_t *icon = nullptr;
    lv_obj_t *caption = nullptr;
};

lv_obj_t *g_scrim;
lv_obj_t *g_panel;
lv_obj_t *g_slider;
lv_obj_t *g_sliderValue;
lv_timer_t *g_timer;
Tile g_light, g_ac;
bool g_open = false;
int32_t g_pressY = 0;

void paint(const Tile &tile, bool active, lv_color_t activeColour, const char *caption)
{
    lv_obj_set_style_bg_color(tile.button, active ? activeColour : UI_COL_SURFACE_ALT, 0);
    lv_obj_set_style_text_color(tile.icon, active ? lv_color_hex(0x10141A) : UI_COL_TEXT_DIM, 0);
    lv_obj_set_style_text_color(tile.caption, active ? lv_color_hex(0x10141A) : UI_COL_TEXT, 0);
    Theme_setText(tile.caption, caption);
}

void refresh()
{
    switch (LightControl_state())
    {
    case LIGHT_ON:
        paint(g_light, true, UI_COL_WARN, "Light on");
        break;
    case LIGHT_OFF:
        paint(g_light, false, UI_COL_WARN, "Light off");
        break;
    case LIGHT_UNKNOWN:
        paint(g_light, false, UI_COL_WARN, "Light ?");
        break;
    default:
        paint(g_light, false, UI_COL_WARN, "No light");
        break;
    }
    if (!AcClient_bound())
    {
        paint(g_ac, false, UI_COL_INFO, "No AC");
    }
    else
    {
        const bool on = AcClient_powerOn();
        paint(g_ac, on, UI_COL_INFO, on ? "AC on" : "AC off");
    }
}

void slideTo(int32_t y, lv_anim_completed_cb_t done)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g_panel);
    lv_anim_set_exec_cb(&a, [](void *obj, int32_t v) { lv_obj_set_y(static_cast<lv_obj_t *>(obj), v); });
    lv_anim_set_values(&a, lv_obj_get_y(g_panel), y);
    lv_anim_set_duration(&a, 180);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, done);
    lv_anim_start(&a);
}

void onLight(lv_event_t *)
{
    if (!LightControl_toggle())
    {
        Notify(NotifySeverity::Warn, "drawer", "Light", "Nothing is bound or no transport is connected");
    }
    refresh();
}

void onAc(lv_event_t *)
{
    if (!AcClient_togglePower())
    {
        Notify(NotifySeverity::Warn, "drawer", "Air conditioner", "Nothing is bound or no transport is connected");
    }
    refresh();
}

void onRest(lv_event_t *)
{
    Drawer_close();
    Rest_now();
}

void onSync(lv_event_t *)
{
    FeedSync_pollNow();
    Notify(NotifySeverity::Info, "drawer", "Syncing the feed");
    Drawer_close();
}

void onSettings(lv_event_t *)
{
    Drawer_close();
    Ui_show(UI_PAGE_SETTINGS);
}

void onBrightness(lv_event_t *e)
{
    const int level = lv_slider_get_value(lv_event_get_target_obj(e));
    Prefs_setBacklightActive(level);
    board_set_backlight(static_cast<uint8_t>(level));
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", level * 100 / 255);
    Theme_setText(g_sliderValue, buf);
}

void onPanelPress(lv_event_t *)
{
    lv_point_t p = {0, 0};
    lv_indev_get_point(lv_indev_active(), &p);
    g_pressY = p.y;
}

void onPanelRelease(lv_event_t *)
{
    lv_point_t p = {0, 0};
    lv_indev_get_point(lv_indev_active(), &p);
    if (g_pressY - p.y >= kSwipeUpPx)
    {
        Drawer_close();
    }
}

Tile makeTile(lv_obj_t *parent, const char *icon, const char *caption, lv_event_cb_t cb)
{
    Tile tile;
    tile.button = lv_obj_create(parent);
    lv_obj_remove_style_all(tile.button);
    lv_obj_set_size(tile.button, kTileW, kTileH);
    lv_obj_set_style_radius(tile.button, 16, 0);
    lv_obj_set_style_bg_color(tile.button, UI_COL_SURFACE_ALT, 0);
    lv_obj_set_style_bg_opa(tile.button, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(tile.button, UI_COL_SURFACE_HI, LV_STATE_PRESSED);
    lv_obj_add_flag(tile.button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(tile.button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(tile.button, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(tile.button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(tile.button, 10, 0);
    lv_obj_add_event_cb(tile.button, cb, LV_EVENT_CLICKED, nullptr);
    tile.icon = Theme_label(tile.button, icon, UI_FONT_XL, UI_COL_TEXT_DIM);
    tile.caption = Theme_label(tile.button, caption, UI_FONT_M, UI_COL_TEXT);
    return tile;
}

void finishClose(lv_anim_t *)
{
    lv_obj_add_flag(g_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(g_timer);
}
} // namespace

void Drawer_create()
{
    g_scrim = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_scrim);
    lv_obj_set_size(g_scrim, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_add_flag(g_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(g_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(g_scrim, [](lv_event_t *) { Drawer_close(); }, LV_EVENT_CLICKED, nullptr);

    g_panel = lv_obj_create(g_scrim);
    lv_obj_remove_style_all(g_panel);
    lv_obj_set_size(g_panel, kPanelW, kPanelH);
    lv_obj_set_pos(g_panel, BOARD_LCD_H_RES - kPanelW, -kPanelH);
    lv_obj_set_style_radius(g_panel, 20, 0);
    lv_obj_set_style_border_width(g_panel, 1, 0);
    lv_obj_set_style_border_side(g_panel, LV_BORDER_SIDE_FULL, 0);
    lv_obj_set_style_bg_color(g_panel, UI_COL_SURFACE, 0);
    lv_obj_set_style_bg_opa(g_panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_panel, UI_COL_BORDER, 0);
    lv_obj_set_style_pad_all(g_panel, UI_GUTTER, 0);
    lv_obj_add_flag(g_panel, LV_OBJ_FLAG_CLICKABLE); // swallow taps so they do not reach the scrim
    lv_obj_remove_flag(g_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(g_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(g_panel, 18, 0);
    lv_obj_add_event_cb(g_panel, onPanelPress, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(g_panel, onPanelRelease, LV_EVENT_RELEASED, nullptr);

    lv_obj_t *tiles = Theme_row(g_panel, kPanelW - 2 * UI_GUTTER, 2 * kTileH + 14);
    lv_obj_set_flex_flow(tiles, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(tiles, 14, 0);
    g_light = makeTile(tiles, UI_ICON_BULB, "Light", onLight);
    g_ac = makeTile(tiles, UI_ICON_SNOWFLAKE, "AC", onAc);
    makeTile(tiles, LV_SYMBOL_POWER, "Rest", onRest);
    makeTile(tiles, LV_SYMBOL_REFRESH, "Sync feed", onSync);
    makeTile(tiles, LV_SYMBOL_SETTINGS, "Settings", onSettings);

    lv_obj_t *row = Theme_row(g_panel, kPanelW - 2 * UI_GUTTER, 48);
    lv_obj_set_style_pad_gap(row, 16, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    Theme_label(row, "Brightness", UI_FONT_M, UI_COL_TEXT_DIM);
    g_slider = lv_slider_create(row);
    lv_obj_set_size(g_slider, 270, 14);
    lv_slider_set_range(g_slider, 8, 255);
    lv_obj_set_style_bg_color(g_slider, UI_COL_SURFACE_HI, LV_PART_MAIN);
    lv_obj_set_style_bg_color(g_slider, UI_COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(g_slider, UI_COL_ACCENT, LV_PART_KNOB);
    lv_obj_set_style_pad_all(g_slider, 8, LV_PART_KNOB);
    lv_obj_add_event_cb(g_slider, onBrightness, LV_EVENT_VALUE_CHANGED, nullptr);
    g_sliderValue = Theme_label(row, "", UI_FONT_M, UI_COL_TEXT);

    g_timer = lv_timer_create([](lv_timer_t *) { refresh(); }, 400, nullptr);
    lv_timer_pause(g_timer);
}

void Drawer_open()
{
    if (g_open || g_scrim == nullptr)
    {
        return;
    }
    g_open = true;
    const int level = Prefs_get().blActive;
    lv_slider_set_value(g_slider, level, LV_ANIM_OFF);
    char buf[8];
    snprintf(buf, sizeof(buf), "%d%%", level * 100 / 255);
    Theme_setText(g_sliderValue, buf);
    refresh();
    lv_obj_remove_flag(g_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_timer_resume(g_timer);
    slideTo(0, nullptr);
}

void Drawer_close()
{
    if (!g_open)
    {
        return;
    }
    g_open = false;
    slideTo(-kPanelH, finishClose);
}

bool Drawer_isOpen()
{
    return g_open;
}

int Drawer_left()
{
    return BOARD_LCD_H_RES - kPanelW;
}
