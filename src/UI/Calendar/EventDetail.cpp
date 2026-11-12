#include "EventDetail.h"

#include <string>
#include <time.h>

#include "App/Calendar.h"
#include "App/Feed.h"
#include "CalFormat.h"
#include "UI/Modal.h"
#include "UI/Theme.h"

namespace
{
lv_obj_t *line(lv_obj_t *body, const std::string &text, const lv_font_t *font, lv_color_t colour)
{
    lv_obj_t *label = Theme_label(body, text.c_str(), font, colour);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}

// "Tue 10 Nov, 14:00 - 14:30" / "Tue 10 Nov - Wed 11 Nov, all day"
std::string whenText(const FeedItem &item, int64_t now)
{
    int64_t start = 0, end = 0;
    bool startDate = false, endDate = false;
    const bool hasStart = Cal_parseTime(item.kind == "event" ? item.startsAt : item.dueAt, start, startDate);
    if (!hasStart)
    {
        return "";
    }
    char day[24], from[8], to[8];
    CalFmt_day(Cal_dayStart(start), now, day, sizeof(day));
    if (item.kind != "event")
    {
        CalFmt_clock(start, from, sizeof(from));
        return startDate ? std::string("Due ") + day : std::string("Due ") + day + ", " + from;
    }
    const bool hasEnd = Cal_parseTime(item.endsAt, end, endDate);
    if (startDate)
    {
        // All day: the end is exclusive, so the last day shown is the one before it.
        std::string text = day;
        if (hasEnd && Cal_dayStart(end, -1) > Cal_dayStart(start))
        {
            char last[24];
            CalFmt_day(Cal_dayStart(end, -1), now, last, sizeof(last));
            text += std::string(" - ") + last;
        }
        return text + ", all day";
    }
    CalFmt_clock(start, from, sizeof(from));
    std::string text = std::string(day) + ", " + from;
    if (hasEnd && end > start)
    {
        CalFmt_clock(end, to, sizeof(to));
        text += std::string(" - ") + to;
    }
    return text;
}
} // namespace

void EventDetail_open(int64_t numericId)
{
    const FeedItem *found = Feed_find(numericId);
    if (!found)
    {
        return;
    }
    const FeedItem item = *found; // the cache can change under an open sheet

    ModalOptions options;
    options.title = item.kind == "event" ? "Event" : item.kind == "task" ? "Task" : "Alert";
    lv_obj_t *body = nullptr;
    lv_obj_t *footer = nullptr;
    const ModalId id = Modal_open(options, &body, &footer);
    if (!body)
    {
        return;
    }

    const int64_t now = static_cast<int64_t>(time(nullptr));
    line(body, item.title.empty() ? item.id : item.title, UI_FONT_XL, UI_COL_TEXT);
    const std::string when = whenText(item, now);
    if (!when.empty())
    {
        line(body, when, UI_FONT_L, UI_COL_ACCENT);
    }
    if (!item.joinUrl.empty())
    {
        line(body, "Join: " + item.joinUrl, UI_FONT_M, UI_COL_INFO);
    }
    std::string origin = item.account.empty() ? item.source : item.account + "  \xE2\x80\xA2  " + item.source;
    if (!item.status.empty())
    {
        origin += "  \xE2\x80\xA2  " + item.status;
    }
    line(body, origin, UI_FONT_M, CalFmt_accountColour(item.account));
    if (!item.description.empty())
    {
        line(body, item.description, UI_FONT_M, UI_COL_TEXT_DIM);
    }
    if (!item.url.empty())
    {
        line(body, item.url, UI_FONT_S, UI_COL_TEXT_FAINT);
    }

    if (footer && item.kind != "event")
    {
        // An event leaves the calendar by itself when it ends; a task or alert stays until the server drops it
        // or it is dismissed here.
        const int64_t itemId = item.numericId;
        Theme_actionButton(footer, "Dismiss", 140, UI_TOUCH, Tone::Warn, [id, itemId]() {
            Feed_dismiss(itemId);
            Modal_close(id);
        });
    }
}
