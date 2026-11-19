#include <stdio.h>
#include <string.h>
#include <string>
#include <time.h>

#include "App/Calendar.h"
#include "App/Feed.h"
#include "Pages.h"
#include "UI/Calendar/CalFormat.h"
#include "UI/Calendar/EventDetail.h"
#include "UI/Theme.h"

// Calendar: the feed's events and deadlines laid out by day. (Tasks have their own page: PageTasks.)
//
//   +----------+---------------------------------------------+
//   | days     | the selected day's agenda                   |
//   | (14)     | all-day first, then timed; WOI deadlines    |
//   |          | tap a row for the detail                    |
//   +----------+---------------------------------------------+
//
// The day column is built once and only has its text updated. The agenda is rebuilt (it is short) whenever the
// calendar changed, the selected day changed, or the minute turned (so "happening now" and "already over" stay true).

namespace
{
constexpr int kDays = 14; // the server's default window: one day back, 14 ahead
constexpr int32_t kDayColW = 210;

lv_obj_t *g_dayRow[kDays];
lv_obj_t *g_dayLabel[kDays];
lv_obj_t *g_dayCount[kDays];
lv_obj_t *g_agendaTitle, *g_agendaSub, *g_agendaList;

int g_selected = 0; // offset from today
// What the lists were last built for. Any change (or g_dirty) rebuilds them.
uint32_t g_builtRevision = UINT32_MAX;
int g_builtSelected = -1;
int64_t g_builtMinute = -1;
bool g_dirty = true;

void restyleDays(int64_t now)
{
    const int64_t today = Cal_dayStart(now);
    for (int i = 0; i < kDays; ++i)
    {
        const int64_t day = Cal_dayStart(today, i);
        char text[24];
        CalFmt_day(day, now, text, sizeof(text));
        Theme_setText(g_dayLabel[i], text);

        const size_t count = Cal_countOnDay(day);
        char badge[8] = "";
        if (count > 0)
        {
            snprintf(badge, sizeof(badge), "%u", static_cast<unsigned>(count));
        }
        Theme_setText(g_dayCount[i], badge);

        const bool selected = i == g_selected;
        Theme_bgColor(g_dayRow[i], selected ? UI_COL_SURFACE_HI : UI_COL_SURFACE);
        Theme_textColor(g_dayLabel[i], selected ? UI_COL_ACCENT : i == 0 ? UI_COL_TEXT : UI_COL_TEXT_DIM);
    }
}

void onDay(lv_event_t *e)
{
    g_selected = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    g_dirty = true;
    PageCalendar_refresh(false);
}

void onOpen(lv_event_t *e)
{
    EventDetail_open(static_cast<int64_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}

lv_obj_t *tappableRow(lv_obj_t *parent, int64_t id, bool highlighted)
{
    lv_obj_t *row = lv_obj_create(parent);
    Theme_plain(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(row, 10, 0);
    lv_obj_set_style_pad_gap(row, 12, 0);
    lv_obj_set_style_radius(row, 10, 0);
    Theme_bgColor(row, UI_COL_SURFACE_ALT);
    lv_obj_set_style_bg_opa(row, highlighted ? LV_OPA_COVER : LV_OPA_50, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(row, onOpen, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(id)));
    return row;
}

lv_obj_t *colourBar(lv_obj_t *parent, lv_color_t colour)
{
    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, 4, 40);
    lv_obj_set_style_radius(bar, 2, 0);
    Theme_bgColor(bar, colour);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    return bar;
}

void addEntry(const CalEntry &entry, int64_t now)
{
    const bool happening = entry.kind == CalKind::Event && !entry.allDay && entry.start <= now && now < entry.end;
    const bool over = entry.kind == CalKind::Event && !entry.allDay && entry.end <= now;

    lv_obj_t *row = tappableRow(g_agendaList, entry.numericId, happening);
    colourBar(row, over ? UI_COL_TEXT_FAINT : CalFmt_accountColour(entry.account));

    char span[32];
    CalFmt_span(entry, span, sizeof(span));
    lv_obj_t *time = Theme_label(row, span, UI_FONT_S, over ? UI_COL_TEXT_FAINT : UI_COL_TEXT_DIM);
    lv_obj_set_width(time, 96);

    lv_obj_t *text = Theme_column(row, 0, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_style_pad_gap(text, 2, 0);
    lv_obj_t *title = Theme_label(text, entry.title.c_str(), UI_FONT_M,
                                  over ? UI_COL_TEXT_FAINT : happening ? UI_COL_ACCENT : UI_COL_TEXT);
    lv_obj_set_width(title, lv_pct(100));
    std::string sub = entry.account;
    if (happening)
    {
        char left[24];
        CalFmt_clock(entry.end, left, sizeof(left));
        sub += std::string(sub.empty() ? "" : "  \xE2\x80\xA2  ") + "until " + left;
    }
    else if (entry.kind == CalKind::Due)
    {
        sub += std::string(sub.empty() ? "" : "  \xE2\x80\xA2  ") + "task";
    }
    if (!sub.empty())
    {
        Theme_label(text, sub.c_str(), UI_FONT_S, UI_COL_TEXT_FAINT);
    }
}

void buildAgenda(int64_t now)
{
    const int64_t today = Cal_dayStart(now);
    const int64_t day = Cal_dayStart(today, g_selected);

    char name[24];
    CalFmt_day(day, now, name, sizeof(name));
    const time_t t = static_cast<time_t>(day + 3600);
    struct tm local;
    localtime_r(&t, &local);
    char date[40];
    strftime(date, sizeof(date), "%A %d %B", &local);
    Theme_setText(g_agendaTitle, date);

    lv_obj_clean(g_agendaList);
    const auto entries = Cal_onDay(day);
    char sub[32];
    snprintf(sub, sizeof(sub), "%s  \xE2\x80\xA2  %u item%s", name, static_cast<unsigned>(entries.size()),
             entries.size() == 1 ? "" : "s");
    Theme_setText(g_agendaSub, sub);

    if (entries.empty())
    {
        lv_obj_t *empty = Theme_label(g_agendaList, now > 1700000000 ? "Nothing scheduled." : "Waiting for the clock.",
                                      UI_FONT_M, UI_COL_TEXT_FAINT);
        lv_obj_set_width(empty, lv_pct(100));
        return;
    }
    for (const CalEntry *entry : entries)
    {
        addEntry(*entry, now);
    }
}

lv_obj_t *scrollList(lv_obj_t *parent)
{
    lv_obj_t *list = Theme_column(parent, lv_pct(100), 0);
    lv_obj_set_flex_grow(list, 1);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_style_pad_gap(list, 8, 0);
    return list;
}
} // namespace

lv_obj_t *PageCalendar_create(lv_obj_t *parent)
{
    lv_obj_t *page = Theme_row(parent, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(page, UI_GUTTER, 0);
    lv_obj_set_style_pad_gap(page, UI_GUTTER, 0);
    lv_obj_set_flex_align(page, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // Days.
    lv_obj_t *days = Theme_column(page, kDayColW, lv_pct(100));
    lv_obj_add_flag(days, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(days, LV_DIR_VER);
    lv_obj_set_style_pad_gap(days, 6, 0);
    for (int i = 0; i < kDays; ++i)
    {
        lv_obj_t *row = lv_obj_create(days);
        Theme_plain(row);
        lv_obj_set_size(row, lv_pct(100), 48);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(row, 14, 0);
        lv_obj_set_style_radius(row, 10, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(row, onDay, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        g_dayRow[i] = row;
        g_dayLabel[i] = Theme_label(row, "", UI_FONT_M, UI_COL_TEXT_DIM);
        g_dayCount[i] = Theme_label(row, "", UI_FONT_S, UI_COL_INFO);
    }

    // The selected day.
    lv_obj_t *agenda = Theme_card(page, 0, lv_pct(100));
    lv_obj_set_flex_grow(agenda, 1);
    lv_obj_set_style_pad_gap(agenda, 6, 0);
    g_agendaTitle = Theme_label(agenda, "", UI_FONT_XL, UI_COL_TEXT);
    g_agendaSub = Theme_label(agenda, "", UI_FONT_S, UI_COL_TEXT_DIM);
    g_agendaList = scrollList(agenda);

    PageCalendar_refresh(true);
    return page;
}

void PageCalendar_refresh(bool force)
{
    const int64_t now = static_cast<int64_t>(time(nullptr));
    const uint32_t revision = Cal_revision();
    const int64_t minute = now / 60; // "happening now" and "already over" change with the clock
    if (!force && !g_dirty && revision == g_builtRevision && g_selected == g_builtSelected && minute == g_builtMinute)
    {
        return;
    }
    g_dirty = false;
    g_builtRevision = revision;
    g_builtSelected = g_selected;
    g_builtMinute = minute;
    restyleDays(now);
    buildAgenda(now);
}
