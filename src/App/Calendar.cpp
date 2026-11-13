#include "Calendar.h"

#include <ctype.h>

#include <algorithm>
#include <stdlib.h>
#include <string.h>
#include <utility>
#include <time.h>

namespace
{
std::vector<CalEntry> g_entries; // sorted: start, then all-day first
uint32_t g_feedRevision = UINT32_MAX;
uint32_t g_revision = 0;

// Days since 1970-01-01 of a civil date (Howard Hinnant's algorithm), so an instant with an offset needs no timegm().
int64_t daysFromCivil(int y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

bool digits(const char *p, int count, int &out)
{
    int v = 0;
    for (int i = 0; i < count; ++i)
    {
        if (p[i] < '0' || p[i] > '9')
        {
            return false;
        }
        v = v * 10 + (p[i] - '0');
    }
    out = v;
    return true;
}

int64_t localMidnight(int y, int m, int d)
{
    struct tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon = m - 1;
    t.tm_mday = d;
    t.tm_isdst = -1;
    return static_cast<int64_t>(mktime(&t));
}

void rebuild()
{
    g_entries.clear();
    for (const auto &pair : Feed_all())
    {
        const FeedItem &item = pair.second;
        CalEntry e;
        e.numericId = item.numericId;
        e.title = item.title.empty() ? item.id : item.title;
        e.account = item.account;
        e.description = item.description;
        e.status = item.status;
        e.url = item.url;
        e.joinUrl = item.joinUrl;
        e.source = item.source;
        bool dateOnly = false;
        if (item.kind == "event")
        {
            int64_t start = 0, end = 0;
            bool endDateOnly = false;
            if (!Cal_parseTime(item.startsAt, start, dateOnly))
            {
                continue;
            }
            if (!Cal_parseTime(item.endsAt, end, endDateOnly) || end < start)
            {
                end = dateOnly ? Cal_dayStart(start, 1) : start; // no usable end: a whole day or a moment
            }
            e.kind = CalKind::Event;
            e.allDay = dateOnly;
            e.start = start;
            e.end = end;
        }
        else if (item.kind == "task" && item.source == "woi")
        {
            // Only WOI deadlines live on the calendar; Linear tasks are the todo list.
            int64_t due = 0;
            if (!Cal_parseTime(item.dueAt, due, dateOnly))
            {
                continue;
            }
            e.kind = CalKind::Due;
            e.allDay = dateOnly;
            e.start = e.end = due;
        }
        else
        {
            continue;
        }
        g_entries.push_back(std::move(e));
    }
    std::stable_sort(g_entries.begin(), g_entries.end(), [](const CalEntry &a, const CalEntry &b) {
        return a.start != b.start ? a.start < b.start : a.allDay > b.allDay;
    });
    ++g_revision;
}

void sync()
{
    if (g_feedRevision != Feed_revision())
    {
        g_feedRevision = Feed_revision();
        rebuild();
    }
}

bool touches(const CalEntry &e, int64_t dayStart, int64_t dayEnd)
{
    if (e.end <= e.start)
    {
        return e.start >= dayStart && e.start < dayEnd;
    }
    return e.start < dayEnd && e.end > dayStart;
}
} // namespace

bool Cal_parseTime(const std::string &text, int64_t &epoch, bool &dateOnly)
{
    const char *s = text.c_str();
    int y, mo, d;
    if (text.size() < 10 || !digits(s, 4, y) || s[4] != '-' || !digits(s + 5, 2, mo) || s[7] != '-' ||
        !digits(s + 8, 2, d) || mo < 1 || mo > 12 || d < 1 || d > 31)
    {
        return false;
    }
    if (text.size() == 10)
    {
        dateOnly = true;
        epoch = localMidnight(y, mo, d);
        return true;
    }
    int hh, mm, ss;
    if (text.size() < 19 || (s[10] != 'T' && s[10] != 't' && s[10] != ' ') || !digits(s + 11, 2, hh) || s[13] != ':' ||
        !digits(s + 14, 2, mm) || s[16] != ':' || !digits(s + 17, 2, ss) || hh > 23 || mm > 59 || ss > 60)
    {
        return false;
    }
    size_t pos = 19;
    if (pos < text.size() && s[pos] == '.') // fractional seconds
    {
        ++pos;
        while (pos < text.size() && s[pos] >= '0' && s[pos] <= '9')
        {
            ++pos;
        }
    }
    int64_t offset = 0;
    if (pos < text.size() && (s[pos] == 'Z' || s[pos] == 'z'))
    {
        ++pos;
    }
    else if (pos < text.size() && (s[pos] == '+' || s[pos] == '-'))
    {
        int oh, om;
        if (text.size() < pos + 6 || !digits(s + pos + 1, 2, oh) || s[pos + 3] != ':' || !digits(s + pos + 4, 2, om))
        {
            return false;
        }
        offset = (oh * 3600 + om * 60) * (s[pos] == '-' ? -1 : 1);
        pos += 6;
    }
    else
    {
        return false; // an instant without an offset is ambiguous; the server always sends one
    }
    if (pos != text.size())
    {
        return false;
    }
    dateOnly = false;
    epoch = daysFromCivil(y, static_cast<unsigned>(mo), static_cast<unsigned>(d)) * 86400 + hh * 3600 + mm * 60 + ss - offset;
    return true;
}

int64_t Cal_dayStart(int64_t epoch, int addDays)
{
    const time_t t = static_cast<time_t>(epoch);
    struct tm local;
    localtime_r(&t, &local);
    local.tm_hour = local.tm_min = local.tm_sec = 0;
    local.tm_mday += addDays;
    local.tm_isdst = -1;
    return static_cast<int64_t>(mktime(&local));
}

std::vector<const CalEntry *> Cal_onDay(int64_t dayStart)
{
    sync();
    const int64_t dayEnd = Cal_dayStart(dayStart + 3600 * 12, 1);
    std::vector<const CalEntry *> out;
    for (const CalEntry &e : g_entries)
    {
        if (touches(e, dayStart, dayEnd) && !(e.kind == CalKind::Due && Feed_isDismissed(e.numericId)))
        {
            out.push_back(&e);
        }
    }
    return out; // g_entries is sorted already
}

size_t Cal_countOnDay(int64_t dayStart) { return Cal_onDay(dayStart).size(); }

const CalEntry *Cal_happening(int64_t now)
{
    sync();
    const CalEntry *best = nullptr;
    for (const CalEntry &e : g_entries)
    {
        if (e.kind == CalKind::Event && !e.allDay && e.start <= now && now < e.end)
        {
            best = &e; // sorted by start, so the last match started last
        }
    }
    return best;
}

const CalEntry *Cal_next(int64_t now)
{
    sync();
    for (const CalEntry &e : g_entries)
    {
        if (e.kind == CalKind::Event && !e.allDay && e.start > now)
        {
            return &e;
        }
    }
    return nullptr;
}

std::vector<const FeedItem *> Cal_todos()
{
    sync();
    // Key: in-progress first, then by due date (overdue first), undated last.
    std::vector<std::pair<std::pair<int, int64_t>, const FeedItem *>> ranked;
    for (const auto &pair : Feed_all())
    {
        const FeedItem &item = pair.second;
        if (item.kind != "task" || Feed_isDismissed(item.numericId) || item.status == "done" ||
            item.status == "completed" || item.status == "canceled")
        {
            continue;
        }
        int64_t due = INT64_MAX;
        bool dateOnly;
        if (!Cal_parseTime(item.dueAt, due, dateOnly))
        {
            due = INT64_MAX;
        }
        const bool started = item.status == "in_progress" || item.status == "started";
        ranked.push_back({{started ? 0 : 1, due}, &item});
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    std::vector<const FeedItem *> out;
    for (const auto &r : ranked)
    {
        out.push_back(r.second);
    }
    return out;
}

TaskGroup Cal_taskGroup(const FeedItem &item)
{
    std::string s = item.status;
    for (char &c : s)
    {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    if (s.find("review") != std::string::npos)
    {
        return TaskGroup::Review;
    }
    if (s == "in_progress" || s == "started")
    {
        return TaskGroup::Doing;
    }
    return TaskGroup::Todo;
}

const char *Cal_taskGroupName(TaskGroup group)
{
    switch (group)
    {
    case TaskGroup::Doing:
        return "Doing";
    case TaskGroup::Review:
        return "Review";
    default:
        return "To do";
    }
}

std::vector<const FeedItem *> Cal_tasks(const std::string &source, int group)
{
    std::vector<const FeedItem *> out;
    for (const FeedItem *item : Cal_todos())
    {
        if ((source.empty() || item->source == source) &&
            (group < 0 || static_cast<int>(Cal_taskGroup(*item)) == group))
        {
            out.push_back(item);
        }
    }
    return out;
}

size_t Cal_count()
{
    sync();
    return g_entries.size();
}

const FeedItem *Cal_item(int64_t numericId) { return Feed_find(numericId); }

uint32_t Cal_revision()
{
    sync();
    return g_revision;
}
