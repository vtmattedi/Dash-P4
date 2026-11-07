#include "Pages.h"

#if DASH_CAMERA

#include <stdio.h>

#include "App/CameraMode.h"
#include "UI/Theme.h"

// The Camera tab: the board's camera live on the panel, the picture controls, and the switch that hands the USB-C port
// to a PC as a webcam. The picture comes from CameraMode (which runs the camera only while this page, or a PC, wants
// it); the controls act on it live and are saved when a slider is let go.
//
//   +------------------------------+--------------+
//   |  preview 644 x 364           |  Picture     |
//   |                              |  Exposure    |
//   +------------------------------+  Brightness  |
//   |  USB webcam  [switch]        |  Saturation  |
//   |  status                      |  Contrast    |
//   +------------------------------+  White bal.  |
//                                  |  Warmth      |
//                                  |  [Reset]     |
//                                  +--------------+

namespace
{
constexpr int32_t kControlsW = UI_CONTENT_W - 2 * UI_GUTTER - kCameraPreviewW - UI_GUTTER;

lv_obj_t *g_image;
lv_obj_t *g_hint;
lv_obj_t *g_fps;
lv_obj_t *g_status;
lv_obj_t *g_share;
lv_obj_t *g_wbAuto;
lv_obj_t *g_wbHold;

struct Row
{
    lv_obj_t *slider = nullptr;
    lv_obj_t *value = nullptr;
};
Row g_ev, g_br, g_sat, g_con, g_warm;

camera_adjust_t g_adj = CAMERA_ADJUST_DEFAULT();
bool g_building = false; // programmatic changes must not feed back into the camera

void formatValues()
{
    char text[24];
    snprintf(text, sizeof(text), "%+.1f EV", g_adj.exposure / 10.0);
    Theme_setText(g_ev.value, text);
    snprintf(text, sizeof(text), "%+d", g_adj.brightness);
    Theme_setText(g_br.value, text);
    snprintf(text, sizeof(text), "%u%%", g_adj.saturation);
    Theme_setText(g_sat.value, text);
    snprintf(text, sizeof(text), "%u%%", g_adj.contrast);
    Theme_setText(g_con.value, text);
    snprintf(text, sizeof(text), "%+d", g_adj.warmth);
    Theme_setText(g_warm.value, text);
}

void styleWb()
{
    auto paint = [](lv_obj_t *button, bool on)
    {
        lv_obj_set_style_bg_color(button, on ? UI_COL_ACCENT : UI_COL_SURFACE_HI, 0);
        lv_obj_set_style_text_color(button, on ? UI_COL_BG : UI_COL_TEXT, 0);
    };
    paint(g_wbAuto, !g_adj.wb_hold);
    paint(g_wbHold, g_adj.wb_hold);
    // Warmth shifts the held gains: it does nothing while the white balance is automatic.
    if (g_adj.wb_hold)
    {
        lv_obj_remove_state(g_warm.slider, LV_STATE_DISABLED);
    }
    else
    {
        lv_obj_add_state(g_warm.slider, LV_STATE_DISABLED);
    }
}

void pushSliders()
{
    g_adj.exposure = static_cast<int8_t>(lv_slider_get_value(g_ev.slider));
    g_adj.brightness = static_cast<int8_t>(lv_slider_get_value(g_br.slider));
    g_adj.saturation = static_cast<uint8_t>(lv_slider_get_value(g_sat.slider));
    g_adj.contrast = static_cast<uint8_t>(lv_slider_get_value(g_con.slider));
    g_adj.warmth = static_cast<int8_t>(lv_slider_get_value(g_warm.slider));
}

void pullSliders()
{
    g_building = true;
    lv_slider_set_value(g_ev.slider, g_adj.exposure, LV_ANIM_OFF);
    lv_slider_set_value(g_br.slider, g_adj.brightness, LV_ANIM_OFF);
    lv_slider_set_value(g_sat.slider, g_adj.saturation, LV_ANIM_OFF);
    lv_slider_set_value(g_con.slider, g_adj.contrast, LV_ANIM_OFF);
    lv_slider_set_value(g_warm.slider, g_adj.warmth, LV_ANIM_OFF);
    g_building = false;
    formatValues();
    styleWb();
}

void onSlider(lv_event_t *e)
{
    if (g_building)
    {
        return;
    }
    pushSliders();
    formatValues();
    // Dragging applies live; letting go saves.
    CameraMode_setAdjust(g_adj, lv_event_get_code(e) == LV_EVENT_RELEASED);
}

void onWb(lv_event_t *e)
{
    g_adj.wb_hold = lv_event_get_user_data(e) != nullptr;
    pushSliders();
    styleWb();
    CameraMode_setAdjust(g_adj, true);
    g_adj = CameraMode_adjust();
}

void onReset(lv_event_t *)
{
    g_adj = CAMERA_ADJUST_DEFAULT();
    CameraMode_setAdjust(g_adj, true);
    g_adj = CameraMode_adjust();
    pullSliders();
}

void onShare(lv_event_t *e)
{
    if (g_building)
    {
        return;
    }
    CameraMode_setSharing(lv_obj_has_state(lv_event_get_target_obj(e), LV_STATE_CHECKED));
}

Row slider(lv_obj_t *parent, const char *title, int min, int max)
{
    Row row;
    lv_obj_t *box = Theme_column(parent, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(box, 2, 0);
    lv_obj_t *head = Theme_row(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_align(head, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    Theme_label(head, title, UI_FONT_S, UI_COL_TEXT_DIM);
    row.value = Theme_label(head, "", UI_FONT_S, UI_COL_TEXT);

    row.slider = lv_slider_create(box);
    lv_obj_set_width(row.slider, lv_pct(92));
    lv_obj_set_style_pad_all(row.slider, 4, LV_PART_KNOB);
    lv_obj_set_style_bg_color(row.slider, UI_COL_ACCENT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(row.slider, UI_COL_ACCENT, LV_PART_KNOB);
    lv_slider_set_range(row.slider, min, max);
    lv_obj_add_event_cb(row.slider, onSlider, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(row.slider, onSlider, LV_EVENT_RELEASED, nullptr);
    return row;
}
} // namespace

lv_obj_t *PageCamera_create(lv_obj_t *parent)
{
    lv_obj_t *page = Theme_row(parent, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(page, UI_GUTTER, 0);
    lv_obj_set_style_pad_gap(page, UI_GUTTER, 0);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);

    // --- Left: the picture, and the webcam switch under it ---
    lv_obj_t *left = Theme_column(page, kCameraPreviewW, lv_pct(100));
    lv_obj_set_style_pad_gap(left, UI_GUTTER, 0);

    lv_obj_t *frame = lv_obj_create(left);
    Theme_plain(frame);
    lv_obj_set_size(frame, kCameraPreviewW, kCameraPreviewH);
    lv_obj_set_style_bg_color(frame, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(frame, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(frame, 12, 0);
    lv_obj_set_style_clip_corner(frame, true, 0);
    g_image = lv_image_create(frame);
    lv_obj_set_pos(g_image, 0, 0);
    g_hint = Theme_label(frame, "Starting the camera...", UI_FONT_M, UI_COL_TEXT_DIM);
    lv_obj_center(g_hint);
    g_fps = Theme_label(frame, "", UI_FONT_S, UI_COL_TEXT);
    lv_obj_add_flag(g_fps, LV_OBJ_FLAG_FLOATING);
    lv_obj_align(g_fps, LV_ALIGN_TOP_RIGHT, -10, 8);

    lv_obj_t *usb = Theme_card(left, kCameraPreviewW, LV_SIZE_CONTENT);
    lv_obj_t *head = Theme_cardHeader(usb, "USB webcam");
    g_share = lv_switch_create(head);
    lv_obj_set_style_bg_color(g_share, UI_COL_ACCENT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_add_event_cb(g_share, onShare, LV_EVENT_VALUE_CHANGED, nullptr);
    g_status = Theme_label(usb, "", UI_FONT_M, UI_COL_TEXT_DIM);
    lv_label_set_long_mode(g_status, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(g_status, lv_pct(100));

    // --- Right: the picture controls ---
    lv_obj_t *controls = Theme_card(page, kControlsW, lv_pct(100));
    lv_obj_set_style_pad_gap(controls, 6, 0);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_COLUMN);
    Theme_cardHeader(controls, "Picture");

    g_ev = slider(controls, "Exposure", -30, 30);
    g_br = slider(controls, "Brightness", -50, 50);
    g_sat = slider(controls, "Saturation", 0, 200);
    g_con = slider(controls, "Contrast", 50, 150);

    Theme_label(controls, "White balance", UI_FONT_S, UI_COL_TEXT_DIM);
    lv_obj_t *wb = Theme_row(controls, lv_pct(100), 44);
    lv_obj_set_style_pad_gap(wb, 8, 0);
    const int32_t half = (kControlsW - 2 * 16 - 8) / 2;
    g_wbAuto = Theme_button(wb, "Auto", half, 44);
    g_wbHold = Theme_button(wb, "Hold", half, 44);
    lv_obj_add_event_cb(g_wbAuto, onWb, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_event_cb(g_wbHold, onWb, LV_EVENT_CLICKED, reinterpret_cast<void *>(1));
    g_warm = slider(controls, "Warmth (hold)", -50, 50);

    lv_obj_t *reset = Theme_button(controls, "Reset", lv_pct(100), 44);
    lv_obj_add_event_cb(reset, onReset, LV_EVENT_CLICKED, nullptr);

    g_adj = CameraMode_adjust();
    pullSliders();
    // No refresh here: refreshing means "on screen", and starts the camera. Ui_show() refreshes when the tab opens.
    return page;
}

void PageCamera_refresh(bool)
{
    // Being refreshed means being on screen: the camera runs while this page is up.
    CameraMode_setPreview(g_image);

    const bool running = CameraMode_running();
    const float fps = CameraMode_fps();
    if (running && fps > 0.0f)
    {
        lv_obj_add_flag(g_hint, LV_OBJ_FLAG_HIDDEN);
        char text[24];
        snprintf(text, sizeof(text), "%.0f fps", static_cast<double>(fps));
        Theme_setText(g_fps, text);
    }
    else
    {
        lv_obj_remove_flag(g_hint, LV_OBJ_FLAG_HIDDEN);
        Theme_setText(g_fps, "");
    }

    Theme_setText(g_status, CameraMode_status());

    g_building = true;
    if (CameraMode_sharing())
    {
        lv_obj_add_state(g_share, LV_STATE_CHECKED);
    }
    else
    {
        lv_obj_remove_state(g_share, LV_STATE_CHECKED);
    }
    g_building = false;
}

void PageCamera_leave()
{
    CameraMode_setPreview(nullptr);
}

#endif // DASH_CAMERA
