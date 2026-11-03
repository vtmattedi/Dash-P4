#include "Rest.h"

#include <stdio.h>
#include <time.h>

#include "App/AcClient.h"
#include "App/Bindings.h"
#include "App/LightControl.h"
#include "App/Net.h"
#include "App/Notifications.h"
#include "App/Prefs.h"
#include "App/Registry.h"
#include "NotificationCentre.h"
#include "Modal.h"
#include "Theme.h"
#include "Toast.h"
#include "board.h"

LV_FONT_DECLARE(font_clock_200);

namespace
{
lv_obj_t *g_root;
// The clock is five cells of fixed width: the font is proportional (a 1 is much narrower than a 0), so one label would
// shift sideways every minute and make the colon dance. Cells 0,1 = hours, 2 = colon, 3,4 = minutes.
constexpr int kDigitCell = 136; // widest digit of font_clock_200 (advance 2141/16) plus a little air
constexpr int kColonCell = 56;
// Where each cell is centred, in a 600 px wide clock whose middle is the colon. The two hour digits and the two minute
// digits sit 124 px apart (a bit closer than a cell), and the colon is 90 px from the nearest digit.
constexpr int kClockWidth = 600;
constexpr int kCellCentre[5] = {86, 210, 300, 390, 514};
lv_obj_t *g_clockCell[5];
lv_obj_t *g_date, *g_topLeft, *g_topRight, *g_bottomLeft, *g_bottomRight;
bool g_resting = false;
bool g_forced = false; // Rest_now(): rest even with the timeout set to never
void (*g_afterTouchWake)() = nullptr; // Rest_nowThen(): one shot

void setClock(int h1, int h2, int m1, int m2)
{
    const int digits[4] = {h1, h2, m1, m2};
    const int cells[4] = {0, 1, 3, 4};
    for (int i = 0; i < 4; ++i)
    {
        char text[2] = {digits[i] < 0 ? '-' : static_cast<char>('0' + digits[i]), 0};
        Theme_setText(g_clockCell[cells[i]], text);
    }
}

// The colon breathes: it fades down and back up once every two seconds, only while the rest screen is showing.
void animColon(void *obj, int32_t v) { lv_obj_set_style_text_opa(static_cast<lv_obj_t *>(obj), static_cast<lv_opa_t>(v), 0); }

void startColonFade()
{
    lv_anim_delete(g_clockCell[2], animColon);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g_clockCell[2]);
    lv_anim_set_exec_cb(&a, animColon);
    lv_anim_set_values(&a, LV_OPA_COVER, 40);
    lv_anim_set_duration(&a, 1000);
    lv_anim_set_playback_duration(&a, 1000);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

void stopColonFade()
{
    lv_anim_delete(g_clockCell[2], animColon);
    lv_obj_set_style_text_opa(g_clockCell[2], LV_OPA_COVER, 0);
}

void refresh()
{
    const time_t now = time(nullptr);
    char buf[80];
    if (now > 1700000000)
    {
        struct tm local;
        localtime_r(&now, &local);
        setClock(local.tm_hour / 10, local.tm_hour % 10, local.tm_min / 10, local.tm_min % 10);
        strftime(buf, sizeof(buf), "%A %d %B", &local);
        Theme_setText(g_date, buf);
    }
    else
    {
        setClock(-1, -1, -1, -1);
        Theme_setText(g_date, "");
    }

    // Top left: what is wrong.
    std::string problems;
    if (Bindings_doorOpen() == 1)
    {
        problems += "Door open\n";
    }
    const NetState net = Net_state();
    if (!net.wifiIp)
    {
        problems += "No Wi-Fi\n";
    }
    else if (net.active == NightMare::ConnectionType::AUTO)
    {
        problems += "No broker or gateway\n";
    }
    if (Notifications_activeCount() > 0 && problems.empty())
    {
        problems += Notifications_activeAt(0).title + "\n";
    }
    Theme_setText(g_topLeft, problems.c_str());

    // Top right: the air conditioner.
    buf[0] = 0;
    if (AcClient_bound() && !AcClient_isStale())
    {
        float room = 0;
        if (AcClient_roomTemperature(room))
        {
            snprintf(buf, sizeof(buf), "%.1f\xC2\xB0  %s", static_cast<double>(room), AcClient_powerOn() ? "AC on" : "AC off");
        }
        else
        {
            snprintf(buf, sizeof(buf), "%s", AcClient_powerOn() ? "AC on" : "AC off");
        }
    }
    Theme_setText(g_topRight, buf);

    // Bottom left: the chosen resource.
    buf[0] = 0;
    const Binding &rest = Bindings_get(Role::Rest);
    if (!rest.device.empty())
    {
        const ResourceState *s = Registry_value(rest.device, rest.resource);
        snprintf(buf, sizeof(buf), "%s  %s", rest.resource.c_str(), s && !s->withdrawn ? s->value.c_str() : "--");
    }
    Theme_setText(g_bottomLeft, buf);

    // Bottom right: what is switched on.
    std::string on;
    if (LightControl_state() == LIGHT_ON)
    {
        on += "Light on\n";
    }
    if (Rgb_on())
    {
        on += "Colour light on\n";
    }
    Theme_setText(g_bottomRight, on.c_str());
}

void goToRest()
{
    if (g_resting)
    {
        return;
    }
    g_resting = true;
    Modal_closeAll();
    Toast_setSuppressed(true);
    refresh();
    startColonFade();
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_root);
    board_set_backlight(static_cast<uint8_t>(Prefs_get().blRest));
}

void onTouch(lv_event_t *)
{
    void (*after)() = g_afterTouchWake;
    Rest_wake(); // clears the hook
    if (after)
    {
        after();
    }
}

void onTick(lv_timer_t *)
{
    if (g_resting)
    {
        refresh();
        return;
    }
    const int after = Prefs_get().restAfterSec;
    if (after > 0 && lv_display_get_inactive_time(nullptr) > static_cast<uint32_t>(after) * 1000u)
    {
        goToRest();
    }
}

void onNotification(const Notification &n, NotifyEvent event)
{
    // Something critical should not sit unseen behind a dimmed clock.
    if (g_resting && event == NotifyEvent::Added && n.severity == NotifySeverity::Crit)
    {
        Rest_wake();
    }
}

lv_obj_t *corner(lv_obj_t *parent, lv_align_t align, const lv_font_t *font, lv_color_t colour, lv_text_align_t text)
{
    lv_obj_t *label = Theme_label(parent, "", font, colour);
    lv_obj_set_width(label, 380);
    lv_obj_set_style_text_align(label, text, 0);
    lv_obj_align(label, align, align == LV_ALIGN_TOP_LEFT || align == LV_ALIGN_BOTTOM_LEFT ? 28 : -28,
                 align == LV_ALIGN_TOP_LEFT || align == LV_ALIGN_TOP_RIGHT ? 24 : -24);
    return label;
}
} // namespace

void Rest_begin()
{
    g_root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_root);
    lv_obj_set_size(g_root, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_set_style_bg_color(g_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_root, onTouch, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *clockBox = lv_obj_create(g_root);
    Theme_plain(clockBox);
    lv_obj_set_size(clockBox, kClockWidth, lv_font_get_line_height(&font_clock_200));
    lv_obj_remove_flag(clockBox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(clockBox, LV_ALIGN_CENTER, 0, -30);
    for (int i = 0; i < 5; ++i)
    {
        const int cell = i == 2 ? kColonCell : kDigitCell;
        g_clockCell[i] = Theme_label(clockBox, i == 2 ? ":" : "-", &font_clock_200, UI_COL_TEXT);
        lv_obj_set_width(g_clockCell[i], cell);
        lv_obj_set_style_text_align(g_clockCell[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(g_clockCell[i], kCellCentre[i] - cell / 2, 0);
    }
    g_date = Theme_label(g_root, "", UI_FONT_XL, UI_COL_TEXT_DIM);
    lv_obj_align(g_date, LV_ALIGN_CENTER, 0, 120);

    g_topLeft = corner(g_root, LV_ALIGN_TOP_LEFT, UI_FONT_L, UI_COL_WARN, LV_TEXT_ALIGN_LEFT);
    g_topRight = corner(g_root, LV_ALIGN_TOP_RIGHT, UI_FONT_L, UI_COL_INFO, LV_TEXT_ALIGN_RIGHT);
    g_bottomLeft = corner(g_root, LV_ALIGN_BOTTOM_LEFT, UI_FONT_L, UI_COL_TEXT_DIM, LV_TEXT_ALIGN_LEFT);
    g_bottomRight = corner(g_root, LV_ALIGN_BOTTOM_RIGHT, UI_FONT_L, UI_COL_WARN, LV_TEXT_ALIGN_RIGHT);

    Notifications_onChange(onNotification);
    lv_timer_create(onTick, 500, nullptr);
}

bool Rest_active() { return g_resting; }

void Rest_now()
{
    g_forced = true;
    goToRest();
}

void Rest_nowThen(void (*afterTouchWake)())
{
    Rest_now();
    g_afterTouchWake = afterTouchWake;
}

void Rest_wake()
{
    g_afterTouchWake = nullptr;
    if (!g_resting)
    {
        return;
    }
    g_resting = false;
    g_forced = false;
    stopColonFade();
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    Toast_setSuppressed(false);
    board_set_backlight(static_cast<uint8_t>(Prefs_get().blActive));
    // Waking is itself activity: without this the very next tick could put it back to rest.
    lv_display_trigger_activity(nullptr);
}
