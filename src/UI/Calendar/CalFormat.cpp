#include "CalFormat.h"

#include <stdio.h>
#include <time.h>

#include "UI/Theme.h"

namespace
{
struct tm localOf(int64_t epoch)
{
    const time_t t = static_cast<time_t>(epoch);
    struct tm local;
    localtime_r(&t, &local);
    return local;
}
} // namespace

void CalFmt_clock(int64_t epoch, char *out, size_t size)
{
    const struct tm t = localOf(epoch);
    snprintf(out, size, "%02d:%02d", t.tm_hour, t.tm_min);
}

void CalFmt_span(const CalEntry &entry, char *out, size_t size)
{
    char a[8], b[8];
    if (entry.kind == CalKind::Due)
    {
        if (entry.allDay)
        {
            snprintf(out, size, "Due");
        }
        else
        {
            CalFmt_clock(entry.start, a, sizeof(a));
            snprintf(out, size, "Due %s", a);
        }
        return;
    }
    if (entry.allDay)
    {
        snprintf(out, size, "All day");
        return;
    }
    CalFmt_clock(entry.start, a, sizeof(a));
    if (entry.end > entry.start)
    {
        CalFmt_clock(entry.end, b, sizeof(b));
        snprintf(out, size, "%s - %s", a, b);
    }
    else
    {
        snprintf(out, size, "%s", a);
    }
}

void CalFmt_day(int64_t dayStart, int64_t now, char *out, size_t size)
{
    const int64_t today = Cal_dayStart(now);
    if (dayStart == today)
    {
        snprintf(out, size, "Today");
    }
    else if (dayStart == Cal_dayStart(now, 1))
    {
        snprintf(out, size, "Tomorrow");
    }
    else
    {
        const struct tm t = localOf(dayStart + 3600);
        strftime(out, size, "%a %d %b", &t);
    }
}

void CalFmt_relative(int64_t deltaSec, char *out, size_t size)
{
    if (deltaSec <= 0)
    {
        snprintf(out, size, "now");
        return;
    }
    const int64_t minutes = (deltaSec + 59) / 60;
    if (minutes < 60)
    {
        snprintf(out, size, "in %d min", static_cast<int>(minutes));
    }
    else if (minutes < 24 * 60)
    {
        const int h = static_cast<int>(minutes / 60), m = static_cast<int>(minutes % 60);
        if (m)
        {
            snprintf(out, size, "in %d h %d min", h, m);
        }
        else
        {
            snprintf(out, size, "in %d h", h);
        }
    }
    else
    {
        snprintf(out, size, "in %d days", static_cast<int>(minutes / (24 * 60)));
    }
}

void CalFmt_due(const FeedItem &item, int64_t now, char *out, size_t size)
{
    out[0] = 0;
    int64_t due;
    bool dateOnly;
    if (!Cal_parseTime(item.dueAt, due, dateOnly))
    {
        return;
    }
    const int64_t today = Cal_dayStart(now);
    const int64_t dueDay = Cal_dayStart(due);
    // A date-only due date is overdue from the next day on; an instant is overdue once it has passed.
    if (dueDay < today || (!dateOnly && due < now))
    {
        snprintf(out, size, "overdue");
    }
    else if (dueDay == today)
    {
        snprintf(out, size, "due today");
    }
    else if (dueDay == Cal_dayStart(now, 1))
    {
        snprintf(out, size, "due tomorrow");
    }
    else
    {
        char day[24];
        CalFmt_day(dueDay, now, day, sizeof(day));
        snprintf(out, size, "due %s", day);
    }
}

lv_color_t CalFmt_accountColour(const std::string &account)
{
    if (account.empty())
    {
        return UI_COL_ACCENT;
    }
    uint32_t hash = 2166136261u; // FNV-1a
    for (unsigned char c : account)
    {
        hash = (hash ^ c) * 16777619u;
    }
    const lv_color_t palette[] = {UI_COL_INFO, UI_COL_OK, UI_COL_ACCENT, UI_COL_WARN, lv_color_hex(0xB48CFF),
                                  lv_color_hex(0x4FD1C5)};
    return palette[hash % (sizeof(palette) / sizeof(palette[0]))];
}
