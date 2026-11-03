#include "Watch.h"

#include <stdlib.h>
#include <string.h>

namespace
{
struct Series
{
    std::string device, resource;
    std::vector<WatchSample> samples;
};

std::vector<Series> g_series;
uint32_t g_revision = 1;

Series *find(const std::string &device, const std::string &resource)
{
    for (Series &s : g_series)
    {
        if (s.device == device && s.resource == resource)
        {
            return &s;
        }
    }
    return nullptr;
}
} // namespace

bool Watch_parseValue(const std::string &text, float &out)
{
    if (text == "true")
    {
        out = 1;
        return true;
    }
    if (text == "false")
    {
        out = 0;
        return true;
    }
    if (text.empty())
    {
        return false;
    }
    char *end = nullptr;
    const float v = strtof(text.c_str(), &end);
    if (!end || *end != '\0')
    {
        return false;
    }
    out = v;
    return true;
}

void Watch_restore(const std::string &persisted)
{
    g_series.clear();
    size_t start = 0;
    while (start <= persisted.size() && g_series.size() < WATCH_MAX_RESOURCES)
    {
        size_t comma = persisted.find(',', start);
        if (comma == std::string::npos)
        {
            comma = persisted.size();
        }
        const std::string item = persisted.substr(start, comma - start);
        const size_t slash = item.find('/');
        if (slash != std::string::npos && slash > 0 && slash + 1 < item.size())
        {
            Series s;
            s.device = item.substr(0, slash);
            s.resource = item.substr(slash + 1);
            g_series.push_back(std::move(s));
        }
        start = comma + 1;
    }
    ++g_revision;
}

std::string Watch_serialize()
{
    std::string out;
    for (const Series &s : g_series)
    {
        out += (out.empty() ? "" : ",") + s.device + "/" + s.resource;
    }
    return out;
}

bool Watch_isWatched(const std::string &device, const std::string &resource)
{
    return find(device, resource) != nullptr;
}

bool Watch_toggle(const std::string &device, const std::string &resource)
{
    for (size_t i = 0; i < g_series.size(); ++i)
    {
        if (g_series[i].device == device && g_series[i].resource == resource)
        {
            g_series.erase(g_series.begin() + i);
            ++g_revision;
            return true;
        }
    }
    if (g_series.size() >= WATCH_MAX_RESOURCES)
    {
        return false;
    }
    Series s;
    s.device = device;
    s.resource = resource;
    g_series.push_back(std::move(s));
    ++g_revision;
    return true;
}

size_t Watch_count() { return g_series.size(); }

const std::vector<WatchSample> *Watch_samples(const std::string &device, const std::string &resource)
{
    Series *s = find(device, resource);
    return s ? &s->samples : nullptr;
}

void Watch_record(const std::string &device, const std::string &resource, const std::string &value, uint32_t nowMs)
{
    Series *s = find(device, resource);
    if (!s)
    {
        return;
    }
    float v;
    if (!Watch_parseValue(value, v))
    {
        return;
    }
    if (!s->samples.empty())
    {
        const WatchSample &last = s->samples.back();
        // An unchanged value is not worth a sample more often than the minimum interval.
        if (last.value == v && nowMs - last.ms < WATCH_MIN_INTERVAL_MS)
        {
            return;
        }
    }
    if (s->samples.size() >= WATCH_MAX_SAMPLES)
    {
        s->samples.erase(s->samples.begin());
    }
    s->samples.push_back({nowMs, v});
    ++g_revision;
}

uint32_t Watch_revision() { return g_revision; }
