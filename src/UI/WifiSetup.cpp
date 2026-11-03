#include "WifiSetup.h"

#include <algorithm>
#include <string>
#include <vector>

#include <NightMare.h>

#include "App/Notifications.h"
#include "Modal.h"

namespace
{
ModalId g_busy = 0;
lv_timer_t *g_poll = nullptr;
uint32_t g_started = 0;

void stopPoll()
{
    if (g_poll)
    {
        lv_timer_delete(g_poll);
        g_poll = nullptr;
    }
}

void askPassword(const std::string &ssid, bool open)
{
    if (open)
    {
        NightMare::WiFiProfile profile;
        profile.ssid = ssid;
        NightMare::WiFiApplyProfile(profile);
        Notify(NotifySeverity::Info, "network", "Joining " + ssid);
        return;
    }
    FormField password;
    password.kind = FormField::Kind::Text;
    password.key = "password";
    password.label = "Password for " + ssid;
    password.hint = "8 to 63 characters";
    password.required = true;
    Modal_form("Join network", {password}, "Join", Tone::Accent, [ssid](const FormValues &v) -> std::string {
        const std::string &pw = v.at("password");
        if (pw.size() < 8 || pw.size() > 63)
        {
            return "A WPA password is 8 to 63 characters.";
        }
        NightMare::WiFiProfile profile;
        profile.ssid = ssid;
        profile.password = pw;
        if (!NightMare::WiFiApplyProfile(profile))
        {
            return "Could not apply the network. Try again.";
        }
        Notify(NotifySeverity::Info, "network", "Joining " + ssid);
        return "";
    });
}

void showResults()
{
    struct Net
    {
        std::string ssid;
        int rssi;
        bool open;
    };
    std::vector<Net> nets;
    const int count = WiFi_scanCount();
    for (int i = 0; i < count; ++i)
    {
        NightMare::WiFiScanResult r;
        if (WiFi_scanResult(static_cast<size_t>(i), r) && !r.ssid.empty())
        {
            nets.push_back({r.ssid, r.rssi, r.authMode == WIFI_AUTH_OPEN});
        }
    }
    std::sort(nets.begin(), nets.end(), [](const Net &a, const Net &b) { return a.rssi > b.rssi; });
    // The same network shows up once per access point; keep the strongest (the list is sorted).
    std::vector<Net> unique;
    for (const Net &n : nets)
    {
        if (std::none_of(unique.begin(), unique.end(), [&n](const Net &u) { return u.ssid == n.ssid; }))
        {
            unique.push_back(n);
        }
    }
    nets.swap(unique);

    std::vector<ListEntry> entries;
    for (const Net &n : nets)
    {
        ListEntry e;
        e.label = n.ssid;
        e.detail = std::to_string(n.rssi) + " dBm";
        e.tone = n.rssi > -60 ? Tone::Ok : n.rssi > -75 ? Tone::Warn : Tone::Danger;
        entries.push_back(e);
    }
    Modal_list("Wi-Fi networks", entries,
               [nets](size_t index) {
                   if (index < nets.size())
                   {
                       askPassword(nets[index].ssid, nets[index].open);
                   }
               },
               "No networks found.", ModalSize::Dialog);
    WiFi_clearScanResults();
}

void onPoll(lv_timer_t *)
{
    const bool done = WiFi_scanComplete();
    const bool timedOut = lv_tick_elaps(g_started) > 15000;
    if (!done && !timedOut)
    {
        return;
    }
    stopPoll();
    Modal_close(g_busy);
    g_busy = 0;
    if (done)
    {
        showResults();
    }
    else
    {
        WiFi_abortScan();
        Modal_message("Scan timed out", "The radio did not finish scanning. Try again.", Tone::Warn);
    }
}
} // namespace

void WifiSetup_open()
{
    if (g_poll)
    {
        return;
    }
    if (!WiFi_startScan())
    {
        Modal_message("Cannot scan", "The Wi-Fi radio is busy or not ready yet.", Tone::Warn);
        return;
    }
    g_started = lv_tick_get();
    g_busy = Modal_busy("Scanning", "Looking for networks...", [] {
        stopPoll();
        WiFi_abortScan();
    });
    g_poll = lv_timer_create(onPoll, 300, nullptr);
}
