#include "Feed.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <utility>

#include <ArduinoJson.h>

namespace
{
std::map<int64_t, FeedItem> g_items; // keyed by numeric_id, which also gives the display order
int64_t g_version = 0;
std::string g_epoch;
uint32_t g_revision = 0;

std::set<int64_t> g_dismissed;
std::string g_dismissEpoch; // numeric ids only mean something within one epoch
void (*g_save)(const std::string &) = nullptr;

std::vector<int64_t> g_visible;
bool g_visibleDirty = true;

void (*g_cacheSave)(const std::string &) = nullptr;
bool g_cacheDirty = false;
int64_t g_cacheDirtyAtMs = 0;

std::mutex g_postMutex;
std::vector<FeedDelta> g_posted;

void saveDismissed()
{
    if (!g_save)
    {
        return;
    }
    std::string text = g_dismissEpoch + ";";
    bool first = true;
    for (int64_t id : g_dismissed)
    {
        text += (first ? "" : ",") + std::to_string(id);
        first = false;
    }
    g_save(text);
}

void loadDismissed(const std::string &text)
{
    g_dismissed.clear();
    g_dismissEpoch.clear();
    const size_t semi = text.find(';');
    if (semi == std::string::npos)
    {
        return;
    }
    g_dismissEpoch = text.substr(0, semi);
    size_t pos = semi + 1;
    while (pos < text.size())
    {
        size_t end = text.find(',', pos);
        if (end == std::string::npos)
        {
            end = text.size();
        }
        const long long id = atoll(text.substr(pos, end - pos).c_str());
        if (id > 0)
        {
            g_dismissed.insert(id);
        }
        pos = end + 1;
    }
}

void changed()
{
    g_visibleDirty = true;
    ++g_revision;
}

void rebuildVisible()
{
    if (!g_visibleDirty)
    {
        return;
    }
    g_visible.clear();
    for (const auto &entry : g_items)
    {
        if (!g_dismissed.count(entry.first))
        {
            g_visible.push_back(entry.first);
        }
    }
    g_visibleDirty = false;
}

// A dismissal for an item the server no longer has is dead weight: ids are never reused within an epoch.
bool pruneDismissed()
{
    bool any = false;
    for (auto it = g_dismissed.begin(); it != g_dismissed.end();)
    {
        if (g_items.count(*it))
        {
            ++it;
        }
        else
        {
            it = g_dismissed.erase(it);
            any = true;
        }
    }
    return any;
}

std::string text(JsonVariantConst v) { return v.is<const char *>() ? std::string(v.as<const char *>()) : std::string(); }

bool wholeNumber(JsonVariantConst v, int64_t &out)
{
    // 1.5 and "7" are both malformed; the server sends integers. (ArduinoJson's is<long long>() is false for both.)
    if (!v.is<long long>())
    {
        return false;
    }
    out = v.as<long long>();
    return true;
}
} // namespace

bool Feed_parse(const char *json, size_t length, FeedDelta &out)
{
    JsonDocument doc;
    if (deserializeJson(doc, json, length) || !doc.is<JsonObjectConst>())
    {
        return false;
    }
    FeedDelta parsed;
    if (!wholeNumber(doc["version"], parsed.version) || parsed.version < 0)
    {
        return false;
    }
    parsed.epoch = text(doc["epoch"]);
    parsed.full = doc["full"] | false;

    if (!doc["items"].isNull())
    {
        if (!doc["items"].is<JsonArrayConst>())
        {
            return false;
        }
        for (JsonVariantConst entry : doc["items"].as<JsonArrayConst>())
        {
            FeedItem item;
            if (!entry.is<JsonObjectConst>() || !wholeNumber(entry["numeric_id"], item.numericId) || item.numericId <= 0)
            {
                return false;
            }
            item.id = text(entry["id"]);
            item.source = text(entry["source"]);
            item.kind = text(entry["kind"]);
            item.title = text(entry["title"]);
            item.status = text(entry["status"]);
            item.description = text(entry["description"]);
            item.account = text(entry["connection"]["alias"]);
            item.startsAt = text(entry["starts_at"]);
            item.endsAt = text(entry["ends_at"]);
            item.dueAt = text(entry["due_at"]);
            item.url = text(entry["url"]);
            item.joinUrl = text(entry["join_url"]);
            parsed.items.push_back(std::move(item));
        }
    }
    if (!doc["removed_ids"].isNull())
    {
        if (!doc["removed_ids"].is<JsonArrayConst>())
        {
            return false;
        }
        for (JsonVariantConst entry : doc["removed_ids"].as<JsonArrayConst>())
        {
            int64_t id = 0;
            if (!wholeNumber(entry, id) || id <= 0)
            {
                return false;
            }
            parsed.removed.push_back(id);
        }
    }
    out = std::move(parsed);
    return true;
}

void Feed_post(FeedDelta &&delta)
{
    std::lock_guard<std::mutex> lock(g_postMutex);
    g_posted.push_back(std::move(delta));
}

void Feed_begin(const std::string &stored, void (*save)(const std::string &))
{
    loadDismissed(stored);
    g_save = save;
}

bool Feed_tick()
{
    std::vector<FeedDelta> batch;
    {
        std::lock_guard<std::mutex> lock(g_postMutex);
        batch.swap(g_posted);
    }
    bool fits = true;
    for (const FeedDelta &delta : batch)
    {
        fits &= Feed_apply(delta);
    }
    return fits;
}

bool Feed_apply(const FeedDelta &delta)
{
    bool dismissalsChanged = false;
    bool itemsChanged = false;

    if (delta.full)
    {
        if (delta.epoch != g_dismissEpoch)
        {
            // Another database: the saved ids point at different items now.
            dismissalsChanged = !g_dismissed.empty() || g_dismissEpoch != delta.epoch;
            g_dismissed.clear();
            g_dismissEpoch = delta.epoch;
        }
        g_items.clear();
        for (const FeedItem &item : delta.items)
        {
            g_items[item.numericId] = item;
        }
        g_epoch = delta.epoch;
        itemsChanged = true;
    }
    else
    {
        // A patch only makes sense on top of the stream it was cut from. (An empty epoch is the server's
        // compatibility mode, where it cannot tell streams apart either.)
        if (!delta.epoch.empty() && delta.epoch != g_epoch)
        {
            return false;
        }
        for (int64_t id : delta.removed)
        {
            itemsChanged |= g_items.erase(id) > 0;
        }
        for (const FeedItem &item : delta.items)
        {
            auto found = g_items.find(item.numericId);
            if (found == g_items.end() || !(found->second == item))
            {
                g_items[item.numericId] = item;
                itemsChanged = true;
            }
        }
    }
    g_version = delta.version;
    if (itemsChanged)
    {
        if (!g_cacheDirty)
        {
            g_cacheDirtyAtMs = -1; // stamped by the next Feed_cacheTick, which owns the clock
        }
        g_cacheDirty = true;
    }

    dismissalsChanged |= pruneDismissed();
    if (dismissalsChanged)
    {
        saveDismissed();
    }
    if (itemsChanged || dismissalsChanged)
    {
        changed();
    }
    return true;
}

std::string Feed_serialize()
{
    JsonDocument doc;
    doc["version"] = static_cast<long long>(g_version);
    doc["epoch"] = g_epoch;
    doc["full"] = true;
    JsonArray items = doc["items"].to<JsonArray>();
    for (const auto &entry : g_items)
    {
        const FeedItem &item = entry.second;
        JsonObject o = items.add<JsonObject>();
        o["numeric_id"] = static_cast<long long>(item.numericId);
        o["id"] = item.id;
        o["source"] = item.source;
        o["kind"] = item.kind;
        o["title"] = item.title;
        if (!item.status.empty())
        {
            o["status"] = item.status;
        }
        if (!item.description.empty())
        {
            o["description"] = item.description;
        }
        if (!item.account.empty())
        {
            o["connection"]["alias"] = item.account;
        }
        if (!item.startsAt.empty())
        {
            o["starts_at"] = item.startsAt;
        }
        if (!item.endsAt.empty())
        {
            o["ends_at"] = item.endsAt;
        }
        if (!item.dueAt.empty())
        {
            o["due_at"] = item.dueAt;
        }
        if (!item.url.empty())
        {
            o["url"] = item.url;
        }
        if (!item.joinUrl.empty())
        {
            o["join_url"] = item.joinUrl;
        }
    }
    std::string out;
    serializeJson(doc, out);
    return out;
}

bool Feed_restore(const std::string &json)
{
    FeedDelta delta;
    if (!Feed_parse(json.data(), json.size(), delta))
    {
        return false;
    }
    delta.full = true;
    if (!Feed_apply(delta))
    {
        return false;
    }
    g_cacheDirty = false; // what was just loaded is what is on flash
    return true;
}

void Feed_setCacheSaver(void (*save)(const std::string &)) { g_cacheSave = save; }

bool Feed_cacheTick(int64_t nowMs, int64_t debounceMs)
{
    if (!g_cacheDirty || !g_cacheSave)
    {
        return false;
    }
    if (g_cacheDirtyAtMs < 0)
    {
        g_cacheDirtyAtMs = nowMs; // the debounce starts when the change was first seen by a tick
        return false;
    }
    if (nowMs - g_cacheDirtyAtMs < debounceMs)
    {
        return false;
    }
    g_cacheDirty = false;
    g_cacheSave(Feed_serialize());
    return true;
}

int64_t Feed_version() { return g_version; }
const std::string &Feed_epoch() { return g_epoch; }
size_t Feed_total() { return g_items.size(); }

size_t Feed_visibleCount()
{
    rebuildVisible();
    return g_visible.size();
}

const FeedItem &Feed_visibleAt(size_t index)
{
    rebuildVisible();
    return g_items.at(g_visible.at(index));
}

const std::map<int64_t, FeedItem> &Feed_all() { return g_items; }

const FeedItem *Feed_find(int64_t numericId)
{
    const auto found = g_items.find(numericId);
    return found == g_items.end() ? nullptr : &found->second;
}

bool Feed_isDismissed(int64_t numericId) { return g_dismissed.count(numericId) > 0; }

void Feed_dismiss(int64_t numericId)
{
    if (!g_items.count(numericId) || !g_dismissed.insert(numericId).second)
    {
        return;
    }
    if (g_dismissEpoch.empty())
    {
        g_dismissEpoch = g_epoch;
    }
    saveDismissed();
    changed();
}

void Feed_dismissAll()
{
    bool any = false;
    for (const auto &entry : g_items)
    {
        any |= g_dismissed.insert(entry.first).second;
    }
    if (any)
    {
        if (g_dismissEpoch.empty())
        {
            g_dismissEpoch = g_epoch;
        }
        saveDismissed();
        changed();
    }
}

uint32_t Feed_revision() { return g_revision; }

void Feed_clear()
{
    g_cacheDirty = false;
    g_items.clear();
    g_version = 0;
    g_epoch.clear();
    {
        std::lock_guard<std::mutex> lock(g_postMutex);
        g_posted.clear();
    }
    changed();
}
