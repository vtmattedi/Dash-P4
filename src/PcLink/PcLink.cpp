#include "PcLink.h"

#include <atomic>

#include "board.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

namespace
{
const char *TAG = "pclink";

constexpr uint32_t kTickMs = 100;
constexpr int kMaxListeners = 4;
// The hand-back tab: flush with the left edge, vertically centred -- the least busy spot of a Windows desktop
// (the taskbar, window buttons and scroll bars are on the other edges).
constexpr uint16_t kTabW = 30;
constexpr uint16_t kTabH = 96;

PcLinkOptions g_options;
bool g_started = false;

// Set from the USB screen's tasks, consumed on the LVGL task.
std::atomic<bool> g_gesture{false};
std::atomic<bool> g_tabTapped{false};
std::atomic<int> g_menuPick{0}; // a usb_screen_icon_t, 0 = none
std::atomic<bool> g_linkEvent{false};

usb_screen_link_t g_link = USB_SCREEN_DETACHED;
bool g_showing = false;
// Both are forgotten whenever the PC stops streaming, so the next connection starts from the automatic rule.
bool g_dismissed = false;     // the user sent the PC away (gesture, PcLink_hide())
bool g_requested = false;     // the user asked for the PC (PcLink_show())
int64_t g_requestedAtUs = 0;  // counts as a fresh frame for the idle rule

void (*g_menuHandler)(PcLinkAction) = nullptr;

PcLinkListener g_listeners[kMaxListeners];
int g_listenerCount = 0;

void onUsbEvent(usb_screen_event_t event, void *)
{
    if (event == USB_SCREEN_EVENT_GESTURE)
    {
        g_gesture.store(true);
    }
    else if (event == USB_SCREEN_EVENT_TAB)
    {
        g_tabTapped.store(true);
    }
    else if (event == USB_SCREEN_EVENT_MENU)
    {
        g_menuPick.store(static_cast<int>(usb_screen_menu_pick()));
    }
    else
    {
        g_linkEvent.store(true);
    }
}

void applyTab()
{
    if (g_options.tab && g_options.touch)
    {
        const usb_screen_rect_t tab = {0, static_cast<uint16_t>((BOARD_LCD_V_RES - kTabH) / 2), kTabW, kTabH};
        usb_screen_set_tab(&tab);
    }
    else
    {
        usb_screen_set_tab(nullptr);
    }
}

void notifyListeners()
{
    for (int i = 0; i < g_listenerCount; ++i)
    {
        g_listeners[i]();
    }
}

bool idleExpired()
{
    if (g_options.idleReturnSec == 0)
    {
        return false;
    }
    usb_screen_stats_t stats;
    usb_screen_get_stats(&stats);
    const int64_t last = stats.last_frame_us > g_requestedAtUs ? stats.last_frame_us : g_requestedAtUs;
    return esp_timer_get_time() - last > static_cast<int64_t>(g_options.idleReturnSec) * 1000000;
}

void setShowing(bool on)
{
    if (on == g_showing)
    {
        return;
    }
    g_showing = on;
    if (on)
    {
        // LVGL first stops drawing and reading touches, then the USB screen starts both.
        board_lvgl_pause(true);
        usb_screen_set_output(true);
        usb_screen_redraw();
    }
    else
    {
        // The USB screen returns only after its last copy and touch read; then LVGL redraws everything.
        usb_screen_set_output(false);
        board_lvgl_pause(false);
    }
    ESP_LOGI(TAG, "panel owner: %s (link %s)", on ? "PC" : "panel UI", usb_screen_link_name(g_link));
}

void evaluate()
{
    const bool wanted = g_link == USB_SCREEN_STREAMING && !g_dismissed && (g_options.autoShow || g_requested) &&
                        !idleExpired();
    setShowing(wanted);
}

// Back to the panel's UI and off the bus: the PC loses its display.
void closeNow()
{
    g_dismissed = false;
    g_requested = false;
    setShowing(false);
    usb_screen_connect(false);
    g_link = usb_screen_link();
}

void onTick(lv_timer_t *)
{
    bool changed = g_linkEvent.exchange(false);
    const usb_screen_link_t link = usb_screen_link();
    if (link != g_link)
    {
        // Forget the user's choices when a stream ends -- not on the steps of a connection that is still coming
        // up (detached -> connected -> streaming), which would cancel a PcLink_show() made a moment earlier.
        if (g_link == USB_SCREEN_STREAMING && link != USB_SCREEN_STREAMING)
        {
            g_dismissed = false;
            g_requested = false;
        }
        g_link = link;
        changed = true;
    }
    const bool wasShowing = g_showing;
    if (g_gesture.exchange(false) && g_showing)
    {
        closeNow();
        changed = true;
    }
    // A tab tap without a menu ends the session; with one, the menu is drawn on the PC's picture and sends picks.
    const bool tabTapped = g_tabTapped.exchange(false) && g_showing;
    const int pick = g_menuPick.exchange(0);
    evaluate();
    if (g_showing)
    {
        // The panel's idle timers (rest screen, dimming) only see LVGL's touches, which go to the PC now.
        lv_display_trigger_activity(nullptr);
    }
    if (changed || wasShowing != g_showing)
    {
        notifyListeners();
    }
    if (tabTapped)
    {
        closeNow();
        notifyListeners();
    }
    if (pick != 0 && g_showing)
    {
        if (pick == USB_SCREEN_ICON_DISCONNECT)
        {
            closeNow();
            notifyListeners();
        }
        else if (g_menuHandler)
        {
            g_menuHandler(pick == USB_SCREEN_ICON_REST ? PcLinkAction::Rest : PcLinkAction::Settings);
        }
    }
}
} // namespace

esp_err_t PcLink_begin(const PcLinkOptions &options)
{
    if (g_started)
    {
        return ESP_OK;
    }
    g_options = options;
    usb_screen_config_t cfg = USB_SCREEN_CONFIG_DEFAULT();
    cfg.width = BOARD_LCD_H_RES;
    cfg.height = BOARD_LCD_V_RES;
    cfg.draw = board_display_draw_frame;
    cfg.draw_idle = board_display_wait_idle;
    cfg.touch = options.touch ? board_touch() : nullptr;
    cfg.on_event = onUsbEvent;
    const esp_err_t err = usb_screen_start(&cfg);
    if (err != ESP_OK)
    {
        ESP_LOGE(TAG, "USB screen did not start: %s", esp_err_to_name(err));
        return err;
    }
    applyTab();
    lv_timer_create(onTick, kTickMs, nullptr);
    g_started = true;
    return ESP_OK;
}

usb_screen_link_t PcLink_link() { return g_link; }

const char *PcLink_linkName() { return usb_screen_link_name(g_link); }

bool PcLink_showing() { return g_showing; }

void PcLink_show()
{
    usb_screen_connect(true); // no-op when already on the bus; the PC enumerates and streams, then it is shown
    g_dismissed = false;
    g_requested = true;
    g_requestedAtUs = esp_timer_get_time();
    evaluate();
    notifyListeners();
}

void PcLink_hide()
{
    g_dismissed = true;
    g_requested = false;
    evaluate();
    notifyListeners();
}

void PcLink_close()
{
    closeNow();
    notifyListeners();
}

bool PcLink_connected() { return usb_screen_connected(); }

void PcLink_onMenu(void (*handler)(PcLinkAction))
{
    g_menuHandler = handler;
    if (handler)
    {
        static const usb_screen_icon_t items[] = {USB_SCREEN_ICON_REST, USB_SCREEN_ICON_SETTINGS,
                                                  USB_SCREEN_ICON_DISCONNECT, USB_SCREEN_ICON_CANCEL};
        usb_screen_set_menu(items, 4);
    }
    else
    {
        usb_screen_set_menu(nullptr, 0);
    }
}

void PcLink_setAuto(bool on)
{
    g_options.autoShow = on;
    if (g_started)
    {
        evaluate();
        notifyListeners();
    }
}

bool PcLink_auto() { return g_options.autoShow; }

void PcLink_onChange(PcLinkListener listener)
{
    if (listener && g_listenerCount < kMaxListeners)
    {
        g_listeners[g_listenerCount++] = listener;
    }
}

void PcLink_setTab(bool on)
{
    g_options.tab = on;
    if (g_started)
    {
        applyTab();
    }
}

bool PcLink_tab() { return g_options.tab; }
