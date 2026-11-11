#pragma once
#include <stdint.h>
#include <string>

// Polls notif.mattediworks.com for the versioned feed (see Feed.h) on its own task and posts each response to the
// LVGL task. The cursor (version + epoch) lives in this task: a delta is only ever advanced past after Feed_post()
// took it, and the cache is RAM, so cache and cursor cannot drift apart. The cache is not persisted; a reboot
// starts from version 0, which is one small request.
//
// Credentials: the dashboard token made in the server's /admin. NOTIF_API_TOKEN in creds.h is the default, the
// `FEED TOKEN <token>` console command stores one in the settings store and wins over it. Without a token nothing
// is requested.

struct FeedSyncStatus
{
    bool hasToken = false;
    int lastHttp = 0;          // last HTTP status, 0 when the request never got an answer
    uint32_t lastOkAgoMs = 0;  // UINT32_MAX when it never succeeded
    uint32_t polls = 0;
    uint32_t resets = 0;       // 409 reset_required answers (plus cache/stream mismatches)
    std::string message;       // "ok", "no token", "HTTP 401", "malformed response" ...
};

/// Read the token and URL, start the task. Call once on the LVGL task after the settings store is open.
void FeedSync_begin();

/// Store a token (empty clears the stored one, falling back to creds.h) and poll now.
void FeedSync_setToken(const std::string &token);

/// Poll now instead of at the next interval.
void FeedSync_pollNow();

/// Throw the cursor away: the next poll asks for version 0 and replaces the cache.
void FeedSync_resync();

FeedSyncStatus FeedSync_status();
