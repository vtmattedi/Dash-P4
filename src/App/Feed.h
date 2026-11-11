#pragma once
#include <stddef.h>
#include <stdint.h>
#include <map>
#include <string>
#include <vector>

// The notification feed from notif.mattediworks.com (MW Notification), the versioned delta protocol of
// mw-notification/docs/versioned-feed.md. Pure data and rules, no LVGL and no ESP-IDF, so the host tests cover it;
// the HTTP polling is FeedSync, the resting screen is the view (UI/Rest).
//
// This is not the on-panel event model (App/Notifications): that one is things the panel noticed itself. The feed
// is what the server wants shown (tasks, alerts from its providers). The panel only displays it and dismisses it
// locally; a dismissal is never sent back, and an item stays dismissed until the server removes it.
//
// Threading: FeedSync's task builds a FeedDelta and hands it over with Feed_post(), the only thread-safe call.
// Everything else is the LVGL task (decisions D7); Feed_tick() applies what was posted.

struct FeedItem
{
    int64_t numericId = 0; // the key: stable across renames and restarts, unlike the string id
    std::string id;        // provider id, "linear:MW-42"
    std::string source;    // "linear"
    std::string kind;      // "task"
    std::string title;
    std::string status;
    std::string description;
    std::string account; // connection.alias: which Google account / Linear workspace it came from
    std::string startsAt; // ISO instant, or YYYY-MM-DD for an all-day event (see Calendar.h)
    std::string endsAt;   // exclusive, like Google's
    std::string dueAt;    // ISO instant or YYYY-MM-DD (tasks)
    std::string url;

    bool operator==(const FeedItem &o) const
    {
        return numericId == o.numericId && id == o.id && source == o.source && kind == o.kind && title == o.title &&
               status == o.status && description == o.description && account == o.account && startsAt == o.startsAt &&
               endsAt == o.endsAt && dueAt == o.dueAt && url == o.url;
    }
};

/// One parsed response of GET /api/v1/feed?version=...
struct FeedDelta
{
    int64_t version = 0;
    std::string epoch;
    bool full = false; // replace the whole cache instead of patching it
    std::vector<FeedItem> items;
    std::vector<int64_t> removed;
};

/// Parse a 200 body. False when it is not a well-formed feed response (no numeric version, items that are not
/// objects, an item without a positive numeric_id). Nothing in `out` is meaningful after a failure.
bool Feed_parse(const char *json, size_t length, FeedDelta &out);

/// Hand a delta to the LVGL task. Safe from any task.
void Feed_post(FeedDelta &&delta);

/// Load the dismissals saved by `save` last time and remember `save` for the next change. Call once, on the LVGL
/// task, after the settings store is open. `stored` is the string `save` was last given ("" when none).
void Feed_begin(const std::string &stored, void (*save)(const std::string &));

/// Apply everything posted since the last call. Call from an lv_timer. Returns false when a posted patch did not
/// fit the cache (see Feed_apply): the poller must restart from version 0.
bool Feed_tick();

/// Apply one delta now (what Feed_tick() does per posted delta). Returns false, leaving the cache untouched, when a
/// patch arrives for another epoch than the cache holds: the caller must resync from version 0.
bool Feed_apply(const FeedDelta &delta);

/// The cache and its stream position.
int64_t Feed_version();
const std::string &Feed_epoch();
size_t Feed_total(); // every cached item, dismissed or not

/// What is left to show: cached and not dismissed, in the order the server created them (numeric_id ascending).
size_t Feed_visibleCount();
const FeedItem &Feed_visibleAt(size_t index);

/// Every cached item by numeric_id, dismissed or not, and one lookup of it (null when absent).
const std::map<int64_t, FeedItem> &Feed_all();
const FeedItem *Feed_find(int64_t numericId);
bool Feed_isDismissed(int64_t numericId);

/// Dismiss on this panel only. Persisted, scoped to the current epoch.
void Feed_dismiss(int64_t numericId);
void Feed_dismissAll();

/// Bumped whenever the visible list changes.
uint32_t Feed_revision();

/// Forget the cache (not the dismissals). For tests and for "reset_required".
void Feed_clear();
