#include <stdio.h>
#include <string>
#include <vector>

#include "Link/LinkServer.h"
#include "Pages.h"
#include "UI/Assets/Icons.h"
#include "UI/Modal.h"
#include "UI/Popover.h"
#include "UI/Theme.h"

// The PC page: what the linked PC is doing and a few things to do to it. Everything on it comes from the companion
// (python -m dashlink) over the DashLink; with no PC linked it says so.
//
//   +--------------------------+----------------------+
//   | Now playing  < play >    | Vitals: CPU RAM ...  |
//   +--------------------------+----------------------+
//   | Windows (editors first,  | Deck: icon tiles that|
//   | 4 a page, tap to switch) | start things on the  |
//   |  ... unread chips ...    | PC (launchers.json)  |
//   +--------------------------+----------------------+
//
// All widgets are built once and updated in place: nothing is created or deleted while the page runs.

namespace
{
constexpr int32_t kPad = 16, kGap = 16, kLeftW = 520;
constexpr int32_t kRightW = UI_CONTENT_W - 2 * kPad - kGap - kLeftW;
constexpr int kRows = 4, kDeck = 6, kChips = 5;

lv_obj_t *g_body, *g_empty;

// Now playing
lv_obj_t *g_mTitle, *g_mArtist, *g_mApp, *g_mPlay;

// Vitals
constexpr int kMeters = 4;
struct Gauge
{
    lv_obj_t *col, *arc, *value, *unit, *caption;
};
Gauge g_gauge[kMeters];
lv_obj_t *g_battery;

// Windows
struct WinRow
{
    lv_obj_t *button, *app, *text;
    uint32_t id;
};
WinRow g_win[kRows];
lv_obj_t *g_winCount, *g_winPrev, *g_winNext, *g_winNone, *g_winCard;
std::vector<PcWindow> g_windows;
int g_winPage = 0;

// Deck and badges
struct DeckTile
{
    lv_obj_t *button, *image, *initial, *caption, *badge, *badgeText;
};
DeckTile g_tile[kDeck];
lv_obj_t *g_launchHint;
std::vector<PcLauncher> g_launchers;
lv_obj_t *g_chip[kChips];
lv_obj_t *g_unreadNone;

uint32_t g_shownRevision = UINT32_MAX;
bool g_shownLinked = false;

bool isEditor(const std::string &app) { return app == "VS Code" || app == "Cursor"; }

// Only when it changes: LVGL repaints an object on every flag call, and this runs from a timer.
void hide(lv_obj_t *o, bool yes)
{
    if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN) == yes)
        return;
    if (yes)
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    else
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *card(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *c = Theme_card(parent, w, h);
    lv_obj_set_pos(c, x, y);
    return c;
}

// --- Now playing -------------------------------------------------------------------------------------------------

void onMedia(lv_event_t *e)
{
    static const char *const kCommand[] = {"prev", "play_pause", "next"};
    LinkServer_mediaKey(kCommand[reinterpret_cast<intptr_t>(lv_event_get_user_data(e))]);
}

void buildMedia(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *c = card(parent, x, y, w, h);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *text = Theme_column(c, 270, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(text, 4, 0);
    g_mApp = Theme_label(text, "NOW PLAYING", UI_FONT_S, UI_COL_TEXT_FAINT);
    g_mTitle = Theme_label(text, "Nothing playing", UI_FONT_L, UI_COL_TEXT);
    lv_obj_set_width(g_mTitle, 270);
    lv_label_set_long_mode(g_mTitle, LV_LABEL_LONG_DOT);
    g_mArtist = Theme_label(text, "", UI_FONT_M, UI_COL_TEXT_DIM);
    lv_obj_set_width(g_mArtist, 270);
    lv_label_set_long_mode(g_mArtist, LV_LABEL_LONG_DOT);

    lv_obj_t *buttons = Theme_row(c, LV_SIZE_CONTENT, 64);
    static const char *const kSymbol[] = {LV_SYMBOL_PREV, LV_SYMBOL_PLAY, LV_SYMBOL_NEXT};
    for (int i = 0; i < 3; ++i)
    {
        lv_obj_t *b = Theme_iconButton(buttons, kSymbol[i], i == 1 ? 72 : 60, i == 1 ? Tone::Accent : Tone::Neutral);
        lv_obj_add_event_cb(b, onMedia, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        if (i == 1)
            g_mPlay = b;
    }
}

void refreshMedia()
{
    const PcMedia m = LinkServer_media();
    if (!m.known)
    {
        Theme_setText(g_mApp, "NOW PLAYING");
        Theme_setText(g_mTitle, "Nothing playing");
        Theme_textColor(g_mTitle, UI_COL_TEXT_DIM);
        Theme_setText(g_mArtist, "");
        Theme_setText(lv_obj_get_child(g_mPlay, 0), LV_SYMBOL_PLAY);
        return;
    }
    std::string head = m.playing ? "PLAYING" : "PAUSED";
    if (!m.app.empty())
        head += "  -  " + m.app;
    Theme_setText(g_mApp, head.c_str());
    Theme_setText(g_mTitle, m.title.c_str());
    Theme_textColor(g_mTitle, UI_COL_TEXT);
    Theme_setText(g_mArtist, m.artist.c_str());
    Theme_setText(lv_obj_get_child(g_mPlay, 0), m.playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

// --- Vitals -----------------------------------------------------------------------------------------------------

// Four arc gauges: the arc is the load (or the share used), the number inside is what you read, the caption names it.
lv_color_t loadColour(int percent)
{
    return percent >= 90 ? UI_COL_DANGER : percent >= 75 ? UI_COL_WARN : UI_COL_OK;
}

constexpr int32_t kGauge = 76;

lv_obj_t *buildGauge(lv_obj_t *parent, const char *caption, Gauge &g)
{
    g.col = Theme_column(parent, kGauge, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(g.col, 2, 0);
    lv_obj_set_flex_align(g.col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    g.arc = lv_arc_create(g.col);
    lv_obj_set_size(g.arc, kGauge, kGauge);
    lv_arc_set_rotation(g.arc, 135);
    lv_arc_set_bg_angles(g.arc, 0, 270);
    lv_arc_set_range(g.arc, 0, 100);
    lv_obj_remove_style(g.arc, nullptr, LV_PART_KNOB);
    lv_obj_remove_flag(g.arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(g.arc, 8, LV_PART_MAIN);
    lv_obj_set_style_arc_width(g.arc, 8, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(g.arc, UI_COL_SURFACE_HI, LV_PART_MAIN);
    lv_obj_t *inside = Theme_column(g.arc, kGauge - 20, LV_SIZE_CONTENT);
    lv_obj_remove_flag(inside, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_pad_gap(inside, 0, 0);
    lv_obj_set_flex_align(inside, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_center(inside);
    g.value = Theme_label(inside, "-", UI_FONT_M, UI_COL_TEXT);
    g.unit = Theme_label(inside, "", UI_FONT_S, UI_COL_TEXT_DIM);
    g.caption = Theme_label(g.col, caption, UI_FONT_S, UI_COL_TEXT_DIM);
    return g.col;
}

void buildVitals(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *c = card(parent, x, y, w, h);
    lv_obj_set_style_pad_gap(c, 6, 0);
    lv_obj_t *head = Theme_cardHeader(c, "This PC");
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    g_battery = Theme_chip(head);
    lv_obj_t *row = Theme_row(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    static const char *const kCaption[kMeters] = {"CPU", "RAM", "GPU", "Disk"};
    for (int i = 0; i < kMeters; ++i)
        buildGauge(row, kCaption[i], g_gauge[i]);
}

void setGauge(Gauge &g, int percent, const char *value, const char *unit)
{
    hide(g.col, percent < 0);
    if (percent < 0)
        return;
    lv_arc_set_value(g.arc, percent);
    if (!lv_color_eq(lv_obj_get_style_arc_color(g.arc, LV_PART_INDICATOR), loadColour(percent)))
        lv_obj_set_style_arc_color(g.arc, loadColour(percent), LV_PART_INDICATOR);
    Theme_setText(g.value, value);
    Theme_setText(g.unit, unit);
}

void refreshVitals()
{
    const PcVitals v = LinkServer_vitals();
    char text[24], unit[24];
    snprintf(text, sizeof(text), "%d%%", v.cpu);
    if (v.cpuTemp >= 0)
        snprintf(unit, sizeof(unit), "%d\xC2\xB0""C", v.cpuTemp);
    else
        snprintf(unit, sizeof(unit), "load");
    setGauge(g_gauge[0], v.cpu, text, unit);
    // RAM: the arc is the share used, the number is what is free.
    if (v.ramFreeGb >= 0)
        snprintf(text, sizeof(text), "%.1f", static_cast<double>(v.ramFreeGb));
    else
        snprintf(text, sizeof(text), "%d%%", v.ram);
    setGauge(g_gauge[1], v.ram, text, v.ramFreeGb >= 0 ? "GB free" : "used");
    snprintf(text, sizeof(text), "%d%%", v.gpu);
    if (v.gpuTemp >= 0)
        snprintf(unit, sizeof(unit), "%d\xC2\xB0""C", v.gpuTemp);
    else
        snprintf(unit, sizeof(unit), "load");
    setGauge(g_gauge[2], v.gpu, text, unit);
    snprintf(text, sizeof(text), "%d%%", v.disk);
    setGauge(g_gauge[3], v.disk, text, "used");

    // The header chip: the battery of a laptop, else the chassis temperature when the PC has no CPU sensor.
    if (v.battery >= 0)
    {
        snprintf(text, sizeof(text), "%s %d%%", v.charging ? "Charging" : "Battery", v.battery);
        Theme_chipSet(g_battery, text, v.battery <= 15 && !v.charging ? UI_COL_DANGER : UI_COL_OK);
        hide(g_battery, false);
    }
    else if (v.temp >= 0 && v.cpuTemp < 0)
    {
        snprintf(text, sizeof(text), "Case %d\xC2\xB0""C", v.temp);
        Theme_chipSet(g_battery, text, UI_COL_INFO);
        hide(g_battery, false);
    }
    else
    {
        hide(g_battery, true);
    }
}

// --- Windows ----------------------------------------------------------------------------------------------------

int winPages() { return g_windows.empty() ? 1 : (static_cast<int>(g_windows.size()) + kRows - 1) / kRows; }

void showWindows()
{
    if (g_winPage >= winPages())
        g_winPage = winPages() - 1;
    if (g_winPage < 0)
        g_winPage = 0;
    for (int i = 0; i < kRows; ++i)
    {
        const size_t index = static_cast<size_t>(g_winPage * kRows + i);
        WinRow &row = g_win[i];
        if (index >= g_windows.size())
        {
            row.id = 0;
            hide(row.button, true);
            continue;
        }
        const PcWindow &w = g_windows[index];
        row.id = w.id;
        hide(row.button, false);
        Theme_chipSet(row.app, w.app.c_str(), isEditor(w.app) ? UI_COL_ACCENT : UI_COL_INFO);
        Theme_setText(row.text, (isEditor(w.app) ? w.workspace : w.title).c_str());
    }
    char text[24];
    snprintf(text, sizeof(text), "%d/%d", g_winPage + 1, winPages());
    Theme_setText(g_winCount, text);
    hide(g_winNone, !g_windows.empty());
    const bool paged = winPages() > 1;
    hide(g_winPrev, !paged);
    hide(g_winNext, !paged);
    hide(g_winCount, !paged);
}

void onWindow(lv_event_t *e)
{
    const WinRow &row = g_win[reinterpret_cast<intptr_t>(lv_event_get_user_data(e))];
    if (row.id)
        LinkServer_focusWindow(row.id);
}

void onWinPage(lv_event_t *e)
{
    g_winPage += reinterpret_cast<intptr_t>(lv_event_get_user_data(e));
    showWindows();
}

void buildWindows(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *c = card(parent, x, y, w, h);
    lv_obj_set_style_pad_gap(c, 8, 0);
    g_winCard = c;
    lv_obj_t *head = Theme_cardHeader(c, "Windows");
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_height(head, 44);
    lv_obj_t *pager = Theme_row(head, LV_SIZE_CONTENT, 44);
    g_winPrev = Theme_iconButton(pager, LV_SYMBOL_LEFT, 44);
    lv_obj_add_event_cb(g_winPrev, onWinPage, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(-1)));
    g_winCount = Theme_label(pager, "1/1", UI_FONT_M, UI_COL_TEXT_DIM);
    g_winNext = Theme_iconButton(pager, LV_SYMBOL_RIGHT, 44);
    lv_obj_add_event_cb(g_winNext, onWinPage, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(1)));

    g_winNone = Theme_label(c, "No windows reported yet", UI_FONT_M, UI_COL_TEXT_FAINT);
    for (int i = 0; i < kRows; ++i)
    {
        WinRow &row = g_win[i];
        row.id = 0;
        row.button = Theme_button(c, "", lv_pct(100), 44, Tone::Neutral);
        lv_obj_add_flag(row.button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(row.button, onWindow, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
        row.text = lv_obj_get_child(row.button, 0);
        lv_label_set_long_mode(row.text, LV_LABEL_LONG_DOT);
        lv_obj_set_width(row.text, w - 2 * UI_GUTTER - 120);
        lv_obj_set_style_text_align(row.text, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(row.text, LV_ALIGN_LEFT_MID, 112, 0);
        row.app = Theme_chip(row.button);
        lv_obj_set_width(row.app, 98);
        lv_obj_t *chipLabel = lv_obj_get_child(row.app, 0);
        lv_label_set_long_mode(chipLabel, LV_LABEL_LONG_DOT);
        lv_obj_set_width(chipLabel, 74);
        lv_obj_set_style_text_align(chipLabel, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(row.app, LV_ALIGN_LEFT_MID, 0, 0);
    }
}

// --- Deck and unread -------------------------------------------------------------------------------------------

constexpr int32_t kTileW = 104, kTileH = 118, kIcon = 56;

// The last tile is "Recent" when the PC reports VS Code folders: like the right-click list on its taskbar icon.
int g_recentTile = -1;

const lv_image_dsc_t *iconFor(const std::string &name);

// A popover over the tile. A folder that already has a VS Code window is tagged "open": picking it switches to that
// window instead of starting another one.
void openRecent(lv_obj_t *anchor)
{
    const std::vector<PcRecent> recent = LinkServer_recent();
    const std::vector<PcWindow> windows = LinkServer_windows();
    // Two sections: the folders that have a window now, then the rest, each in most-recent-first order.
    std::vector<PopoverItem> items;
    std::vector<std::string> names; // names[k] is what picking item k opens
    for (const bool wantOpen : {true, false})
    {
        for (const PcRecent &r : recent)
        {
            bool open = false;
            for (const PcWindow &w : windows)
                open = open || (isEditor(w.app) && w.workspace == r.name);
            if (open != wantOpen)
                continue;
            PopoverItem item;
            item.label = r.name;
            item.detail = r.where;
            item.icon = &img_icon_vscode;
            item.section = wantOpen ? "Open" : "Recent";
            items.push_back(item);
            names.push_back(r.name);
        }
    }
    Popover_open(anchor, items, [names](size_t index) {
        if (index < names.size())
            LinkServer_openRecent(names[index]);
    }, "No recent folders.");
}

void onLaunch(lv_event_t *e)
{
    const int i = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    if (i == g_recentTile)
    {
        openRecent(lv_event_get_current_target_obj(e));
        return;
    }
    if (static_cast<size_t>(i) >= g_launchers.size())
        return;
    const PcLauncher &launcher = g_launchers[static_cast<size_t>(i)];
    if (launcher.menu.empty())
    {
        LinkServer_launch(launcher.name);
        return;
    }
    // A tile with choices: they open from the tile like the recent list does.
    std::vector<PopoverItem> items(launcher.menu.size());
    for (size_t k = 0; k < items.size(); ++k)
    {
        items[k].label = launcher.menu[k].name;
        items[k].icon = iconFor(launcher.menu[k].icon);
    }
    const std::string name = launcher.name;
    const std::vector<PcChoice> menu = launcher.menu;
    Popover_open(lv_event_get_current_target_obj(e), items, [name, menu](size_t index) {
        if (index < menu.size())
            LinkServer_launch(name, menu[index].name);
    });
}

const lv_image_dsc_t *iconFor(const std::string &name)
{
    if (name == "vscode")
        return &img_icon_vscode;
    if (name == "github")
        return &img_icon_github;
    if (name == "trackfy")
        return &img_icon_trackfy;
    if (name == "stackport")
        return &img_icon_stackport;
    return nullptr;
}

void buildDeck(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h)
{
    lv_obj_t *c = card(parent, x, y, w, h);
    lv_obj_set_style_pad_gap(c, 8, 0);
    Theme_cardHeader(c, "Deck");
    lv_obj_t *grid = Theme_row(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(grid, 8, 0);
    for (int i = 0; i < kDeck; ++i)
    {
        DeckTile &t = g_tile[i];
        t.button = lv_obj_create(grid);
        lv_obj_remove_style_all(t.button);
        lv_obj_set_size(t.button, kTileW, kTileH);
        lv_obj_set_style_radius(t.button, 16, 0);
        lv_obj_set_style_bg_color(t.button, UI_COL_SURFACE_ALT, 0);
        lv_obj_set_style_bg_opa(t.button, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(t.button, UI_COL_SURFACE_HI, LV_STATE_PRESSED);
        lv_obj_add_flag(t.button, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(t.button, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_flex_flow(t.button, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(t.button, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(t.button, 8, 0);
        lv_obj_add_flag(t.button, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_event_cb(t.button, onLaunch, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));

        t.image = lv_image_create(t.button);
        lv_obj_remove_flag(t.image, LV_OBJ_FLAG_CLICKABLE);
        // No picture for it: its first letter in a disc, so every tile still has a face.
        t.initial = lv_obj_create(t.button);
        lv_obj_remove_style_all(t.initial);
        lv_obj_set_size(t.initial, kIcon, kIcon);
        lv_obj_set_style_radius(t.initial, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(t.initial, UI_COL_SURFACE_HI, 0);
        lv_obj_set_style_bg_opa(t.initial, LV_OPA_COVER, 0);
        lv_obj_remove_flag(t.initial, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(t.initial, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *letter = Theme_label(t.initial, "", UI_FONT_XL, UI_COL_ACCENT);
        lv_obj_center(letter);
        t.caption = Theme_label(t.button, "", UI_FONT_S, UI_COL_TEXT);
        lv_obj_set_width(t.caption, kTileW - 12);
        lv_label_set_long_mode(t.caption, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(t.caption, LV_TEXT_ALIGN_CENTER, 0);
        // A notification dot in the corner: how many windows the app has open.
        t.badge = lv_obj_create(t.button);
        lv_obj_remove_style_all(t.badge);
        lv_obj_add_flag(t.badge, LV_OBJ_FLAG_FLOATING);
        lv_obj_remove_flag(t.badge, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(t.badge, 26, 26);
        lv_obj_set_style_radius(t.badge, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(t.badge, UI_COL_ACCENT, 0);
        lv_obj_set_style_bg_opa(t.badge, LV_OPA_COVER, 0);
        lv_obj_align(t.badge, LV_ALIGN_TOP_RIGHT, -6, 6);
        lv_obj_add_flag(t.badge, LV_OBJ_FLAG_HIDDEN);
        t.badgeText = Theme_label(t.badge, "", UI_FONT_S, UI_COL_BG);
        lv_obj_center(t.badgeText);
    }
    g_launchHint = Theme_label(c, "Nothing set up. List the things to start in launchers.json on the PC (see pc/README.md).", UI_FONT_S,
                               UI_COL_TEXT_FAINT);
    lv_obj_set_width(g_launchHint, w - 2 * UI_GUTTER);
}

void buildUnread(lv_obj_t *card)
{
    lv_obj_t *chips = Theme_row(card, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(chips, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(chips, 8, 0);
    for (lv_obj_t *&chip : g_chip)
    {
        chip = Theme_chip(chips);
        lv_obj_add_flag(chip, LV_OBJ_FLAG_HIDDEN);
    }
    g_unreadNone = Theme_label(chips, "Nothing unread", UI_FONT_S, UI_COL_TEXT_FAINT);
}

void refreshLists()
{
    g_windows = LinkServer_windows();
    showWindows();

    g_launchers = LinkServer_launchers();
    g_recentTile = -1;
    if (!LinkServer_recent().empty())
    {
        if (g_launchers.size() >= static_cast<size_t>(kDeck))
            g_launchers.resize(kDeck - 1);
        g_recentTile = static_cast<int>(g_launchers.size());
        g_launchers.push_back(PcLauncher{"Code", "vscode", {}});
    }
    for (int i = 0; i < kDeck; ++i)
    {
        DeckTile &t = g_tile[i];
        const bool used = static_cast<size_t>(i) < g_launchers.size();
        hide(t.button, !used);
        if (!used)
            continue;
        const PcLauncher &l = g_launchers[static_cast<size_t>(i)];
        const lv_image_dsc_t *icon = iconFor(l.icon);
        hide(t.image, icon == nullptr);
        hide(t.initial, icon != nullptr);
        if (icon)
        {
            lv_image_set_src(t.image, icon);
        }
        else
        {
            char letter[2] = {l.name.empty() ? '?' : l.name[0], 0};
            Theme_setText(lv_obj_get_child(t.initial, 0), letter);
        }
        Theme_setText(t.caption, l.name.c_str());
        int open = 0; // the Code tile counts its editor windows
        if (i == g_recentTile)
        {
            for (const PcWindow &w : g_windows)
                open += isEditor(w.app) ? 1 : 0;
        }
        hide(t.badge, open == 0);
        if (open > 0)
        {
            char count[8];
            snprintf(count, sizeof(count), "%d", open);
            Theme_setText(t.badgeText, count);
        }
    }
    hide(g_launchHint, !g_launchers.empty());

    const std::vector<PcBadge> badges = LinkServer_badges();
    for (int i = 0; i < kChips; ++i)
    {
        const bool used = static_cast<size_t>(i) < badges.size();
        hide(g_chip[i], !used);
        if (used)
        {
            char text[40];
            snprintf(text, sizeof(text), "%s  %d", badges[static_cast<size_t>(i)].app.c_str(), badges[static_cast<size_t>(i)].count);
            Theme_chipSet(g_chip[i], text, UI_COL_ACCENT);
        }
    }
    hide(g_unreadNone, !badges.empty());
}
} // namespace

lv_obj_t *PagePc_create(lv_obj_t *parent)
{
    constexpr int32_t kTop = 176; // the first row: now playing and vitals
    constexpr int32_t kBodyH = UI_CONTENT_H - 2 * kPad;
    constexpr int32_t kRowB = kPad + kTop + kGap; // where the second row starts
    constexpr int32_t kRightX = kPad + kLeftW + kGap;

    g_body = lv_obj_create(parent);
    Theme_plain(g_body);
    lv_obj_set_size(g_body, lv_pct(100), lv_pct(100));
    buildMedia(g_body, kPad, kPad, kLeftW, kTop);
    buildVitals(g_body, kRightX, kPad, kRightW, kTop);
    buildWindows(g_body, kPad, kRowB, kLeftW, kPad + kBodyH - kRowB);
    buildUnread(g_winCard); // the chips sit under the window list
    buildDeck(g_body, kRightX, kRowB, kRightW, kPad + kBodyH - kRowB);

    g_empty = Theme_card(parent, 560, LV_SIZE_CONTENT);
    lv_obj_center(g_empty);
    lv_obj_set_style_pad_all(g_empty, 28, 0);
    Theme_label(g_empty, "No PC linked", UI_FONT_XL, UI_COL_TEXT);
    lv_obj_t *how = Theme_label(g_empty, "Run the companion on the PC (python -m dashlink --host <this panel> --token <token>). The Network page shows both.", UI_FONT_M, UI_COL_TEXT_DIM);
    lv_obj_set_width(how, 500);
    return parent;
}

void PagePc_refresh(bool force)
{
    const bool linked = LinkServer_connected();
    if (linked != g_shownLinked || force)
    {
        g_shownLinked = linked;
        hide(g_body, !linked);
        hide(g_empty, linked);
        g_shownRevision = UINT32_MAX;
    }
    if (!linked)
        return;
    const uint32_t revision = LinkServer_pcRevision();
    if (revision != g_shownRevision)
    {
        g_shownRevision = revision;
        refreshLists();
    }
    refreshMedia();
    refreshVitals();
}
