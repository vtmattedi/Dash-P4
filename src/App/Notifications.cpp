#include "Notifications.h"

#include <algorithm>
#include <deque>
#include <mutex>
#include <time.h>
#include <vector>

#ifdef ESP_PLATFORM
#include "esp_timer.h"
#endif

namespace
{
constexpr size_t kMaxListeners = 4;

#ifdef ESP_PLATFORM
uint32_t defaultNowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }
#else
uint32_t defaultNowMs() { return 0; }
#endif

int64_t defaultEpoch()
{
    const time_t now = time(nullptr);
    // Before SNTP the clock sits near 1970; report "unknown" rather than a wrong date.
    return now > 1700000000 ? static_cast<int64_t>(now) : 0;
}

uint32_t (*g_nowMs)() = defaultNowMs;
int64_t (*g_epoch)() = defaultEpoch;

std::deque<Notification> g_history; // front = newest
uint32_t g_nextId = 1;
uint32_t g_revision = 0;
NotifyListener g_listeners[kMaxListeners];

struct Pending
{
    NotifySeverity severity;
    std::string source, title, body;
};
std::mutex g_pendingMutex;
std::vector<Pending> g_pending;

void emit(const Notification &n, NotifyEvent event)
{
    ++g_revision;
    for (NotifyListener l : g_listeners)
    {
        if (l)
        {
            l(n, event);
        }
    }
}

void trim()
{
    while (g_history.size() > NOTIFICATIONS_CAPACITY)
    {
        // Oldest first, but never drop a live condition.
        auto victim = g_history.end();
        for (auto it = g_history.end(); it != g_history.begin();)
        {
            --it;
            if (!(it->sticky && it->active))
            {
                victim = it;
                break;
            }
        }
        if (victim == g_history.end())
        {
            return; // all live; let it grow rather than lose a condition
        }
        g_history.erase(victim);
    }
}

Notification *findKey(const std::string &key)
{
    for (Notification &n : g_history)
    {
        if (n.sticky && n.active && n.key == key)
        {
            return &n;
        }
    }
    return nullptr;
}
} // namespace

void Notifications_setClock(uint32_t (*nowMs)(), int64_t (*epoch)())
{
    g_nowMs = nowMs ? nowMs : defaultNowMs;
    g_epoch = epoch ? epoch : defaultEpoch;
}

uint32_t Notify(NotifySeverity severity, const std::string &source, const std::string &title,
                const std::string &body)
{
    Notification n;
    n.id = g_nextId++;
    n.severity = severity;
    n.source = source;
    n.title = title;
    n.body = body;
    n.atMs = g_nowMs();
    n.epoch = g_epoch();
    g_history.push_front(n);
    trim();
    emit(g_history.front(), NotifyEvent::Added);
    return n.id;
}

uint32_t NotifySticky(const std::string &key, NotifySeverity severity, const std::string &source,
                      const std::string &title, const std::string &body)
{
    if (Notification *existing = findKey(key))
    {
        if (existing->severity == severity && existing->title == title && existing->body == body)
        {
            return existing->id; // nothing new to say
        }
        existing->severity = severity;
        existing->source = source;
        existing->title = title;
        existing->body = body;
        existing->atMs = g_nowMs();
        existing->epoch = g_epoch();
        existing->read = false;
        emit(*existing, NotifyEvent::Updated);
        return existing->id;
    }
    Notification n;
    n.id = g_nextId++;
    n.severity = severity;
    n.source = source;
    n.title = title;
    n.body = body;
    n.key = key;
    n.sticky = true;
    n.atMs = g_nowMs();
    n.epoch = g_epoch();
    g_history.push_front(n);
    trim();
    emit(g_history.front(), NotifyEvent::Added);
    return n.id;
}

void NotifyClear(const std::string &key, const std::string &resolvedTitle)
{
    Notification *n = findKey(key);
    if (!n)
    {
        return;
    }
    n->active = false;
    n->read = true; // a finished condition is not something left to read
    const Notification copy = *n;
    emit(copy, NotifyEvent::Cleared);
    if (!resolvedTitle.empty())
    {
        Notify(NotifySeverity::Ok, copy.source, resolvedTitle);
    }
}

void NotifyAsync(NotifySeverity severity, const std::string &source, const std::string &title,
                 const std::string &body)
{
    std::lock_guard<std::mutex> lock(g_pendingMutex);
    if (g_pending.size() < 32) // a runaway producer must not grow without bound
    {
        g_pending.push_back({severity, source, title, body});
    }
}

void Notifications_tick()
{
    std::vector<Pending> batch;
    {
        std::lock_guard<std::mutex> lock(g_pendingMutex);
        batch.swap(g_pending);
    }
    for (const Pending &p : batch)
    {
        Notify(p.severity, p.source, p.title, p.body);
    }
}

size_t Notifications_count() { return g_history.size(); }
const Notification &Notifications_at(size_t i) { return g_history[i]; }

const Notification *Notifications_find(uint32_t id)
{
    for (const Notification &n : g_history)
    {
        if (n.id == id)
        {
            return &n;
        }
    }
    return nullptr;
}

namespace
{
std::vector<const Notification *> activeSorted()
{
    std::vector<const Notification *> out;
    for (const Notification &n : g_history)
    {
        if (n.sticky && n.active)
        {
            out.push_back(&n);
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Notification *a, const Notification *b)
                     { return static_cast<int>(a->severity) > static_cast<int>(b->severity); });
    return out;
}
} // namespace

size_t Notifications_activeCount()
{
    size_t n = 0;
    for (const Notification &x : g_history)
    {
        n += (x.sticky && x.active) ? 1 : 0;
    }
    return n;
}

const Notification &Notifications_activeAt(size_t index)
{
    return *activeSorted()[index];
}

uint32_t Notifications_unread()
{
    uint32_t n = 0;
    for (const Notification &x : g_history)
    {
        n += x.read ? 0 : 1;
    }
    return n;
}

void Notifications_markRead(uint32_t id)
{
    for (Notification &n : g_history)
    {
        if (n.id == id && !n.read)
        {
            n.read = true;
            emit(n, NotifyEvent::Updated);
            return;
        }
    }
}

void Notifications_markAllRead()
{
    bool any = false;
    for (Notification &n : g_history)
    {
        any = any || !n.read;
        n.read = true;
    }
    if (any && !g_history.empty())
    {
        emit(g_history.front(), NotifyEvent::Reset);
    }
}

void Notifications_clearHistory()
{
    std::deque<Notification> keep;
    for (Notification &n : g_history)
    {
        if (n.sticky && n.active)
        {
            keep.push_back(n);
        }
    }
    g_history.swap(keep);
    Notification none;
    emit(none, NotifyEvent::Reset);
}

uint32_t Notifications_revision() { return g_revision; }

bool Notifications_onChange(NotifyListener listener)
{
    for (NotifyListener &slot : g_listeners)
    {
        if (slot == listener)
        {
            return true;
        }
    }
    for (NotifyListener &slot : g_listeners)
    {
        if (!slot)
        {
            slot = listener;
            return true;
        }
    }
    return false;
}
