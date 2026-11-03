#pragma once
#include <stdint.h>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "Theme.h"

// The one place that creates a scrim and a dialog. Everything that needs to interrupt the
// page -- a confirmation, a command's output, a busy spinner, a form, the device sheet,
// the notification centre -- opens a modal here, so they all look, animate and dismiss
// the same way.
//
// Modals live on LVGL's top layer, stacked in the order they were opened. Tapping the
// scrim dismisses a cancelable one. Callers keep a ModalId rather than a pointer: the
// widgets can disappear under them (the user taps away, a newer modal replaces them), and
// an id stays safe to use afterwards (it simply does nothing).
//
// LVGL task only (hold board_lvgl_lock() elsewhere).

using ModalId = uint32_t;

enum class ModalSize : uint8_t
{
    Dialog, // 560 wide, centred
    Wide,   // 780 wide, centred
    Drawer, // 440 wide, full height, from the right
    Sheet   // 800 wide, full height, from the right
};

struct ModalOptions
{
    std::string title;
    ModalSize size = ModalSize::Dialog;
    bool cancelable = true;       // scrim tap and the close button dismiss it
    bool showClose = true;        // the X in the header
    std::function<void()> onClosed; // runs after the widgets are gone, however it closed
};

/// Open a modal. `body` is a vertical, scrollable container for the content; `footer` is a
/// right-aligned button row (empty until the caller adds buttons). Either may be null.
ModalId Modal_open(const ModalOptions &options, lv_obj_t **body = nullptr, lv_obj_t **footer = nullptr);

void Modal_close(ModalId id);
void Modal_closeAll();
bool Modal_isOpen(ModalId id);
size_t Modal_openCount();

/// Replace the title of an open modal.
void Modal_setTitle(ModalId id, const std::string &title);

/// The body container of an open modal, or null.
lv_obj_t *Modal_body(ModalId id);

/// Use the shared on-screen keyboard for a text area that lives outside a modal (e.g. a
/// page's search box). Modal forms attach their own text areas automatically.
void Keyboard_attach(lv_obj_t *textarea);

// --- Ready-made kinds -----------------------------------------------------------

/// Yes/no. `onConfirm` runs when the confirm button is tapped; the modal closes first.
ModalId Modal_confirm(const std::string &title, const std::string &message,
                      const std::string &confirmLabel, Tone tone, std::function<void()> onConfirm,
                      const std::string &cancelLabel = "Cancel");

/// A message with a single button.
ModalId Modal_message(const std::string &title, const std::string &message, Tone tone = Tone::Info);

/// Scrollable pre-formatted text (command output, JSON). Returns the id; update with
/// Modal_setText().
ModalId Modal_text(const std::string &title, const std::string &text, ModalSize size = ModalSize::Wide);
void Modal_setText(ModalId id, const std::string &text);

/// A spinner and a message, with an optional Cancel. Update the message with
/// Modal_setBusyText(), end it with Modal_close().
ModalId Modal_busy(const std::string &title, const std::string &message, std::function<void()> onCancel = nullptr);
void Modal_setBusyText(ModalId id, const std::string &message);

// --- Lists ------------------------------------------------------------------------

struct ListEntry
{
    std::string label;
    std::string detail; // second line, dimmer
    Tone tone = Tone::Neutral; // colours the dot; Neutral draws none
    bool dot = true;
};

/// A scrollable list of choices. Tapping one closes the modal and calls `onPick(index)`.
ModalId Modal_list(const std::string &title, const std::vector<ListEntry> &entries,
                   std::function<void(size_t)> onPick, const std::string &emptyText = "Nothing to choose from.",
                   ModalSize size = ModalSize::Dialog);

// --- Forms ------------------------------------------------------------------------

struct FormField
{
    enum class Kind : uint8_t
    {
        Text,   // one line, on-screen keyboard
        Json,   // several lines, on-screen keyboard
        Number, // - [value] + stepper
        Switch,
        Choice, // dropdown
        Note    // read-only explanatory text
    };
    Kind kind = Kind::Text;
    std::string key;     // the name in the submitted values
    std::string label;
    std::string hint;    // placeholder / helper text
    std::string value;   // initial value as text ("true"/"false" for Switch)
    double min = 0, max = 100, step = 1; // Number
    std::vector<std::string> choices;    // Choice
    bool required = false;
};

/// key -> value as text. Switches are "true"/"false"; Numbers use the step's precision.
using FormValues = std::map<std::string, std::string>;

/// A form dialog. `onSubmit` returns an error message to show and keep the form open, or
/// an empty string to close it.
ModalId Modal_form(const std::string &title, const std::vector<FormField> &fields,
                   const std::string &submitLabel, Tone tone,
                   std::function<std::string(const FormValues &)> onSubmit);
