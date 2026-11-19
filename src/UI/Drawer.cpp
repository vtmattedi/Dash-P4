#include "Drawer.h"

#include <stdio.h>
#include <stdlib.h>

#include <esp_system.h>

#include <ArduinoJson.h>

#include <NightMare.h>

#include "App/AcClient.h"
#include "App/Bindings.h"
#include "App/Registry.h"
#include "App/LedFeedback.h"
#include "App/LedStrip.h"
#include "App/LightControl.h"
#include "App/Net.h"
#include "App/Notifications.h"
#include "App/PcPower.h"
#include "App/Prefs.h"
#include "Link/LinkServer.h"
#include "Modal.h"
#include "Rest.h"
#include "Theme.h"
#include "Ui.h"
#include "board.h"

namespace
{
constexpr int kPanelW = 580; // four tiles across, anchored under the status icons
constexpr int kPanelH = 430;
constexpr int kTileW = 122; // four across
constexpr int kTileH = 108;
constexpr int kCardH = 84; // the Sherlock card under the tiles

// The bed would be U+F236, which the 28 px font does not carry yet (FONTS.md says how to add it); until it does the
// card shows the home symbol that is already there.
#ifndef UI_ICON_BED
#define UI_ICON_BED LV_SYMBOL_HOME
#endif
constexpr int kSwipeUpPx = 50;
constexpr int kZoneH = 90;        // the strip at the top right where a downward drag picks the drawer up
constexpr int kDragStartPx = 14;  // movement before it counts as a drag and not a tap
constexpr int kOpenFraction = 35; // percent of the panel that must be out for release to open it

struct Tile
{
    lv_obj_t *button = nullptr;
    lv_obj_t *icon = nullptr;
    lv_obj_t *caption = nullptr;
};

lv_indev_t *g_indev;
lv_timer_t *g_dragTimer;
bool g_tracking = false;
bool g_dragging = false;
lv_point_t g_dragStart = {0, 0};
lv_obj_t *g_scrim;
lv_obj_t *g_panel;
lv_obj_t *g_sherlock;
lv_obj_t *g_sherlockIcon;
lv_obj_t *g_sherlockTitle;
lv_obj_t *g_sherlockBar;
lv_obj_t *g_sherlockSensors;

lv_timer_t *g_timer;
Tile g_light, g_ac, g_power, g_led, g_espNow;

lv_obj_t *g_connInfo; // one line under the tiles: the broker or the ESP-NOW gateway
bool g_open = false;
int32_t g_pressY = 0;

// Disabled is not off: off is a thing we know is dark, disabled is a thing with nothing true to show (not bound, offline,
// no data yet). Dimmed, and it takes no taps.
void setDisabled(lv_obj_t *obj, bool disabled)
{
    if (disabled)
    {
        lv_obj_add_state(obj, LV_STATE_DISABLED);
    }
    else
    {
        lv_obj_remove_state(obj, LV_STATE_DISABLED);
    }
    lv_obj_set_style_opa(obj, disabled ? LV_OPA_50 : LV_OPA_COVER, 0);
}

void paint(const Tile &tile, bool active, lv_color_t activeColour, const char *caption, bool disabled = false)
{
    setDisabled(tile.button, disabled);
    active = active && !disabled;
    Theme_bgColor(tile.button, active ? activeColour : UI_COL_SURFACE_ALT);
    Theme_textColor(tile.icon, active ? lv_color_hex(0x10141A) : UI_COL_TEXT_DIM);
    Theme_textColor(tile.caption, active ? lv_color_hex(0x10141A) : UI_COL_TEXT);
    Theme_setText(tile.caption, caption);
}

void refreshLed();
Tile makeTile(lv_obj_t *parent, const char *icon, const char *caption, lv_event_cb_t cb);

// Sherlock, the bedroom board: the bar shows what its strip is doing (the colour role), the text its climate sensors.
void refreshSherlock()
{
    const bool bound = Rgb_bound();
    const std::string &device = Bindings_get(Role::Rgb).device;
    const Device *dev = bound ? Registry_find(device) : nullptr;
    const bool offline = dev != nullptr && dev->seenStatus && !dev->online;
    // Disabled is not off: off is a strip we know is dark, disabled is a card with nothing true to show.
    const bool disabled = !bound || offline || !Rgb_hasState();
    const bool on = !disabled && Rgb_on();
    setDisabled(g_sherlock, disabled);
    Theme_bgColor(g_sherlockBar, on ? lv_color_hex(Rgb_colour()) : UI_COL_SURFACE_HI);
    lv_obj_set_style_bg_opa(g_sherlockBar, disabled ? LV_OPA_TRANSP : LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(g_sherlockBar, disabled ? 1 : 0, 0);
    Theme_borderColor(g_sherlockBar, UI_COL_BORDER);
    Theme_textColor(g_sherlockIcon, on ? UI_COL_TEXT : UI_COL_TEXT_DIM);
    if (disabled)
    {
        Theme_setText(g_sherlockSensors, !bound ? "Not bound" : (offline ? "Offline" : "No data"));
        return;
    }
    const std::string temp = Registry_valueText(device, "temperature");
    const std::string hum = Registry_valueText(device, "dht_humidity");
    char buf[40];
    int n = 0;
    if (temp.empty())
    {
        n = snprintf(buf, sizeof(buf), "--\xC2\xB0""C");
    }
    else
    {
        n = snprintf(buf, sizeof(buf), "%.1f\xC2\xB0""C", atof(temp.c_str()));
    }
    if (!hum.empty() && n > 0 && n < static_cast<int>(sizeof(buf)))
    {
        snprintf(buf + n, sizeof(buf) - n, "   %d%%", static_cast<int>(atof(hum.c_str()) + 0.5));
    }
    Theme_setText(g_sherlockSensors, buf);
}

void onSherlock(lv_event_t *)
{
    if (!Rgb_setOn(!Rgb_on()))
    {
        Notify(NotifySeverity::Warn, "drawer", "Sherlock", "No transport is connected");
    }
    refreshSherlock();
}

// What the ESP-NOW gateway says about itself, as one line. The library consumes the gateway's own retained status and
// network announcements (they decide which gateway it joins), so the Registry never sees them: that part comes from
// GatewayCandidateGet(). Only the client list reaches the Registry, under the gateway's id. Rebuilt only on a change.
std::string gatewayLine()
{
    static std::string cached;
    static std::string key;
    const NightMare::GatewayCandidateStatus gw = NightMare::GatewayCandidateGet();
    if (gw.id.empty())
    {
        return "No gateway heard yet";
    }
    const Device *device = Registry_find(gw.id);
    const std::string clientsJson = device ? device->gatewayClients : std::string();
    const std::string now = gw.id + (gw.known ? "|1" : "|0") + (gw.remoteMqttReady ? "1" : "0") + (gw.localMqttReady ? "1" : "0") +
                            std::to_string(gw.channel) + "|" + gw.generation + "|" + std::to_string(clientsJson.size()) + "|" +
                            std::to_string(device ? device->revision : 0);
    if (now != key)
    {
        key = now;
        cached = gw.id + (gw.known ? "  online" : "  offline");
        if (gw.known)
        {
            if (gw.channel)
                cached += "   ch " + std::to_string(gw.channel);
            cached += std::string("   broker ") + ((gw.remoteMqttReady || gw.localMqttReady) ? "ready" : "down");
        }
        JsonDocument clients;
        if (!clientsJson.empty() && !deserializeJson(clients, clientsJson))
        {
            int total = 0, connected = 0;
            for (JsonObjectConst c : clients["clients"].as<JsonArrayConst>())
            {
                ++total;
                connected += (c["connected"] | false) ? 1 : 0;
            }
            cached += "   " + std::to_string(connected) + " of " + std::to_string(total) + " devices connected";
        }
    }
    return cached;
}

// The ESP-NOW tile is the one switch: Wi-Fi is core and MQTT is the default, so only the optional radio link is toggled here.
// The line under the tiles says what the network is doing.
void refreshConnectivity()
{
    const Prefs &prefs = Prefs_get();
    const NetState net = Net_state();
#if NM_NETWORK_ESPNOW
    if (!prefs.espNowOn)
    {
        paint(g_espNow, false, UI_COL_INFO, "ESP-NOW off");
    }
    else if (Net_applying())
    {
        paint(g_espNow, true, UI_COL_INFO, "Switching");
    }
    else
    {
        paint(g_espNow, net.espNow, UI_COL_INFO, net.espNow ? "ESP-NOW linked" : "Searching");
    }
#else
    paint(g_espNow, false, UI_COL_INFO, "No ESP-NOW", true);
#endif
    if (prefs.espNowOn)
    {
        std::string line = gatewayLine();
        if (prefs.mqttOn && !net.espNow)
        {
            line += "   (MQTT carries traffic meanwhile)";
        }
        Theme_setText(g_connInfo, line.c_str());
    }
    else if (prefs.mqttOn)
    {
        Theme_setText(g_connInfo, net.mqtt ? "Broker connected" : "Broker not reachable");
    }
    else
    {
        Theme_setText(g_connInfo, "MQTT and ESP-NOW are off: no network traffic");
    }
}

void onEspNow(lv_event_t *)
{
#if NM_NETWORK_ESPNOW
    if (Net_applying())
    {
        return;
    }
    Prefs_setEspNowOn(!Prefs_get().espNowOn);
    Net_applyPrefs();
    refreshConnectivity();
#endif
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
        paint(g_light, false, UI_COL_WARN, "Light ?", true);
        break;
    default:
        paint(g_light, false, UI_COL_WARN, "No light", true);
        break;
    }
    if (!AcClient_bound())
    {
        paint(g_ac, false, UI_COL_INFO, "No AC", true);
    }
    else if (AcClient_isStale())
    {
        paint(g_ac, false, UI_COL_INFO, "AC ?", true);
    }
    else
    {
        const bool on = AcClient_powerOn();
        paint(g_ac, on, UI_COL_INFO, on ? "AC on" : "AC off");
    }
    refreshConnectivity();
    refreshSherlock();
    refreshLed();
}

// The strip is a status light now (App/LedFeedback): the tile only switches it on or off.
void refreshLed()
{
    if (!LedStrip_ready())
    {
        paint(g_led, false, UI_COL_ACCENT, "No LED", true);
        return;
    }
    const bool on = LedFeedback_enabled();
    paint(g_led, on, UI_COL_ACCENT, on ? "LED on" : "LED off");
}

void onLedToggle(lv_event_t *)
{
    LedFeedback_setEnabled(!LedFeedback_enabled());
    refreshLed();
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

// One way to turn things off: this tile opens three choices, each an icon. The first is this panel's own screen (a touch brings
// it back); the other two are the linked PC's, so they are dimmed with no PC linked. What each does, to the PC and to this panel,
// is in App/PcPower.h: the same code answers another panel asking over the network.
ModalId g_powerModal = 0;

void onPowerChoice(lv_event_t *e)
{
    const int choice = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    Modal_close(g_powerModal);
    if (choice == 0)
    {
        Rest_screenOff();
        return;
    }
    if (choice == 3)
    {
        Modal_confirm("Reset the panel?", "The panel restarts and is unavailable for about ten seconds.", "Reset", Tone::Danger,
                      [] { esp_restart(); });
        return;
    }
    if (!(choice == 1 ? PcPower_monitorsOff() : PcPower_sleep()))
    {
        Notify(NotifySeverity::Warn, "drawer", "PC", "No PC is linked");
    }
}

void onPower(lv_event_t *)
{
    Drawer_close();
    ModalOptions options;
    options.title = "Turn off";
    options.size = ModalSize::Wide; // four tiles and a rule do not fit the 560 px dialog
    lv_obj_t *body = nullptr;
    g_powerModal = Modal_open(options, &body);
    lv_obj_t *row = Theme_row(body, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(row, 16, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const char *const kIcon[4] = {LV_SYMBOL_EYE_CLOSE, LV_SYMBOL_IMAGE, LV_SYMBOL_PAUSE, LV_SYMBOL_REFRESH};
    static const char *const kName[4] = {"Screen", "Monitors", "Sleep PC", "Reset"};
    const bool linked = LinkServer_connected();
    for (int i = 0; i < 4; ++i)
    {
        if (i == 3) // a vertical rule keeps the destructive choice apart from the three turn-off ones
        {
            lv_obj_t *rule = lv_obj_create(row);
            lv_obj_remove_style_all(rule);
            lv_obj_set_size(rule, 2, 100);
            lv_obj_set_style_bg_opa(rule, LV_OPA_COVER, 0);
            Theme_bgColor(rule, UI_COL_BORDER);
            lv_obj_remove_flag(rule, LV_OBJ_FLAG_CLICKABLE);
        }
        const Tile t = makeTile(row, kIcon[i], kName[i], onPowerChoice);
        lv_obj_set_size(t.button, 150, 130);
        if (i == 3) // last, and red: it restarts this panel
        {
            Theme_bgColor(t.button, UI_COL_DANGER);
            lv_obj_set_style_text_color(t.icon, lv_color_hex(0x2A0009), 0); // dark on the red fill
            lv_obj_set_style_text_color(t.caption, lv_color_hex(0x2A0009), 0);
        }
        lv_obj_remove_event_cb(t.button, onPowerChoice);
        lv_obj_add_event_cb(t.button, onPowerChoice, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        setDisabled(t.button, i > 0 && i < 3 && !linked); // Reset is this panel's own, so it never needs a PC
    }
    lv_obj_t *note = Theme_label(body, linked ? "Screen: this panel only, a touch wakes it. Monitors and Sleep: the linked PC."
                                              : "No PC is linked, so only this panel's screen can be turned off.",
                                 UI_FONT_S, UI_COL_TEXT_FAINT);
    lv_obj_set_width(note, lv_pct(100));
}

void onSettings(lv_event_t *)
{
    Drawer_close();
    Rest_wake();
    Ui_show(UI_PAGE_SETTINGS);
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
    Theme_bgColor(tile.button, UI_COL_SURFACE_ALT);
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

// Make the panel ready to be seen: slider and tiles current, scrim up, refresh timer running.
void prepare()
{
    // Above the rest screen too (it is created later, on the same layer).
    lv_obj_move_foreground(g_scrim);
    refresh();
    lv_obj_remove_flag(g_scrim, LV_OBJ_FLAG_HIDDEN);
    lv_timer_resume(g_timer);
}

void endDrag()
{
    if (!g_tracking)
    {
        return;
    }
    g_tracking = false;
    lv_timer_pause(g_dragTimer);
    if (!g_dragging)
    {
        return;
    }
    g_dragging = false;
    const int shown = lv_obj_get_y(g_panel) + kPanelH; // 0 hidden .. kPanelH fully out
    if (shown * 100 >= kPanelH * kOpenFraction)
    {
        g_open = true;
        slideTo(0, nullptr);
    }
    else
    {
        g_open = false;
        slideTo(-kPanelH, finishClose);
    }
}

// Follows the finger while a drag that started in the zone is going on; also notices the release, which LVGL does not
// report once the press has been taken away from the object under it.
void onDragTick(lv_timer_t *)
{
    if (lv_indev_get_state(g_indev) != LV_INDEV_STATE_PRESSED)
    {
        endDrag();
        return;
    }
    lv_point_t p = {0, 0};
    lv_indev_get_point(g_indev, &p);
    const int dy = p.y - g_dragStart.y;
    const int dx = p.x - g_dragStart.x;
    if (!g_dragging)
    {
        if (dy < kDragStartPx || dy < (dx < 0 ? -dx : dx))
        {
            return;
        }
        g_dragging = true;
        lv_indev_wait_release(g_indev); // the press is the drawer's now: no click on whatever is under the finger
        lv_anim_delete(g_panel, nullptr);
        prepare();
    }
    int y = -kPanelH + dy;
    y = y < -kPanelH ? -kPanelH : (y > 0 ? 0 : y);
    lv_obj_set_y(g_panel, y);
}

void onIndev(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_RELEASED)
    {
        endDrag();
        return;
    }
    if (g_open)
    {
        return;
    }
    lv_point_t p = {0, 0};
    lv_indev_get_point(g_indev, &p);
    if (p.x >= BOARD_LCD_H_RES - kPanelW && p.y < kZoneH)
    {
        g_tracking = true;
        g_dragging = false;
        g_dragStart = p;
        lv_timer_resume(g_dragTimer);
    }
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
    Theme_bgColor(g_panel, UI_COL_SURFACE);
    lv_obj_set_style_bg_opa(g_panel, LV_OPA_COVER, 0);
    Theme_borderColor(g_panel, UI_COL_BORDER);
    lv_obj_set_style_pad_all(g_panel, UI_GUTTER, 0);
    lv_obj_add_flag(g_panel, LV_OBJ_FLAG_CLICKABLE); // swallow taps so they do not reach the scrim
    lv_obj_remove_flag(g_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(g_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(g_panel, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_gap(g_panel, 18, 0);
    lv_obj_add_event_cb(g_panel, onPanelPress, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(g_panel, onPanelRelease, LV_EVENT_RELEASED, nullptr);

    lv_obj_t *tiles = Theme_row(g_panel, kPanelW - 2 * UI_GUTTER, kTileH);
    lv_obj_set_flex_flow(tiles, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(tiles, 14, 0);
    g_light = makeTile(tiles, UI_ICON_BULB, "Light", onLight);
    g_ac = makeTile(tiles, UI_ICON_SNOWFLAKE, "AC", onAc);
    g_power = makeTile(tiles, LV_SYMBOL_POWER, "Power", onPower);
    makeTile(tiles, LV_SYMBOL_SETTINGS, "Settings", onSettings);

    // Second row: the two switches that are not about a room, as tap tiles like the ones above.
    lv_obj_t *tiles2 = Theme_row(g_panel, kPanelW - 2 * UI_GUTTER, kTileH);
    lv_obj_set_flex_flow(tiles2, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_gap(tiles2, 14, 0);
    g_led = makeTile(tiles2, LV_SYMBOL_CHARGE, "LED", onLedToggle);
    g_espNow = makeTile(tiles2, LV_SYMBOL_WIFI, "ESP-NOW", onEspNow);

    // Sherlock: tap to switch its strip; the bar is the strip's colour, the text its temperature and humidity.
    g_sherlock = lv_obj_create(g_panel);
    lv_obj_remove_style_all(g_sherlock);
    lv_obj_set_size(g_sherlock, kPanelW - 2 * UI_GUTTER, kCardH); // as wide as the tile row
    lv_obj_set_style_radius(g_sherlock, 16, 0);
    Theme_bgColor(g_sherlock, UI_COL_SURFACE_ALT);
    lv_obj_set_style_bg_opa(g_sherlock, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(g_sherlock, UI_COL_SURFACE_HI, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(g_sherlock, 20, 0);
    lv_obj_set_style_pad_gap(g_sherlock, 18, 0);
    lv_obj_add_flag(g_sherlock, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g_sherlock, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(g_sherlock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_sherlock, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(g_sherlock, onSherlock, LV_EVENT_CLICKED, nullptr);
    g_sherlockIcon = Theme_label(g_sherlock, UI_ICON_BED, UI_FONT_XL, UI_COL_TEXT_DIM);

    lv_obj_t *info = lv_obj_create(g_sherlock);
    lv_obj_remove_style_all(info);
    lv_obj_set_size(info, 120, LV_SIZE_CONTENT);
    lv_obj_remove_flag(info, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(info, 8, 0);
    g_sherlockTitle = Theme_label(info, "Sherlock", UI_FONT_M, UI_COL_TEXT);
    g_sherlockBar = lv_obj_create(info);
    lv_obj_remove_style_all(g_sherlockBar);
    lv_obj_set_size(g_sherlockBar, 110, 8);
    lv_obj_set_style_radius(g_sherlockBar, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(g_sherlockBar, LV_OPA_COVER, 0);
    lv_obj_remove_flag(g_sherlockBar, LV_OBJ_FLAG_CLICKABLE);

    g_sherlockSensors = Theme_label(g_sherlock, "", UI_FONT_L, UI_COL_TEXT);
    lv_obj_set_flex_grow(g_sherlockSensors, 1);
    lv_obj_set_style_text_align(g_sherlockSensors, LV_TEXT_ALIGN_RIGHT, 0);
    refreshSherlock();

    // What the network is doing, as a line (no card: the only switch is the ESP-NOW tile).
    g_connInfo = Theme_label(g_panel, "", UI_FONT_S, UI_COL_TEXT_DIM);
    lv_obj_set_width(g_connInfo, kPanelW - 2 * UI_GUTTER);
    lv_label_set_long_mode(g_connInfo, LV_LABEL_LONG_DOT);
    refreshConnectivity();
    refreshLed();

    g_timer = lv_timer_create([](lv_timer_t *) { refresh(); }, 400, nullptr);
    lv_timer_pause(g_timer);

    // The pickup zone: any press in the top-right strip, whatever is drawn there, may turn into a downward drag.
    g_indev = lv_indev_get_next(nullptr);
    g_dragTimer = lv_timer_create(onDragTick, 16, nullptr);
    lv_timer_pause(g_dragTimer);
    if (g_indev != nullptr)
    {
        lv_indev_add_event_cb(g_indev, onIndev, LV_EVENT_PRESSED, nullptr);
        lv_indev_add_event_cb(g_indev, onIndev, LV_EVENT_RELEASED, nullptr);
    }
}

void Drawer_open()
{
    if (g_open || g_scrim == nullptr)
    {
        return;
    }
    g_open = true;
    prepare();
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
