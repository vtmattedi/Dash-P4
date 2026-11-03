#include "PcScreen.h"

#include <cctype>
#include <cstdio>
#include <string>

#include "board.h"
#include "esp_log.h"

#include "Notifications.h"
#include "Prefs.h"
#include "PcLink/PcLink.h"
#include "UI/Rest.h"
#include "UI/Theme.h"
#include "UI/Ui.h"

static const char *TAG = "pcscreen";

namespace
{
// detached | connected | streaming | showing. Read-only on the network; PCSCREEN changes it.
ManagedSensor<std::string> g_resource{"pc_screen"};

usb_screen_link_t g_lastLink = USB_SCREEN_DETACHED;
bool g_lastShowing = false;
char g_summary[96] = "Not connected";

const char *stateName()
{
    if (!PcLink_connected())
    {
        return "closed";
    }
    return PcLink_showing() ? "showing" : PcLink_linkName();
}

// --- The menu that grows out of the hand-back tab (drawn on the PC's picture by the USB screen) ----------------
//   Rest        the panel rests, the PC keeps its monitor; the touch that wakes it goes straight back to the PC
//   Settings    closes the connection (the PC loses the display) and opens Settings
//   Disconnect  closes the connection (handled by PcLink); Cancel only closes the menu
void onMenu(PcLinkAction action)
{
    if (action == PcLinkAction::Rest)
    {
        PcLink_hide();
        Rest_nowThen(PcLink_show);
    }
    else
    {
        PcLink_close();
        Ui_show(UI_PAGE_SETTINGS);
    }
}

void refreshSummary()
{
    switch (PcLink_link())
    {
    case USB_SCREEN_DETACHED:
        snprintf(g_summary, sizeof(g_summary), "%s", PcLink_connected() ? "Not connected" : "Connection closed");
        break;
    case USB_SCREEN_MOUNTED:
        snprintf(g_summary, sizeof(g_summary), "Connected, no picture (driver or display off in Windows)");
        break;
    case USB_SCREEN_STREAMING:
        snprintf(g_summary, sizeof(g_summary), "%s", PcLink_showing() ? "Streaming, on the panel now"
                                                                     : "Streaming, behind the dashboard");
        break;
    }
}

void onChange()
{
    const usb_screen_link_t link = PcLink_link();
    const bool showing = PcLink_showing();

    if (showing && !g_lastShowing)
    {
        // The rest screen dims the backlight; the PC's picture needs the normal brightness.
        Rest_wake();
    }
    if (link == USB_SCREEN_STREAMING && g_lastLink != USB_SCREEN_STREAMING)
    {
        Notify(NotifySeverity::Info, "pc", "PC display connected",
               PcLink_auto() ? "Tap the tab at the left edge for Rest / Settings / Disconnect; three fingers for two seconds closes it."
                             : "The PC button in the status bar shows it.");
    }
    else if (link != USB_SCREEN_STREAMING && g_lastLink == USB_SCREEN_STREAMING)
    {
        Notify(NotifySeverity::Info, "pc", "PC display disconnected");
    }
    g_lastLink = link;
    g_lastShowing = showing;

    refreshSummary();
    g_resource.setValue(stateName());
}

NightMareResults answer(bool ok, const std::string &text)
{
    NightMareResults res;
    res.result = ok;
    res.response = text;
    return res;
}

std::string statusJson()
{
    usb_screen_stats_t s;
    usb_screen_get_stats(&s);
    char line[300];
    snprintf(line, sizeof(line),
             "{\"link\":\"%s\",\"closed\":%s,\"showing\":%s,\"auto\":%s,\"received\":%lu,\"decoded\":%lu,\"drawn\":%lu,"
             "\"dropped\":%lu,\"errors\":%lu,\"fps\":%.1f}",
             PcLink_linkName(), PcLink_connected() ? "false" : "true", PcLink_showing() ? "true" : "false",
             PcLink_auto() ? "true" : "false",
             static_cast<unsigned long>(s.received), static_cast<unsigned long>(s.decoded),
             static_cast<unsigned long>(s.drawn), static_cast<unsigned long>(s.dropped),
             static_cast<unsigned long>(s.errors), static_cast<double>(s.fps));
    return line;
}
} // namespace

void PcScreen_begin()
{
    if (!board_lvgl_lock(0))
    {
        return;
    }
    PcLinkOptions options;
    options.autoShow = Prefs_get().pcAuto;
    options.tab = Prefs_get().pcTab;
    if (PcLink_begin(options) == ESP_OK)
    {
        PcLink_onChange(onChange);
        PcLink_onMenu(onMenu);
        g_resource.setValue(stateName());
        ESP_LOGI(TAG, "PC screen ready on the high-speed USB port (auto %s)", options.autoShow ? "on" : "off");
    }
    else
    {
        snprintf(g_summary, sizeof(g_summary), "Unavailable: the USB screen did not start");
        g_resource.setAvailable(false);
    }
    board_lvgl_unlock();
}

const char *PcScreen_summary() { return g_summary; }

NightMareResults PcScreen_command(const NightMareMessage &message)
{
    if (!board_lvgl_lock(500))
    {
        return answer(false, "UI busy, try again");
    }
    bool ok = true;
    std::string out;
    if (message.subcommand.empty())
    {
        out = statusJson();
    }
    else if (message.subcommand == "SHOW")
    {
        PcLink_show();
        out = PcLink_showing() ? "showing the PC" : std::string("the PC is not streaming (") + PcLink_linkName() + ")";
    }
    else if (message.subcommand == "HIDE")
    {
        PcLink_hide();
        out = "showing the dashboard (connection kept)";
    }
    else if (message.subcommand == "CLOSE")
    {
        PcLink_close();
        out = "connection closed; PCSCREEN SHOW reconnects";
    }
    else if (message.subcommand == "AUTO")
    {
        // args[0] is the subcommand itself (the parser fills both), so the value is args[1].
        std::string arg = message.argc >= 2 ? message.args[1] : std::string();
        for (char &c : arg)
        {
            c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
        }
        if (arg == "ON" || arg == "OFF")
        {
            Prefs_setPcAuto(arg == "ON");
            PcLink_setAuto(arg == "ON");
        }
        else if (!arg.empty())
        {
            ok = false;
        }
        out = ok ? std::string("auto ") + (PcLink_auto() ? "on" : "off") : "usage: PCSCREEN AUTO [ON|OFF]";
    }
    else
    {
        ok = false;
        out = "usage: PCSCREEN [SHOW|HIDE|CLOSE|AUTO [ON|OFF]]";
    }
    board_lvgl_unlock();
    return answer(ok, out);
}
