#include "Theme.h"

#include <stdio.h>
#include <string.h>

namespace
{
lv_style_t g_card, g_plain, g_button, g_buttonPressed, g_chip;
bool g_ready = false;

void styleButtonTone(lv_obj_t *button, Tone tone)
{
    lv_color_t base;
    lv_color_t text = UI_COL_TEXT;
    switch (tone)
    {
    case Tone::Accent:
        base = UI_COL_ACCENT;
        text = lv_color_hex(0x1A1208);
        break;
    case Tone::Ok:
        base = UI_COL_OK;
        text = lv_color_hex(0x07200F);
        break;
    case Tone::Warn:
        base = UI_COL_WARN;
        text = lv_color_hex(0x2A1F00);
        break;
    case Tone::Danger:
        base = UI_COL_DANGER;
        text = lv_color_hex(0x2A0009);
        break;
    case Tone::Info:
        base = UI_COL_INFO;
        text = lv_color_hex(0x04202F);
        break;
    default:
        base = UI_COL_SURFACE_HI;
        break;
    }
    lv_obj_set_style_bg_color(button, base, 0);
    lv_obj_set_style_text_color(button, text, 0);
    lv_obj_set_style_bg_color(button, lv_color_darken(base, 40), LV_STATE_PRESSED);
}
} // namespace

lv_color_t Theme_toneColor(Tone tone)
{
    switch (tone)
    {
    case Tone::Accent:
        return UI_COL_ACCENT;
    case Tone::Ok:
        return UI_COL_OK;
    case Tone::Warn:
        return UI_COL_WARN;
    case Tone::Danger:
        return UI_COL_DANGER;
    case Tone::Info:
        return UI_COL_INFO;
    default:
        return UI_COL_TEXT_DIM;
    }
}

void Theme_init(lv_display_t *display)
{
    if (g_ready)
    {
        return;
    }
    // LVGL's default theme, dark, with our accent: gives sane defaults for switches,
    // sliders, dropdowns, keyboards and text areas that we do not restyle by hand.
    lv_theme_t *theme = lv_theme_default_init(display, UI_COL_ACCENT, UI_COL_INFO, true, UI_FONT_M);
    lv_display_set_theme(display, theme);

    lv_style_init(&g_plain);
    lv_style_set_bg_opa(&g_plain, LV_OPA_TRANSP);
    lv_style_set_border_width(&g_plain, 0);
    lv_style_set_pad_all(&g_plain, 0);
    lv_style_set_radius(&g_plain, 0);
    lv_style_set_shadow_width(&g_plain, 0);
    lv_style_set_outline_width(&g_plain, 0);

    lv_style_init(&g_card);
    lv_style_set_bg_color(&g_card, UI_COL_SURFACE);
    lv_style_set_bg_opa(&g_card, LV_OPA_COVER);
    lv_style_set_border_color(&g_card, UI_COL_BORDER);
    lv_style_set_border_width(&g_card, 1);
    lv_style_set_radius(&g_card, UI_RADIUS);
    lv_style_set_pad_all(&g_card, UI_GUTTER);
    lv_style_set_pad_gap(&g_card, 10);
    lv_style_set_shadow_width(&g_card, 0);
    lv_style_set_text_color(&g_card, UI_COL_TEXT);

    lv_style_init(&g_button);
    lv_style_set_bg_opa(&g_button, LV_OPA_COVER);
    lv_style_set_radius(&g_button, 12);
    lv_style_set_border_width(&g_button, 0);
    lv_style_set_shadow_width(&g_button, 0);
    lv_style_set_pad_all(&g_button, 8);
    lv_style_set_text_font(&g_button, UI_FONT_M);

    lv_style_init(&g_buttonPressed);

    lv_style_init(&g_chip);
    lv_style_set_radius(&g_chip, LV_RADIUS_CIRCLE);
    lv_style_set_pad_hor(&g_chip, 12);
    lv_style_set_pad_ver(&g_chip, 4);
    lv_style_set_border_width(&g_chip, 0);
    lv_style_set_text_font(&g_chip, UI_FONT_S);

    // The screen itself.
    lv_obj_t *screen = lv_display_get_screen_active(display);
    lv_obj_set_style_bg_color(screen, UI_COL_BG, 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, UI_COL_TEXT, 0);
    g_ready = true;
}

void Theme_plain(lv_obj_t *obj)
{
    lv_obj_remove_style_all(obj);
    lv_obj_add_style(obj, &g_plain, 0);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *Theme_card(lv_obj_t *parent, int32_t width, int32_t height)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_remove_style_all(card);
    lv_obj_add_style(card, &g_card, 0);
    lv_obj_set_size(card, width, height);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    return card;
}

lv_obj_t *Theme_column(lv_obj_t *parent, int32_t width, int32_t height)
{
    lv_obj_t *box = lv_obj_create(parent);
    Theme_plain(box);
    lv_obj_set_size(box, width, height);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(box, 10, 0);
    return box;
}

lv_obj_t *Theme_row(lv_obj_t *parent, int32_t width, int32_t height)
{
    lv_obj_t *box = lv_obj_create(parent);
    Theme_plain(box);
    lv_obj_set_size(box, width, height);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_gap(box, 10, 0);
    return box;
}

lv_obj_t *Theme_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t colour)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, colour, 0);
    return label;
}

lv_obj_t *Theme_button(lv_obj_t *parent, const char *text, int32_t width, int32_t height, Tone tone)
{
    lv_obj_t *button = lv_button_create(parent);
    lv_obj_remove_style_all(button);
    lv_obj_add_style(button, &g_button, 0);
    lv_obj_set_size(button, width, height);
    lv_obj_remove_flag(button, LV_OBJ_FLAG_SCROLLABLE);
    styleButtonTone(button, tone);
    lv_obj_t *label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    return button;
}

lv_obj_t *Theme_actionButton(lv_obj_t *parent, const char *text, int32_t width, int32_t height, Tone tone,
                             std::function<void()> action)
{
    lv_obj_t *button = Theme_button(parent, text, width, height, tone);
    auto *heap = new std::function<void()>(std::move(action));
    lv_obj_add_event_cb(
        button,
        [](lv_event_t *e) {
            auto *fn = static_cast<std::function<void()> *>(lv_event_get_user_data(e));
            if (fn && *fn)
            {
                (*fn)();
            }
        },
        LV_EVENT_CLICKED, heap);
    lv_obj_add_event_cb(
        button, [](lv_event_t *e) { delete static_cast<std::function<void()> *>(lv_event_get_user_data(e)); },
        LV_EVENT_DELETE, heap);
    return button;
}

lv_obj_t *Theme_iconButton(lv_obj_t *parent, const char *symbol, int32_t size, Tone tone)
{
    lv_obj_t *button = Theme_button(parent, symbol, size, size, tone);
    lv_obj_set_style_text_font(button, UI_FONT_L, 0);
    return button;
}

lv_obj_t *Theme_chip(lv_obj_t *parent)
{
    lv_obj_t *chip = lv_obj_create(parent);
    lv_obj_remove_style_all(chip);
    lv_obj_add_style(chip, &g_chip, 0);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(chip, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *label = lv_label_create(chip);
    lv_label_set_text(label, "");
    return chip;
}

void Theme_chipSet(lv_obj_t *chip, const char *text, lv_color_t colour)
{
    lv_obj_t *label = lv_obj_get_child(chip, 0);
    if (label)
    {
        Theme_setText(label, text);
    }
    lv_obj_set_style_bg_color(chip, colour, 0);
    lv_obj_set_style_bg_opa(chip, LV_OPA_20, 0);
    lv_obj_set_style_text_color(chip, colour, 0);
}

lv_obj_t *Theme_dot(lv_obj_t *parent, int32_t size, lv_color_t colour)
{
    lv_obj_t *dot = lv_obj_create(parent);
    lv_obj_remove_style_all(dot);
    lv_obj_set_size(dot, size, size);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, colour, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    return dot;
}

lv_obj_t *Theme_cardHeader(lv_obj_t *card, const char *title)
{
    lv_obj_t *row = Theme_row(card, lv_pct(100), LV_SIZE_CONTENT);
    Theme_label(row, title, UI_FONT_L, UI_COL_TEXT);
    return row;
}

void Theme_setText(lv_obj_t *label, const char *text)
{
    const char *current = lv_label_get_text(label);
    if (current && strcmp(current, text) == 0)
    {
        return;
    }
    lv_label_set_text(label, text);
}

void Theme_formatAge(uint32_t ageMs, char *out, size_t outSize)
{
    const uint32_t s = ageMs / 1000;
    if (s < 5)
    {
        snprintf(out, outSize, "just now");
    }
    else if (s < 60)
    {
        snprintf(out, outSize, "%us ago", (unsigned)s);
    }
    else if (s < 3600)
    {
        snprintf(out, outSize, "%um ago", (unsigned)(s / 60));
    }
    else if (s < 86400)
    {
        snprintf(out, outSize, "%uh ago", (unsigned)(s / 3600));
    }
    else
    {
        snprintf(out, outSize, "%ud ago", (unsigned)(s / 86400));
    }
}
