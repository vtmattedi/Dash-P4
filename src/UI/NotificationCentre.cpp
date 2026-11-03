#include "NotificationCentre.h"

#include <stdio.h>

#include "App/Notifications.h"
#include "Modal.h"
#include "esp_timer.h"

namespace
{
ModalId g_id = 0;
lv_obj_t *g_list = nullptr;
lv_obj_t *g_countLabel = nullptr;
uint32_t g_shownRevision = 0;
int g_filter = 0; // 0 all, 1 warnings and worse, 2 critical only
lv_obj_t *g_filterButtons[3] = {nullptr, nullptr, nullptr};

Tone toneFor(NotifySeverity s)
{
    switch (s)
    {
    case NotifySeverity::Ok:
        return Tone::Ok;
    case NotifySeverity::Warn:
        return Tone::Warn;
    case NotifySeverity::Crit:
        return Tone::Danger;
    default:
        return Tone::Info;
    }
}

bool passes(const Notification &n)
{
    switch (g_filter)
    {
    case 1:
        return n.severity == NotifySeverity::Warn || n.severity == NotifySeverity::Crit;
    case 2:
        return n.severity == NotifySeverity::Crit;
    default:
        return true;
    }
}

void onRowClick(lv_event_t *e)
{
    const uint32_t id = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(lv_event_get_user_data(e)));
    Notifications_markRead(id);
    NotificationCentre_refresh();
}

void addRow(const Notification &n, uint32_t nowMs)
{
    lv_obj_t *row = lv_obj_create(g_list);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, n.read ? UI_COL_SURFACE : UI_COL_SURFACE_ALT, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, 10, 0);
    lv_obj_set_style_pad_all(row, 12, 0);
    lv_obj_set_style_pad_gap(row, 12, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_add_event_cb(row, onRowClick, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<uintptr_t>(n.id)));

    lv_obj_t *dot = Theme_dot(row, 12, Theme_toneColor(toneFor(n.severity)));
    lv_obj_set_style_margin_top(dot, 6, 0);

    lv_obj_t *text = Theme_column(row, 300, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(text, 2, 0);
    lv_obj_t *title = Theme_label(text, n.title.c_str(), UI_FONT_M, n.read ? UI_COL_TEXT_DIM : UI_COL_TEXT);
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    if (!n.body.empty())
    {
        lv_obj_t *body = Theme_label(text, n.body.c_str(), UI_FONT_S, UI_COL_TEXT_DIM);
        lv_obj_set_width(body, lv_pct(100));
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    }
    char meta[96];
    char age[24];
    Theme_formatAge(nowMs - n.atMs, age, sizeof(age));
    snprintf(meta, sizeof(meta), "%s  -  %s", n.source.c_str(), age);
    Theme_label(text, meta, UI_FONT_S, UI_COL_TEXT_FAINT);

    if (n.sticky && n.active)
    {
        lv_obj_t *chip = Theme_chip(row);
        Theme_chipSet(chip, "live", Theme_toneColor(toneFor(n.severity)));
    }
}

void rebuild()
{
    if (!g_list)
    {
        return;
    }
    lv_obj_clean(g_list);
    const uint32_t nowMs = static_cast<uint32_t>(esp_timer_get_time() / 1000);

    size_t shown = 0;
    // Live conditions first, most severe first.
    for (size_t i = 0; i < Notifications_activeCount(); ++i)
    {
        const Notification &n = Notifications_activeAt(i);
        if (passes(n))
        {
            addRow(n, nowMs);
            ++shown;
        }
    }
    // Then the history, newest first, skipping what was already shown above.
    for (size_t i = 0; i < Notifications_count(); ++i)
    {
        const Notification &n = Notifications_at(i);
        if (n.sticky && n.active)
        {
            continue;
        }
        if (passes(n))
        {
            addRow(n, nowMs);
            ++shown;
        }
    }
    if (shown == 0)
    {
        Theme_label(g_list, "Nothing here.", UI_FONT_M, UI_COL_TEXT_FAINT);
    }
    if (g_countLabel)
    {
        char buf[48];
        snprintf(buf, sizeof(buf), "%u unread", (unsigned)Notifications_unread());
        Theme_setText(g_countLabel, buf);
    }
    for (int i = 0; i < 3; ++i)
    {
        if (g_filterButtons[i])
        {
            lv_obj_set_style_bg_color(g_filterButtons[i], i == g_filter ? UI_COL_ACCENT : UI_COL_SURFACE_HI, 0);
            lv_obj_set_style_text_color(g_filterButtons[i], i == g_filter ? lv_color_hex(0x1A1208) : UI_COL_TEXT, 0);
        }
    }
    g_shownRevision = Notifications_revision();
}

void onFilter(lv_event_t *e)
{
    g_filter = static_cast<int>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    rebuild();
}

void onClosed()
{
    g_id = 0;
    g_list = nullptr;
    g_countLabel = nullptr;
    for (auto &b : g_filterButtons)
    {
        b = nullptr;
    }
}
} // namespace

void NotificationCentre_open()
{
    if (Modal_isOpen(g_id))
    {
        return;
    }
    lv_obj_t *body, *footer;
    ModalOptions options;
    options.title = "Notifications";
    options.size = ModalSize::Drawer;
    options.onClosed = onClosed;
    g_id = Modal_open(options, &body, &footer);

    // The body of a drawer is the list itself: filters on top, rows below.
    lv_obj_t *filters = Theme_row(body, lv_pct(100), 48);
    static const char *labels[3] = {"All", "Warnings", "Critical"};
    for (int i = 0; i < 3; ++i)
    {
        g_filterButtons[i] = Theme_button(filters, labels[i], 110, 44, Tone::Neutral);
        lv_obj_add_event_cb(g_filterButtons[i], onFilter, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));
    }
    g_countLabel = Theme_label(filters, "", UI_FONT_S, UI_COL_TEXT_DIM);

    g_list = Theme_column(body, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(g_list, 8, 0);

    lv_obj_t *markRead = Theme_button(footer, "Mark all read", 170, 52, Tone::Neutral);
    lv_obj_add_event_cb(markRead, [](lv_event_t *) { Notifications_markAllRead(); NotificationCentre_refresh(); },
                        LV_EVENT_CLICKED, nullptr);
    lv_obj_t *clear = Theme_button(footer, "Clear", 120, 52, Tone::Danger);
    lv_obj_add_event_cb(clear, [](lv_event_t *) { Notifications_clearHistory(); NotificationCentre_refresh(); },
                        LV_EVENT_CLICKED, nullptr);
    rebuild();
}

void NotificationCentre_close()
{
    Modal_close(g_id);
}

bool NotificationCentre_isOpen()
{
    return Modal_isOpen(g_id);
}

void NotificationCentre_refresh()
{
    if (NotificationCentre_isOpen() && g_shownRevision != Notifications_revision())
    {
        rebuild();
    }
    else if (NotificationCentre_isOpen())
    {
        // Ages tick even when the model does not change; cheap enough once a call.
    }
}
