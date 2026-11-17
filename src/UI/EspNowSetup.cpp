#include "EspNowSetup.h"

#include <string>
#include <vector>

#include <NightMare.h>

#include "App/EspNowScan.h"
#include "App/Net.h"
#include "Modal.h"

namespace
{
constexpr uint32_t kListenMs = 8000;
lv_timer_t *g_poll = nullptr;
ModalId g_busy = 0;
uint32_t g_started = 0;

void showResults()
{
    const std::vector<EspNowPeer> peers = EspNowScan_peers();
    EspNowScan_stop();
    std::vector<ListEntry> entries;
    for (const EspNowPeer &p : peers)
    {
        ListEntry e;
        e.label = p.gateway[0] ? std::string(p.gateway) : EspNowScan_macText(p.mac);
        e.detail = EspNowScan_macText(p.mac) + "   " + std::to_string(p.rssi) + " dBm   ch " + std::to_string(p.channel) +
                   "   " + std::to_string(p.frames) + " frames" +
                   (p.lastType >= 0 ? std::string("   ") + EspNowScan_typeName(p.lastType) : std::string());
        e.tone = p.rssi > -60 ? Tone::Ok : p.rssi > -75 ? Tone::Warn : Tone::Danger;
        entries.push_back(e);
    }
    Modal_list("ESP-NOW devices heard", entries, [](size_t) {}, "Nothing heard. A gateway announces itself every few seconds; check it is on this channel.",
               ModalSize::Dialog);
}

void onPoll(lv_timer_t *)
{
    if (lv_tick_elaps(g_started) < kListenMs)
    {
        return;
    }
    lv_timer_delete(g_poll);
    g_poll = nullptr;
    Modal_close(g_busy);
    g_busy = 0;
    showResults();
}
} // namespace

void EspNowScanUi_open()
{
    if (g_poll)
    {
        return;
    }
    if (!Net_state().espNowEnabled)
    {
        Modal_message("ESP-NOW is off", "Choose ESP-NOW under Network first: the scanner listens through its radio link.", Tone::Warn);
        return;
    }
    EspNowScan_start();
    g_started = lv_tick_get();
    g_busy = Modal_busy("Listening", "Collecting ESP-NOW frames...", [] {
        if (g_poll)
        {
            lv_timer_delete(g_poll);
            g_poll = nullptr;
        }
        EspNowScan_stop();
    });
    g_poll = lv_timer_create(onPoll, 300, nullptr);
}
