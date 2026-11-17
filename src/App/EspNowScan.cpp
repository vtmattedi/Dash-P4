#include "EspNowScan.h"

#include <algorithm>
#include <string.h>

#include <NightMare.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "lvgl.h"

#if NM_NETWORK_ESPNOW
#include <Network/EspNow/NightMareEspNow/Frame.h>
#include "nm_espnow_tap.h"

namespace
{
constexpr int kMaxPeers = 24;
EspNowPeer g_peers[kMaxPeers];
int g_count = 0;
bool g_listening = false;
SemaphoreHandle_t g_lock = nullptr;

// ESP-Hosted task. Cheap: a lookup, a copy, and for a BEACON the library's own id parser.
void onFrame(const uint8_t src[6], int rssi, int channel, const uint8_t *payload, int len)
{
    if (!g_listening || !g_lock || xSemaphoreTake(g_lock, 0) != pdTRUE)
    {
        return;
    }
    EspNowPeer *peer = nullptr;
    for (int i = 0; i < g_count; ++i)
    {
        if (memcmp(g_peers[i].mac, src, 6) == 0)
        {
            peer = &g_peers[i];
            break;
        }
    }
    if (!peer && g_count < kMaxPeers)
    {
        peer = &g_peers[g_count++];
        memset(peer, 0, sizeof(*peer));
        memcpy(peer->mac, src, 6);
        peer->lastType = -1;
    }
    if (peer)
    {
        peer->rssi = rssi;
        peer->channel = channel;
        peer->lastMs = lv_tick_get();
        ++peer->frames;
        peer->lastType = -1;
        if (len >= static_cast<int>(NightMare::FrameHeaderSize) && len <= static_cast<int>(sizeof(NightMare::Frame)))
        {
            NightMare::Frame frame;
            memset(&frame, 0, sizeof(frame));
            memcpy(&frame, payload, len);
            if (frame.header.type <= static_cast<uint8_t>(NightMare::FrameType::ERROR) &&
                frame.header.length + NightMare::FrameHeaderSize == static_cast<size_t>(len))
            {
                peer->lastType = frame.header.type;
                if (frame.header.type == static_cast<uint8_t>(NightMare::FrameType::BEACON))
                {
                    NightMare::beaconGatewayId(frame, peer->gateway, sizeof(peer->gateway));
                }
            }
        }
    }
    xSemaphoreGive(g_lock);
}
} // namespace

void EspNowScan_start()
{
    if (!g_lock)
    {
        g_lock = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(g_lock, portMAX_DELAY);
    g_count = 0;
    g_listening = true;
    xSemaphoreGive(g_lock);
    nm_espnow_set_tap(onFrame);
}

void EspNowScan_stop()
{
    nm_espnow_set_tap(nullptr);
    g_listening = false;
}

bool EspNowScan_listening() { return g_listening; }

std::vector<EspNowPeer> EspNowScan_peers()
{
    std::vector<EspNowPeer> out;
    if (g_lock && xSemaphoreTake(g_lock, pdMS_TO_TICKS(50)) == pdTRUE)
    {
        out.assign(g_peers, g_peers + g_count);
        xSemaphoreGive(g_lock);
    }
    std::sort(out.begin(), out.end(), [](const EspNowPeer &a, const EspNowPeer &b) { return a.rssi > b.rssi; });
    return out;
}
#else
void EspNowScan_start() {}
void EspNowScan_stop() {}
bool EspNowScan_listening() { return false; }
std::vector<EspNowPeer> EspNowScan_peers() { return {}; }
#endif

std::string EspNowScan_macText(const uint8_t mac[6])
{
    char text[20];
    snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return text;
}

const char *EspNowScan_typeName(int type)
{
    static const char *const names[] = {"beacon", "connect", "challenge", "auth", "connack", "disconnect", "ping",
                                        "pong", "subscribe", "unsubscribe", "message", "last will", "ack", "error"};
    return type >= 0 && type < 14 ? names[type] : "other";
}
