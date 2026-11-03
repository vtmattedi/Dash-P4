#pragma once
#include <stdint.h>
#include <functional>
#include <string>
#include <vector>

// Round-trip time to a device, measured with the console's PING over MQTTP: the request
// goes out on the active transport, the device answers PONG, and the time between is what
// is shown. Pings are sent one at a time, ~300 ms apart, so a burst never floods a small
// device or the broker.
//
// This measures the whole path (panel -> broker/gateway -> device -> back), including the
// device's own command handling; it is the number that matters when "is it responding?" is
// the question, not a pure network latency.
//
// LVGL task only. Drive with Ping_tick() every ~50-100 ms.

struct PingStats
{
    std::string device;
    int total = 0;     // requested
    int sent = 0;
    int received = 0;
    int lost = 0;
    uint32_t minMs = 0, maxMs = 0;
    float avgMs = 0;
    std::vector<int32_t> rtts; // per ping, in order; -1 = lost
    bool running = false;
};

using PingUpdate = std::function<void(const PingStats &)>;

/// Start a run, replacing any run in progress. `onUpdate` fires after every reply or loss.
void Ping_start(const std::string &device, int count, PingUpdate onUpdate, uint32_t timeoutMs = 3000);
/// Stop early; the callback fires once with running == false.
void Ping_stop();
bool Ping_running();
const PingStats &Ping_stats();

void Ping_tick();
