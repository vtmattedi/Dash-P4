#include "BoardTest.h"

#include <stdio.h>

#include "board.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

namespace
{
struct TestState
{
    lv_obj_t *dot;
    lv_obj_t *touchLabel;
    lv_obj_t *memLabel;
    lv_obj_t *backlightLabel;
    lv_obj_t *pad;
};

TestState g_test;
lv_timer_t *g_timer = nullptr;

void onPadTouch(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev)
    {
        return;
    }
    lv_point_t p;
    lv_indev_get_point(indev, &p);

    lv_area_t pad;
    lv_obj_get_coords(g_test.pad, &pad);
    lv_obj_set_pos(g_test.dot, p.x - pad.x1 - 14, p.y - pad.y1 - 14);
    lv_obj_remove_flag(g_test.dot, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text_fmt(g_test.touchLabel, "touch  x=%d  y=%d%s", (int)p.x, (int)p.y,
                          lv_event_get_code(e) == LV_EVENT_RELEASED ? "  (released)" : "");
}

void onBacklight(lv_event_t *e)
{
    lv_obj_t *slider = (lv_obj_t *)lv_event_get_target(e);
    const int level = lv_slider_get_value(slider);
    board_set_backlight((uint8_t)level);
    lv_label_set_text_fmt(g_test.backlightLabel, "backlight  %d / 255", level);
}

void onRootDeleted(lv_event_t *)
{
    // The widgets are going: the timer that writes into them must go first.
    if (g_timer)
    {
        lv_timer_delete(g_timer);
        g_timer = nullptr;
    }
    g_test = TestState();
}

void onClose(lv_event_t *e)
{
    lv_obj_delete(static_cast<lv_obj_t *>(lv_event_get_user_data(e)));
}

void refreshMemory(lv_timer_t *)
{
    if (!g_test.memLabel)
    {
        return;
    }
    lv_label_set_text_fmt(
        g_test.memLabel,
        "internal  %u KB free (largest %u KB)\npsram     %u KB free of %u KB\nuptime    %lld s",
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned)(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL) / 1024),
        (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024),
        (unsigned)(heap_caps_get_total_size(MALLOC_CAP_SPIRAM) / 1024),
        (long long)(esp_timer_get_time() / 1000000));
}
} // namespace

lv_obj_t *BoardTest_create(lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_size(root, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    // Colour bars: white, red, green, blue, cyan, magenta, yellow, grey. A swapped red/blue
    // or an inverted panel shows here immediately.
    static const uint32_t bars[] = {0xFFFFFF, 0xFF0000, 0x00FF00, 0x0000FF,
                                    0x00FFFF, 0xFF00FF, 0xFFFF00, 0x808080};
    const int barW = BOARD_LCD_H_RES / 8;
    for (int i = 0; i < 8; ++i)
    {
        lv_obj_t *bar = lv_obj_create(root);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, barW, 140);
        lv_obj_set_pos(bar, i * barW, 0);
        lv_obj_set_style_bg_color(bar, lv_color_hex(bars[i]), 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    }

    // Touch pad: everything below the bars. The dot follows the finger.
    g_test.pad = lv_obj_create(root);
    lv_obj_remove_style_all(g_test.pad);
    lv_obj_set_size(g_test.pad, BOARD_LCD_H_RES - 360, BOARD_LCD_V_RES - 140);
    lv_obj_set_pos(g_test.pad, 0, 140);
    lv_obj_set_style_bg_color(g_test.pad, lv_color_hex(0x10141B), 0);
    lv_obj_set_style_bg_opa(g_test.pad, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(g_test.pad, lv_color_hex(0x2A3441), 0);
    lv_obj_set_style_border_width(g_test.pad, 2, 0);
    lv_obj_remove_flag(g_test.pad, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_test.pad, onPadTouch, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(g_test.pad, onPadTouch, LV_EVENT_PRESSING, nullptr);
    lv_obj_add_event_cb(g_test.pad, onPadTouch, LV_EVENT_RELEASED, nullptr);

    g_test.touchLabel = lv_label_create(g_test.pad);
    lv_label_set_text(g_test.touchLabel, "touch the pad");
    lv_obj_set_style_text_color(g_test.touchLabel, lv_color_hex(0xE8EDF5), 0);
    lv_obj_set_style_text_font(g_test.touchLabel, &lv_font_montserrat_20, 0);
    lv_obj_align(g_test.touchLabel, LV_ALIGN_TOP_LEFT, 16, 12);

    g_test.dot = lv_obj_create(g_test.pad);
    lv_obj_remove_style_all(g_test.dot);
    lv_obj_set_size(g_test.dot, 28, 28);
    lv_obj_set_style_radius(g_test.dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(g_test.dot, lv_color_hex(0xFF9849), 0);
    lv_obj_set_style_bg_opa(g_test.dot, LV_OPA_COVER, 0);
    lv_obj_add_flag(g_test.dot, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(g_test.dot, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(g_test.dot, LV_OBJ_FLAG_CLICKABLE);

    // Side panel: backlight and memory.
    lv_obj_t *side = lv_obj_create(root);
    lv_obj_remove_style_all(side);
    lv_obj_set_size(side, 360, BOARD_LCD_V_RES - 140);
    lv_obj_set_pos(side, BOARD_LCD_H_RES - 360, 140);
    lv_obj_remove_flag(side, LV_OBJ_FLAG_SCROLLABLE);

    g_test.backlightLabel = lv_label_create(side);
    lv_obj_set_style_text_color(g_test.backlightLabel, lv_color_hex(0xE8EDF5), 0);
    lv_obj_set_style_text_font(g_test.backlightLabel, &lv_font_montserrat_20, 0);
    lv_label_set_text_fmt(g_test.backlightLabel, "backlight  %d / 255", board_get_backlight());
    lv_obj_align(g_test.backlightLabel, LV_ALIGN_TOP_LEFT, 20, 20);

    lv_obj_t *slider = lv_slider_create(side);
    lv_slider_set_range(slider, 8, 255);
    lv_slider_set_value(slider, board_get_backlight() < 8 ? 200 : board_get_backlight(), LV_ANIM_OFF);
    lv_obj_set_size(slider, 320, 24);
    lv_obj_align(slider, LV_ALIGN_TOP_LEFT, 20, 70);
    lv_obj_set_ext_click_area(slider, 24);
    lv_obj_add_event_cb(slider, onBacklight, LV_EVENT_VALUE_CHANGED, nullptr);

    g_test.memLabel = lv_label_create(side);
    lv_obj_set_style_text_color(g_test.memLabel, lv_color_hex(0x8A97A8), 0);
    lv_obj_set_style_text_font(g_test.memLabel, &lv_font_montserrat_16, 0);
    lv_obj_align(g_test.memLabel, LV_ALIGN_TOP_LEFT, 20, 140);
    refreshMemory(nullptr);
    g_timer = lv_timer_create(refreshMemory, 1000, nullptr);
    lv_obj_add_event_cb(root, onRootDeleted, LV_EVENT_DELETE, nullptr);

    // Way out, for when the page is opened from Settings -> Diagnostics.
    lv_obj_t *close = lv_button_create(side);
    lv_obj_set_size(close, 200, 56);
    lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, -20, -20);
    lv_obj_t *closeLabel = lv_label_create(close);
    lv_label_set_text(closeLabel, "Close");
    lv_obj_center(closeLabel);
    lv_obj_add_event_cb(close, onClose, LV_EVENT_CLICKED, root);

    return root;
}
