#include "Popover.h"

#include <memory>

namespace
{
constexpr int32_t kWidth = 400, kGap = 8, kMargin = 8, kPad = 6, kRow = 52;

lv_obj_t *g_catcher = nullptr; // transparent, full screen: a tap outside the panel lands here
std::shared_ptr<std::function<void(size_t)>> g_onPick;

void onOutside(lv_event_t *) { Popover_close(); }

void onItem(lv_event_t *e)
{
    const size_t index = static_cast<size_t>(reinterpret_cast<intptr_t>(lv_event_get_user_data(e)));
    std::shared_ptr<std::function<void(size_t)>> cb = g_onPick; // survives the widgets
    Popover_close();
    if (cb && *cb)
        (*cb)(index);
}

lv_obj_t *buildItem(lv_obj_t *panel, const PopoverItem &item, size_t index)
{
    lv_obj_t *row = lv_obj_create(panel);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), kRow);
    lv_obj_set_style_radius(row, 8, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_color(row, UI_COL_SURFACE_HI, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_pad_hor(row, 12, 0);
    lv_obj_set_style_pad_gap(row, 10, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(row, onItem, LV_EVENT_CLICKED, reinterpret_cast<void *>(static_cast<intptr_t>(index)));

    if (item.icon)
    {
        lv_obj_t *image = lv_image_create(row);
        lv_image_set_src(image, item.icon);
        lv_image_set_scale(image, 128); // 56 px in flash, 28 px here
        lv_obj_set_size(image, 28, 28);
        lv_obj_remove_flag(image, LV_OBJ_FLAG_CLICKABLE);
    }
    // A fixed width, so a long path is cut with "..." on one line instead of wrapping under its first letters.
    const int32_t textW = kWidth - 2 * kPad - 24 - (item.icon ? 38 : 0) - (item.badge.empty() ? 0 : 66);
    lv_obj_t *text = Theme_column(row, textW, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_gap(text, 1, 0);
    lv_obj_remove_flag(text, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *label = Theme_label(text, item.label.c_str(), UI_FONT_M, UI_COL_TEXT);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(label, textW);
    if (!item.detail.empty())
    {
        lv_obj_t *detail = Theme_label(text, item.detail.c_str(), UI_FONT_S, UI_COL_TEXT_DIM);
        lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
        lv_obj_set_width(detail, textW);
    }
    if (!item.badge.empty())
    {
        lv_obj_t *chip = Theme_chip(row);
        Theme_chipSet(chip, item.badge.c_str(), UI_COL_OK);
        lv_obj_remove_flag(chip, LV_OBJ_FLAG_CLICKABLE);
    }
    return row;
}
} // namespace

void Popover_close()
{
    g_onPick.reset();
    if (g_catcher)
    {
        lv_obj_t *c = g_catcher;
        g_catcher = nullptr;
        lv_obj_delete(c); // the panel is its child
    }
}

bool Popover_isOpen() { return g_catcher != nullptr; }

void Popover_open(lv_obj_t *anchor, const std::vector<PopoverItem> &items, std::function<void(size_t)> onPick,
                  const std::string &emptyText)
{
    Popover_close();
    if (anchor == nullptr)
        return;
    g_onPick = std::make_shared<std::function<void(size_t)>>(std::move(onPick));

    g_catcher = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(g_catcher);
    lv_obj_set_size(g_catcher, lv_pct(100), lv_pct(100));
    lv_obj_add_flag(g_catcher, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(g_catcher, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(g_catcher, onOutside, LV_EVENT_CLICKED, nullptr);

    lv_obj_t *panel = lv_obj_create(g_catcher);
    lv_obj_remove_style_all(panel);
    lv_obj_set_width(panel, kWidth);
    lv_obj_set_height(panel, LV_SIZE_CONTENT);
    Theme_bgColor(panel, UI_COL_SURFACE);
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(panel, 12, 0);
    Theme_borderColor(panel, UI_COL_BORDER);
    Theme_borderWidth(panel, 1);
    lv_obj_set_style_pad_all(panel, kPad, 0);
    lv_obj_set_style_pad_gap(panel, 2, 0);
    lv_obj_add_flag(panel, LV_OBJ_FLAG_CLICKABLE); // a tap on the panel's own margin must not dismiss it
    lv_obj_add_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(panel, LV_DIR_VER);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);

    for (size_t i = 0; i < items.size(); ++i)
    {
        if (!items[i].section.empty() && (i == 0 || items[i].section != items[i - 1].section))
        {
            lv_obj_t *head = Theme_label(panel, items[i].section.c_str(), UI_FONT_S, UI_COL_TEXT_FAINT);
            lv_obj_set_style_pad_hor(head, 12, 0);
            lv_obj_set_style_pad_top(head, i == 0 ? 6 : 12, 0);
            lv_obj_set_style_pad_bottom(head, 4, 0);
        }
        buildItem(panel, items[i], i);
    }
    if (items.empty())
    {
        lv_obj_t *none = Theme_label(panel, emptyText.c_str(), UI_FONT_M, UI_COL_TEXT_DIM);
        lv_obj_set_style_pad_all(none, 10, 0);
    }

    // Measure, then put it beside the anchor: to its right when there is room, else to its left, top edges level and
    // slid up if it would run off the bottom. A list taller than the screen scrolls.
    lv_area_t a;
    lv_obj_get_coords(anchor, &a);
    lv_obj_update_layout(panel);
    int32_t h = lv_obj_get_height(panel);
    const int32_t maxH = BOARD_LCD_V_RES - 2 * kMargin;
    if (h > maxH)
    {
        lv_obj_set_height(panel, maxH);
        h = maxH;
    }
    int32_t x = a.x2 + 1 + kGap;
    if (x + kWidth > BOARD_LCD_H_RES - kMargin)
        x = a.x1 - kGap - kWidth;
    if (x < kMargin)
        x = kMargin;
    int32_t y = a.y1;
    if (y + h > BOARD_LCD_V_RES - kMargin)
        y = BOARD_LCD_V_RES - kMargin - h;
    if (y < kMargin)
        y = kMargin;
    lv_obj_set_pos(panel, x, y);
}
