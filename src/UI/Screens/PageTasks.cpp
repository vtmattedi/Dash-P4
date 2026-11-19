#include <stdio.h>
#include <string.h>
#include <string>
#include <time.h>

#include "App/Calendar.h"
#include "App/Stackport.h"
#include "App/Feed.h"
#include "App/FeedSync.h"
#include "App/Notifications.h"
#include "Link/LinkServer.h"
#include "Pages.h"
#include "UI/Calendar/CalFormat.h"
#include "UI/Calendar/EventDetail.h"
#include "UI/Theme.h"

// Tasks: a board of cards. Every open Linear and WOI task, collapsed into three columns by where it is at.
//
//   [ Stackport down: api, db ]                                  (only while something is down)
//   [All] [Linear] [WOI]                                                          12 tasks
//   +--- To do  5 -------+  +--- Doing  4 -------+  +--- Review  3 ------+
//   | +----------------+ |  | +----------------+ |  | +----------------+ |
//   | | Title      [x] | |  | | Title      [x] | |  | | Title      [x] | |
//   | | Linear  due .. | |  | | WOI     due .. | |  | | ...            | |
//   | +----------------+ |  | +----------------+ |  | +----------------+ |
//   +--------------------+  +--------------------+  +--------------------+
//
// To do holds todo / unstarted / backlog / triage; Doing holds in_progress / started; Review holds anything whose
// status mentions "review" (see Cal_taskGroup: Linear's own "In Review" state is not distinguishable by its type yet).
// The chips and column headers are built once; the cards are rebuilt (capped, and only while this page is on screen)
// whenever the feed changed, a filter changed, a task was dismissed, or the minute turned (due labels).

namespace
{
// Per column. Real feeds are big (a snapshot had 154 WOI tasks); every card is several LVGL objects and LVGL's small
// allocations count against internal RAM, so only the first cards are built and the rest are summarised.
constexpr int kMaxCards = 20;
constexpr int kSourceCount = 3;
constexpr int kColumns = 3;

const char *const kSourceKey[kSourceCount] = {"", "linear", "woi"};
const char *const kSourceName[kSourceCount] = {"All", "Linear", "WOI"};
const TaskGroup kColumnGroup[kColumns] = {TaskGroup::Todo, TaskGroup::Doing, TaskGroup::Review};

int g_source = 0; // index into kSourceKey

lv_obj_t *g_sourceChip[kSourceCount];
lv_obj_t *g_alert, *g_alertText;
lv_obj_t *g_summary;
lv_obj_t *g_columnTitle[kColumns], *g_columnList[kColumns];

uint32_t g_builtRevision = UINT32_MAX;
int64_t g_builtMinute = -1;
bool g_dirty = true;

lv_color_t columnColour(TaskGroup group)
{
    return group == TaskGroup::Doing ? UI_COL_ACCENT : group == TaskGroup::Review ? UI_COL_INFO : UI_COL_TEXT_DIM;
}

void styleChip(lv_obj_t *chip, bool selected)
{
    Theme_bgColor(chip, selected ? UI_COL_SURFACE_HI : UI_COL_SURFACE);
    Theme_borderColor(chip, selected ? UI_COL_ACCENT : UI_COL_BORDER);
    Theme_textColor(lv_obj_get_child(chip, 0), selected ? UI_COL_ACCENT : UI_COL_TEXT_DIM);
}

void onSource(lv_event_t *e)
{
    g_source = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    g_dirty = true;
    PageTasks_refresh(false);
}

bool g_suppressClick = false; // LVGL also sends a click when a long press is released

void onPress(lv_event_t *) { g_suppressClick = false; }

// Hold: open the task's link (a Linear issue, a WOI page) in Chrome on the linked PC.
void onHold(lv_event_t *e)
{
    g_suppressClick = true;
    const FeedItem *item = Feed_find(static_cast<int64_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
    if (!item || item->url.empty())
    {
        Notify(NotifySeverity::Info, "link", "No link", "This task has no link to open.");
    }
    else if (LinkServer_openUrl(item->url, "chrome") == 0)
    {
        Notify(NotifySeverity::Warn, "link", "No PC linked", "Start the companion app on the PC (Network page).");
    }
    else
    {
        Notify(NotifySeverity::Info, "link", "Opening on the PC", item->title);
    }
}

void onOpen(lv_event_t *e)
{
    if (g_suppressClick)
    {
        g_suppressClick = false;
        return;
    }
    EventDetail_open(static_cast<int64_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}

void onDismiss(lv_event_t *e)
{
    Feed_dismiss(static_cast<int64_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
    g_dirty = true; // not rebuilt here: this card holds the button being pressed; the page timer does it
}

lv_obj_t *makeChip(lv_obj_t *parent, const char *text, int id)
{
    lv_obj_t *chip = lv_obj_create(parent);
    Theme_plain(chip);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, 44);
    lv_obj_set_style_pad_hor(chip, 18, 0);
    lv_obj_set_style_radius(chip, 22, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(chip, 2, 0);
    lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(chip, onSource, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(id)));
    Theme_label(chip, text, UI_FONT_M, UI_COL_TEXT_DIM);
    styleChip(chip, false);
    return chip;
}

void addCard(lv_obj_t *list, const FeedItem &item, int64_t now)
{
    lv_obj_t *card = lv_obj_create(list);
    Theme_plain(card);
    lv_obj_set_size(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_radius(card, 12, 0);
    Theme_bgColor(card, UI_COL_SURFACE_ALT);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(card, 6, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(card, onPress, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(card, onOpen, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(item.numericId)));
    lv_obj_add_event_cb(card, onHold, LV_EVENT_LONG_PRESSED, reinterpret_cast<void *>(static_cast<intptr_t>(item.numericId)));

    // Title row: up to two lines, and the dismiss button.
    lv_obj_t *top = Theme_row(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(top, 8, 0);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_t *title = Theme_label(top, item.title.empty() ? item.id.c_str() : item.title.c_str(), UI_FONT_M, UI_COL_TEXT);
    lv_obj_set_flex_grow(title, 1);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_height(title, lv_font_get_line_height(UI_FONT_M) * 2 + 2);
    lv_obj_t *x = Theme_iconButton(top, LV_SYMBOL_CLOSE, 40);
    lv_obj_add_event_cb(x, onDismiss, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(item.numericId)));

    // Meta row: where it comes from, and when it is due.
    lv_obj_t *meta = Theme_row(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(meta, 10, 0);
    lv_obj_set_flex_align(meta, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    // The connection's alias ("Wakecap", "WoI"), not the kind of tool; the colour still says which tool it is.
    const bool linear = item.source == "linear";
    lv_obj_t *origin = Theme_label(meta, item.account.empty() ? item.source.c_str() : item.account.c_str(), UI_FONT_S,
                                   linear ? UI_COL_INFO : UI_COL_ACCENT);
    lv_label_set_long_mode(origin, LV_LABEL_LONG_DOT);
    lv_obj_set_width(origin, 150);
    char due[32];
    CalFmt_due(item, now, due, sizeof(due));
    if (due[0])
    {
        lv_obj_t *dueLabel =
            Theme_label(meta, due, UI_FONT_S, strcmp(due, "overdue") == 0 ? UI_COL_DANGER : UI_COL_TEXT_DIM);
        lv_obj_set_flex_grow(dueLabel, 1);
        lv_obj_set_style_text_align(dueLabel, LV_TEXT_ALIGN_RIGHT, 0);
    }
}

void rebuild(int64_t now)
{
    // Stackport: only shown while a project is down or unhealthy.
    std::string names;
    for (const SpProject &p : Stackport_projects())
    {
        if (p.state == "down" || p.state == "problem")
        {
            names += (names.empty() ? "" : ", ") + p.name;
        }
    }
    if (names.empty())
    {
        lv_obj_add_flag(g_alert, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        names = (Stackport_live() ? "Stackport down: " : "Stackport down (last known): ") + names;
        Theme_setText(g_alertText, names.c_str());
        lv_obj_remove_flag(g_alert, LV_OBJ_FLAG_HIDDEN);
    }

    for (int i = 0; i < kSourceCount; ++i)
    {
        styleChip(g_sourceChip[i], i == g_source);
    }

    size_t total = 0;
    for (int c = 0; c < kColumns; ++c)
    {
        lv_obj_clean(g_columnList[c]);
        const auto tasks = Cal_tasks(kSourceKey[g_source], static_cast<int>(kColumnGroup[c]));
        total += tasks.size();

        char head[40];
        snprintf(head, sizeof(head), "%s  %u", Cal_taskGroupName(kColumnGroup[c]), static_cast<unsigned>(tasks.size()));
        Theme_setText(g_columnTitle[c], head);

        if (tasks.empty())
        {
            lv_obj_t *empty = Theme_label(g_columnList[c], "Empty", UI_FONT_M, UI_COL_TEXT_FAINT);
            lv_obj_set_width(empty, lv_pct(100));
            lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
            continue;
        }
        int cards = 0;
        for (const FeedItem *item : tasks)
        {
            if (cards++ >= kMaxCards)
            {
                break;
            }
            addCard(g_columnList[c], *item, now);
        }
        if (tasks.size() > static_cast<size_t>(kMaxCards))
        {
            char more[32];
            snprintf(more, sizeof(more), "+%u more", static_cast<unsigned>(tasks.size() - kMaxCards));
            lv_obj_t *label = Theme_label(g_columnList[c], more, UI_FONT_M, UI_COL_TEXT_FAINT);
            lv_obj_set_width(label, lv_pct(100));
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        }
    }

    char summary[32];
    snprintf(summary, sizeof(summary), "%u task%s", static_cast<unsigned>(total), total == 1 ? "" : "s");
    Theme_setText(g_summary, summary);
}
} // namespace

lv_obj_t *PageTasks_create(lv_obj_t *parent)
{
    lv_obj_t *page = Theme_column(parent, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(page, UI_GUTTER, 0);
    lv_obj_set_style_pad_gap(page, 12, 0);

    // The Stackport strip.
    g_alert = lv_obj_create(page);
    Theme_plain(g_alert);
    lv_obj_set_size(g_alert, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(g_alert, 10, 0);
    lv_obj_set_style_radius(g_alert, 10, 0);
    Theme_bgColor(g_alert, UI_COL_DANGER);
    lv_obj_set_style_bg_opa(g_alert, LV_OPA_30, 0);
    g_alertText = Theme_label(g_alert, "", UI_FONT_M, UI_COL_TEXT);
    lv_obj_set_width(g_alertText, lv_pct(100));
    lv_obj_add_flag(g_alert, LV_OBJ_FLAG_HIDDEN);

    // Source filter and the count.
    lv_obj_t *filters = Theme_row(page, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(filters, 8, 0);
    lv_obj_set_flex_align(filters, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    for (int i = 0; i < kSourceCount; ++i)
    {
        g_sourceChip[i] = makeChip(filters, kSourceName[i], i);
    }
    g_summary = Theme_label(filters, "", UI_FONT_M, UI_COL_TEXT_FAINT);
    lv_obj_set_flex_grow(g_summary, 1);
    lv_obj_set_style_text_align(g_summary, LV_TEXT_ALIGN_RIGHT, 0);
    // Manual sync: the background poll is slow (the saved cache shows data at power-up), this fetches now.
    lv_obj_t *sync = Theme_iconButton(filters, LV_SYMBOL_REFRESH, 44);
    lv_obj_add_event_cb(sync, [](lv_event_t *) { FeedSync_pollNow(); }, LV_EVENT_CLICKED, nullptr);

    // The board.
    lv_obj_t *board = Theme_row(page, lv_pct(100), 0);
    lv_obj_set_flex_grow(board, 1);
    lv_obj_set_style_pad_gap(board, 12, 0);
    lv_obj_set_flex_align(board, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    for (int c = 0; c < kColumns; ++c)
    {
        lv_obj_t *column = Theme_card(board, 0, lv_pct(100));
        lv_obj_set_flex_grow(column, 1);
        lv_obj_set_style_pad_gap(column, 8, 0);
        lv_obj_set_style_pad_all(column, 10, 0);

        lv_obj_t *head = Theme_row(column, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_gap(head, 8, 0);
        lv_obj_set_flex_align(head, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        Theme_dot(head, 12, columnColour(kColumnGroup[c]));
        g_columnTitle[c] = Theme_label(head, "", UI_FONT_L, UI_COL_TEXT);

        lv_obj_t *list = Theme_column(column, lv_pct(100), 0);
        lv_obj_set_flex_grow(list, 1);
        lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_scroll_dir(list, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
        lv_obj_set_style_pad_gap(list, 8, 0);
        g_columnList[c] = list;
    }

    PageTasks_refresh(true);
    return page;
}

void PageTasks_refresh(bool force)
{
    const int64_t now = static_cast<int64_t>(time(nullptr));
    const uint32_t revision = Cal_revision() + Stackport_revision(); // both only ever grow, so the sum changes with either
    const int64_t minute = now / 60;
    if (!force && !g_dirty && revision == g_builtRevision && minute == g_builtMinute)
    {
        return;
    }
    g_dirty = false;
    g_builtRevision = revision;
    g_builtMinute = minute;
    rebuild(now);
}
