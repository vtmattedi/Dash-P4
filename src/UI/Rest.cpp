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
#include "Calendar/EventDetail.h"
#include "Calendar/StackportSheet.h"
#include "Link/LinkServer.h"
#include "Modal.h"
#include "StatusIcons.h"
#include "Theme.h"
#include "board.h"

LV_FONT_DECLARE(font_clock_200);
LV_IMAGE_DECLARE(img_mw_logo);
LV_IMAGE_DECLARE(img_stackport);

// The resting screen: a black wall display you can also touch. Tap the calendar to open it in the app, tap the Stackport
// card for the service list; swipe up from the bottom edge to unlock. A first touch only brightens the screen (it dims
// again after a quiet spell), so a stray touch never opens the app. Layout (1024 x 600):
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
lv_obj_t *g_canvas; // where the build functions below draw: the root
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
// Calendar: an accordion. One event is open as a panel, the rest are single rows (see the calendar section below).
constexpr int kSlots = 5;
constexpr int kCardH = 224; // an open event
constexpr int kRowH = 44;   // a folded event
constexpr int kSlotGap = 8;
lv_obj_t *g_nowTag, *g_nowText;
struct Slot
{
    lv_obj_t *row, *rowBar, *rowTime, *rowTitle;                             // folded
    lv_obj_t *card, *bar, *tag, *rel, *title, *when, *account, *desc, *hint; // open
    int64_t id; // the feed item behind it (0: the slot is unused)
};
Slot g_slot[kSlots];
int g_open = 0;       // the slot that is a panel
int64_t g_openId = 0; // its event: what a refresh keeps open
int g_layoutKey = -1; // what applyLayout() last set up
bool g_animating = false;
int g_animFrom = 0, g_animTo = 0;
bool g_suppressClick = false; // LVGL also sends a click when a long press is released

int64_t g_nowId = 0; // the feed item behind the NOW line (0: none)
int64_t g_todoId[kTodoRows];
int32_t g_pressY = 0;
uint32_t g_restStart = 0; // lv_tick when the rest screen came up
bool g_bright = false;    // a touch brightened the screen; it dims after kDimAfterMs without one
constexpr uint32_t kDimAfterMs = 15000;
constexpr int kUnlockSwipePx = 90;
constexpr int kZoneH = 70;

bool g_resting = false;
// A short message in the top-left corner (what became of "open on the PC"), shown over the usual problems text.
std::string g_flash;
uint32_t g_flashAt = 0;
uint32_t g_openAction = 0, g_openActionAt = 0;

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
        // Nothing to say: the Wi-Fi icon in the top bar is red, and feed messages mean nothing without a network.
    }
    else if (!feed.hasToken)
    {
        problems = "Feed: no token";
    }
    else if (feed.message != "ok" && feed.lastHttp != 0)
    {
        problems = "Feed: " + feed.message;
    }
    else if (feed.message != "ok" && Feed_total() > 0)
    {
        problems = "Saved data, syncing"; // restored from flash; nothing fetched yet this boot
    }
    if (!g_flash.empty() && lv_tick_elaps(g_flashAt) < 3500)
    {
        problems = g_flash;
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
    // Only what is being worked on (Linear in progress, WOI in progress). The backlog, and what waits for review, is on the
    // Tasks page; a wall display shows what matters now.
    const auto todos = Cal_tasks("", static_cast<int>(TaskGroup::Doing));
    char buf[32];
    snprintf(buf, sizeof(buf), todos.empty() ? "DOING" : "DOING  \xE2\x80\xA2  %u", static_cast<unsigned>(todos.size()));
    Theme_setText(g_todoHeader, buf);

    for (int i = 0; i < kTodoRows; ++i)
    {
        TodoRow &row = g_todo[i];
        g_todoId[i] = i < static_cast<int>(todos.size()) ? todos[i]->numericId : 0;
        if (i >= static_cast<int>(todos.size()))
        {
            show(row.root, i == 0 && todos.empty());
            if (i == 0 && todos.empty())
            {
                lv_obj_set_style_bg_opa(row.dot, LV_OPA_TRANSP, 0);
                Theme_setText(row.title, "Nothing in progress");
                lv_obj_set_style_text_color(row.title, UI_COL_TEXT_FAINT, 0);
                Theme_setText(row.due, "");
            }
            continue;
        }
        const FeedItem &item = *todos[i];
        show(row.root, true);
        lv_obj_set_style_bg_opa(row.dot, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row.dot, UI_COL_ACCENT, 0);
        Theme_setText(row.title, item.title.empty() ? item.id.c_str() : item.title.c_str());
        lv_obj_set_style_text_color(row.title, UI_COL_TEXT, 0);
        char due[32];
        CalFmt_due(item, now, due, sizeof(due));
        const bool overdue = strcmp(due, "overdue") == 0;
        Theme_setText(row.due, due[0] ? due : item.account.empty() ? item.source.c_str() : item.account.c_str());
        lv_obj_set_style_text_color(row.due, overdue ? UI_COL_DANGER : UI_COL_TEXT_DIM, 0);
    }
}

// ---------------------------------------------------------------- calendar: an accordion
//
// The next event is open as a panel and the ones after it are single rows. Tap a row: it grows into the panel while the
// open panel folds into a row, in one animation. Hold any event: its link opens in Chrome on the linked PC.

void flash(const std::string &text)
{
    g_flash = text;
    g_flashAt = lv_tick_get();
}

void openOnPc(int64_t id)
{
    const FeedItem *item = Feed_find(id);
    // The meeting itself when the event has one (Meet, Zoom, Teams); otherwise the item's own page.
    const std::string target = !item || item->joinUrl.empty() ? (item ? item->url : std::string()) : item->joinUrl;
    if (target.empty())
    {
        flash("No link on this event");
        return;
    }
    const uint32_t action = LinkServer_openUrl(target, "chrome");
    if (action == 0)
    {
        flash("No PC linked");
        return;
    }
    g_openAction = action;
    g_openActionAt = lv_tick_get();
    flash("Opening on the PC...");
}

// What the PC answered.
void pollOpenResult()
{
    if (g_openAction == 0)
    {
        return;
    }
    const LinkResult result = LinkServer_result(g_openAction);
    if (result.done)
    {
        flash(result.ok ? (result.message.empty() ? std::string("Opened on the PC") : result.message)
                        : "PC: " + result.message);
        g_openAction = 0;
    }
    else if (lv_tick_elaps(g_openActionAt) > 6000)
    {
        flash("The PC did not answer");
        g_openAction = 0;
    }
}

// The resting shape: the open event a panel, the others rows, unused slots nothing.
void applyLayout()
{
    int key = g_open;
    for (int i = 0; i < kSlots; ++i)
    {
        Slot &slot = g_slot[i];
        const bool used = slot.id != 0;
        const bool open = used && i == g_open;
        show(slot.card, open);
        show(slot.row, used && !open);
        lv_obj_set_height(slot.card, kCardH);
        lv_obj_set_height(slot.row, kRowH);
        key |= (used ? 1 : 0) << (8 + i);
    }
    g_layoutKey = key;
}

void animSlots(void *, int32_t v) // v: 0..1000
{
    Slot &from = g_slot[g_animFrom];
    Slot &to = g_slot[g_animTo];
    lv_obj_set_height(from.card, kCardH * (1000 - v) / 1000);
    lv_obj_set_height(from.row, kRowH * v / 1000);
    lv_obj_set_height(to.card, kCardH * v / 1000);
    lv_obj_set_height(to.row, kRowH * (1000 - v) / 1000);
}

void animSlotsDone(lv_anim_t *)
{
    g_animating = false;
    g_open = g_animTo;
    g_openId = g_slot[g_open].id;
    applyLayout();
}

void openSlot(int to)
{
    if (g_animating || to == g_open || to < 0 || to >= kSlots || g_slot[to].id == 0)
    {
        return;
    }
    g_animFrom = g_open;
    g_animTo = to;
    g_animating = true;
    show(g_slot[g_animFrom].card, true);
    show(g_slot[g_animFrom].row, true);
    show(g_slot[to].card, true);
    show(g_slot[to].row, true);
    animSlots(nullptr, 0); // the first frame, before LVGL gets to it
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, &g_animFrom);
    lv_anim_set_exec_cb(&a, animSlots);
    lv_anim_set_values(&a, 0, 1000);
    lv_anim_set_duration(&a, 280);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_completed_cb(&a, animSlotsDone);
    lv_anim_start(&a);
}

int slotOf(lv_event_t *e) { return static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))); }

void onSlotPress(lv_event_t *) { g_suppressClick = false; }

void onRowClick(lv_event_t *e)
{
    if (g_suppressClick)
    {
        g_suppressClick = false;
        return;
    }
    openSlot(slotOf(e));
}

void onCardClick(lv_event_t *e)
{
    if (g_suppressClick)
    {
        g_suppressClick = false;
        return;
    }
    EventDetail_open(g_slot[slotOf(e)].id);
}

void onSlotHold(lv_event_t *e)
{
    g_suppressClick = true;
    openOnPc(g_slot[slotOf(e)].id);
}

void fillSlot(Slot &slot, const CalEntry &entry, int64_t now, bool isNext)
{
    slot.id = entry.numericId;
    const lv_color_t colour = CalFmt_accountColour(entry.account);
    const int64_t day = Cal_dayStart(entry.start);
    const bool today = day == Cal_dayStart(now);

    char when[40], span[40], rel[32], dayName[24] = "";
    if (today)
    {
        CalFmt_clock(entry.start, when, sizeof(when));
    }
    else
    {
        const time_t t = static_cast<time_t>(entry.start);
        struct tm local;
        localtime_r(&t, &local);
        strftime(when, sizeof(when), "%a %H:%M", &local);
        CalFmt_day(day, now, dayName, sizeof(dayName));
    }
    CalFmt_span(entry, span, sizeof(span));
    CalFmt_relative(entry.start - now, rel, sizeof(rel));

    // folded
    lv_obj_set_style_bg_color(slot.rowBar, colour, 0);
    Theme_setText(slot.rowTime, when);
    Theme_setText(slot.rowTitle, entry.title.c_str());

    // open
    lv_obj_set_style_bg_color(slot.bar, colour, 0);
    Theme_setText(slot.tag, isNext ? "NEXT" : today ? "LATER TODAY" : dayName);
    Theme_setText(slot.rel, rel);
    Theme_setText(slot.title, entry.title.c_str());
    char line[88];
    snprintf(line, sizeof(line), "%s%s%s", today ? "" : dayName, today ? "" : "  ", span);
    Theme_setText(slot.when, line);
    Theme_setText(slot.account, entry.account.c_str());
    Theme_setText(slot.desc, entry.description.c_str());
    Theme_setText(slot.hint, !entry.joinUrl.empty() ? "Hold to join on the PC"
                             : entry.url.empty()    ? ""
                                                    : "Hold to open on the PC");
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
    g_nowId = current ? current->numericId : allDay ? allDay->numericId : 0;
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

    // The accordion: the next event, then what follows it within two days.
    std::vector<const CalEntry *> list;
    if (next)
    {
        list.push_back(next);
        const int64_t today = Cal_dayStart(now);
        for (int d = 0; d < 3 && list.size() < static_cast<size_t>(kSlots); ++d)
        {
            for (const CalEntry *e : Cal_onDay(Cal_dayStart(today, d)))
            {
                if (list.size() < static_cast<size_t>(kSlots) && e->kind == CalKind::Event && !e->allDay && e != next &&
                    e->start >= next->start && e->start > now && e->start >= Cal_dayStart(today, d))
                {
                    list.push_back(e);
                }
            }
        }
    }

    // What stays open: the event that was open, if it is still on the list.
    int open = 0;
    bool found = false;
    for (size_t i = 0; i < list.size(); ++i)
    {
        if (list[i]->numericId == g_openId)
        {
            open = static_cast<int>(i);
            found = true;
        }
    }
    for (int i = 0; i < kSlots; ++i)
    {
        if (i < static_cast<int>(list.size()))
        {
            fillSlot(g_slot[i], *list[i], now, list[i] == next);
        }
        else
        {
            g_slot[i].id = 0;
        }
    }
    if (!g_animating)
    {
        g_open = found ? open : 0;
        g_openId = list.empty() ? 0 : list[g_open]->numericId;
        int key = g_open;
        for (int i = 0; i < kSlots; ++i)
        {
            key |= (g_slot[i].id != 0 ? 1 : 0) << (8 + i);
        }
        if (key != g_layoutKey)
        {
            applyLayout();
        }
    }
}

// ---------------------------------------------------------------- pager pages 1 and 2

void unlockNow()
{
    void (*after)() = g_afterTouchWake;
    Rest_wake(); // clears the hook
    if (after)
    {
        after();
    }
}

void onZonePress(lv_event_t *)
{
    lv_point_t p = {0, 0};
    lv_indev_get_point(lv_indev_active(), &p);
    g_pressY = p.y;
}

void onZoneRelease(lv_event_t *)
{
    lv_point_t p = {0, 0};
    lv_indev_get_point(lv_indev_active(), &p);
    if (g_pressY - p.y >= kUnlockSwipePx)
    {
        unlockNow();
    }
}

// A tap on something with a feed item behind it opens its detail. Modals are created after the rest screen, so they
// appear above it: the rest screen stays up underneath.
void onOpenItem(lv_event_t *e)
{
    if (g_suppressClick)
    {
        g_suppressClick = false;
        return;
    }
    const int64_t id = *static_cast<const int64_t *>(lv_event_get_user_data(e));
    if (id != 0)
    {
        EventDetail_open(id);
    }
}

void onOpenStackport(lv_event_t *) { StackportSheet_open(); }

// Hold: open the item's link in Chrome on the linked PC (an event's calendar page, a Linear issue ...).
void onHoldItem(lv_event_t *e)
{
    const int64_t id = *static_cast<const int64_t *>(lv_event_get_user_data(e));
    if (id != 0)
    {
        g_suppressClick = true;
        openOnPc(id);
    }
}

void makeTappable(lv_obj_t *obj, const int64_t *idSlot)
{
    lv_obj_add_flag(obj, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(obj, 6);
    lv_obj_add_event_cb(obj, onSlotPress, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(obj, onOpenItem, LV_EVENT_CLICKED, const_cast<int64_t *>(idSlot));
    lv_obj_add_event_cb(obj, onHoldItem, LV_EVENT_LONG_PRESSED, const_cast<int64_t *>(idSlot));
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

    pollOpenResult();
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
    g_restStart = lv_tick_get();
    g_bright = false;
    Modal_closeAll();
    refresh();
    startColonFade();
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_root);
    // The app (status bar, rail, page) sits under this screen and LVGL draws the active screen before the top layer, so
    // every animation here -- the colon, a modal's scrim -- also re-rendered the whole app for nothing. Not drawn at all
    // while resting; this screen is opaque and covers all of it.
    lv_obj_add_flag(lv_screen_active(), LV_OBJ_FLAG_HIDDEN);
    board_set_backlight(static_cast<uint8_t>(Prefs_get().blRest));
}

void onTick(lv_timer_t *)
{
    if (g_resting)
    {
        // A touch brightens the screen but leaves the rest view; it dims again after a quiet spell.
        const uint32_t idle = lv_display_get_inactive_time(nullptr);
        const bool bright = idle < lv_tick_elaps(g_restStart) && idle < kDimAfterMs;
        if (bright != g_bright)
        {
            g_bright = bright;
            board_set_backlight(static_cast<uint8_t>(bright ? Prefs_get().blActive : Prefs_get().blRest));
        }
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
    lv_obj_set_width(g_problems, 300);
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
    lv_obj_t *clockBox = lv_obj_create(g_canvas);
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
    g_date = Theme_label(g_canvas, "", UI_FONT_XL, UI_COL_TEXT_DIM);
    lv_obj_set_width(g_date, kClockWidth);
    lv_obj_set_style_text_align(g_date, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(g_date, LV_ALIGN_TOP_LEFT, kLeftX, dateY);
}

void buildStackport(int y)
{
    g_stackPanel = panel(g_canvas, kLeftX + 24, y, kClockWidth - 48, 64);
    lv_obj_add_flag(g_stackPanel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_stackPanel, onOpenStackport, LV_EVENT_CLICKED, nullptr);
    // The Stackport mark (opaque RGB565 already blended onto the card colour), then the arc.
    lv_obj_t *mark = lv_image_create(g_stackPanel);
    lv_image_set_src(mark, &img_stackport);
    lv_obj_remove_flag(mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(mark, LV_ALIGN_LEFT_MID, 12, 0);

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
    lv_obj_align(g_stackArc, LV_ALIGN_LEFT_MID, 70, 0);
    g_stackCount = Theme_label(g_stackArc, "-", UI_FONT_M, UI_COL_TEXT);
    lv_obj_center(g_stackCount);

    g_stackTitle = lineLabel(g_stackPanel, UI_FONT_L, UI_COL_TEXT_DIM);
    lv_obj_set_width(g_stackTitle, 190);
    lv_obj_align(g_stackTitle, LV_ALIGN_LEFT_MID, 134, 0);
    g_stackSub = lineLabel(g_stackPanel, UI_FONT_M, UI_COL_TEXT_DIM);
    lv_obj_set_width(g_stackSub, kClockWidth - 48 - 134 - 190 - 18);
    lv_obj_set_style_text_align(g_stackSub, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(g_stackSub, LV_ALIGN_RIGHT_MID, -18, 0);
}

void buildTodos(int y)
{
    const int x = kLeftX + 24, w = kClockWidth - 48;
    g_todoHeader = Theme_label(g_canvas, "DOING", UI_FONT_S, UI_COL_TEXT_FAINT);
    lv_obj_set_pos(g_todoHeader, x + 2, y);
    for (int i = 0; i < kTodoRows; ++i)
    {
        TodoRow &row = g_todo[i];
        row.root = lv_obj_create(g_canvas);
        Theme_plain(row.root);
        makeTappable(row.root, &g_todoId[i]);
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
    g_nowTag = Theme_label(g_canvas, "NOW", UI_FONT_S, UI_COL_TEXT_FAINT);
    lv_obj_set_pos(g_nowTag, kRightX + 2, y + 6);
    g_nowText = lineLabel(g_canvas, UI_FONT_L, UI_COL_TEXT_FAINT);
    makeTappable(g_nowText, &g_nowId);
    lv_obj_set_width(g_nowText, kRightW - 64);
    lv_obj_set_pos(g_nowText, kRightX + 64, y + 2);
    y += 36;

    // The accordion: one column; each slot has an open panel and a folded row, one of them shown.
    lv_obj_t *column = lv_obj_create(g_canvas);
    Theme_plain(column);
    lv_obj_set_pos(column, kRightX, y);
    lv_obj_set_size(column, kRightW, kCardH + (kSlots - 1) * (kRowH + kSlotGap) + kSlotGap);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(column, 0, 0);
    lv_obj_set_style_pad_gap(column, 0, 0);
    lv_obj_remove_flag(column, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < kSlots; ++i)
    {
        Slot &slot = g_slot[i];
        slot.id = 0;
        void *index = reinterpret_cast<void *>(static_cast<intptr_t>(i));

        // The open panel.
        slot.card = panel(column, 0, 0, kRightW, kCardH);
        lv_obj_set_style_margin_bottom(slot.card, kSlotGap, 0);
        lv_obj_set_style_bg_color(slot.card, UI_COL_SURFACE_HI, LV_STATE_PRESSED);
        lv_obj_add_flag(slot.card, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(slot.card, onSlotPress, LV_EVENT_PRESSED, index);
        lv_obj_add_event_cb(slot.card, onCardClick, LV_EVENT_CLICKED, index);
        lv_obj_add_event_cb(slot.card, onSlotHold, LV_EVENT_LONG_PRESSED, index);
        slot.bar = bar(slot.card, 6, kCardH - 36);
        lv_obj_align(slot.bar, LV_ALIGN_LEFT_MID, 12, 0);
        slot.tag = Theme_label(slot.card, "", UI_FONT_S, UI_COL_TEXT_FAINT);
        lv_obj_set_pos(slot.tag, 34, 18);
        slot.rel = lineLabel(slot.card, UI_FONT_L, UI_COL_ACCENT);
        lv_obj_set_width(slot.rel, 200);
        lv_obj_set_style_text_align(slot.rel, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(slot.rel, LV_ALIGN_TOP_RIGHT, -18, 14);
        slot.title = Theme_label(slot.card, "", UI_FONT_XL, UI_COL_TEXT);
        lv_obj_set_width(slot.title, kRightW - 56);
        lv_label_set_long_mode(slot.title, LV_LABEL_LONG_DOT);
        lv_obj_set_height(slot.title, lv_font_get_line_height(UI_FONT_XL) * 2 + 4);
        lv_obj_set_pos(slot.title, 34, 42);
        slot.when = lineLabel(slot.card, UI_FONT_L, UI_COL_TEXT);
        lv_obj_set_width(slot.when, kRightW - 56);
        lv_obj_set_pos(slot.when, 34, 118);
        slot.account = lineLabel(slot.card, UI_FONT_S, UI_COL_TEXT_DIM);
        lv_obj_set_width(slot.account, kRightW - 56);
        lv_obj_set_pos(slot.account, 34, 148);
        slot.desc = Theme_label(slot.card, "", UI_FONT_S, UI_COL_TEXT_FAINT);
        lv_obj_set_width(slot.desc, kRightW - 56);
        lv_label_set_long_mode(slot.desc, LV_LABEL_LONG_DOT);
        lv_obj_set_height(slot.desc, lv_font_get_line_height(UI_FONT_S) * 2 + 2);
        lv_obj_set_pos(slot.desc, 34, 168);
        slot.hint = lineLabel(slot.card, UI_FONT_S, UI_COL_TEXT_FAINT);
        lv_obj_set_width(slot.hint, kRightW - 56);
        lv_obj_set_pos(slot.hint, 34, 202);

        // The folded row.
        slot.row = lv_obj_create(column);
        Theme_plain(slot.row);
        lv_obj_set_size(slot.row, kRightW, kRowH);
        lv_obj_set_style_margin_bottom(slot.row, kSlotGap, 0);
        lv_obj_set_flex_flow(slot.row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(slot.row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(slot.row, 12, 0);
        lv_obj_set_style_radius(slot.row, 10, 0);
        lv_obj_set_style_bg_color(slot.row, UI_COL_SURFACE_ALT, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(slot.row, LV_OPA_COVER, LV_STATE_PRESSED);
        lv_obj_add_flag(slot.row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(slot.row, onSlotPress, LV_EVENT_PRESSED, index);
        lv_obj_add_event_cb(slot.row, onRowClick, LV_EVENT_CLICKED, index);
        lv_obj_add_event_cb(slot.row, onSlotHold, LV_EVENT_LONG_PRESSED, index);
        slot.rowBar = bar(slot.row, 5, 30);
        slot.rowTime = Theme_label(slot.row, "", UI_FONT_M, UI_COL_TEXT_DIM);
        lv_obj_set_width(slot.rowTime, 84);
        slot.rowTitle = lineLabel(slot.row, UI_FONT_L, UI_COL_TEXT);
        lv_obj_set_flex_grow(slot.rowTitle, 1);

        show(slot.card, false);
        show(slot.row, false);
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
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);

    // Vertical rhythm of the left column: clock, a gap, the date, a gap, then stackport and the todos.
    const int clockY = kBarH - 4;
    const int dateY = clockY + lv_font_get_line_height(&font_clock_200) + 8;
    const int stackY = dateY + lv_font_get_line_height(UI_FONT_XL) + 16;
    const int todoY = stackY + 64 + 14;

    // The MW monogram, behind everything else (created first so it sits at the bottom). It is an opaque RGB565 image
    // already tinted dim orange: it only ever sits on this black background, and a recoloured alpha mask made LVGL decode
    // it again on every draw (task watchdog).
    lv_obj_t *logo = lv_image_create(g_root);
    lv_image_set_src(logo, &img_mw_logo);
    lv_obj_remove_flag(logo, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(logo, LV_ALIGN_CENTER, 0, 20); // the middle of the screen, 400 px square

    g_canvas = g_root;

    buildTopBar();
    buildClock(clockY, dateY);
    buildStackport(stackY);
    buildTodos(todoY);
    buildCalendar();

    // The unlock edge: a swipe up that starts in the bottom strip. The pill is only a hint.
    lv_obj_t *zone = lv_obj_create(g_root);
    Theme_plain(zone);
    lv_obj_set_size(zone, BOARD_LCD_H_RES, kZoneH);
    lv_obj_align(zone, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(zone, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(zone, onZonePress, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(zone, onZoneRelease, LV_EVENT_RELEASED, nullptr);
    lv_obj_t *pill = lv_obj_create(zone);
    lv_obj_remove_style_all(pill);
    lv_obj_set_size(pill, 140, 6);
    lv_obj_set_style_radius(pill, 3, 0);
    lv_obj_set_style_bg_color(pill, UI_COL_TEXT_FAINT, 0);
    lv_obj_set_style_bg_opa(pill, LV_OPA_COVER, 0);
    lv_obj_remove_flag(pill, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(pill, LV_ALIGN_BOTTOM_MID, 0, -12);

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
    lv_obj_remove_flag(lv_screen_active(), LV_OBJ_FLAG_HIDDEN); // the app again, before this screen goes
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    board_set_backlight(static_cast<uint8_t>(Prefs_get().blActive));
    // Waking is itself activity: without this the very next tick could put it back to rest.
    lv_display_trigger_activity(nullptr);
}
