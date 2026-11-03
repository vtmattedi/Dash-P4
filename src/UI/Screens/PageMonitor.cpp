#include <stdio.h>
#include <string>

#include "App/Net.h"
#include "Explorer/TopicTap.h"
#include "Pages.h"
#include "UI/Modal.h"
#include "UI/Theme.h"
#include "esp_timer.h"

// The traffic monitor: a live tail of every message the panel hears, newest first, with a
// substring filter, pause and clear. By default that is what the panel already follows (status,
// info, telemetry, manifests, resource states); "Capture all" additionally follows `#`, i.e.
// literally everything on the broker -- useful for finding out what a device publishes, noisy on
// a busy broker, so it is off until asked, and off again when switched off or when the page is left.
// Nothing is recorded unless this page (or a device sheet's Topics tab) is on screen: see Tap_acquire().

namespace
{
lv_obj_t *g_filter, *g_stats, *g_list, *g_pauseLabel, *g_allLabel;
bool g_captureAll = false;
std::string g_query;
uint32_t g_shownRevision = UINT32_MAX;
std::string g_shownQuery = "\x01";
uint32_t g_lastBuildMs = 0;
uint32_t g_rateMark = 0, g_rateTotal = 0;
float g_rate = 0;

void setCaptureAll(bool on)
{
    if (on == g_captureAll)
    {
        return;
    }
    g_captureAll = on;
    if (on)
    {
        Net_subscribe("#");
    }
    else
    {
        Net_unsubscribe("#");
    }
    Theme_setText(g_allLabel, on ? "Capture all: ON" : "Capture all: off");
    lv_obj_set_style_bg_color(lv_obj_get_parent(g_allLabel), on ? UI_COL_ACCENT : UI_COL_SURFACE_HI, 0);
    lv_obj_set_style_text_color(lv_obj_get_parent(g_allLabel), on ? lv_color_hex(0x1A1208) : UI_COL_TEXT, 0);
}

uint32_t nowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

bool matches(const TapEntry &e)
{
    if (g_query.empty())
    {
        return true;
    }
    return e.topic.find(g_query) != std::string::npos || e.text.find(g_query) != std::string::npos;
}

void rebuild()
{
    lv_obj_clean(g_list);
    const uint32_t now = nowMs();
    size_t shown = 0;
    for (size_t i = 0; i < Tap_count() && shown < 80; ++i)
    {
        const TapEntry &e = Tap_at(i);
        if (!matches(e))
        {
            continue;
        }
        lv_obj_t *row = Theme_row(g_list, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_gap(row, 12, 0);
        lv_obj_set_style_pad_ver(row, 3, 0);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        char age[24];
        Theme_formatAge(now - e.atMs, age, sizeof(age));
        lv_obj_t *a = Theme_label(row, age, UI_FONT_S, UI_COL_TEXT_FAINT);
        lv_obj_set_width(a, 80);
        lv_obj_t *t = Theme_label(row, (std::string(e.retained ? "R " : "  ") + e.topic).c_str(), UI_FONT_S, UI_COL_ACCENT);
        lv_obj_set_width(t, 330);
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        lv_obj_t *v = Theme_label(row, e.text.c_str(), UI_FONT_S, e.binary ? UI_COL_TEXT_FAINT : UI_COL_TEXT_DIM);
        lv_obj_set_flex_grow(v, 1);
        lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
        ++shown;
    }
    if (shown == 0)
    {
        Theme_label(g_list, "Nothing to show.", UI_FONT_M, UI_COL_TEXT_FAINT);
    }
    g_shownRevision = Tap_revision();
    g_shownQuery = g_query;
    g_lastBuildMs = now;
}

void updateStats()
{
    const uint32_t now = nowMs();
    if (now - g_rateMark >= 2000)
    {
        g_rate = (Tap_total() - g_rateTotal) * 1000.0f / (now - g_rateMark);
        g_rateMark = now;
        g_rateTotal = Tap_total();
    }
    const NetStats stats = Net_stats();
    char buf[160];
    snprintf(buf, sizeof(buf), "%.1f msg/s   %u recorded   %u dropped by the inbox%s", static_cast<double>(g_rate),
             (unsigned)Tap_total(), (unsigned)stats.dropped, Tap_paused() ? "   PAUSED" : "");
    Theme_setText(g_stats, buf);
}
} // namespace

lv_obj_t *PageMonitor_create(lv_obj_t *parent)
{
    lv_obj_t *page = Theme_column(parent, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(page, UI_GUTTER, 0);
    lv_obj_set_style_pad_gap(page, 10, 0);

    lv_obj_t *bar = Theme_row(page, lv_pct(100), 56);
    g_filter = lv_textarea_create(bar);
    lv_textarea_set_one_line(g_filter, true);
    lv_textarea_set_placeholder_text(g_filter, "Filter by topic or text");
    lv_obj_set_flex_grow(g_filter, 1);
    lv_obj_set_height(g_filter, 52);
    lv_obj_set_style_text_font(g_filter, UI_FONT_M, 0);
    lv_obj_add_event_cb(g_filter, [](lv_event_t *) { g_query = lv_textarea_get_text(g_filter); }, LV_EVENT_VALUE_CHANGED,
                        nullptr);
    Keyboard_attach(g_filter);

    lv_obj_t *pause = Theme_actionButton(bar, Tap_paused() ? "Resume" : "Pause", 120, 52, Tone::Neutral, [] {
        Tap_setPaused(!Tap_paused());
        Theme_setText(g_pauseLabel, Tap_paused() ? "Resume" : "Pause");
    });
    g_pauseLabel = lv_obj_get_child(pause, 0);
    Theme_actionButton(bar, "Clear", 100, 52, Tone::Neutral, [] { Tap_clear(); });
    lv_obj_t *all = Theme_actionButton(bar, "Capture all: off", 200, 52, Tone::Neutral, [] { setCaptureAll(!g_captureAll); });
    g_allLabel = lv_obj_get_child(all, 0);

    g_stats = Theme_label(page, "", UI_FONT_S, UI_COL_TEXT_DIM);
    g_list = Theme_column(page, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(g_list, 1);
    lv_obj_add_flag(g_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(g_list, LV_DIR_VER);
    lv_obj_set_style_pad_gap(g_list, 0, 0);
    return page;
}

void PageMonitor_refresh(bool force)
{
    updateStats();
    // Rebuilding rows is the expensive part, so a busy network is sampled at 2 Hz.
    const bool dirty = g_shownRevision != Tap_revision() || g_shownQuery != g_query;
    if (force || (dirty && nowMs() - g_lastBuildMs >= 500))
    {
        rebuild();
    }
}

void PageMonitor_leave()
{
    setCaptureAll(false);
}
