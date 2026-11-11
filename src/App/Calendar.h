#pragma once
#include <stdint.h>
#include <string>
#include <vector>

#include "Feed.h"

// The calendar: a time-ordered view over the feed. Pure rules, no LVGL, so the host tests cover it.
//
// What the server sends (mw-notification FeedItem):
//   kind "event"  starts_at / ends_at, either ISO instants with an offset ("2026-11-10T14:00:00-03:00") or, for an
//                 all-day event, bare dates ("2026-11-10"). ends_at is EXCLUSIVE, like Google's.
//   kind "task"   from source "woi": due_at, an instant or a date; a WOI task with no due date is not shown.
//                 from source "linear": not on the calendar, it is a todo (Cal_todos).
//   kind "service" (stackport): an alert while status is "down" (Cal_alerts), never on the calendar.
//
// An entry is placed on LOCAL days (the panel's timezone), so an evening event never lands on tomorrow. All-day
// events cover whole local days; a due task is a point in time.
//
// Rebuilt lazily from the feed: every call first checks Feed_revision(). LVGL task only.

enum class CalKind : uint8_t
{
    Event, // starts_at..ends_at
    Due    // a task's due date
};

struct CalEntry
{
    int64_t numericId = 0; // the feed item's id, for Feed_dismiss and detail lookups
    CalKind kind = CalKind::Event;
    bool allDay = false;
    int64_t start = 0; // epoch seconds (UTC); for all-day, local midnight of the first day
    int64_t end = 0;   // exclusive; == start for a Due entry
    std::string title, account, description, status, url, source;
};

/// Parse a feed timestamp. `dateOnly` is set for "YYYY-MM-DD", whose epoch is local midnight. False on anything
/// else (including empty).
bool Cal_parseTime(const std::string &text, int64_t &epoch, bool &dateOnly);

/// Local midnight of the day `addDays` after the one containing `epoch` (negative for earlier).
int64_t Cal_dayStart(int64_t epoch, int addDays = 0);

/// Entries that touch the local day starting at `dayStart`, all-day first, then by start. Due entries the user
/// dismissed are left out.
std::vector<const CalEntry *> Cal_onDay(int64_t dayStart);

/// How many entries touch that day (the dots on a day strip).
size_t Cal_countOnDay(int64_t dayStart);

/// The timed event happening at `now` (the one that started last when several overlap), or null.
const CalEntry *Cal_happening(int64_t now);

/// The next timed event starting after `now`, or null. All-day entries are never "next".
const CalEntry *Cal_next(int64_t now);

/// Type 3, todos: every open Linear and WOI task (not done), in progress first, then by due date, undated last.
/// A WOI task with a due date is also a calendar deadline. Not dismissed.
std::vector<const FeedItem *> Cal_todos();

/// Where a task is at, collapsed to the three columns of the Tasks board. Linear sends its state type (triage, backlog,
/// unstarted, started) and WOI todo / in_progress; anything with "review" in its status (a state name the server may
/// send later) is Review, because Linear's "In Review" state is of type "started" and cannot be told apart by type.
enum class TaskGroup : uint8_t
{
    Todo,  // todo, unstarted, backlog, triage, anything unknown
    Doing, // in_progress, started
    Review // status mentions "review"
};
TaskGroup Cal_taskGroup(const FeedItem &item);
const char *Cal_taskGroupName(TaskGroup group); // "To do", "Doing", "Review"

/// The Tasks page's list: Cal_todos() narrowed to one source ("linear", "woi"; empty = any) and one group
/// (-1 = any, else a TaskGroup value). Same order as Cal_todos().
std::vector<const FeedItem *> Cal_tasks(const std::string &source, int group);

/// How many services the feed knows about and how many of them are down (Stackport watcher).
struct ServiceSummary
{
    size_t total = 0, down = 0;
};
ServiceSummary Cal_services();

/// Type 2, Stackport watcher: services whose status is "down", not dismissed.
std::vector<const FeedItem *> Cal_alerts();

/// What wants attention outside the calendar, in the order to show it: down services, then Linear todos.
std::vector<const FeedItem *> Cal_attention();

/// Total entries on the calendar (dismissed Due entries included).
size_t Cal_count();

/// The feed item behind an entry or a list row, or null once the server removed it.
const FeedItem *Cal_item(int64_t numericId);

/// Bumped when the calendar's contents changed.
uint32_t Cal_revision();
