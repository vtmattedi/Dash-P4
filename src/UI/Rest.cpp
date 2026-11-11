#include "Rest.h"

#include <stdio.h>
#include <string.h>
#include <string>
#include <time.h>
#include <vector>

#include "App/Calendar.h"
#include "App/Feed.h"
#include "App/FeedSync.h"
#include "App/Net.h"
#include "App/Prefs.h"
#include "Calendar/CalFormat.h"
#include "Modal.h"
#include "StatusIcons.h"
#include "Theme.h"
#include "Toast.h"
#include "board.h"

LV_FONT_DECLARE(font_clock_200);
LV_IMAGE_DECLARE(img_mw_logo);

// The resting screen: a black wall display. Layout (1024 x 600):
//
//   +-----------------------------------------------------------+
//   | problems (left)                           status icons    |  top bar
//   |        12:34                    | NOW   current event     |
//   |     Tuesday 11 November         | +---------------------+ |
//   |  [ Stackport: all up / N down ] | | NEXT        in 25 m | |  calendar
//   |  TODOS                          | |  Next event title   | |
//   |  . todo                  due    | +---------------------+ |
//   |  . todo                         |  later events ...       |
//   +-----------------------------------------------------------+
//
// Only feed data and the network are shown here; the NightMare devices are left out on purpose for now.

namespace
{
lv_obj_t *g_root;
// The clock is five cells of fixed width: the font is proportional (a 1 is much narrower than a 0), so one label would
// shift sideways every minute and make the colon dance. Cells 0,1 = hours, 2 = colon, 3,4 = minutes.
constexpr int kDigitCell = 136; // widest digit of font_clock_200 (advance 2141/16) plus a little air
constexpr int kColonCell = 56;
// Where each cell is centred, in a 600 px wide clock whose middle is the colon. The two hour digits and the two minute
// digits sit 124 px apart (a bit closer than a cell), and the colon is 90 px from the nearest digit.
constexpr int kClockWidth = 600;
constexpr int kCellCentre[5] = {86, 210, 300, 390, 514};
lv_obj_t *g_clockCell[5];

constexpr int kLeftX = 8;      // the clock box's left edge; the left column is kClockWidth wide
constexpr int kBarH = 44;      // top bar
constexpr int kRightX = 640;   // calendar column
constexpr int kRightW = 360;
constexpr int kTodoRows = 4;
constexpr int kLaterRows = 4;

// Top bar.
lv_obj_t *g_problems, *g_iconFeed;
StatusIcons g_icons;
// Clock block.
lv_obj_t *g_date;
// Stackport.
lv_obj_t *g_stackPanel, *g_stackArc, *g_stackCount, *g_stackTitle, *g_stackSub;
// Todos.
struct TodoRow
{
    lv_obj_t *root, *dot, *title, *due;
};
lv_obj_t *g_todoHeader;
TodoRow g_todo[kTodoRows];
// Calendar.
lv_obj_t *g_nowTag, *g_nowText;
lv_obj_t *g_nextPanel, *g_nextBar, *g_nextTag, *g_nextRel, *g_nextTitle, *g_nextWhen, *g_nextAccount;
lv_obj_t *g_laterHeader;
struct LaterRow
{
    lv_obj_t *root, *bar, *time, *title;
};
LaterRow g_later[kLaterRows];

bool g_resting = false;
bool g_forced = false; // Rest_now(): rest even with the timeout set to never
void (*g_afterTouchWake)() = nullptr; // Rest_nowThen(): one shot

void setClock(int h1, int h2, int m1, int m2)
{
    const int digits[4] = {h1, h2, m1, m2};
    const int cells[4] = {0, 1, 3, 4};
    for (int i = 0; i < 4; ++i)
    {
        char text[2] = {digits[i] < 0 ? '-' : static_cast<char>('0' + digits[i]), 0};
        Theme_setText(g_clockCell[cells[i]], text);
    }
}

// The colon breathes: it fades down and back up once every two seconds, only while the rest screen is showing.
void animColon(void *obj, int32_t v) { lv_obj_set_style_text_opa(static_cast<lv_obj_t *>(obj), static_cast<lv_opa_t>(v), 0); }

void startColonFade()
{
    lv_anim_delete(g_clockCell[2], animColon);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, g_clockCell[2]);
    lv_anim_set_exec_cb(&a, animColon);
    lv_anim_set_values(&a, LV_OPA_COVER, 40);
    lv_anim_set_duration(&a, 1000);
    lv_anim_set_playback_duration(&a, 1000);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

void stopColonFade()
{
    lv_anim_delete(g_clockCell[2], animColon);
    lv_obj_set_style_text_opa(g_clockCell[2], LV_OPA_COVER, 0);
}

void show(lv_obj_t *obj, bool shown)
{
    if (shown)
    {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    }
}

// A label that stays on one line and ends in "..." instead of wrapping.
lv_obj_t *lineLabel(lv_obj_t *parent, const lv_font_t *font, lv_color_t colour)
{
    lv_obj_t *label = Theme_label(parent, "", font, colour);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_height(label, lv_font_get_line_height(font));
    return label;
}

// A rounded dark panel placed at an absolute position; children are laid out by the caller.
lv_obj_t *panel(lv_obj_t *parent, int x, int y, int w, int h)
{
    lv_obj_t *p = lv_obj_create(parent);
    Theme_plain(p);
    lv_obj_set_pos(p, x, y);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, UI_COL_SURFACE, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(p, UI_RADIUS, 0);
    lv_obj_set_style_border_width(p, 1, 0);
    lv_obj_set_style_border_color(p, UI_COL_BORDER, 0);
    return p;
}

lv_obj_t *bar(lv_obj_t *parent, int w, int h)
{
    lv_obj_t *b = lv_obj_create(parent);
    lv_obj_remove_style_all(b);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, 3, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    return b;
}

// ---------------------------------------------------------------- top bar

void refreshTopBar()
{
    const NetState net = Net_state();
    const FeedSyncStatus feed = FeedSync_status();

    std::string problems;
    if (!net.wifiIp)
    {
        problems = "No Wi-Fi";
    }
    else if (!feed.hasToken)
    {
        problems = "Feed: no token";
    }
    else if (feed.message != "ok" && feed.lastHttp != 0)
    {
        problems = "Feed: " + feed.message;
    }
    Theme_setText(g_problems, problems.c_str());

    StatusIcons_refresh(g_icons);
    const bool feedOk = feed.hasToken && feed.message == "ok";
    lv_obj_set_style_text_color(g_iconFeed, feedOk ? UI_COL_TEXT_DIM : feed.hasToken ? UI_COL_WARN : UI_COL_DANGER, 0);
}

// ---------------------------------------------------------------- stackport

void refreshStackport()
{
    const ServiceSummary s = Cal_services();
    const auto alerts = Cal_alerts();
    char buf[48];
    const unsigned up = static_cast<unsigned>(s.total - s.down);
    lv_arc_set_range(g_stackArc, 0, s.total > 0 ? static_cast<int32_t>(s.total) : 1);
    lv_arc_set_value(g_stackArc, static_cast<int32_t>(up));
    snprintf(buf, sizeof(buf), s.total > 0 ? "%u" : "-", up);
    Theme_setText(g_stackCount, buf);
    if (!alerts.empty())
    {
        lv_obj_set_style_border_color(g_stackPanel, UI_COL_DANGER, 0);
        lv_obj_set_style_arc_color(g_stackArc, UI_COL_DANGER, LV_PART_INDICATOR);
        snprintf(buf, sizeof(buf), "Stackport: %u down", static_cast<unsigned>(alerts.size()));
        Theme_setText(g_stackTitle, buf);
        lv_obj_set_style_text_color(g_stackTitle, UI_COL_DANGER, 0);
        std::string names;
        for (const FeedItem *item : alerts)
        {
            names += (names.empty() ? "" : ", ") + (item->title.empty() ? item->id : item->title);
        }
        Theme_setText(g_stackSub, names.c_str());
    }
    else if (s.total > 0)
    {
        lv_obj_set_style_border_color(g_stackPanel, UI_COL_BORDER, 0);
        lv_obj_set_style_arc_color(g_stackArc, UI_COL_OK, LV_PART_INDICATOR);
        Theme_setText(g_stackTitle, "Stackport");
        lv_obj_set_style_text_color(g_stackTitle, UI_COL_OK, 0);
        snprintf(buf, sizeof(buf), "All %u services up", static_cast<unsigned>(s.total));
        Theme_setText(g_stackSub, buf);
    }
    else
    {
        lv_obj_set_style_border_color(g_stackPanel, UI_COL_BORDER, 0);
        lv_obj_set_style_arc_color(g_stackArc, UI_COL_TEXT_FAINT, LV_PART_INDICATOR);
        Theme_setText(g_stackTitle, "Stackport");
        lv_obj_set_style_text_color(g_stackTitle, UI_COL_TEXT_DIM, 0);
        Theme_setText(g_stackSub, "No data yet");
    }
}

// ---------------------------------------------------------------- todos

void refreshTodos(int64_t now)
{
    const auto todos = Cal_todos();
    char buf[32];
    snprintf(buf, sizeof(buf), todos.empty() ? "TODO" : "TODO  \xE2\x80\xA2  %u", static_cast<unsigned>(todos.size()));
    Theme_setText(g_todoHeader, buf);

    for (int i = 0; i < kTodoRows; ++i)
    {
        TodoRow &row = g_todo[i];
        if (i >= static_cast<int>(todos.size()))
        {
            show(row.root, i == 0 && todos.empty());
            if (i == 0 && todos.empty())
            {
                lv_obj_set_style_bg_opa(row.dot, LV_OPA_TRANSP, 0);
                Theme_setText(row.title, "Nothing to do");
                lv_obj_set_style_text_color(row.title, UI_COL_TEXT_FAINT, 0);
                Theme_setText(row.due, "");
            }
            continue;
        }
        const FeedItem &item = *todos[i];
        show(row.root, true);
        const bool started = item.status == "in_progress" || item.status == "started";
        lv_obj_set_style_bg_opa(row.dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row.dot, started ? UI_COL_ACCENT : UI_COL_TEXT_FAINT, 0);
        Theme_setText(row.title, item.title.empty() ? item.id.c_str() : item.title.c_str());
        lv_obj_set_style_text_color(row.title, UI_COL_TEXT, 0);
        char due[32];
        CalFmt_due(item, now, due, sizeof(due));
        const bool overdue = strcmp(due, "overdue") == 0;
        Theme_setText(row.due, due[0] ? due : item.source.c_str());
        lv_obj_set_style_text_color(row.due, overdue ? UI_COL_DANGER : UI_COL_TEXT_DIM, 0);
    }
}

// ---------------------------------------------------------------- calendar

void setLater(LaterRow &row, bool shown, lv_color_t colour = UI_COL_BORDER, const char *time = "",
              const char *title = "", lv_color_t titleColour = UI_COL_TEXT)
{
    show(row.root, shown);
    if (!shown)
    {
        return;
    }
    lv_obj_set_style_bg_color(row.bar, colour, 0);
    Theme_setText(row.time, time);
    Theme_setText(row.title, title);
    lv_obj_set_style_text_color(row.title, titleColour, 0);
}

void refreshCalendar(int64_t now)
{
    const bool clockOk = now > 1700000000;
    const CalEntry *current = clockOk ? Cal_happening(now) : nullptr;
    const CalEntry *next = clockOk ? Cal_next(now) : nullptr;

    // NOW: the event under way, else today's all-day event, else "free".
    const CalEntry *allDay = nullptr;
    if (clockOk && !current)
    {
        for (const CalEntry *e : Cal_onDay(Cal_dayStart(now)))
        {
            if (e->kind == CalKind::Event && e->allDay)
            {
                allDay = e;
                break;
            }
        }
    }
    char buf[96];
    if (current)
    {
        char until[8];
        CalFmt_clock(current->end, until, sizeof(until));
        snprintf(buf, sizeof(buf), "%s  \xE2\x80\xA2  until %s", current->title.c_str(), until);
        Theme_setText(g_nowTag, "NOW");
        Theme_setText(g_nowText, buf);
        lv_obj_set_style_text_color(g_nowText, UI_COL_ACCENT, 0);
    }
    else if (allDay)
    {
        Theme_setText(g_nowTag, "TODAY");
        Theme_setText(g_nowText, allDay->title.c_str());
        lv_obj_set_style_text_color(g_nowText, UI_COL_TEXT, 0);
    }
    else
    {
        Theme_setText(g_nowTag, "NOW");
        Theme_setText(g_nowText, clockOk ? "Free" : "Waiting for the clock");
        lv_obj_set_style_text_color(g_nowText, UI_COL_TEXT_FAINT, 0);
    }

    // NEXT: the big card.
    if (next)
    {
        lv_obj_set_style_bg_color(g_nextBar, CalFmt_accountColour(next->account), 0);
        lv_obj_set_style_bg_opa(g_nextBar, LV_OPA_COVER, 0);
        char rel[32];
        CalFmt_relative(next->start - now, rel, sizeof(rel));
        Theme_setText(g_nextRel, rel);
        Theme_setText(g_nextTitle, next->title.c_str());
        lv_obj_set_style_text_color(g_nextTitle, UI_COL_TEXT, 0);
        char day[24] = "", span[40];
        const int64_t startDay = Cal_dayStart(next->start);
        if (startDay != Cal_dayStart(now))
        {
            CalFmt_day(startDay, now, day, sizeof(day));
        }
        CalFmt_span(*next, span, sizeof(span));
        snprintf(buf, sizeof(buf), "%s%s%s", day, day[0] ? "  " : "", span);
        Theme_setText(g_nextWhen, buf);
        Theme_setText(g_nextAccount, next->account.c_str());
    }
    else
    {
        lv_obj_set_style_bg_opa(g_nextBar, LV_OPA_TRANSP, 0);
        Theme_setText(g_nextRel, "");
        Theme_setText(g_nextTitle, clockOk ? "Nothing coming up" : "");
        lv_obj_set_style_text_color(g_nextTitle, UI_COL_TEXT_FAINT, 0);
        Theme_setText(g_nextWhen, "");
        Theme_setText(g_nextAccount, "");
    }

    // LATER: what follows the next event, within two days.
    std::vector<const CalEntry *> later;
    if (next)
    {
        const int64_t today = Cal_dayStart(now);
        for (int d = 0; d < 3; ++d)
        {
            for (const CalEntry *e : Cal_onDay(Cal_dayStart(today, d)))
            {
                if (e->kind == CalKind::Event && !e->allDay && e != next && e->start >= next->start &&
                    e->start > now && e->start >= Cal_dayStart(today, d))
                {
                    later.push_back(e);
                }
            }
        }
    }
    show(g_laterHeader, !later.empty());
    if (!later.empty())
    {
        snprintf(buf, sizeof(buf), later.size() > static_cast<size_t>(kLaterRows) ? "THEN  \xE2\x80\xA2  %u" : "THEN",
                 static_cast<unsigned>(later.size()));
        Theme_setText(g_laterHeader, buf);
    }
    for (int i = 0; i < kLaterRows; ++i)
    {
        if (i >= static_cast<int>(later.size()))
        {
            setLater(g_later[i], false);
            continue;
        }
        const CalEntry &e = *later[i];
        char time[24];
        const int64_t day = Cal_dayStart(e.start);
        if (day == Cal_dayStart(now))
        {
            CalFmt_clock(e.start, time, sizeof(time));
        }
        else
        {
            const time_t t = static_cast<time_t>(e.start);
            struct tm local;
            localtime_r(&t, &local);
            strftime(time, sizeof(time), "%a %H:%M", &local);
        }
        setLater(g_later[i], true, CalFmt_accountColour(e.account), time, e.title.c_str());
    }
}

void refresh()
{
    const time_t now = time(nullptr);
    if (now > 1700000000)
    {
        struct tm local;
        localtime_r(&now, &local);
        setClock(local.tm_hour / 10, local.tm_hour % 10, local.tm_min / 10, local.tm_min % 10);
        char buf[48];
        strftime(buf, sizeof(buf), "%A %d %B", &local);
        Theme_setText(g_date, buf);
    }
    else
    {
        setClock(-1, -1, -1, -1);
        Theme_setText(g_date, "");
    }

    refreshTopBar();
    refreshStackport();
    refreshTodos(static_cast<int64_t>(now));
    refreshCalendar(static_cast<int64_t>(now));
}

void goToRest()
{
    if (g_resting)
    {
        return;
    }
    g_resting = true;
    Modal_closeAll();
    Toast_setSuppressed(true);
    refresh();
    startColonFade();
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_root);
    board_set_backlight(static_cast<uint8_t>(Prefs_get().blRest));
}

void onTouch(lv_event_t *)
{
    void (*after)() = g_afterTouchWake;
    Rest_wake(); // clears the hook
    if (after)
    {
        after();
    }
}

void onTick(lv_timer_t *)
{
    if (g_resting)
    {
        refresh();
        return;
    }
    const int after = Prefs_get().restAfterSec;
    if (after > 0 && lv_display_get_inactive_time(nullptr) > static_cast<uint32_t>(after) * 1000u)
    {
        goToRest();
    }
}

// ---------------------------------------------------------------- construction

void buildTopBar()
{
    g_problems = Theme_label(g_root, "", UI_FONT_M, UI_COL_WARN);
    lv_obj_set_width(g_problems, 500);
    lv_obj_align(g_problems, LV_ALIGN_TOP_LEFT, 24, 12);

    lv_obj_t *icons = Theme_row(g_root, LV_SIZE_CONTENT, kBarH);
    lv_obj_set_style_pad_gap(icons, 22, 0);
    lv_obj_set_flex_align(icons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_align(icons, LV_ALIGN_TOP_RIGHT, -24, 0);
    g_iconFeed = Theme_label(icons, LV_SYMBOL_REFRESH, UI_FONT_L, UI_COL_TEXT_DIM);
    StatusIcons_create(g_icons, icons, kBarH); // the same icons as the status bar: Wi-Fi, broker, AC, light, door, battery
}

void buildClock(int clockY, int dateY)
{
    lv_obj_t *clockBox = lv_obj_create(g_root);
    Theme_plain(clockBox);
    lv_obj_set_size(clockBox, kClockWidth, lv_font_get_line_height(&font_clock_200));
    lv_obj_remove_flag(clockBox, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(clockBox, LV_ALIGN_TOP_LEFT, kLeftX, clockY);
    for (int i = 0; i < 5; ++i)
    {
        const int cell = i == 2 ? kColonCell : kDigitCell;
        g_clockCell[i] = Theme_label(clockBox, i == 2 ? ":" : "-", &font_clock_200, UI_COL_TEXT);
        lv_obj_set_width(g_clockCell[i], cell);
        lv_obj_set_style_text_align(g_clockCell[i], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(g_clockCell[i], kCellCentre[i] - cell / 2, 0);
    }
    // Centred under the clock, with air between them.
    g_date = Theme_label(g_root, "", UI_FONT_XL, UI_COL_TEXT_DIM);
    lv_obj_set_width(g_date, kClockWidth);
    lv_obj_set_style_text_align(g_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_date, LV_ALIGN_TOP_LEFT, kLeftX, dateY);
}

void buildStackport(int y)
{
    g_stackPanel = panel(g_root, kLeftX + 24, y, kClockWidth - 48, 64);
    // An arc that fills with the share of services that are up; the number inside is how many are.
    g_stackArc = lv_arc_create(g_stackPanel);
    lv_obj_set_size(g_stackArc, 52, 52);
    lv_arc_set_rotation(g_stackArc, 135);
    lv_arc_set_bg_angles(g_stackArc, 0, 270);
    lv_arc_set_range(g_stackArc, 0, 1);
    lv_arc_set_value(g_stackArc, 0);
    lv_obj_remove_style(g_stackArc, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(g_stackArc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(g_stackArc, 6, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g_stackArc, 6, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g_stackArc, UI_COL_SURFACE_HI, LV_PART_MAIN);
    lv_obj_set_style_arc_color(g_stackArc, UI_COL_TEXT_FAINT, LV_PART_INDICATOR);
    lv_obj_align(g_stackArc, LV_ALIGN_LEFT_MID, 10, 0);
    g_stackCount = Theme_label(g_stackArc, "-", UI_FONT_M, UI_COL_TEXT);
    lv_obj_center(g_stackCount);

    g_stackTitle = lineLabel(g_stackPanel, UI_FONT_L, UI_COL_TEXT_DIM);
    lv_obj_set_width(g_stackTitle, 200);
    lv_obj_align(g_stackTitle, LV_ALIGN_LEFT_MID, 74, 0);
    g_stackSub = lineLabel(g_stackPanel, UI_FONT_M, UI_COL_TEXT_DIM);
    lv_obj_set_width(g_stackSub, kClockWidth - 48 - 74 - 200 - 18);
    lv_obj_set_style_text_align(g_stackSub, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(g_stackSub, LV_ALIGN_RIGHT_MID, -18, 0);
}

void buildTodos(int y)
{
    const int x = kLeftX + 24, w = kClockWidth - 48;
    g_todoHeader = Theme_label(g_root, "TODO", UI_FONT_S, UI_COL_TEXT_FAINT);
    lv_obj_set_pos(g_todoHeader, x + 2, y);
    for (int i = 0; i < kTodoRows; ++i)
    {
        TodoRow &row = g_todo[i];
        row.root = lv_obj_create(g_root);
        Theme_plain(row.root);
        lv_obj_set_size(row.root, w, 30);
        lv_obj_set_pos(row.root, x, y + 22 + i * 31);
        lv_obj_set_flex_flow(row.root, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row.root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(row.root, 12, 0);
        row.dot = Theme_dot(row.root, 10, UI_COL_TEXT_FAINT);
        row.title = lineLabel(row.root, UI_FONT_L, UI_COL_TEXT);
        lv_obj_set_flex_grow(row.title, 1);
        row.due = lineLabel(row.root, UI_FONT_S, UI_COL_TEXT_DIM);
        lv_obj_set_width(row.due, 110);
        lv_obj_set_style_text_align(row.due, LV_TEXT_ALIGN_RIGHT, 0);
        show(row.root, false);
    }
}

void buildCalendar()
{
    int y = kBarH + 8;
    // NOW line.
    g_nowTag = Theme_label(g_root, "NOW", UI_FONT_S, UI_COL_TEXT_FAINT);
    lv_obj_set_pos(g_nowTag, kRightX + 2, y + 6);
    g_nowText = lineLabel(g_root, UI_FONT_L, UI_COL_TEXT_FAINT);
    lv_obj_set_width(g_nowText, kRightW - 64);
    lv_obj_set_pos(g_nowText, kRightX + 64, y + 2);
    y += 36;

    // The big NEXT card.
    constexpr int kNextH = 224;
    g_nextPanel = panel(g_root, kRightX, y, kRightW, kNextH);
    g_nextBar = bar(g_nextPanel, 6, kNextH - 36);
    lv_obj_align(g_nextBar, LV_ALIGN_LEFT_MID, 12, 0);
    g_nextTag = Theme_label(g_nextPanel, "NEXT", UI_FONT_S, UI_COL_TEXT_FAINT);
    lv_obj_set_pos(g_nextTag, 34, 18);
    g_nextRel = lineLabel(g_nextPanel, UI_FONT_L, UI_COL_ACCENT);
    lv_obj_set_width(g_nextRel, 200);
    lv_obj_set_style_text_align(g_nextRel, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(g_nextRel, LV_ALIGN_TOP_RIGHT, -18, 14);
    g_nextTitle = Theme_label(g_nextPanel, "", UI_FONT_XL, UI_COL_TEXT);
    lv_obj_set_width(g_nextTitle, kRightW - 56);
    lv_label_set_long_mode(g_nextTitle, LV_LABEL_LONG_DOT);
    lv_obj_set_height(g_nextTitle, lv_font_get_line_height(UI_FONT_XL) * 3 + 4);
    lv_obj_set_pos(g_nextTitle, 34, 46);
    g_nextWhen = lineLabel(g_nextPanel, UI_FONT_L, UI_COL_TEXT);
    lv_obj_set_width(g_nextWhen, kRightW - 56);
    lv_obj_align(g_nextWhen, LV_ALIGN_BOTTOM_LEFT, 34, -46);
    g_nextAccount = lineLabel(g_nextPanel, UI_FONT_S, UI_COL_TEXT_DIM);
    lv_obj_set_width(g_nextAccount, kRightW - 56);
    lv_obj_align(g_nextAccount, LV_ALIGN_BOTTOM_LEFT, 34, -20);
    y += kNextH + 14;

    // What follows.
    g_laterHeader = Theme_label(g_root, "THEN", UI_FONT_S, UI_COL_TEXT_FAINT);
    lv_obj_set_pos(g_laterHeader, kRightX + 2, y);
    y += 22;
    for (int i = 0; i < kLaterRows; ++i)
    {
        LaterRow &row = g_later[i];
        row.root = lv_obj_create(g_root);
        Theme_plain(row.root);
        lv_obj_set_size(row.root, kRightW, 44);
        lv_obj_set_pos(row.root, kRightX, y + i * 46);
        lv_obj_set_flex_flow(row.root, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row.root, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(row.root, 12, 0);
        row.bar = bar(row.root, 5, 30);
        row.time = Theme_label(row.root, "", UI_FONT_M, UI_COL_TEXT_DIM);
        lv_obj_set_width(row.time, 84);
        row.title = lineLabel(row.root, UI_FONT_L, UI_COL_TEXT);
        lv_obj_set_flex_grow(row.title, 1);
        show(row.root, false);
    }
}
} // namespace

void Rest_begin()
{
    g_root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_root);
    lv_obj_set_size(g_root, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_set_style_bg_color(g_root, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(g_root, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_root, onTouch, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);

    // Vertical rhythm of the left column: clock, a gap, the date, a gap, then stackport and the todos.
    const int clockY = kBarH - 4;
    const int dateY = clockY + lv_font_get_line_height(&font_clock_200) + 8;
    const int stackY = dateY + lv_font_get_line_height(UI_FONT_XL) + 16;
    const int todoY = stackY + 64 + 14;

    // The MW monogram, faint, behind everything else (created first so it sits at the bottom).
    lv_obj_t *logo = lv_image_create(g_root);
    lv_image_set_src(logo, &img_mw_logo);
    lv_obj_set_style_image_recolor(logo, UI_COL_ACCENT, 0);
    lv_obj_set_style_image_recolor_opa(logo, LV_OPA_COVER, 0);
    lv_obj_set_style_image_opa(logo, 56, 0); // about 22 %: visible, still behind everything
    lv_obj_remove_flag(logo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, 20); // the middle of the screen, 480 px tall

    buildTopBar();
    buildClock(clockY, dateY);
    buildStackport(stackY);
    buildTodos(todoY);
    buildCalendar();

    lv_timer_create(onTick, 500, nullptr);
}

bool Rest_active() { return g_resting; }

void Rest_now()
{
    g_forced = true;
    goToRest();
}

void Rest_nowThen(void (*afterTouchWake)())
{
    Rest_now();
    g_afterTouchWake = afterTouchWake;
}

void Rest_wake()
{
    g_afterTouchWake = nullptr;
    if (!g_resting)
    {
        return;
    }
    g_resting = false;
    g_forced = false;
    stopColonFade();
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    Toast_setSuppressed(false);
    board_set_backlight(static_cast<uint8_t>(Prefs_get().blActive));
    // Waking is itself activity: without this the very next tick could put it back to rest.
    lv_display_trigger_activity(nullptr);
}
