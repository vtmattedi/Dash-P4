#include "Ping.h"

#include "App/Mqttp.h"

namespace
{
constexpr uint32_t kGapMs = 300;

PingStats g_stats;
PingUpdate g_update;
uint32_t g_timeoutMs = 3000;
uint32_t g_nextAt = 0;
uint32_t g_outstanding = 0; // Mqttp request id
uint32_t g_run = 0;         // bumped per start, so a late callback from an older run is ignored

void finishIfDone()
{
    if (g_stats.sent >= g_stats.total && g_outstanding == 0)
    {
        g_stats.running = false;
    }
}

void notify()
{
    if (g_update)
    {
        // Copy: the callback may restart the run.
        const PingUpdate fn = g_update;
        const PingStats copy = g_stats;
        fn(copy);
    }
}

void record(int32_t rtt)
{
    g_stats.rtts.push_back(rtt);
    if (rtt < 0)
    {
        ++g_stats.lost;
    }
    else
    {
        const uint32_t v = static_cast<uint32_t>(rtt);
        g_stats.minMs = g_stats.received == 0 || v < g_stats.minMs ? v : g_stats.minMs;
        g_stats.maxMs = v > g_stats.maxMs ? v : g_stats.maxMs;
        g_stats.avgMs = (g_stats.avgMs * g_stats.received + v) / (g_stats.received + 1);
        ++g_stats.received;
    }
}
} // namespace

void Ping_start(const std::string &device, int count, PingUpdate onUpdate, uint32_t timeoutMs)
{
    if (g_stats.running)
    {
        Mqttp_unlisten(g_stats.device);
    }
    if (g_outstanding)
    {
        Mqttp_cancel(g_outstanding); // its callback sees a stale run and is ignored
        g_outstanding = 0;
    }
    ++g_run;
    g_stats = PingStats();
    g_stats.device = device;
    g_stats.total = count;
    g_stats.running = count > 0;
    g_update = std::move(onUpdate);
    g_timeoutMs = timeoutMs;
    g_nextAt = Mqttp_now();
    if (count > 0)
    {
        Mqttp_listen(device); // held for the whole run, so the first ping is not delayed by a subscribe
    }
}

void Ping_stop()
{
    if (!g_stats.running)
    {
        return;
    }
    ++g_run;
    if (g_outstanding)
    {
        Mqttp_cancel(g_outstanding);
        g_outstanding = 0;
    }
    g_stats.running = false;
    Mqttp_unlisten(g_stats.device);
    notify();
}

bool Ping_running() { return g_stats.running; }
const PingStats &Ping_stats() { return g_stats; }

void Ping_tick()
{
    if (!g_stats.running || g_outstanding != 0 || g_stats.sent >= g_stats.total)
    {
        return;
    }
    if (static_cast<int32_t>(Mqttp_now() - g_nextAt) < 0)
    {
        return;
    }
    ++g_stats.sent;
    const uint32_t run = g_run;
    const uint32_t id = Mqttp_request(
        g_stats.device, "PING",
        [run](const MqttpResult &result) {
            if (run != g_run)
            {
                return; // an older run's reply
            }
            g_outstanding = 0;
            const bool pong = result.status == MqttpStatus::Ok && result.response.find("PONG") != std::string::npos;
            record(pong ? static_cast<int32_t>(result.rttMs) : -1);
            g_nextAt = Mqttp_now() + kGapMs;
            finishIfDone();
            if (!g_stats.running)
            {
                Mqttp_unlisten(g_stats.device);
            }
            notify();
        },
        g_timeoutMs);
    if (id == 0)
    {
        // Request table full: count it lost and try the next one later.
        record(-1);
        g_nextAt = Mqttp_now() + kGapMs;
        finishIfDone();
        if (!g_stats.running)
        {
            Mqttp_unlisten(g_stats.device);
        }
        notify();
        return;
    }
    g_outstanding = id;
}
