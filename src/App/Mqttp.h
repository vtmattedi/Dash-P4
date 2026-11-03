#pragma once
#include <stdint.h>
#include <functional>
#include <string>

#include "NetTypes.h"

// MQTTP: correlated console request/response, the same protocol the backend uses.
//
//   request  ->  <device>/console/controlled/<id>/in     the command text
//   reply    <-  <device>/console/controlled/<id>/out    the handler's result string
//
// This is how the Explorer asks a device anything: INFO sections, PING, STATUS, CONFIG,
// REBOOT. Several requests can be in flight (the Ping tool bursts), each with its own
// id and its own deadline.
//
// Three things learned the hard way on the old panel are baked in:
//
//  * Listening is per DEVICE, not per request. A subscribe is only queued by the
//    client; a fast device answers `INFO` in about a millisecond, which beats the
//    broker's SUBACK, and the reply is dropped. So `<dev>/console/controlled/+/out` is
//    taken in advance (Mqttp_listen when a device page opens), and a request to a
//    device that is not yet being listened to is held back briefly (kSettleMs) instead
//    of racing the subscription.
//  * Replies over 512 bytes arrive chunked as `;;<n>/<total>;;<data>` (1-based) and the
//    data may itself contain ";;". `;;error;;<text>` marks a failure.
//  * An empty reply cannot be told from silence (the library does not publish empty
//    payloads), so "no answer" is reported as a timeout.
//
// Not thread-safe: LVGL task only. Everything is driven by Mqttp_tick() and
// Mqttp_handleMessage(); nothing blocks.

enum class MqttpStatus : uint8_t
{
    Pending,   // queued, waiting out the subscription settle time, or awaiting the reply
    Ok,
    Timeout,
    Failed,    // publish refused (no transport), or the device answered ;;error;;
    Cancelled
};

struct MqttpResult
{
    uint32_t id = 0;
    MqttpStatus status = MqttpStatus::Pending;
    std::string device;
    std::string command;
    std::string response;
    uint32_t rttMs = 0;       // request sent -> last chunk received
    bool truncated = false;   // reply exceeded MQTTP_RESPONSE_MAX
};

/// Called once per request, when it finishes (Ok/Timeout/Failed/Cancelled).
using MqttpCallback = std::function<void(const MqttpResult &)>;

/// The seams to the network and the clock; set once at startup (and by tests).
struct MqttpPorts
{
    bool (*publish)(const std::string &topic, const std::string &payload);
    void (*subscribe)(const std::string &filter);
    void (*unsubscribe)(const std::string &filter);
    uint32_t (*nowMs)();
};
void Mqttp_setPorts(const MqttpPorts &ports);

/// Cap on a reassembled reply; longer replies are truncated, not failed.
#define MQTTP_RESPONSE_MAX 8192
/// Concurrent requests.
#define MQTTP_MAX_PENDING 8
#define MQTTP_DEFAULT_TIMEOUT_MS 8000

/// Start listening for a device's replies (reference counted). Call when its page opens.
void Mqttp_listen(const std::string &device);
/// Give it back (the subscription lingers ~15 s so reopening a page costs nothing).
void Mqttp_unlisten(const std::string &device);

/// Send `command` to `device`. Returns the request id, or 0 if the table is full.
/// The callback always fires exactly once, never from inside this call.
uint32_t Mqttp_request(const std::string &device, const std::string &command, MqttpCallback callback,
                       uint32_t timeoutMs = MQTTP_DEFAULT_TIMEOUT_MS);

/// Abandon a request; its callback fires with Cancelled. No-op if already finished.
void Mqttp_cancel(uint32_t id);

/// Drive deadlines, held-back sends and subscription lingering. Call every ~50-100 ms.
void Mqttp_tick();

/// Offer an incoming message. True if it was a controlled-console reply (consumed even
/// when it matched no request, e.g. a late reply to a timed-out one).
bool Mqttp_handleMessage(const NetMessage &message);

uint32_t Mqttp_pendingCount();

/// The clock the module runs on (ms), so users can schedule against the same time base.
uint32_t Mqttp_now();

/// A reply for an unknown id is dropped; this counts them (late replies after a timeout).
uint32_t Mqttp_strayReplies();
