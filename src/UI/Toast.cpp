#include "Toast.h"

#include <vector>

#include "App/Notifications.h"
#include "Theme.h"

namespace
{
constexpr int kMaxVisible = 3;
constexpr int32_t kWidth = 400;

struct Toast
{
    lv_obj_t *card = nullptr;
    lv_timer_t *timer = nullptr;
};

lv_obj_t *g_host = nullptr;
std::vector<Toast *> g_toasts; // oldest first
bool g_suppressed = false;

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

const char *symbolFor(NotifySeverity s)
{
    switch (s)
    {
    case NotifySeverity::Ok:
        return LV_SYMBOL_OK;
    case NotifySeverity::Warn:
        return LV_SYMBOL_WARNING;
    case NotifySeverity::Crit:
        return LV_SYMBOL_WARNING;
    default:
        return LV_SYMBOL_BELL;
    }
}

uint32_t lifetimeMs(NotifySeverity s)
{
    switch (s)
    {
    case NotifySeverity::Warn:
        return 8000;
    case NotifySeverity::Crit:
        return 30000;
    default:
        return 4000;
    }
}

void remove(Toast *t)
{
    for (size_t i = 0; i < g_toasts.size(); ++i)
    {
        if (g_toasts[i] == t)
        {
            g_toasts.erase(g_toasts.begin() + i);
            break;
        }
    }
    if (t->timer)
    {
        lv_timer_delete(t->timer);
        t->timer = nullptr;
    }
    if (t->card)
    {
        lv_obj_t *card = t->card;
        t->card = nullptr;
        lv_obj_delete(card);
    }
    delete t;
}

void onTimeout(lv_timer_t *timer)
{
    Toast *t = static_cast<Toast *>(lv_timer_get_user_data(timer));
    remove(t);
}

void onTap(lv_event_t *e)
{
    remove(static_cast<Toast *>(lv_event_get_user_data(e)));
}

void animX(void *obj, int32_t v) { lv_obj_set_style_translate_x(static_cast<lv_obj_t *>(obj), v, 0); }

void show(const Notification &n)
{
    if (!g_host || g_suppressed)
    {
        return;
    }
    while (g_toasts.size() >= kMaxVisible)
    {
        remove(g_toasts.front());
    }

    Toast *t = new Toast();
    const lv_color_t colour = Theme_toneColor(toneFor(n.severity));

    t->card = lv_obj_create(g_host);
    lv_obj_remove_style_all(t->card);
    lv_obj_set_size(t->card, kWidth, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(t->card, UI_COL_SURFACE_HI, 0);
    lv_obj_set_style_bg_opa(t->card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(t->card, 12, 0);
    lv_obj_set_style_border_color(t->card, colour, 0);
    lv_obj_set_style_border_width(t->card, 4, 0);
    lv_obj_set_style_border_side(t->card, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_shadow_width(t->card, 24, 0);
    lv_obj_set_style_shadow_opa(t->card, LV_OPA_50, 0);
    lv_obj_set_style_shadow_color(t->card, lv_color_black(), 0);
    lv_obj_set_style_pad_all(t->card, 14, 0);
    lv_obj_set_style_pad_gap(t->card, 12, 0);
    lv_obj_remove_flag(t->card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(t->card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(t->card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    lv_obj_t *icon = Theme_label(t->card, symbolFor(n.severity), UI_FONT_L, colour);
    lv_obj_set_width(icon, 26);

    lv_obj_t *text = Theme_column(t->card, kWidth - 14 * 2 - 26 - 12 - 4, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(text, 2, 0);
    lv_obj_t *title = Theme_label(text, n.title.c_str(), UI_FONT_M, UI_COL_TEXT);
    lv_obj_set_width(title, lv_pct(100));
    lv_label_set_long_mode(title, LV_LABEL_LONG_WRAP);
    if (!n.body.empty())
    {
        lv_obj_t *body = Theme_label(text, n.body.c_str(), UI_FONT_S, UI_COL_TEXT_DIM);
        lv_obj_set_width(body, lv_pct(100));
        lv_label_set_long_mode(body, LV_LABEL_LONG_WRAP);
    }

    lv_obj_add_flag(t->card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(t->card, onTap, LV_EVENT_CLICKED, t);

    t->timer = lv_timer_create(onTimeout, lifetimeMs(n.severity), t);
    lv_timer_set_repeat_count(t->timer, 1);
    g_toasts.push_back(t);

    lv_obj_move_foreground(g_host);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, t->card);
    lv_anim_set_exec_cb(&a, animX);
    lv_anim_set_values(&a, kWidth + 20, 0);
    lv_anim_set_duration(&a, 220);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

void onNotification(const Notification &n, NotifyEvent event)
{
    if (event == NotifyEvent::Added)
    {
        show(n);
    }
    else if (event == NotifyEvent::Updated && n.sticky && !n.read)
    {
        show(n); // a live condition changed (e.g. Warn -> Crit)
    }
}
} // namespace

void Toast_begin()
{
    if (g_host)
    {
        return;
    }
    g_host = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_host);
    lv_obj_set_size(g_host, kWidth, LV_SIZE_CONTENT);
    lv_obj_align(g_host, LV_ALIGN_TOP_RIGHT, -12, UI_STATUSBAR_H + 8);
    lv_obj_remove_flag(g_host, LV_OBJ_FLAG_CLICKABLE); // the gaps between toasts pass touches through
    lv_obj_remove_flag(g_host, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(g_host, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(g_host, 10, 0);
    Notifications_onChange(onNotification);
}

void Toast_clear()
{
    while (!g_toasts.empty())
    {
        remove(g_toasts.back());
    }
}

void Toast_setSuppressed(bool suppressed)
{
    g_suppressed = suppressed;
    if (suppressed)
    {
        Toast_clear();
    }
}
