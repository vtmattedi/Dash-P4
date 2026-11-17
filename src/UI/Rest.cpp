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
#include "App/Stackport.h"
#include "Link/LinkServer.h"
#include "Drawer.h"
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
bool g_dark = false;      // Rest_screenOff(): backlight off until a touch
lv_obj_t *g_dim = nullptr; // transparent catcher over the rest screen while dark
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
void (*g_onDarkTouch)() = nullptr;    // Rest_screenOffThen(): one shot, the touch that brightens the dark screen

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

// A card that grows into the whole left column when tapped, over the clock. Folded: the Stackport mark and three arcs that
// are always there (UP: projects ready, NET: one ring, receive in blue then send in orange, DISK), a status line, and two more arcs on the
// right that only exist while they are above zero (RUN: actions going, ALERT: alerts). Open: three sections -- the host in
// bars, what is running and what is wrong, and the projects. Tap again (or wait) and it folds back.
constexpr int kStackHeaderH = 96;
constexpr int kStackTopY = kBarH + 8; // where the open card starts
constexpr int kStackBottomY = 524;    // and ends: above the unlock strip
constexpr uint32_t kStackAutoCloseMs = 30000;
constexpr int kStackArcSize = 56;
constexpr int kStackW = kClockWidth - 48;
constexpr int kStackStep = 72;
constexpr int kArcY = (kStackHeaderH - kStackArcSize) / 2 - 4; // centred on the Stackport mark, the labels hang below
enum StackArcId
{
    ARC_UP = 0,
    ARC_NET,
    ARC_DISK,
    ARC_RUN,
    ARC_ALERT,
    ARC_COUNT
};

struct StackArc
{
    lv_obj_t *arc, *value, *label;
    lv_obj_t *inner = nullptr; // NET only: a second ring on top, so the one ring has two colours (receive, then send)
};
lv_obj_t *g_stackPanel, *g_stackBody, *g_stackTitle, *g_stackSub;
StackArc g_stackArcs[ARC_COUNT];
float g_netPeak = 0; // the network rings are drawn against the busiest recent moment
int g_stackY = 0;
bool g_stackOpen = false;
uint32_t g_stackOpenAt = 0;
uint32_t g_stackBuiltRev = UINT32_MAX;
int g_stackLayoutKey = -1;

lv_color_t metricColour(float percent)
{
    return percent >= 90 ? UI_COL_DANGER : percent >= 75 ? UI_COL_WARN : UI_COL_OK;
}

void setArc(StackArc &a, int value, int max, lv_color_t colour, const char *text)
{
    lv_arc_set_range(a.arc, 0, max > 0 ? max : 1);
    lv_arc_set_value(a.arc, value);
    lv_obj_set_style_arc_color(a.arc, colour, LV_PART_INDICATOR);
    Theme_setText(a.value, text);
}

lv_color_t projectStateColour(const std::string &state)
{
    return state == "ready" ? UI_COL_OK
           : state == "down" ? UI_COL_DANGER
           : state == "problem" ? UI_COL_WARN
                                : UI_COL_TEXT_FAINT;
}

// "850 B/s", "12 KB/s", "1.4 MB/s".
std::string rateText(double bytesPerSecond)
{
    char buf[24];
    if (bytesPerSecond >= 1024.0 * 1024.0)
    {
        snprintf(buf, sizeof(buf), "%.1f MB/s", bytesPerSecond / (1024.0 * 1024.0));
    }
    else if (bytesPerSecond >= 1024.0)
    {
        snprintf(buf, sizeof(buf), "%.0f KB/s", bytesPerSecond / 1024.0);
    }
    else
    {
        snprintf(buf, sizeof(buf), "%.0f B/s", bytesPerSecond);
    }
    return buf;
}

// A labelled bar with its numbers on the right (the host section of the open card).
void meter(int width, const char *name, float percent, const char *text, lv_color_t colour)
{
    lv_obj_t *row = Theme_row(g_stackBody, width, 26);
    lv_obj_set_style_pad_gap(row, 10, 0);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *n = Theme_label(row, name, UI_FONT_S, UI_COL_TEXT_FAINT);
    lv_obj_set_width(n, 40);
    lv_obj_t *bar = lv_bar_create(row);
    lv_bar_set_range(bar, 0, 100);
    lv_bar_set_value(bar, static_cast<int32_t>(percent + 0.5f), LV_ANIM_OFF);
    lv_obj_set_height(bar, 10);
    lv_obj_set_flex_grow(bar, 1);
    lv_obj_set_style_bg_color(bar, UI_COL_SURFACE_HI, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, Stackport_live() ? colour : UI_COL_WARN, LV_PART_INDICATOR);
    lv_obj_t *t = Theme_label(row, text, UI_FONT_S, UI_COL_TEXT_DIM);
    lv_obj_set_width(t, 160);
    lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_RIGHT, 0);
}

void sectionTitle(const char *title)
{
    lv_obj_t *l = Theme_label(g_stackBody, title, UI_FONT_S, UI_COL_TEXT_FAINT);
    lv_obj_set_width(l, kStackW - 32);
    lv_obj_set_style_pad_top(l, 8, 0);
}

// The part that only exists while the card is open. Rebuilt when the tracker's data changes (a few dozen objects).
void rebuildStackBody()
{
    lv_obj_clean(g_stackBody);
    g_stackBuiltRev = Stackport_revision();
    const int w = kStackW - 32;

    if (!Stackport_hasData())
    {
        Theme_label(g_stackBody, "No data from the Stackport bridge yet.", UI_FONT_M, UI_COL_TEXT_DIM);
        return;
    }
    if (!Stackport_live())
    {
        std::string why = std::string("Last known. The link is ") + Stackport_linkText(Stackport_link());
        if (!Stackport_reason().empty())
        {
            why += " (" + Stackport_reason() + ")";
        }
        lv_obj_t *l = Theme_label(g_stackBody, why.c_str(), UI_FONT_M, UI_COL_WARN);
        lv_obj_set_width(l, w);
    }

    // 1. The host, in bars.
    const SpHost &h = Stackport_host();
    if (h.valid)
    {
        char text[64];
        sectionTitle("HOST");
        snprintf(text, sizeof(text), "%.0f%%   load %.2f", h.cpuPercent, h.load1);
        meter(w, "CPU", h.cpuPercent, text, metricColour(h.cpuPercent));
        snprintf(text, sizeof(text), "%.0f%%   %.1f / %.1f GB", h.memPercent, h.memUsedMb / 1024.0, h.memTotalMb / 1024.0);
        meter(w, "RAM", h.memPercent, text, metricColour(h.memPercent));
        snprintf(text, sizeof(text), "%.0f%%", h.diskUsedPct);
        meter(w, "DISK", h.diskUsedPct, text, metricColour(h.diskUsedPct));
        const float peak = g_netPeak > 1.0f ? g_netPeak : 1.0f;
        meter(w, "IN", static_cast<float>(h.netRxBps) * 100.0f / peak, rateText(h.netRxBps).c_str(), UI_COL_INFO);
        meter(w, "OUT", static_cast<float>(h.netTxBps) * 100.0f / peak, rateText(h.netTxBps).c_str(), UI_COL_ACCENT);
    }

    // 2. What is running and what is wrong.
    sectionTitle("NOW");
    if (Stackport_actions().empty() && Stackport_alerts().empty())
    {
        Theme_label(g_stackBody, "Nothing running, no alerts.", UI_FONT_M, UI_COL_TEXT_DIM);
    }
    for (const SpAction &action : Stackport_actions())
    {
        // A tinted card per action with its progress.
        lv_obj_t *card = lv_obj_create(g_stackBody);
        lv_obj_remove_style_all(card);
        lv_obj_set_size(card, w, LV_SIZE_CONTENT);
        lv_obj_set_style_radius(card, 10, 0);
        lv_obj_set_style_bg_color(card, UI_COL_INFO, 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_20, 0);
        lv_obj_set_style_border_width(card, 1, 0);
        lv_obj_set_style_border_color(card, UI_COL_INFO, 0);
        lv_obj_set_style_border_opa(card, LV_OPA_50, 0);
        lv_obj_set_style_pad_all(card, 10, 0);
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_gap(card, 6, 0);
        lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *top = Theme_row(card, lv_pct(100), LV_SIZE_CONTENT);
        char head[96];
        snprintf(head, sizeof(head), "%s  %s", action.kind.c_str(), action.projectName.c_str());
        head[0] = static_cast<char>(toupper(static_cast<unsigned char>(head[0])));
        lv_obj_t *name = lineLabel(top, UI_FONT_L, UI_COL_TEXT);
        lv_obj_set_flex_grow(name, 1);
        Theme_setText(name, head);
        char pct[8] = "...";
        if (action.progress >= 0)
        {
            snprintf(pct, sizeof(pct), "%d%%", action.progress);
        }
        Theme_label(top, pct, UI_FONT_L, UI_COL_INFO);
        lv_obj_t *bar = lv_bar_create(card);
        lv_bar_set_range(bar, 0, 100);
        lv_bar_set_value(bar, action.progress >= 0 ? action.progress : 100, LV_ANIM_OFF);
        lv_obj_set_size(bar, lv_pct(100), 8);
        lv_obj_set_style_bg_color(bar, UI_COL_SURFACE_HI, LV_PART_MAIN);
        lv_obj_set_style_bg_color(bar, UI_COL_INFO, LV_PART_INDICATOR);
        lv_obj_set_style_bg_opa(bar, action.progress >= 0 ? LV_OPA_COVER : LV_OPA_40, LV_PART_INDICATOR); // no figure: dim, full
    }
    for (const SpAlert &alert : Stackport_alerts())
    {
        lv_obj_t *row = Theme_row(g_stackBody, w, LV_SIZE_CONTENT);
        lv_obj_set_style_pad_gap(row, 10, 0);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        Theme_dot(row, 10, alert.severity == "critical" ? UI_COL_DANGER : alert.severity == "warning" ? UI_COL_WARN : UI_COL_INFO);
        lv_obj_t *l = lineLabel(row, UI_FONT_M, UI_COL_TEXT);
        lv_obj_set_flex_grow(l, 1);
        Theme_setText(l, (alert.message.empty() ? alert.code : alert.message).c_str());
    }

    // 3. The projects: a summary, then each one.
    sectionTitle("PROJECTS");
    const SpSummary s = Stackport_summary();
    lv_obj_t *summary = Theme_row(g_stackBody, w, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(summary, 18, 0);
    auto chip = [&](size_t n, const char *what, lv_color_t colour) {
        if (n > 0)
        {
            char text[32];
            snprintf(text, sizeof(text), "%u %s", static_cast<unsigned>(n), what);
            Theme_label(summary, text, UI_FONT_M, colour);
        }
    };
    chip(s.ready, "ready", UI_COL_OK);
    chip(s.problem, "unhealthy", UI_COL_WARN);
    chip(s.down, "down", UI_COL_DANGER);
    chip(s.paused, "paused", UI_COL_TEXT_FAINT);
    lv_obj_t *grid = Theme_row(g_stackBody, w, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, 4, 0);
    for (const SpProject &p : Stackport_projects())
    {
        lv_obj_t *cell = Theme_row(grid, w / 2 - 2, 30);
        lv_obj_set_style_pad_gap(cell, 8, 0);
        lv_obj_set_flex_align(cell, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        Theme_dot(cell, 10, projectStateColour(p.state));
        lv_obj_t *name = lineLabel(cell, UI_FONT_M, p.state == "ready" ? UI_COL_TEXT : projectStateColour(p.state));
        lv_obj_set_flex_grow(name, 1);
        Theme_setText(name, p.name.c_str());
        char extra[24] = "";
        if (p.state != "ready")
        {
            snprintf(extra, sizeof(extra), "%s", p.state.c_str());
        }
        else if (p.p95Ms >= 0)
        {
            snprintf(extra, sizeof(extra), "%.0f ms", p.p95Ms);
        }
        lv_obj_t *tag = lineLabel(cell, UI_FONT_S, UI_COL_TEXT_DIM);
        lv_obj_set_width(tag, 76);
        lv_obj_set_style_text_align(tag, LV_TEXT_ALIGN_RIGHT, 0);
        Theme_setText(tag, extra);
    }
}

void placeArc(StackArc &a, int x)
{
    lv_obj_set_pos(a.arc, x, kArcY);
    if (a.inner)
    {
        lv_obj_set_pos(a.inner, x, kArcY);
    }
    lv_obj_set_pos(a.label, x - (kStackStep - kStackArcSize) / 2, kArcY + kStackArcSize + 2);
}

// The three fixed arcs sit on the left, the status line next to them, RUN and ALERT at the right edge when they show.
void layoutStack(bool run, bool alert)
{
    const int key = (run ? 1 : 0) | (alert ? 2 : 0);
    if (key == g_stackLayoutKey)
    {
        return;
    }
    g_stackLayoutKey = key;
    for (int i = ARC_UP; i <= ARC_DISK; ++i)
    {
        placeArc(g_stackArcs[i], 72 + i * kStackStep);
    }
    const int alertX = kStackW - 14 - kStackArcSize;
    const int runX = alert ? alertX - kStackStep : alertX;
    placeArc(g_stackArcs[ARC_ALERT], alertX);
    placeArc(g_stackArcs[ARC_RUN], runX);
    for (int i : {ARC_RUN, ARC_ALERT})
    {
        const bool visible = i == ARC_RUN ? run : alert;
        show(g_stackArcs[i].arc, visible);
        show(g_stackArcs[i].label, visible);
    }
    const int textX = 72 + 3 * kStackStep + 2;
    const int right = run ? runX : (alert ? alertX : kStackW);
    const int width = right - textX - (run || alert ? 8 : 14);
    lv_obj_set_width(g_stackTitle, width);
    lv_obj_set_width(g_stackSub, width);
    lv_obj_set_x(g_stackTitle, textX);
    lv_obj_set_x(g_stackSub, textX);
}

void refreshStackport()
{
    const SpSummary s = Stackport_summary();
    const auto &alerts = Stackport_alerts();
    const auto &actions = Stackport_actions();
    const SpHost &host = Stackport_host();
    char buf[24];

    const size_t bad = s.down + s.problem;
    lv_color_t accent = UI_COL_TEXT_FAINT;
    lv_color_t border = UI_COL_BORDER;
    std::string title = "Stackport";
    std::string sub;
    if (!Stackport_hasData())
    {
        sub = Stackport_link() == SpLink::None ? "no data yet" : Stackport_linkText(Stackport_link());
    }
    else if (bad > 0)
    {
        accent = border = UI_COL_DANGER;
        snprintf(buf, sizeof(buf), "%u down", static_cast<unsigned>(bad));
        title = buf;
        for (const SpProject &p : Stackport_projects())
        {
            if (p.state == "down" || p.state == "problem")
            {
                sub += (sub.empty() ? "" : ", ") + p.name;
            }
        }
    }
    else if (!alerts.empty())
    {
        accent = UI_COL_WARN;
        title = "Warning";
        sub = alerts[0].subject + ": " + alerts[0].code;
    }
    else
    {
        accent = UI_COL_OK;
        title = "All up";
        snprintf(buf, sizeof(buf), "%u projects", static_cast<unsigned>(s.counted));
        sub = buf;
    }
    // Numbers the bridge can no longer vouch for are shown as old, not as current.
    const bool live = Stackport_live();
    if (Stackport_hasData() && !live)
    {
        accent = UI_COL_WARN;
        border = UI_COL_BORDER;
        sub = std::string(Stackport_linkText(Stackport_link())) + ", last known";
    }
    lv_obj_set_style_border_color(g_stackPanel, border, 0);
    Theme_setText(g_stackTitle, title.c_str());
    lv_obj_set_style_text_color(g_stackTitle, accent, 0);
    Theme_setText(g_stackSub, sub.c_str());

    // UP: the colour follows the projects' health only (down / unhealthy red, all up green); alerts such as a full disk have
    // their own arc and the heading above. Faint with no data, amber when the numbers are old.
    lv_color_t upColour = UI_COL_TEXT_FAINT;
    if (Stackport_hasData())
    {
        upColour = !live ? UI_COL_WARN : bad > 0 ? UI_COL_DANGER : UI_COL_OK;
    }
    snprintf(buf, sizeof(buf), s.counted > 0 ? "%u" : "-", static_cast<unsigned>(s.ready));
    setArc(g_stackArcs[ARC_UP], static_cast<int>(s.ready), static_cast<int>(s.counted), upColour, buf);

    // NET and DISK.
    const bool haveHost = Stackport_hasData() && host.valid;
    StackArc &net = g_stackArcs[ARC_NET];
    if (haveHost)
    {
        // One ring for the two directions added up, against the busiest recent moment (floor 200 KB/s, easing down
        // slowly): orange (send) is the whole length, blue (receive) is drawn over its first part. The middle shows the total.
        const float rx = static_cast<float>(host.netRxBps), tx = static_cast<float>(host.netTxBps);
        const float rate = rx + tx;
        g_netPeak = rate > g_netPeak ? rate : g_netPeak * 0.995f;
        if (g_netPeak < 200.0f * 1024.0f)
        {
            g_netPeak = 200.0f * 1024.0f;
        }
        if (rate >= 1024.0f * 1024.0f)
        {
            snprintf(buf, sizeof(buf), "%.1fM", rate / (1024.0f * 1024.0f));
        }
        else if (rate >= 1024.0f)
        {
            snprintf(buf, sizeof(buf), "%.0fK", rate / 1024.0f);
        }
        else
        {
            snprintf(buf, sizeof(buf), "%.0f", rate);
        }
        setArc(net, static_cast<int>(rate * 100.0f / g_netPeak), 100, live ? UI_COL_ACCENT : UI_COL_WARN, buf);
        lv_arc_set_range(net.inner, 0, 100);
        lv_arc_set_value(net.inner, static_cast<int>(rx * 100.0f / g_netPeak));
        lv_obj_set_style_arc_color(net.inner, live ? UI_COL_INFO : UI_COL_WARN, LV_PART_INDICATOR);
        snprintf(buf, sizeof(buf), "%.0f", host.diskUsedPct);
        setArc(g_stackArcs[ARC_DISK], static_cast<int>(host.diskUsedPct + 0.5f), 100,
               live ? metricColour(host.diskUsedPct) : UI_COL_WARN, buf);
    }
    else
    {
        setArc(net, 0, 1, UI_COL_TEXT_FAINT, "-");
        lv_arc_set_value(net.inner, 0);
        setArc(g_stackArcs[ARC_DISK], 0, 1, UI_COL_TEXT_FAINT, "-");
    }

    // RUN: how many actions are going now; the arc is their average progress (full when none reports one).
    int sum = 0, known = 0;
    for (const SpAction &action : actions)
    {
        if (action.progress >= 0)
        {
            sum += action.progress;
            ++known;
        }
    }
    snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(actions.size()));
    setArc(g_stackArcs[ARC_RUN], known > 0 ? sum / known : 100, 100, UI_COL_INFO, buf);

    // ALERT: how many; the ring is full, red when one is critical, else amber.
    bool critical = false;
    for (const SpAlert &alert : alerts)
    {
        critical = critical || alert.severity == "critical";
    }
    snprintf(buf, sizeof(buf), "%u", static_cast<unsigned>(alerts.size()));
    setArc(g_stackArcs[ARC_ALERT], 100, 100, critical ? UI_COL_DANGER : UI_COL_WARN, buf);

    layoutStack(!actions.empty(), !alerts.empty());

    if (g_stackOpen && g_stackBuiltRev != Stackport_revision())
    {
        rebuildStackBody();
    }
}

// 0 = folded, 1000 = open. The card moves from under the date up over the clock and grows with it.
void stackAnim(void *, int32_t t)
{
    const int h0 = kStackHeaderH, h1 = kStackBottomY - kStackTopY;
    const int y = g_stackY + (kStackTopY - g_stackY) * t / 1000;
    const int h = h0 + (h1 - h0) * t / 1000;
    lv_obj_set_y(g_stackPanel, y);
    lv_obj_set_height(g_stackPanel, h);
    lv_obj_set_height(g_stackBody, h > kStackHeaderH ? h - kStackHeaderH - 6 : 0);
}

void stackSetOpen(bool open, bool animate)
{
    if (open == g_stackOpen && animate)
    {
        return;
    }
    g_stackOpen = open;
    g_stackOpenAt = lv_tick_get();
    lv_anim_delete(nullptr, stackAnim);
    if (open)
    {
        lv_obj_move_foreground(g_stackPanel);
        rebuildStackBody();
        show(g_stackBody, true);
    }
    if (!animate)
    {
        stackAnim(nullptr, open ? 1000 : 0);
        show(g_stackBody, open);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_exec_cb(&a, stackAnim);
    lv_anim_set_values(&a, open ? 0 : 1000, open ? 1000 : 0);
    lv_anim_set_duration(&a, 240);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    if (!open)
    {
        lv_anim_set_completed_cb(&a, [](lv_anim_t *) { show(g_stackBody, false); });
    }
    lv_anim_start(&a);
}

void onStackTap(lv_event_t *) { stackSetOpen(!g_stackOpen, true); }

lv_obj_t *makeRing(int size, int width)
{
    lv_obj_t *arc = lv_arc_create(g_stackPanel);
    lv_obj_set_size(arc, size, size);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 1);
    lv_arc_set_value(arc, 0);
    lv_obj_remove_style(arc, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(arc, width, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, UI_COL_SURFACE_HI, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, UI_COL_TEXT_FAINT, LV_PART_INDICATOR);
    return arc;
}

void buildStackport(int y)
{
    g_stackY = y;
    g_stackPanel = panel(g_canvas, kLeftX + 24, y, kStackW, kStackHeaderH);
    lv_obj_add_flag(g_stackPanel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g_stackPanel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_stackPanel, onStackTap, LV_EVENT_CLICKED, nullptr);

    // The Stackport mark (opaque RGB565 already blended onto the card colour).
    lv_obj_t *mark = lv_image_create(g_stackPanel);
    lv_image_set_src(mark, &img_stackport);
    lv_obj_remove_flag(mark, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_pos(mark, 14, (kStackHeaderH - 48) / 2);

    static const char *const kNames[ARC_COUNT] = {"UP", "NET", "DISK", "RUN", "ALERTS"};
    for (int i = 0; i < ARC_COUNT; ++i)
    {
        StackArc &a = g_stackArcs[i];
        a.arc = makeRing(kStackArcSize, i == ARC_NET ? 5 : 6);
        a.value = Theme_label(a.arc, "-", i == ARC_NET ? UI_FONT_S : UI_FONT_M, UI_COL_TEXT);
        lv_obj_center(a.value);
        if (i == ARC_NET)
        {
            a.inner = makeRing(kStackArcSize, 5); // receive, over the first part of the send ring
            lv_obj_set_style_arc_opa(a.inner, LV_OPA_TRANSP, LV_PART_MAIN);
        }
        a.label = Theme_label(g_stackPanel, kNames[i], UI_FONT_S, UI_COL_TEXT_FAINT);
        lv_obj_set_width(a.label, kStackStep);
        lv_obj_set_style_text_align(a.label, LV_TEXT_ALIGN_CENTER, 0);
    }

    g_stackTitle = lineLabel(g_stackPanel, UI_FONT_L, UI_COL_TEXT_DIM);
    lv_obj_set_pos(g_stackTitle, 0, kStackHeaderH / 2 - 26);
    g_stackSub = lineLabel(g_stackPanel, UI_FONT_S, UI_COL_TEXT_DIM);
    lv_obj_set_pos(g_stackSub, 0, kStackHeaderH / 2 + 2);
    layoutStack(true, true); // positions everything once; the first refresh hides what is zero
    g_stackLayoutKey = -1;

    g_stackBody = lv_obj_create(g_stackPanel);
    Theme_plain(g_stackBody);
    lv_obj_set_pos(g_stackBody, 16, kStackHeaderH);
    lv_obj_set_size(g_stackBody, kStackW - 32, 0);
    lv_obj_set_flex_flow(g_stackBody, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(g_stackBody, 4, 0);
    lv_obj_set_scrollbar_mode(g_stackBody, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(g_stackBody, LV_OBJ_FLAG_HIDDEN);
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

    if (g_stackOpen && lv_tick_elaps(g_stackOpenAt) > kStackAutoCloseMs)
    {
        stackSetOpen(false, true);
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
    Drawer_close();
    stackSetOpen(false, false);
    refresh();
    startColonFade();
    lv_obj_remove_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_root);
    // The app (status bar, rail, page) sits under this screen and LVGL draws the active screen before the top layer, so
    // every animation here -- the colon, a modal's scrim -- also re-rendered the whole app for nothing. Not drawn at all
    // while resting; this screen is opaque and covers all of it.
    lv_obj_add_flag(lv_screen_active(), LV_OBJ_FLAG_HIDDEN);
    board_set_backlight(static_cast<uint8_t>(g_dark ? 0 : Prefs_get().blRest));
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
            board_set_backlight(static_cast<uint8_t>(bright ? Prefs_get().blActive : (g_dark ? 0 : Prefs_get().blRest)));
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
    StatusIcons_create(g_icons, icons, kBarH); // the same icons as the status bar: Wi-Fi, broker, AC, light, door
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
    const int todoY = stackY + kStackHeaderH + 14;

    // The MW monogram, behind everything else (created first so it sits at the bottom). It is an opaque RGB565 image
    // already dimmed (white and orange halves): it only ever sits on this black background, and a recoloured alpha mask made LVGL decode
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

    // Over everything on this screen while it is dark: the first touch only turns the screen back on.
    g_dim = lv_obj_create(g_root);
    lv_obj_remove_style_all(g_dim);
    lv_obj_set_size(g_dim, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_add_flag(g_dim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(g_dim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(g_dim, [](lv_event_t *) {
        g_dark = false;
        lv_obj_add_flag(g_dim, LV_OBJ_FLAG_HIDDEN);
        g_bright = true;
        board_set_backlight(static_cast<uint8_t>(Prefs_get().blActive));
        void (*hook)() = g_onDarkTouch;
        g_onDarkTouch = nullptr;
        if (hook)
        {
            hook();
        }
    }, LV_EVENT_PRESSED, nullptr);

    lv_timer_create(onTick, 500, nullptr);
}

bool Rest_active() { return g_resting; }

void Rest_now()
{
    g_forced = true;
    goToRest();
}

void Rest_screenOff()
{
    g_dark = true;
    if (g_resting)
    {
        g_restStart = lv_tick_get();
        g_bright = false;
        board_set_backlight(0);
    }
    else
    {
        g_forced = true;
        goToRest(); // sets the backlight to 0 as g_dark is set
    }
    lv_obj_remove_flag(g_dim, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_dim);
}

void Rest_screenOffThen(void (*onTouch)())
{
    Rest_screenOff();
    g_onDarkTouch = onTouch;
}

void Rest_nowThen(void (*afterTouchWake)())
{
    Rest_now();
    g_afterTouchWake = afterTouchWake;
}

void Rest_wake()
{
    g_afterTouchWake = nullptr;
    g_onDarkTouch = nullptr;
    if (!g_resting)
    {
        return;
    }
    g_resting = false;
    g_forced = false;
    g_dark = false;
    lv_obj_add_flag(g_dim, LV_OBJ_FLAG_HIDDEN);
    stopColonFade();
    lv_obj_remove_flag(lv_screen_active(), LV_OBJ_FLAG_HIDDEN); // the app again, before this screen goes
    lv_obj_add_flag(g_root, LV_OBJ_FLAG_HIDDEN);
    board_set_backlight(static_cast<uint8_t>(Prefs_get().blActive));
    // Waking is itself activity: without this the very next tick could put it back to rest.
    lv_display_trigger_activity(nullptr);
}
