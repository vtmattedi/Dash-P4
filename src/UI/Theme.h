#pragma once
#include <functional>

#include "lvgl.h"

#include "board_pins.h"

// Shared look for every screen: dark (the panel hangs on a wall and is looked at in
// dim rooms), the NightMare orange as the one accent, big touch targets.
//
// Layout of the 1024x600 landscape screen:
//
//   +--------------------------------------------------------------+
//   | status bar                                           44 px   |
//   +------+-------------------------------------------------------+
//   | nav  |  banner (only while a condition is live)              |
//   | rail |  page                                                 |
//   | 96px |  928 x 556                                            |
//   +------+-------------------------------------------------------+

#define UI_COL_BG lv_color_hex(0x0E1116)
#define UI_COL_SURFACE lv_color_hex(0x171C24)
#define UI_COL_SURFACE_ALT lv_color_hex(0x212936)
#define UI_COL_SURFACE_HI lv_color_hex(0x2B3544)
#define UI_COL_BORDER lv_color_hex(0x2A3441)
#define UI_COL_TEXT lv_color_hex(0xE8EDF5)
#define UI_COL_TEXT_DIM lv_color_hex(0x8A97A8)
#define UI_COL_TEXT_FAINT lv_color_hex(0x5A6678)
#define UI_COL_ACCENT lv_color_hex(0xFF9849) // the NightMare mark's orange
#define UI_COL_OK lv_color_hex(0x37C978)
#define UI_COL_WARN lv_color_hex(0xFFC64B)
#define UI_COL_DANGER lv_color_hex(0xF2566A)
#define UI_COL_INFO lv_color_hex(0x4FB0F5)
#define UI_COL_SCRIM lv_color_hex(0x000000)

#define UI_STATUSBAR_H 44
#define UI_NAVRAIL_W 96
#define UI_CONTENT_W (BOARD_LCD_H_RES - UI_NAVRAIL_W)
#define UI_CONTENT_H (BOARD_LCD_V_RES - UI_STATUSBAR_H)
#define UI_GUTTER 16
#define UI_TOUCH 56 // minimum comfortable touch target
#define UI_RADIUS 14

#define UI_FONT_S (&lv_font_montserrat_14)
#define UI_FONT_M (&lv_font_montserrat_16)
#define UI_FONT_L (&lv_font_montserrat_20)
#define UI_FONT_XL (&lv_font_montserrat_28)
#define UI_FONT_XXL (&lv_font_montserrat_40)

/// Semantic colour of a control or message.
enum class Tone : uint8_t
{
    Neutral,
    Accent,
    Ok,
    Warn,
    Danger,
    Info
};

lv_color_t Theme_toneColor(Tone tone);

/// Build the shared styles and install LVGL's default theme in dark mode with our
/// accent. Call once, before creating any widget.
void Theme_init(lv_display_t *display);

/// A rounded surface with the standard border and padding. Use LV_SIZE_CONTENT or
/// lv_pct() for either dimension.
lv_obj_t *Theme_card(lv_obj_t *parent, int32_t width, int32_t height);

/// Transparent flex containers, for grouping without a card's chrome.
lv_obj_t *Theme_column(lv_obj_t *parent, int32_t width, int32_t height);
lv_obj_t *Theme_row(lv_obj_t *parent, int32_t width, int32_t height);

/// A label with a role applied.
lv_obj_t *Theme_label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t colour);

/// A flat finger-sized button. Returns the button; its label is the first child.
lv_obj_t *Theme_button(lv_obj_t *parent, const char *text, int32_t width, int32_t height,
                       Tone tone = Tone::Neutral);

/// A button whose click runs a callable (it is owned by the button and freed with it).
lv_obj_t *Theme_actionButton(lv_obj_t *parent, const char *text, int32_t width, int32_t height, Tone tone,
                             std::function<void()> action);

/// A round-cornered square button with a symbol (LV_SYMBOL_*).
lv_obj_t *Theme_iconButton(lv_obj_t *parent, const char *symbol, int32_t size, Tone tone = Tone::Neutral);

/// A small status pill. Set with Theme_chipSet().
lv_obj_t *Theme_chip(lv_obj_t *parent);
void Theme_chipSet(lv_obj_t *chip, const char *text, lv_color_t colour);

/// A little coloured dot, for online/offline and severity.
lv_obj_t *Theme_dot(lv_obj_t *parent, int32_t size, lv_color_t colour);

/// A card heading: returns the row, with `title` already in it.
lv_obj_t *Theme_cardHeader(lv_obj_t *card, const char *title);

/// Strip every default scroll/click behaviour from a layout-only container.
void Theme_plain(lv_obj_t *obj);

/// Set a label's text only when it differs. lv_label_set_text reallocates and
/// invalidates even for identical text, which is steady churn on a refresh loop.
void Theme_setText(lv_obj_t *label, const char *text);

/// "3m ago" / "2h ago" for a duration in milliseconds.
void Theme_formatAge(uint32_t ageMs, char *out, size_t outSize);
