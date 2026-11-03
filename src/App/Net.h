#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>

#include <NightMare.h>

#include "NetTypes.h"

// The panel's link to the NightMare network.
//
// NightMareNetwork owns the transports (MQTT primary, ESP-NOW backup) and the
// failover between them. This file owns three things on top:
//
//   1. THE THREADING BOUNDARY. The library delivers messages and status changes on
//      its own tasks (MQTT, Wi-Fi events). All application state -- the registry,
//      the notification model, the explorer -- belongs to the LVGL task. So callbacks
//      here only copy into a bounded inbox, and Net_drain() (an lv_timer, see
//      App_begin) hands the copies to the registered listeners on the LVGL task.
//      Nothing in the app is ever touched from a library task.
//
//   2. THE SUBSCRIPTION TABLE. The library keeps no subscription state and installs
//      nothing on its own for a wildcard watcher, so the panel remembers what it
//      wants and replays it on every (re)connect, whichever transport that is.
//
//   3. A SNAPSHOT of transport health for the status bar and notifications.
//
// Publishing is safe from any task.

/// A transport layer changed state. Mirrors NightMare::NetworkStatusEvent.
struct NetStatus
{
    NightMare::NetworkLayer layer;
    NightMare::ConnectivityState state;
    NightMare::ConnectivityState previous;
    bool up;
};

using NetMessageFn = void (*)(const NetMessage &message);
using NetStatusFn = void (*)(const NetStatus &status);
using NetConnectedFn = void (*)();

/// Install the library callbacks and the standing subscriptions. Call once, after
/// App_begin() set up the inbox and before startNightMareESP().
void Net_begin();

/// Listeners, all called on the LVGL task from Net_drain(). Up to 8 of each.
bool Net_onMessage(NetMessageFn fn);
bool Net_onStatus(NetStatusFn fn);
/// Called (on the LVGL task) each time a transport (re)connects, after the
/// standing subscriptions were replayed.
bool Net_onConnected(NetConnectedFn fn);

/// Deliver queued messages and status changes to the listeners, spending at most
/// `budgetMs`. Call from an lv_timer.
void Net_drain(uint32_t budgetMs);

/// Subscribe now (if a transport is up) and on every reconnect. Reference-counted:
/// each Net_subscribe needs a matching Net_unsubscribe before the filter is dropped.
void Net_subscribe(const std::string &filter);
void Net_unsubscribe(const std::string &filter);

bool Net_publish(const std::string &topic, const std::string &payload, bool retained = false);
bool Net_publishBytes(const std::string &topic, const uint8_t *payload, size_t length,
                      bool retained = false);

/// Snapshot of transport health.
struct NetState
{
    NightMare::ConnectionType active;    // which transport carries traffic (AUTO: none)
    NightMare::ConnectionType preferred; // which one the user asked for
    NightMare::ConnectionState connection;
    bool wifiIp;   // station has an address
    bool mqtt;     // MQTT connected
    bool espNow;   // ESP-NOW session up
};
NetState Net_state();

/// Counters. `dropped` should stay 0; a rising count means the LVGL task is not
/// draining fast enough (or a message exceeded NET_MAX_PAYLOAD).
struct NetStats
{
    uint32_t received;
    uint32_t delivered;
    uint32_t dropped;
    uint32_t bytes;
    uint32_t connects;
    uint32_t inboxHighWater;
};
NetStats Net_stats();

/// Log every received topic to the serial console. For debugging on hardware.
void Net_setTrace(bool on);
bool Net_trace();

/// Largest message the inbox accepts (a 16KB manifest plus slack).
#define NET_MAX_PAYLOAD (20 * 1024)
