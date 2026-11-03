#include "Ui.h"
#include "Explorer/TopicTap.h"

#include <stdio.h>

#include "App/Notifications.h"
#include "NotificationCentre.h"
#include "Screens/Pages.h"
#include "StatusBar.h"
#include "Theme.h"
#include "Toast.h"
#if DASH_PC_SCREEN
#include "PcLink/PcLink.h"
#endif

namespace
{
struct PageDef
{
    const char *title;
    const char *symbol;
    const char *label;
    lv_obj_t *(*create)(lv_obj_t *);
    void (*refresh)(bool);
};

const PageDef kPages[UI_PAGE_COUNT] = {
    {"Home", LV_SYMBOL_HOME, "Home", PageHome_create, PageHome_refresh},
    {"Network", LV_SYMBOL_WIFI, "Network", PageNetwork_create, PageNetwork_refresh},
    {"Monitor", LV_SYMBOL_EYE_OPEN, "Monitor", PageMonitor_create, PageMonitor_refresh},
#if DASH_PC_SCREEN
    {"PC screen", LV_SYMBOL_IMAGE, "PC", PagePc_create, PagePc_refresh},
#endif
    {"Settings", LV_SYMBOL_SETTINGS, "Settings", PageSettings_create, PageSettings_refresh},
};

lv_obj_t *g_pageBox[UI_PAGE_COUNT];
lv_obj_t *g_tab[UI_PAGE_COUNT];
lv_obj_t *g_tabBar[UI_PAGE_COUNT];
UiPage g_current = UI_PAGE_COUNT; // none yet

lv_obj_t *g_banner;
lv_obj_t *g_bannerBar;
lv_obj_t *g_bannerIcon;
lv_obj_t *g_bannerText;
lv_obj_t *g_bannerMore;
uint32_t g_bannerRevision = UINT32_MAX;

void styleTab(UiPage page)
{
    const bool active = (page == g_current);
    lv_obj_set_style_bg_color(g_tab[page], active ? UI_COL_SURFACE_ALT : UI_COL_SURFACE, 0);
    lv_obj_set_style_text_color(g_tab[page], active ? UI_COL_ACCENT : UI_COL_TEXT_DIM, 0);
    lv_obj_set_style_bg_opa(g_tabBar[page], active ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
}

void onTab(lv_event_t *e)
{
    Ui_show(static_cast<UiPage>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e))));
}

lv_obj_t *buildNavRail(lv_obj_t *parent)
{
    lv_obj_t *rail = lv_obj_create(parent);
    Theme_plain(rail);
    lv_obj_set_size(rail, UI_NAVRAIL_W, UI_CONTENT_H);
    lv_obj_set_pos(rail, 0, UI_STATUSBAR_H);
    lv_obj_set_style_bg_color(rail, UI_COL_SURFACE, 0);
    lv_obj_set_style_bg_opa(rail, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(rail, LV_BORDER_SIDE_RIGHT, 0);
    lv_obj_set_style_border_width(rail, 1, 0);
    lv_obj_set_style_border_color(rail, UI_COL_BORDER, 0);
    lv_obj_set_flex_flow(rail, LV_FLEX_FLOW_COLUMN);

    for (int i = 0; i < UI_PAGE_COUNT; ++i)
    {
        lv_obj_t *tab = lv_obj_create(rail);
        Theme_plain(tab);
        lv_obj_set_size(tab, UI_NAVRAIL_W - 1, 100);
        lv_obj_set_style_bg_opa(tab, LV_OPA_COVER, 0);
        lv_obj_add_flag(tab, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(tab, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_gap(tab, 6, 0);
        lv_obj_add_event_cb(tab, onTab, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(i)));

        lv_obj_t *icon = lv_label_create(tab);
        lv_label_set_text(icon, kPages[i].symbol);
        lv_obj_set_style_text_font(icon, UI_FONT_XL, 0);
        lv_obj_t *label = lv_label_create(tab);
        lv_label_set_text(label, kPages[i].label);
        lv_obj_set_style_text_font(label, UI_FONT_S, 0);

        // The active marker: a bar down the inside edge.
        lv_obj_t *bar = lv_obj_create(tab);
        lv_obj_remove_style_all(bar);
        lv_obj_add_flag(bar, LV_OBJ_FLAG_FLOATING);
        lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(bar, 4, 60);
        lv_obj_align(bar, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_set_style_bg_color(bar, UI_COL_ACCENT, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_TRANSP, 0);

        g_tab[i] = tab;
        g_tabBar[i] = bar;
    }
    // Ui_show() only restyles the tab it leaves and the one it opens; without this the others kept LVGL's default
    // white until they had been visited once.
    for (int i = 0; i < UI_PAGE_COUNT; ++i)
    {
        styleTab(static_cast<UiPage>(i));
    }
    return rail;
}

// --- Banner: the most severe live condition, with a count of the rest ---------------------

void onBanner(lv_event_t *)
{
    NotificationCentre_open();
}

void buildBanner(lv_obj_t *parent)
{
    g_banner = lv_obj_create(parent);
    Theme_plain(g_banner);
    lv_obj_set_size(g_banner, lv_pct(100), 44);
    lv_obj_set_style_bg_color(g_banner, UI_COL_SURFACE_ALT, 0);
    lv_obj_set_style_bg_opa(g_banner, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(g_banner, UI_GUTTER, 0);
    lv_obj_set_flex_flow(g_banner, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_banner, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(g_banner, 12, 0);
    lv_obj_add_flag(g_banner, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(g_banner, onBanner, LV_EVENT_CLICKED, nullptr);

    g_bannerBar = lv_obj_create(g_banner);
    lv_obj_remove_style_all(g_bannerBar);
    lv_obj_add_flag(g_bannerBar, LV_OBJ_FLAG_FLOATING);
    lv_obj_set_size(g_bannerBar, 5, lv_pct(100));
    lv_obj_align(g_bannerBar, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(g_bannerBar, LV_OPA_COVER, 0);

    g_bannerIcon = Theme_label(g_banner, LV_SYMBOL_WARNING, UI_FONT_L, UI_COL_WARN);
    g_bannerText = Theme_label(g_banner, "", UI_FONT_M, UI_COL_TEXT);
    lv_obj_set_flex_grow(g_bannerText, 1);
    lv_label_set_long_mode(g_bannerText, LV_LABEL_LONG_DOT);
    g_bannerMore = Theme_label(g_banner, "", UI_FONT_S, UI_COL_TEXT_DIM);
    lv_obj_add_flag(g_banner, LV_OBJ_FLAG_HIDDEN);
}

void refreshBanner()
{
    if (g_bannerRevision == Notifications_revision())
    {
        return;
    }
    g_bannerRevision = Notifications_revision();

    const size_t live = Notifications_activeCount();
    if (live == 0)
    {
        lv_obj_add_flag(g_banner, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    const Notification &top = Notifications_activeAt(0); // most severe first
    lv_color_t colour = UI_COL_WARN;
    if (top.severity == NotifySeverity::Crit)
    {
        colour = UI_COL_DANGER;
    }
    else if (top.severity == NotifySeverity::Info)
    {
        colour = UI_COL_INFO;
    }
    else if (top.severity == NotifySeverity::Ok)
    {
        colour = UI_COL_OK;
    }
    lv_obj_set_style_bg_color(g_bannerBar, colour, 0);
    lv_obj_set_style_text_color(g_bannerIcon, colour, 0);
    std::string text = top.title;
    if (!top.body.empty())
    {
        text += "  -  " + top.body;
    }
    Theme_setText(g_bannerText, text.c_str());
    if (live > 1)
    {
        char buf[24];
        snprintf(buf, sizeof(buf), "+%u more", (unsigned)(live - 1));
        Theme_setText(g_bannerMore, buf);
    }
    else
    {
        Theme_setText(g_bannerMore, "");
    }
    lv_obj_remove_flag(g_banner, LV_OBJ_FLAG_HIDDEN);
}

void onTick(lv_timer_t *)
{
    StatusBar_refresh();
    refreshBanner();
    NotificationCentre_refresh();
    if (g_current < UI_PAGE_COUNT)
    {
        kPages[g_current].refresh(false);
    }
}
} // namespace

void Ui_begin(lv_display_t *display)
{
    Theme_init(display);
    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    StatusBar_create(screen);
    buildNavRail(screen);

    // The content column: banner on top (hidden when there is nothing to say), page below.
    lv_obj_t *content = lv_obj_create(screen);
    Theme_plain(content);
    lv_obj_set_size(content, UI_CONTENT_W, UI_CONTENT_H);
    lv_obj_set_pos(content, UI_NAVRAIL_W, UI_STATUSBAR_H);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    buildBanner(content);

    lv_obj_t *host = lv_obj_create(content);
    Theme_plain(host);
    lv_obj_set_width(host, lv_pct(100));
    lv_obj_set_flex_grow(host, 1);

    for (int i = 0; i < UI_PAGE_COUNT; ++i)
    {
        g_pageBox[i] = lv_obj_create(host);
        Theme_plain(g_pageBox[i]);
        lv_obj_set_size(g_pageBox[i], lv_pct(100), lv_pct(100));
        lv_obj_add_flag(g_pageBox[i], LV_OBJ_FLAG_HIDDEN);
        kPages[i].create(g_pageBox[i]);
    }

    Toast_begin();
    Ui_show(UI_PAGE_HOME);
    lv_timer_create(onTick, 250, nullptr);
}

void Ui_show(UiPage page)
{
    if (page >= UI_PAGE_COUNT)
    {
        return;
    }
    const UiPage previous = g_current;
    g_current = page;
    if (previous < UI_PAGE_COUNT && previous != page)
    {
        lv_obj_add_flag(g_pageBox[previous], LV_OBJ_FLAG_HIDDEN);
        styleTab(previous);
        if (previous == UI_PAGE_MONITOR)
        {
            PageMonitor_leave();
            Tap_release(); // the tail is only kept while the monitor is on screen
        }
    }
    if (page == UI_PAGE_MONITOR && previous != page)
    {
        Tap_acquire();
    }
    lv_obj_remove_flag(g_pageBox[page], LV_OBJ_FLAG_HIDDEN);
    styleTab(page);
    StatusBar_setTitle(kPages[page].title);
    kPages[page].refresh(true);
#if DASH_PC_SCREEN
    if (page == UI_PAGE_PC)
    {
        // Opening the PC tab connects (if the connection was closed) and shows the PC as soon as it streams.
        PcLink_show();
    }
#endif
}

UiPage Ui_current()
{
    return g_current;
}

lv_obj_t *Ui_pageContainer(UiPage page)
{
    return page < UI_PAGE_COUNT ? g_pageBox[page] : nullptr;
}
