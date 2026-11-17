#include "Ui.h"

#include <stdio.h>

#include "Screens/Pages.h"
#include "StatusBar.h"
#include "Theme.h"
#include "CallDock.h"
#include "Drawer.h"

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
    {"Calendar", LV_SYMBOL_LIST, "Calendar", PageCalendar_create, PageCalendar_refresh},
    {"Tasks", LV_SYMBOL_OK, "Tasks", PageTasks_create, PageTasks_refresh},
    {"Network", LV_SYMBOL_WIFI, "Network", PageNetwork_create, PageNetwork_refresh},
    {"Settings", LV_SYMBOL_SETTINGS, "Settings", PageSettings_create, PageSettings_refresh},
};

lv_obj_t *g_pageBox[UI_PAGE_COUNT];
lv_obj_t *g_tab[UI_PAGE_COUNT];
lv_obj_t *g_tabBar[UI_PAGE_COUNT];
UiPage g_current = UI_PAGE_COUNT; // none yet


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
        // 100 px each while they fit; the rail is 556 px tall.
        lv_obj_set_size(tab, UI_NAVRAIL_W - 1, UI_CONTENT_H / UI_PAGE_COUNT < 100 ? UI_CONTENT_H / UI_PAGE_COUNT : 100);
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

void onTick(lv_timer_t *)
{
    StatusBar_refresh();
    if (g_current < UI_PAGE_COUNT)
    {
        kPages[g_current].refresh(false);
    }
}
} // namespace

void Ui_begin(lv_display_t *display)
{
    Theme_init(display);
    lv_sysmon_hide_performance(display); // the FPS / CPU readout
    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    StatusBar_create(screen);
    buildNavRail(screen);

    // The content column: the page.
    lv_obj_t *content = lv_obj_create(screen);
    Theme_plain(content);
    lv_obj_set_size(content, UI_CONTENT_W, UI_CONTENT_H);
    lv_obj_set_pos(content, UI_NAVRAIL_W, UI_STATUSBAR_H);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
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

    Drawer_create();
    CallDock_create();
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
    }
    lv_obj_remove_flag(g_pageBox[page], LV_OBJ_FLAG_HIDDEN);
    styleTab(page);
    kPages[page].refresh(true);
}

static bool g_perfShown = false;

bool Ui_perfShown() { return g_perfShown; }

void Ui_setPerf(bool on)
{
    lv_display_t *display = lv_display_get_default();
    if (on == g_perfShown || !display)
    {
        return;
    }
    if (on)
    {
        lv_sysmon_show_performance(display);
    }
    else
    {
        lv_sysmon_hide_performance(display);
    }
    g_perfShown = on;
}

UiPage Ui_current()
{
    return g_current;
}

lv_obj_t *Ui_pageContainer(UiPage page)
{
    return page < UI_PAGE_COUNT ? g_pageBox[page] : nullptr;
}
