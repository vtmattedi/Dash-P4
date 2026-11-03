#pragma once

// Who owns the panel: its own UI (LVGL), or the PC through the USB screen.
//
// The PC takes the panel when its display driver starts streaming (the first frame after the USB connection
// comes up), and gives it back when the connection goes away (cable out, PC asleep, display switched off in
// Windows), when someone taps the small tab at the left edge of the PC's picture, or holds three fingers on the
// screen for two seconds (which also closes the connection). After that the panel keeps its own UI until the PC
// connects again or PcLink_show() is called.
//
// Everything here runs on the LVGL task; call it with the LVGL lock held (or from LVGL callbacks).

#include <cstdint>
#include "esp_err.h"
#include "usb_screen.h"

struct PcLinkOptions
{
    /// Take the panel as soon as the PC streams. Off: only PcLink_show() hands it over.
    bool autoShow = true;
    /// Give the panel back after this long without a new frame, 0 = never. Windows sends frames only when the
    /// desktop changes, so a short value hands the panel back while someone is reading a still page.
    uint32_t idleReturnSec = 0;
    /// Forward touches to the PC as a touch screen.
    bool touch = true;
    /// Paint a small tab at the left edge of the PC's picture; tapping it returns to the panel's UI (needs touch).
    bool tab = true;
};

/// Start the USB screen (on the board's high-speed USB-C port) and the ownership watch. Call once, after
/// board_init(), with the LVGL lock held.
esp_err_t PcLink_begin(const PcLinkOptions &options);

usb_screen_link_t PcLink_link();
/// "detached", "connected" or "streaming".
const char *PcLink_linkName();
/// The PC owns the panel right now.
bool PcLink_showing();

/// Show the PC: puts the device back on the USB bus if it was closed, and shows the PC's picture as soon as it
/// streams (now, if it already does). It stays up until the link drops or it is hidden or closed.
void PcLink_show();
/// Back to the panel's own UI, connection kept (the PC still has its monitor). Nothing takes the panel again until
/// PcLink_show(), or the PC's next connection.
void PcLink_hide();
/// Back to the panel's own UI and off the USB bus: the PC loses the display. PcLink_show() reconnects.
void PcLink_close();
/// The device is on the USB bus (not closed). True does not mean a PC is there: see PcLink_link().
bool PcLink_connected();

/// The menu that grows out of the hand-back tab, drawn on the PC's picture: Rest, Settings, Disconnect, Cancel
/// (icons, left edge). Disconnect and Cancel are handled here (Disconnect = PcLink_close()); `handler` gets Rest and
/// Settings, with the PC still on the panel -- hide or close it as the action needs (PcLink_hide() keeps the
/// connection, PcLink_close() ends it). Without a menu the tab simply closes the session. The three-finger hold
/// always closes it.
enum class PcLinkAction
{
    Rest,
    Settings
};
void PcLink_onMenu(void (*handler)(PcLinkAction action));

void PcLink_setAuto(bool on);
bool PcLink_auto();

/// Show or hide the hand-back tab (takes effect with the PC's next frame when it is on the panel now).
void PcLink_setTab(bool on);
bool PcLink_tab();

/// Called on the LVGL task after the link state or the owner changed. Up to four listeners.
using PcLinkListener = void (*)();
void PcLink_onChange(PcLinkListener listener);
