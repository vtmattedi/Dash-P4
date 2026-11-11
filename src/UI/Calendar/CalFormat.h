#pragma once
#include <stddef.h>
#include <stdint.h>

#include <string>

#include "App/Calendar.h"
#include "lvgl.h"

// Text for the calendar views, in the panel's local time. Shared by the Calendar page, the resting screen and the
// Home card so "in 25 min" and "Tomorrow" read the same everywhere.

/// "14:05"
void CalFmt_clock(int64_t epoch, char *out, size_t size);

/// "14:00 - 14:30", "All day", "Due 14:00", "Due"
void CalFmt_span(const CalEntry &entry, char *out, size_t size);

/// "Today", "Tomorrow", otherwise "Wed 12 Nov". `dayStart` is a local midnight.
void CalFmt_day(int64_t dayStart, int64_t now, char *out, size_t size);

/// "now", "in 25 min", "in 2 h 10 min", "in 3 days" for a start `deltaSec` away (negative: already started).
void CalFmt_relative(int64_t deltaSec, char *out, size_t size);

/// "overdue", "due today", "due tomorrow", "due Wed 12 Nov", or "" for a task with no due date.
void CalFmt_due(const FeedItem &item, int64_t now, char *out, size_t size);

/// A stable colour per account ("Work", "Personal" ...), so an event's origin reads at a glance. Empty -> the accent.
lv_color_t CalFmt_accountColour(const std::string &account);
