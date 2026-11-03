#include "Modal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <memory>

namespace
{
struct Modal
{
    ModalId id = 0;
    lv_obj_t *scrim = nullptr;
    lv_obj_t *panel = nullptr;
    lv_obj_t *title = nullptr;
    lv_obj_t *body = nullptr;
    lv_obj_t *footer = nullptr;
    lv_obj_t *aux = nullptr; // kind-specific label (text area, busy message, form error)
    ModalSize size = ModalSize::Dialog;
    bool cancelable = true;
    std::function<void()> onClosed;
    std::function<void()> onCancel;       // busy: Cancel pressed
    std::function<std::string(const FormValues &)> onSubmit;
    std::vector<FormField> fields;
    std::vector<lv_obj_t *> inputs;       // one per field (null for notes)
    bool closing = false;
};

std::vector<Modal *> g_modals; // bottom to top
ModalId g_nextId = 1;
lv_obj_t *g_keyboard = nullptr;
Modal *g_keyboardOwner = nullptr;

Modal *find(ModalId id)
{
    for (Modal *m : g_modals)
    {
        if (m->id == id && !m->closing)
        {
            return m;
        }
    }
    return nullptr;
}

Modal *fromObj(lv_obj_t *obj)
{
    for (Modal *m : g_modals)
    {
        for (lv_obj_t *o = obj; o; o = lv_obj_get_parent(o))
        {
            if (o == m->scrim)
            {
                return m;
            }
        }
    }
    return nullptr;
}

bool isSide(ModalSize s) { return s == ModalSize::Drawer || s == ModalSize::Sheet; }

int32_t widthFor(ModalSize s)
{
    switch (s)
    {
    case ModalSize::Wide:
        return 780;
    case ModalSize::Drawer:
        return 440;
    case ModalSize::Sheet:
        return 800;
    default:
        return 560;
    }
}

// --- Keyboard ---------------------------------------------------------------------

void hideKeyboard()
{
    if (g_keyboard)
    {
        lv_obj_add_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);
        lv_keyboard_set_textarea(g_keyboard, nullptr);
    }
    if (g_keyboardOwner && !g_keyboardOwner->closing && g_keyboardOwner->panel && !isSide(g_keyboardOwner->size))
    {
        lv_obj_align(g_keyboardOwner->panel, LV_ALIGN_CENTER, 0, 0);
    }
    g_keyboardOwner = nullptr;
}

void onKeyboardEvent(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL)
    {
        hideKeyboard();
    }
}

void ensureKeyboard()
{
    if (g_keyboard)
    {
        return;
    }
    g_keyboard = lv_keyboard_create(lv_layer_top());
    lv_obj_set_size(g_keyboard, BOARD_LCD_H_RES, 250);
    lv_obj_align(g_keyboard, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_event_cb(g_keyboard, onKeyboardEvent, LV_EVENT_ALL, nullptr);
    lv_obj_add_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);
}

void onTextFocus(lv_event_t *e)
{
    lv_obj_t *ta = lv_event_get_target_obj(e);
    Modal *m = fromObj(ta); // null for a text area outside any modal
    ensureKeyboard();
    lv_keyboard_set_textarea(g_keyboard, ta);
    lv_obj_remove_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(g_keyboard);
    g_keyboardOwner = m;
    if (m && !isSide(m->size))
    {
        // Lift the dialog into the part of the screen the keyboard leaves free.
        lv_obj_align(m->panel, LV_ALIGN_TOP_MID, 0, 6);
    }
    lv_obj_scroll_to_view(ta, LV_ANIM_ON);
}

void onTextDefocus(lv_event_t *e)
{
    (void)e;
    if (g_keyboard && lv_keyboard_get_textarea(g_keyboard) == lv_event_get_target_obj(e))
    {
        hideKeyboard();
    }
}

// --- Open / close -----------------------------------------------------------------

void destroy(Modal *m)
{
    if (m->closing)
    {
        return;
    }
    m->closing = true;
    if (g_keyboardOwner == m)
    {
        hideKeyboard();
    }
    std::function<void()> after = std::move(m->onClosed);
    lv_obj_t *scrim = m->scrim;
    for (size_t i = 0; i < g_modals.size(); ++i)
    {
        if (g_modals[i] == m)
        {
            g_modals.erase(g_modals.begin() + i);
            break;
        }
    }
    delete m;
    if (scrim)
    {
        lv_obj_delete(scrim);
    }
    if (after)
    {
        after();
    }
}

void onScrimClick(lv_event_t *e)
{
    lv_obj_t *target = lv_event_get_target_obj(e);
    Modal *m = static_cast<Modal *>(lv_event_get_user_data(e));
    // Only a tap on the scrim itself; a tap inside the panel is not a dismissal.
    if (m && target == m->scrim && m->cancelable && !m->closing)
    {
        if (g_keyboardOwner == m && g_keyboard && !lv_obj_has_flag(g_keyboard, LV_OBJ_FLAG_HIDDEN))
        {
            hideKeyboard();
            return;
        }
        destroy(m);
    }
}

void onCloseClick(lv_event_t *e)
{
    Modal *m = static_cast<Modal *>(lv_event_get_user_data(e));
    if (m)
    {
        destroy(m);
    }
}

void animTranslateX(void *obj, int32_t v) { lv_obj_set_style_translate_x(static_cast<lv_obj_t *>(obj), v, 0); }
void animTranslateY(void *obj, int32_t v) { lv_obj_set_style_translate_y(static_cast<lv_obj_t *>(obj), v, 0); }
// Fades only the scrim's own dimming. Animating the object's opacity instead would make LVGL render the whole modal (panel,
// text, every row) into an offscreen ARGB layer on every frame of the animation, which is very slow on this display.
void animScrimDim(void *obj, int32_t v) { lv_obj_set_style_bg_opa(static_cast<lv_obj_t *>(obj), static_cast<lv_opa_t>(v), 0); }

void animate(lv_obj_t *obj, lv_anim_exec_xcb_t cb, int32_t from, int32_t to, uint32_t ms)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, obj);
    lv_anim_set_exec_cb(&a, cb);
    lv_anim_set_values(&a, from, to);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}
} // namespace

void Keyboard_attach(lv_obj_t *textarea)
{
    lv_obj_add_event_cb(textarea, onTextFocus, LV_EVENT_FOCUSED, nullptr);
    lv_obj_add_event_cb(textarea, onTextDefocus, LV_EVENT_DEFOCUSED, nullptr);
}

ModalId Modal_open(const ModalOptions &options, lv_obj_t **bodyOut, lv_obj_t **footerOut)
{
    Modal *m = new Modal();
    m->id = g_nextId++;
    m->size = options.size;
    m->cancelable = options.cancelable;
    m->onClosed = options.onClosed;

    // Scrim: covers the whole screen, swallows touches, dims what is behind.
    m->scrim = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(m->scrim);
    lv_obj_set_size(m->scrim, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_set_style_bg_color(m->scrim, UI_COL_SCRIM, 0);
    lv_obj_set_style_bg_opa(m->scrim, LV_OPA_60, 0);
    lv_obj_remove_flag(m->scrim, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(m->scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(m->scrim, onScrimClick, LV_EVENT_CLICKED, m);

    // Panel.
    const bool side = isSide(m->size);
    m->panel = lv_obj_create(m->scrim);
    lv_obj_remove_style_all(m->panel);
    lv_obj_set_style_bg_color(m->panel, UI_COL_SURFACE, 0);
    lv_obj_set_style_bg_opa(m->panel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(m->panel, UI_COL_BORDER, 0);
    lv_obj_set_style_border_width(m->panel, 1, 0);
    lv_obj_set_style_radius(m->panel, side ? 0 : UI_RADIUS + 4, 0);
    lv_obj_set_style_pad_all(m->panel, 0, 0);
    lv_obj_set_style_text_color(m->panel, UI_COL_TEXT, 0);
    lv_obj_remove_flag(m->panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(m->panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_width(m->panel, widthFor(m->size));
    if (side)
    {
        lv_obj_set_height(m->panel, BOARD_LCD_V_RES);
        lv_obj_align(m->panel, LV_ALIGN_TOP_RIGHT, 0, 0);
    }
    else
    {
        lv_obj_set_height(m->panel, LV_SIZE_CONTENT);
        lv_obj_set_style_max_height(m->panel, BOARD_LCD_V_RES - 60, 0);
        lv_obj_align(m->panel, LV_ALIGN_CENTER, 0, 0);
    }

    // Header: title and close.
    lv_obj_t *header = lv_obj_create(m->panel);
    Theme_plain(header);
    lv_obj_set_size(header, lv_pct(100), 64);
    lv_obj_set_style_pad_hor(header, 20, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_color(header, UI_COL_BORDER, 0);
    m->title = Theme_label(header, options.title.c_str(), UI_FONT_L, UI_COL_TEXT);
    lv_label_set_long_mode(m->title, LV_LABEL_LONG_DOT);
    lv_obj_set_flex_grow(m->title, 1);
    if (options.showClose)
    {
        lv_obj_t *close = Theme_iconButton(header, LV_SYMBOL_CLOSE, 44, Tone::Neutral);
        lv_obj_add_event_cb(close, onCloseClick, LV_EVENT_CLICKED, m);
    }

    // Body: grows, scrolls.
    m->body = lv_obj_create(m->panel);
    Theme_plain(m->body);
    lv_obj_add_flag(m->body, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(m->body, LV_DIR_VER);
    lv_obj_set_width(m->body, lv_pct(100));
    lv_obj_set_height(m->body, side ? LV_SIZE_CONTENT : LV_SIZE_CONTENT);
    if (side)
    {
        lv_obj_set_flex_grow(m->body, 1);
    }
    lv_obj_set_style_pad_all(m->body, 20, 0);
    lv_obj_set_style_pad_gap(m->body, 12, 0);
    lv_obj_set_flex_flow(m->body, LV_FLEX_FLOW_COLUMN);

    // Footer: buttons, right aligned.
    m->footer = lv_obj_create(m->panel);
    Theme_plain(m->footer);
    lv_obj_set_size(m->footer, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(m->footer, 14, 0);
    lv_obj_set_style_pad_gap(m->footer, 12, 0);
    lv_obj_set_flex_flow(m->footer, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(m->footer, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    g_modals.push_back(m);
    lv_obj_move_foreground(m->scrim);

    // Entrance.
    animate(m->scrim, animScrimDim, 0, LV_OPA_60, 160);
    if (side)
    {
        animate(m->panel, animTranslateX, widthFor(m->size) / 3, 0, 220);
    }
    else
    {
        animate(m->panel, animTranslateY, 28, 0, 180);
    }

    if (bodyOut)
    {
        *bodyOut = m->body;
    }
    if (footerOut)
    {
        *footerOut = m->footer;
    }
    return m->id;
}

void Modal_close(ModalId id)
{
    if (Modal *m = find(id))
    {
        destroy(m);
    }
}

void Modal_closeAll()
{
    while (!g_modals.empty())
    {
        destroy(g_modals.back());
    }
}

bool Modal_isOpen(ModalId id) { return find(id) != nullptr; }
size_t Modal_openCount() { return g_modals.size(); }

void Modal_setTitle(ModalId id, const std::string &title)
{
    if (Modal *m = find(id))
    {
        Theme_setText(m->title, title.c_str());
    }
}

lv_obj_t *Modal_body(ModalId id)
{
    Modal *m = find(id);
    return m ? m->body : nullptr;
}

// --- Ready-made kinds -----------------------------------------------------------------

namespace
{
struct ButtonCtx
{
    ModalId id;
    std::function<void()> action; // runs after the modal closes
    bool close;
};

void onButton(lv_event_t *e)
{
    ButtonCtx *ctx = static_cast<ButtonCtx *>(lv_event_get_user_data(e));
    if (!ctx)
    {
        return;
    }
    const ModalId id = ctx->id;
    std::function<void()> action = ctx->action;
    const bool close = ctx->close;
    if (close)
    {
        Modal_close(id); // frees ctx through the button's delete handler
    }
    if (action)
    {
        action();
    }
}

void onButtonDeleted(lv_event_t *e)
{
    delete static_cast<ButtonCtx *>(lv_event_get_user_data(e));
}

lv_obj_t *footerButton(lv_obj_t *footer, ModalId id, const std::string &text, Tone tone,
                       std::function<void()> action, bool close = true, int32_t width = 150)
{
    lv_obj_t *button = Theme_button(footer, text.c_str(), width, 52, tone);
    ButtonCtx *ctx = new ButtonCtx{id, std::move(action), close};
    lv_obj_add_event_cb(button, onButton, LV_EVENT_CLICKED, ctx);
    lv_obj_add_event_cb(button, onButtonDeleted, LV_EVENT_DELETE, ctx);
    return button;
}

lv_obj_t *bodyText(lv_obj_t *body, const std::string &text, lv_color_t colour = UI_COL_TEXT)
{
    lv_obj_t *label = Theme_label(body, text.c_str(), UI_FONT_M, colour);
    lv_obj_set_width(label, lv_pct(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    return label;
}
} // namespace

ModalId Modal_confirm(const std::string &title, const std::string &message, const std::string &confirmLabel,
                      Tone tone, std::function<void()> onConfirm, const std::string &cancelLabel)
{
    lv_obj_t *body, *footer;
    ModalOptions options;
    options.title = title;
    const ModalId id = Modal_open(options, &body, &footer);
    bodyText(body, message);
    footerButton(footer, id, cancelLabel, Tone::Neutral, nullptr);
    footerButton(footer, id, confirmLabel, tone == Tone::Neutral ? Tone::Accent : tone, std::move(onConfirm));
    return id;
}

ModalId Modal_message(const std::string &title, const std::string &message, Tone tone)
{
    lv_obj_t *body, *footer;
    ModalOptions options;
    options.title = title;
    const ModalId id = Modal_open(options, &body, &footer);
    bodyText(body, message);
    footerButton(footer, id, "OK", tone == Tone::Neutral ? Tone::Accent : tone, nullptr);
    return id;
}

ModalId Modal_text(const std::string &title, const std::string &text, ModalSize size)
{
    lv_obj_t *body, *footer;
    ModalOptions options;
    options.title = title;
    options.size = size;
    const ModalId id = Modal_open(options, &body, &footer);
    if (Modal *m = find(id))
    {
        // A recessed block with the output in it.
        lv_obj_t *block = Theme_card(body, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_bg_color(block, UI_COL_BG, 0);
        lv_obj_set_style_pad_all(block, 14, 0);
        m->aux = Theme_label(block, text.c_str(), UI_FONT_S, UI_COL_TEXT);
        lv_obj_set_width(m->aux, lv_pct(100));
        lv_label_set_long_mode(m->aux, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_max_height(m->panel, BOARD_LCD_V_RES - 60, 0);
    }
    footerButton(footer, id, "Close", Tone::Neutral, nullptr);
    return id;
}

void Modal_setText(ModalId id, const std::string &text)
{
    if (Modal *m = find(id))
    {
        if (m->aux)
        {
            Theme_setText(m->aux, text.c_str());
        }
    }
}

ModalId Modal_busy(const std::string &title, const std::string &message, std::function<void()> onCancel)
{
    lv_obj_t *body, *footer;
    ModalOptions options;
    options.title = title;
    options.cancelable = false;
    options.showClose = false;
    const ModalId id = Modal_open(options, &body, &footer);
    if (Modal *m = find(id))
    {
        lv_obj_t *row = Theme_row(body, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_t *spinner = lv_spinner_create(row);
        lv_obj_set_size(spinner, 44, 44);
        lv_obj_set_style_arc_color(spinner, UI_COL_ACCENT, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(spinner, UI_COL_SURFACE_HI, LV_PART_MAIN);
        lv_obj_set_style_arc_width(spinner, 5, LV_PART_INDICATOR);
        lv_obj_set_style_arc_width(spinner, 5, LV_PART_MAIN);
        m->aux = Theme_label(row, message.c_str(), UI_FONT_M, UI_COL_TEXT);
        lv_obj_set_flex_grow(m->aux, 1);
        lv_label_set_long_mode(m->aux, LV_LABEL_LONG_WRAP);
    }
    if (onCancel)
    {
        footerButton(footer, id, "Cancel", Tone::Neutral, std::move(onCancel));
    }
    return id;
}

void Modal_setBusyText(ModalId id, const std::string &message)
{
    Modal_setText(id, message); // same slot: the aux label
}

// --- Lists ------------------------------------------------------------------------------

namespace
{
struct PickCtx
{
    ModalId id;
    size_t index;
    std::shared_ptr<std::function<void(size_t)>> onPick;
};

void onPickClick(lv_event_t *e)
{
    PickCtx *ctx = static_cast<PickCtx *>(lv_event_get_user_data(e));
    const ModalId id = ctx->id;
    const size_t index = ctx->index;
    std::shared_ptr<std::function<void(size_t)>> cb = ctx->onPick; // survives the widgets
    Modal_close(id);
    if (cb && *cb)
    {
        (*cb)(index);
    }
}

void onPickDeleted(lv_event_t *e) { delete static_cast<PickCtx *>(lv_event_get_user_data(e)); }
} // namespace

ModalId Modal_list(const std::string &title, const std::vector<ListEntry> &entries,
                   std::function<void(size_t)> onPick, const std::string &emptyText, ModalSize size)
{
    lv_obj_t *body, *footer;
    ModalOptions options;
    options.title = title;
    options.size = size;
    const ModalId id = Modal_open(options, &body, &footer);
    lv_obj_set_style_pad_gap(body, 8, 0);
    if (size == ModalSize::Dialog || size == ModalSize::Wide)
    {
        lv_obj_set_style_max_height(body, BOARD_LCD_V_RES - 200, 0);
    }
    auto shared = std::make_shared<std::function<void(size_t)>>(std::move(onPick));
    for (size_t i = 0; i < entries.size(); ++i)
    {
        const ListEntry &entry = entries[i];
        lv_obj_t *row = lv_obj_create(body);
        lv_obj_remove_style_all(row);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_min_height(row, 60, 0);
        lv_obj_set_style_bg_color(row, UI_COL_SURFACE_ALT, 0);
        lv_obj_set_style_bg_color(row, UI_COL_SURFACE_HI, LV_STATE_PRESSED);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(row, 12, 0);
        lv_obj_set_style_pad_hor(row, 16, 0);
        lv_obj_set_style_pad_ver(row, 8, 0);
        lv_obj_set_style_pad_gap(row, 14, 0);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(row, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        if (entry.dot)
        {
            Theme_dot(row, 12, entry.tone == Tone::Neutral ? UI_COL_TEXT_FAINT : Theme_toneColor(entry.tone));
        }
        lv_obj_t *text = Theme_column(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
        lv_obj_set_flex_grow(text, 1);
        lv_obj_set_style_pad_gap(text, 2, 0);
        lv_obj_t *label = Theme_label(text, entry.label.c_str(), UI_FONT_M, UI_COL_TEXT);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
        lv_obj_set_width(label, lv_pct(100));
        if (!entry.detail.empty())
        {
            lv_obj_t *detail = Theme_label(text, entry.detail.c_str(), UI_FONT_S, UI_COL_TEXT_DIM);
            lv_label_set_long_mode(detail, LV_LABEL_LONG_DOT);
            lv_obj_set_width(detail, lv_pct(100));
        }
        PickCtx *ctx = new PickCtx{id, i, shared};
        lv_obj_add_event_cb(row, onPickClick, LV_EVENT_CLICKED, ctx);
        lv_obj_add_event_cb(row, onPickDeleted, LV_EVENT_DELETE, ctx);
    }
    if (entries.empty())
    {
        bodyText(body, emptyText, UI_COL_TEXT_DIM);
    }
    footerButton(footer, id, "Cancel", Tone::Neutral, nullptr);
    return id;
}

// --- Forms -------------------------------------------------------------------------------

namespace
{
int decimalsFor(double step)
{
    int d = 0;
    double s = step;
    while (d < 4 && s - static_cast<long>(s) > 1e-9)
    {
        s *= 10;
        ++d;
    }
    return d;
}

std::string formatNumber(double v, double step)
{
    char buf[40];
    snprintf(buf, sizeof(buf), "%.*f", decimalsFor(step), v);
    return buf;
}

struct StepCtx
{
    lv_obj_t *label;
    double min, max, step;
    int direction;
};

void onStep(lv_event_t *e)
{
    StepCtx *ctx = static_cast<StepCtx *>(lv_event_get_user_data(e));
    double v = atof(lv_label_get_text(ctx->label));
    v += ctx->direction * ctx->step;
    if (v < ctx->min)
    {
        v = ctx->min;
    }
    if (v > ctx->max)
    {
        v = ctx->max;
    }
    lv_label_set_text(ctx->label, formatNumber(v, ctx->step).c_str());
}

void onStepDeleted(lv_event_t *e) { delete static_cast<StepCtx *>(lv_event_get_user_data(e)); }

void onSubmitClick(lv_event_t *e)
{
    Modal *m = static_cast<Modal *>(lv_event_get_user_data(e));
    if (!m || m->closing || !m->onSubmit)
    {
        return;
    }
    FormValues values;
    std::string problem;
    for (size_t i = 0; i < m->fields.size(); ++i)
    {
        const FormField &f = m->fields[i];
        lv_obj_t *input = m->inputs[i];
        if (!input || f.kind == FormField::Kind::Note)
        {
            continue;
        }
        std::string v;
        switch (f.kind)
        {
        case FormField::Kind::Text:
        case FormField::Kind::Json:
            v = lv_textarea_get_text(input);
            break;
        case FormField::Kind::Number:
            v = lv_label_get_text(input);
            break;
        case FormField::Kind::Switch:
            v = lv_obj_has_state(input, LV_STATE_CHECKED) ? "true" : "false";
            break;
        case FormField::Kind::Choice:
        {
            char buf[96];
            lv_dropdown_get_selected_str(input, buf, sizeof(buf));
            v = buf;
            break;
        }
        default:
            break;
        }
        if (f.required && v.empty() && problem.empty())
        {
            problem = f.label + " is required";
        }
        values[f.key] = v;
    }
    if (problem.empty())
    {
        problem = m->onSubmit(values);
    }
    if (problem.empty())
    {
        destroy(m);
    }
    else if (m->aux)
    {
        Theme_setText(m->aux, problem.c_str());
        lv_obj_remove_flag(m->aux, LV_OBJ_FLAG_HIDDEN);
    }
}
} // namespace

ModalId Modal_form(const std::string &title, const std::vector<FormField> &fields, const std::string &submitLabel,
                   Tone tone, std::function<std::string(const FormValues &)> onSubmit)
{
    lv_obj_t *body, *footer;
    ModalOptions options;
    options.title = title;
    const ModalId id = Modal_open(options, &body, &footer);
    Modal *m = find(id);
    if (!m)
    {
        return id;
    }
    m->fields = fields;
    m->onSubmit = std::move(onSubmit);
    m->inputs.assign(fields.size(), nullptr);

    for (size_t i = 0; i < fields.size(); ++i)
    {
        const FormField &f = fields[i];
        if (f.kind == FormField::Kind::Note)
        {
            bodyText(body, f.label.empty() ? f.value : f.label, UI_COL_TEXT_DIM);
            continue;
        }
        lv_obj_t *group = Theme_column(body, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_gap(group, 6, 0);
        if (!f.label.empty())
        {
            Theme_label(group, (f.label + (f.required ? " *" : "")).c_str(), UI_FONT_S, UI_COL_TEXT_DIM);
        }

        switch (f.kind)
        {
        case FormField::Kind::Text:
        case FormField::Kind::Json:
        {
            lv_obj_t *ta = lv_textarea_create(group);
            lv_obj_set_width(ta, lv_pct(100));
            lv_textarea_set_one_line(ta, f.kind == FormField::Kind::Text);
            if (f.kind == FormField::Kind::Json)
            {
                lv_obj_set_height(ta, 130);
            }
            lv_textarea_set_text(ta, f.value.c_str());
            lv_textarea_set_placeholder_text(ta, f.hint.c_str());
            lv_obj_set_style_text_font(ta, UI_FONT_M, 0);
            lv_obj_add_event_cb(ta, onTextFocus, LV_EVENT_FOCUSED, nullptr);
            lv_obj_add_event_cb(ta, onTextDefocus, LV_EVENT_DEFOCUSED, nullptr);
            m->inputs[i] = ta;
            break;
        }
        case FormField::Kind::Number:
        {
            lv_obj_t *row = Theme_row(group, lv_pct(100), 56);
            lv_obj_t *minus = Theme_button(row, LV_SYMBOL_MINUS, 56, 56, Tone::Neutral);
            lv_obj_t *value = Theme_label(row, "", UI_FONT_XL, UI_COL_TEXT);
            lv_obj_set_flex_grow(value, 1);
            lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_t *plus = Theme_button(row, LV_SYMBOL_PLUS, 56, 56, Tone::Neutral);
            double v = f.value.empty() ? f.min : atof(f.value.c_str());
            lv_label_set_text(value, formatNumber(v, f.step).c_str());
            StepCtx *down = new StepCtx{value, f.min, f.max, f.step, -1};
            StepCtx *up = new StepCtx{value, f.min, f.max, f.step, +1};
            lv_obj_add_event_cb(minus, onStep, LV_EVENT_CLICKED, down);
            lv_obj_add_event_cb(minus, onStepDeleted, LV_EVENT_DELETE, down);
            lv_obj_add_event_cb(plus, onStep, LV_EVENT_CLICKED, up);
            lv_obj_add_event_cb(plus, onStepDeleted, LV_EVENT_DELETE, up);
            m->inputs[i] = value;
            break;
        }
        case FormField::Kind::Switch:
        {
            lv_obj_t *sw = lv_switch_create(group);
            lv_obj_set_size(sw, 80, 44);
            if (f.value == "true" || f.value == "1")
            {
                lv_obj_add_state(sw, LV_STATE_CHECKED);
            }
            m->inputs[i] = sw;
            break;
        }
        case FormField::Kind::Choice:
        {
            lv_obj_t *dd = lv_dropdown_create(group);
            lv_obj_set_width(dd, lv_pct(100));
            lv_obj_set_height(dd, 52);
            std::string options_text;
            int selected = 0;
            for (size_t c = 0; c < f.choices.size(); ++c)
            {
                options_text += (c ? "\n" : "") + f.choices[c];
                if (f.choices[c] == f.value)
                {
                    selected = static_cast<int>(c);
                }
            }
            lv_dropdown_set_options(dd, options_text.c_str());
            lv_dropdown_set_selected(dd, selected);
            lv_obj_set_style_text_font(dd, UI_FONT_M, 0);
            m->inputs[i] = dd;
            break;
        }
        default:
            break;
        }
    }

    // Validation / error message, hidden until needed.
    m->aux = Theme_label(body, "", UI_FONT_M, UI_COL_DANGER);
    lv_obj_set_width(m->aux, lv_pct(100));
    lv_label_set_long_mode(m->aux, LV_LABEL_LONG_WRAP);
    lv_obj_add_flag(m->aux, LV_OBJ_FLAG_HIDDEN);

    footerButton(footer, id, "Cancel", Tone::Neutral, nullptr);
    lv_obj_t *submit = Theme_button(footer, submitLabel.c_str(), 170, 52, tone == Tone::Neutral ? Tone::Accent : tone);
    lv_obj_add_event_cb(submit, onSubmitClick, LV_EVENT_CLICKED, m);
    return id;
}
