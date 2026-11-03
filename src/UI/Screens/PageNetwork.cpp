#include <algorithm>
#include <stdio.h>
#include <string>
#include <vector>

#include "App/Net.h"
#include "App/Notifications.h"
#include "App/Registry.h"
#include "Pages.h"
#include "UI/Explorer/DeviceSheet.h"
#include "UI/Modal.h"
#include "UI/Theme.h"
#include "esp_timer.h"

// The Network Explorer's front page: every device the panel has heard, searchable and
// filterable. Tap a device for its sheet (overview, resources, console, ping, topics).
//
// The list is rebuilt only when the *set* it shows changes (a device appeared, went online or
// offline, the filter moved); everything else -- last heard, resource counts -- is refreshed
// in place, so scrolling and touch are never interrupted by traffic.

namespace
{
enum Filter { FilterAll = 0, FilterOnline, FilterOffline, FilterCount };
enum Sort { SortName = 0, SortRecent, SortCount };

lv_obj_t *g_search;
lv_obj_t *g_filterButtons[FilterCount];
lv_obj_t *g_sortLabel;
lv_obj_t *g_summary;
lv_obj_t *g_gateways;
lv_obj_t *g_list;

int g_filter = FilterAll;
int g_sort = SortName;
std::string g_query;

struct Row
{
    std::string name;
    lv_obj_t *dot;
    lv_obj_t *sub;
    lv_obj_t *age;
};
std::vector<Row> g_rows;
uint32_t g_shownStructure = UINT32_MAX;
std::string g_shownQuery = "\x01";
int g_shownFilter = -1, g_shownSort = -1;
uint32_t g_lastLabelsMs = 0;

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

std::string lower(std::string s)
{
    for (char &c : s)
    {
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool matches(const Device &d)
{
    if (g_filter == FilterOnline && !d.online)
    {
        return false;
    }
    if (g_filter == FilterOffline && d.online)
    {
        return false;
    }
    if (g_query.empty())
    {
        return true;
    }
    const std::string q = lower(g_query);
    return lower(d.name).find(q) != std::string::npos || lower(d.hardware).find(q) != std::string::npos ||
           lower(d.kind).find(q) != std::string::npos;
}

std::vector<const Device *> selection()
{
    std::vector<const Device *> out;
    for (const Device *d : Registry_devices())
    {
        if (matches(*d))
        {
            out.push_back(d);
        }
    }
    if (g_sort == SortRecent)
    {
        std::stable_sort(out.begin(), out.end(), [](const Device *a, const Device *b) { return a->lastSeenMs > b->lastSeenMs; });
    }
    else
    {
        // Online first, then by name: what the user came to see is at the top.
        std::stable_sort(out.begin(), out.end(), [](const Device *a, const Device *b) { return a->online && !b->online; });
    }
    return out;
}

std::string subText(const Device &d)
{
    std::string text;
    if (d.kind == "gateway")
    {
        text = "gateway";
    }
    else if (!d.hardware.empty())
    {
        text = d.hardware;
    }
    text += (text.empty() ? "" : "   ") + std::to_string(d.resources.size()) + " resources";
    if (d.seenStatus == false)
    {
        text += "   (no status heard)";
    }
    return text;
}

void onRow(lv_event_t *e)
{
    DeviceSheet_open(*static_cast<std::string *>(lv_event_get_user_data(e)));
}

void refreshLabels(const std::vector<const Device *> &devices)
{
    const uint32_t now = nowMs();
    for (size_t i = 0; i < g_rows.size() && i < devices.size(); ++i)
    {
        const Device &d = *devices[i];
        char age[24];
        Theme_formatAge(now - d.lastSeenMs, age, sizeof(age));
        Theme_setText(g_rows[i].age, age);
        Theme_setText(g_rows[i].sub, subText(d).c_str());
        lv_obj_set_style_bg_color(g_rows[i].dot, d.online ? UI_COL_OK : UI_COL_TEXT_FAINT, 0);
    }
    size_t online = 0;
    for (const Device *d : Registry_devices())
    {
        online += d->online ? 1 : 0;
    }
    char buf[96];
    snprintf(buf, sizeof(buf), "%u shown   -   %u known, %u online", (unsigned)devices.size(),
             (unsigned)Registry_deviceCount(), (unsigned)online);
    Theme_setText(g_summary, buf);
}

void rebuildGateways()
{
    lv_obj_clean(g_gateways);
    bool any = false;
    for (const Device *d : Registry_devices())
    {
        if (d->kind != "gateway")
        {
            continue;
        }
        any = true;
        lv_obj_t *chip = Theme_chip(g_gateways);
        Theme_chipSet(chip, (d->name + (d->online ? "  online" : "  offline")).c_str(), d->online ? UI_COL_OK : UI_COL_WARN);
    }
    if (any)
    {
        lv_obj_remove_flag(g_gateways, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(g_gateways, LV_OBJ_FLAG_HIDDEN);
    }
}

void rebuild(const std::vector<const Device *> &devices)
{
    lv_obj_clean(g_list);
    g_rows.clear();
    for (const Device *d : devices)
    {
        lv_obj_t *row = lv_obj_create(g_list);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), 72);
        lv_obj_set_style_bg_color(row, UI_COL_SURFACE, 0);
        lv_obj_set_style_bg_color(row, UI_COL_SURFACE_ALT, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 14, 0);
        lv_obj_set_style_pad_hor(row, 18, 0);
        lv_obj_set_style_pad_gap(row, 16, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        auto *name = new std::string(d->name);
        lv_obj_add_event_cb(row, onRow, LV_EVENT_CLICKED, name);
        lv_obj_add_event_cb(row, [](lv_event_t *e) { delete static_cast<std::string *>(lv_event_get_user_data(e)); },
                            LV_EVENT_DELETE, name);

        Row r;
        r.name = d->name;
        r.dot = Theme_dot(row, 16, d->online ? UI_COL_OK : UI_COL_TEXT_FAINT);
        lv_obj_t *text = Theme_column(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(text, 1);
        lv_obj_set_style_pad_gap(text, 2, 0);
        lv_obj_t *title = Theme_label(text, d->name.c_str(), UI_FONT_L, d->online ? UI_COL_TEXT : UI_COL_TEXT_DIM);
        lv_obj_set_width(title, lv_pct(100));
        lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
        r.sub = Theme_label(text, "", UI_FONT_S, UI_COL_TEXT_FAINT);
        lv_obj_set_width(r.sub, lv_pct(100));
        lv_label_set_long_mode(r.sub, LV_LABEL_LONG_DOT);
        r.age = Theme_label(row, "", UI_FONT_S, UI_COL_TEXT_DIM);
        Theme_label(row, LV_SYMBOL_RIGHT, UI_FONT_M, UI_COL_TEXT_FAINT);
        g_rows.push_back(r);
    }
    if (devices.empty())
    {
        Theme_label(g_list,
                    Registry_deviceCount() == 0 ? "No devices heard yet. They appear as soon as the broker replays their status."
                                                : "No device matches the search or filter.",
                    UI_FONT_M, UI_COL_TEXT_FAINT);
    }
}

void styleFilters()
{
    for (int i = 0; i < FilterCount; ++i)
    {
        const bool active = (i == g_filter);
        lv_obj_set_style_bg_color(g_filterButtons[i], active ? UI_COL_ACCENT : UI_COL_SURFACE_HI, 0);
        lv_obj_set_style_text_color(g_filterButtons[i], active ? lv_color_hex(0x1A1208) : UI_COL_TEXT, 0);
    }
    Theme_setText(g_sortLabel, g_sort == SortName ? "Sort: status" : "Sort: recent");
}

void onSearch(lv_event_t *)
{
    g_query = lv_textarea_get_text(g_search);
}
} // namespace

lv_obj_t *PageNetwork_create(lv_obj_t *parent)
{
    lv_obj_t *page = Theme_column(parent, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(page, UI_GUTTER, 0);
    lv_obj_set_style_pad_gap(page, 10, 0);

    lv_obj_t *bar = Theme_row(page, lv_pct(100), 56);
    g_search = lv_textarea_create(bar);
    lv_textarea_set_one_line(g_search, true);
    lv_textarea_set_placeholder_text(g_search, LV_SYMBOL_EYE_OPEN "  Search devices");
    lv_obj_set_flex_grow(g_search, 1);
    lv_obj_set_height(g_search, 52);
    lv_obj_set_style_text_font(g_search, UI_FONT_M, 0);
    lv_obj_add_event_cb(g_search, onSearch, LV_EVENT_VALUE_CHANGED, nullptr);
    Keyboard_attach(g_search);

    static const char *labels[FilterCount] = {"All", "Online", "Offline"};
    for (int i = 0; i < FilterCount; ++i)
    {
        g_filterButtons[i] = Theme_actionButton(bar, labels[i], 100, 52, Tone::Neutral, [i] {
            g_filter = i;
            styleFilters();
        });
    }
    lv_obj_t *sort = Theme_actionButton(bar, "", 150, 52, Tone::Neutral, [] {
        g_sort = (g_sort + 1) % SortCount;
        styleFilters();
    });
    g_sortLabel = lv_obj_get_child(sort, 0);
    Theme_actionButton(bar, LV_SYMBOL_REFRESH, 60, 52, Tone::Neutral, [] {
        // Ask every device to re-publish its manifest. Others' retained documents are already here.
        if (Net_publish("all/console/in", ">manifest msgpack"))
        {
            Notify(NotifySeverity::Info, "explorer", "Manifests requested", "Devices will answer in a moment.");
        }
        else
        {
            Notify(NotifySeverity::Warn, "explorer", "Could not send", "No transport is connected.");
        }
    });

    g_summary = Theme_label(page, "", UI_FONT_S, UI_COL_TEXT_DIM);
    g_gateways = Theme_row(page, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_add_flag(g_gateways, LV_OBJ_FLAG_HIDDEN);

    g_list = Theme_column(page, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(g_list, 1);
    lv_obj_add_flag(g_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    lv_obj_set_style_pad_gap(g_list, 8, 0);

    styleFilters();
    return page;
}

void PageNetwork_refresh(bool force)
{
    const std::vector<const Device *> devices = selection();
    const bool changed = force || g_shownStructure != Registry_structureRevision() || g_shownQuery != g_query ||
                         g_shownFilter != g_filter || g_shownSort != g_sort ||
                         devices.size() != g_rows.size() ||
                         !std::equal(devices.begin(), devices.end(), g_rows.begin(),
                                     [](const Device *d, const Row &r) { return d->name == r.name; });
    if (changed)
    {
        g_shownStructure = Registry_structureRevision();
        g_shownQuery = g_query;
        g_shownFilter = g_filter;
        g_shownSort = g_sort;
        rebuildGateways();
        rebuild(devices);
        refreshLabels(devices);
        g_lastLabelsMs = nowMs();
        return;
    }
    if (nowMs() - g_lastLabelsMs >= 1000)
    {
        g_lastLabelsMs = nowMs();
        refreshLabels(devices);
    }
}
