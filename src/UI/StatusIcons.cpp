#include "StatusIcons.h"

#include <stdio.h>
#include <string.h>

#include "App/AcClient.h"
#include "App/Battery.h"
#include "App/Bindings.h"
#include "App/LightControl.h"
#include "App/Net.h"
#include "App/SysMon.h"
#include "Theme.h"

void StatusIcons_create(StatusIcons &icons, lv_obj_t *parent, int32_t height)
{
    icons.row = Theme_row(parent, LV_SIZE_CONTENT, height);
    lv_obj_set_style_pad_gap(icons.row, 20, 0);
    lv_obj_set_flex_align(icons.row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // Memory monitor first, small and dim.
    icons.ram = Theme_label(icons.row, "RAM --", UI_FONT_S, UI_COL_TEXT_DIM);
    icons.psram = Theme_label(icons.row, "PSRAM --", UI_FONT_S, UI_COL_TEXT_DIM);
    icons.wifi = Theme_label(icons.row, LV_SYMBOL_WIFI, UI_FONT_L, UI_COL_TEXT_FAINT);
    icons.mqtt = Theme_label(icons.row, UI_ICON_EXCHANGE, UI_FONT_L, UI_COL_TEXT_FAINT);
    icons.ac = Theme_label(icons.row, UI_ICON_SNOWFLAKE, UI_FONT_L, UI_COL_TEXT_FAINT);
    icons.light = Theme_label(icons.row, UI_ICON_BULB, UI_FONT_L, UI_COL_TEXT_FAINT);
    icons.door = Theme_label(icons.row, UI_ICON_DOOR_CLOSED, UI_FONT_L, UI_COL_TEXT_FAINT);

    // Battery: symbol, percentage, and a bolt while charging. Hidden until a battery is seen.
    icons.battery = Theme_row(icons.row, LV_SIZE_CONTENT, height);
    lv_obj_set_style_pad_gap(icons.battery, 6, 0);
    lv_obj_set_flex_align(icons.battery, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    icons.bolt = Theme_label(icons.battery, LV_SYMBOL_CHARGE, UI_FONT_M, UI_COL_OK);
    lv_obj_t *symbol = Theme_label(icons.battery, LV_SYMBOL_BATTERY_FULL, UI_FONT_L, UI_COL_TEXT_DIM);
    icons.batteryText = Theme_label(icons.battery, "", UI_FONT_M, UI_COL_TEXT_DIM);
    lv_obj_set_user_data(icons.battery, symbol); // the level symbol, swapped by refresh
    lv_obj_add_flag(icons.battery, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(icons.bolt, LV_OBJ_FLAG_HIDDEN);
}

static void paint(lv_obj_t *label, lv_color_t colour) { lv_obj_set_style_text_color(label, colour, 0); }

void StatusIcons_refresh(const StatusIcons &icons)
{
    if (!icons.row)
    {
        return;
    }
    // Memory, as the share in use. The internal pool is what runs out first.
    const SysMonState mem = SysMon_state();
    char text[24];
    if (mem.internalUsedPct >= 0)
    {
        snprintf(text, sizeof(text), "RAM %d%%", mem.internalUsedPct);
        Theme_setText(icons.ram, text);
        paint(icons.ram, mem.internalUsedPct >= 90 ? UI_COL_DANGER : mem.internalUsedPct >= 80 ? UI_COL_WARN : UI_COL_TEXT_DIM);
    }
    if (mem.psramUsedPct >= 0)
    {
        snprintf(text, sizeof(text), "PSRAM %d%%", mem.psramUsedPct);
        Theme_setText(icons.psram, text);
        paint(icons.psram, mem.psramUsedPct >= 90 ? UI_COL_DANGER : mem.psramUsedPct >= 80 ? UI_COL_WARN : UI_COL_TEXT_DIM);
    }

    const NetState net = Net_state();
    const bool trying = net.connection == NightMare::ConnectionState::CONNECTING ||
                        net.connection == NightMare::ConnectionState::DISCOVERING;

    paint(icons.wifi, net.wifiIp ? UI_COL_OK : trying ? UI_COL_WARN : UI_COL_DANGER);

    lv_color_t broker = UI_COL_TEXT_FAINT;
    if (net.mqtt)
    {
        broker = UI_COL_OK;
    }
    else if (net.espNow)
    {
        broker = UI_COL_INFO;
    }
    else if (net.wifiIp && trying)
    {
        broker = UI_COL_WARN;
    }
    else if (net.wifiIp)
    {
        broker = UI_COL_DANGER;
    }
    paint(icons.mqtt, broker);

    lv_color_t ac = UI_COL_TEXT_FAINT;
    if (AcClient_bound() && !AcClient_isStale())
    {
        ac = AcClient_powerOn() ? UI_COL_INFO : UI_COL_TEXT_DIM;
    }
    paint(icons.ac, ac);

    const bool lit = LightControl_state() == LIGHT_ON || Rgb_on();
    paint(icons.light, lit ? UI_COL_WARN : UI_COL_TEXT_DIM);

    const int door = Bindings_doorOpen();
    paint(icons.door, door == 1 ? UI_COL_DANGER : door == 0 ? UI_COL_OK : UI_COL_TEXT_FAINT);
    const char *doorIcon = door == 1 ? UI_ICON_DOOR_OPEN : UI_ICON_DOOR_CLOSED;
    if (strcmp(lv_label_get_text(icons.door), doorIcon) != 0)
    {
        lv_label_set_text_static(icons.door, doorIcon);
    }

    // Battery.
    const BatteryState battery = Battery_state();
    if (!battery.present)
    {
        lv_obj_add_flag(icons.battery, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(icons.battery, LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *symbol = static_cast<lv_obj_t *>(lv_obj_get_user_data(icons.battery));
    const char *level = battery.percent >= 88   ? LV_SYMBOL_BATTERY_FULL
                        : battery.percent >= 63 ? LV_SYMBOL_BATTERY_3
                        : battery.percent >= 38 ? LV_SYMBOL_BATTERY_2
                        : battery.percent >= 13 ? LV_SYMBOL_BATTERY_1
                                                : LV_SYMBOL_BATTERY_EMPTY;
    if (strcmp(lv_label_get_text(symbol), level) != 0)
    {
        lv_label_set_text_static(symbol, level);
    }
    const lv_color_t colour = battery.percent >= 30 ? UI_COL_OK : battery.percent >= 15 ? UI_COL_WARN : UI_COL_DANGER;
    paint(symbol, colour);
    paint(icons.batteryText, battery.charging ? UI_COL_OK : colour);
    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", battery.percent);
    Theme_setText(icons.batteryText, pct);
    if (battery.charging)
    {
        lv_obj_remove_flag(icons.bolt, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(icons.bolt, LV_OBJ_FLAG_HIDDEN);
    }
}
