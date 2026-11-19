#include "CallDock.h"

#include "Link/LinkServer.h"
#include "Theme.h"
#include "lvgl.h"

namespace
{
constexpr int32_t kW = 400;
constexpr int32_t kH = 84;
constexpr int32_t kBottomGap = 20;

lv_obj_t *g_dock;
lv_obj_t *g_app;
lv_obj_t *g_state;
lv_obj_t *g_button;
lv_obj_t *g_slash[2]; // the cut through the microphone: a dark gap under a light stroke
uint32_t g_pendingUntil; // after a tap: wait for the PC to report the new state before accepting another

constexpr int32_t kButton = 64;
constexpr int32_t kIcon = 36;

lv_obj_t *part(lv_obj_t *parent, int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

lv_obj_t *stroke(lv_obj_t *parent, lv_color_t colour, int32_t width)
{
    static const lv_point_precise_t kLine[] = {{4, 4}, {32, 32}};
    lv_obj_t *line = lv_line_create(parent);
    lv_line_set_points(line, kLine, 2);
    lv_obj_set_style_line_width(line, width, 0);
    lv_obj_set_style_line_color(line, colour, 0);
    lv_obj_set_style_line_rounded(line, true, 0);
    lv_obj_remove_flag(line, LV_OBJ_FLAG_CLICKABLE);
    return line;
}

// A microphone drawn from plain shapes (the UI fonts have no microphone glyph): capsule, cup, stem, foot, and, when muted,
// a slash across it.
void buildMicIcon(lv_obj_t *parent)
{
    lv_obj_t *icon = part(parent, (kButton - kIcon) / 2, (kButton - kIcon) / 2, kIcon, kIcon, 0);
    lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
    const lv_color_t ink = lv_color_hex(0x10141A); // dark on the green / red button
    lv_obj_t *capsule = part(icon, 12, 1, 12, 20, 6);
    Theme_bgColor(capsule, ink);
    lv_obj_set_style_bg_opa(capsule, LV_OPA_COVER, 0);
    lv_obj_t *cup = part(icon, 6, 9, 24, 16, 12);
    Theme_borderColor(cup, ink);
    lv_obj_set_style_border_width(cup, 3, 0);
    lv_obj_set_style_border_side(cup, static_cast<lv_border_side_t>(LV_BORDER_SIDE_LEFT | LV_BORDER_SIDE_RIGHT | LV_BORDER_SIDE_BOTTOM), 0);
    lv_obj_t *stem = part(icon, 16, 25, 4, 5, 0);
    Theme_bgColor(stem, ink);
    lv_obj_set_style_bg_opa(stem, LV_OPA_COVER, 0);
    lv_obj_t *foot = part(icon, 10, 30, 16, 3, 1);
    Theme_bgColor(foot, ink);
    lv_obj_set_style_bg_opa(foot, LV_OPA_COVER, 0);
    g_slash[0] = stroke(icon, UI_COL_DANGER, 8); // the gap the cut leaves in the microphone
    g_slash[1] = stroke(icon, ink, 3);
}

void onTap(lv_event_t *)
{
    if (lv_tick_get() < g_pendingUntil)
    {
        return; // one tap at a time; the PC's report or the timeout releases it
    }
    if (LinkServer_micMute("toggle") != 0)
    {
        g_pendingUntil = lv_tick_get() + 1500;
    }
}

void refresh(lv_timer_t *)
{
    const PcCall call = LinkServer_call();
    if (!call.active)
    {
        lv_obj_add_flag(g_dock, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_obj_remove_flag(g_dock, LV_OBJ_FLAG_HIDDEN);
    static bool shownMuted = false;
    if (call.muted != shownMuted)
    {
        g_pendingUntil = 0; // the PC reported the change
    }
    shownMuted = call.muted;
    lv_label_set_text(g_app, call.app.empty() ? "In a call" : call.app.c_str());
    lv_label_set_text(g_state, call.muted ? "Microphone muted" : "Microphone live");
    Theme_textColor(g_state, call.muted ? UI_COL_DANGER : UI_COL_OK);
    Theme_bgColor(g_button, call.muted ? UI_COL_DANGER : UI_COL_OK);
    for (lv_obj_t *line : g_slash)
    {
        if (call.muted)
            lv_obj_remove_flag(line, LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(line, LV_OBJ_FLAG_HIDDEN);
    }
}
} // namespace

void CallDock_create()
{
    g_dock = lv_obj_create(lv_layer_sys());
    Theme_plain(g_dock);
    lv_obj_set_size(g_dock, kW, kH);
    lv_obj_align(g_dock, LV_ALIGN_BOTTOM_MID, 0, -kBottomGap);
    Theme_bgColor(g_dock, UI_COL_SURFACE_ALT);
    lv_obj_set_style_bg_opa(g_dock, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(g_dock, kH / 2, 0);
    lv_obj_set_style_border_width(g_dock, 2, 0);
    Theme_borderColor(g_dock, UI_COL_BORDER);
    lv_obj_set_style_pad_hor(g_dock, 22, 0);
    lv_obj_set_flex_flow(g_dock, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(g_dock, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(g_dock, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *text = lv_obj_create(g_dock);
    Theme_plain(text);
    lv_obj_set_size(text, 260, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
    g_app = Theme_label(text, "", UI_FONT_L, UI_COL_TEXT);
    g_state = Theme_label(text, "", UI_FONT_M, UI_COL_TEXT_DIM);

    // The round microphone button: green and whole while live, red and cut while muted.
    g_button = lv_obj_create(g_dock);
    lv_obj_remove_style_all(g_button);
    lv_obj_set_size(g_button, kButton, kButton);
    lv_obj_set_style_radius(g_button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(g_button, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_button, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g_button, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_button, onTap, LV_EVENT_CLICKED, nullptr);
    buildMicIcon(g_button);

    lv_timer_create(refresh, 250, nullptr);
}
