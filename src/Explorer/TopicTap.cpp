#include "TopicTap.h"

#include <deque>

namespace
{
std::deque<TapEntry> g_tail; // front = newest
bool g_paused = false;
int g_users = 0;
uint32_t g_revision = 1;
uint32_t g_total = 0;

bool printable(unsigned char c) { return c == '\n' || c == '\t' || (c >= 0x20 && c < 0x7F) || c >= 0x80; }
} // namespace

void Tap_acquire() { ++g_users; }

void Tap_release()
{
    if (g_users > 0 && --g_users == 0)
    {
        g_tail.clear();
        g_tail.shrink_to_fit();
        g_paused = false;
        ++g_revision;
    }
}

bool Tap_active() { return g_users > 0; }

void Tap_record(const NetMessage &m)
{
    if (g_paused || g_users == 0)
    {
        return;
    }
    TapEntry e;
    e.atMs = m.receivedMs;
    e.retained = m.retained;
    e.topic = m.topic;
    e.length = m.length;

    // Binary (MessagePack manifests, hardware documents) reads as junk; say so instead.
    size_t bad = 0;
    const size_t look = m.length < TAP_TEXT_MAX ? m.length : TAP_TEXT_MAX;
    for (size_t i = 0; i < look; ++i)
    {
        bad += printable(m.payload[i]) ? 0 : 1;
    }
    e.binary = m.length > 0 && bad * 10 > look; // more than 10% unprintable
    if (e.binary)
    {
        e.text = "<" + std::to_string(m.length) + " bytes binary>";
    }
    else
    {
        e.text.assign(reinterpret_cast<const char *>(m.payload), look);
        for (char &c : e.text)
        {
            if (c == '\n' || c == '\t' || c == '\r')
            {
                c = ' ';
            }
        }
        if (m.length > look)
        {
            e.text += "...";
        }
    }
    g_tail.push_front(std::move(e));
    while (g_tail.size() > TAP_CAPACITY)
    {
        g_tail.pop_back();
    }
    ++g_total;
    ++g_revision;
}

size_t Tap_count() { return g_tail.size(); }
const TapEntry &Tap_at(size_t i) { return g_tail[i]; }
void Tap_setPaused(bool paused) { g_paused = paused; ++g_revision; }
bool Tap_paused() { return g_paused; }

void Tap_clear()
{
    g_tail.clear();
    ++g_revision;
}

uint32_t Tap_revision() { return g_revision; }
uint32_t Tap_total() { return g_total; }

bool Tap_topicIsFor(const std::string &topic, const std::string &device)
{
    return topic.size() > device.size() && topic.compare(0, device.size(), device) == 0 && topic[device.size()] == '/';
}
