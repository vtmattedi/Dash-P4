#pragma once

// USB extended display for Windows.
//
// The PC runs Espressif's USB Extended Display driver (an Indirect Display Driver, signed build of
// xfz1986_usb_graphic). It finds us by VID/PID on the P4's high-speed USB port, reads the panel's size from
// the vendor interface string, and streams the desktop as JPEG frames over a bulk endpoint. We decode them
// with the P4's hardware JPEG engine and copy them into the panel. Touches go back to the PC as a HID
// multi-touch digitizer (five fingers).
//
// Product-agnostic: no LVGL, no NightMare. The panel may have a second owner (an LVGL UI); this component
// only draws while its output is on, and the caller decides when that is (see usb_screen_set_output()).
//
// Derived from esp-iot-solution examples/usb/device/usb_extend_screen (Apache-2.0), see NOTES.md.

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_lcd_types.h"
#include "esp_lcd_touch.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    USB_SCREEN_DETACHED = 0, ///< no host: cable out, or the host suspended the bus (sleep, display power-off)
    USB_SCREEN_MOUNTED,      ///< enumerated, but no frame since: driver missing, or the display is off in Windows
    USB_SCREEN_STREAMING,    ///< at least one valid frame since the mount
} usb_screen_link_t;

typedef enum {
    USB_SCREEN_EVENT_LINK,    ///< usb_screen_link() changed
    USB_SCREEN_EVENT_GESTURE, ///< the hand-back gesture: three or more fingers held for two seconds (output on only)
    USB_SCREEN_EVENT_TAB,     ///< the hand-back tab was tapped and there is no menu (output on only; see usb_screen_set_tab())
    USB_SCREEN_EVENT_MENU,    ///< a menu icon was tapped: usb_screen_menu_pick() says which (see usb_screen_set_menu())
} usb_screen_event_t;

typedef enum {
    USB_SCREEN_ICON_REST = 1,
    USB_SCREEN_ICON_SETTINGS,
    USB_SCREEN_ICON_DISCONNECT,
    USB_SCREEN_ICON_CANCEL, ///< closes the menu and raises nothing
} usb_screen_icon_t;
#define USB_SCREEN_MENU_MAX 4

typedef struct {
    uint16_t x, y, w, h;
} usb_screen_rect_t;

/// Called from the USB or touch task. Keep it short; post to your own task for anything real.
typedef void (*usb_screen_event_cb_t)(usb_screen_event_t event, void *ctx);

/// Draw a packed full-screen RGB565 image (width x height). It may return while the copy is still running
/// (a DMA copy), provided the next call -- and draw_idle -- first waits for it. board_display_draw_frame()
/// and board_display_wait_idle() are the pair for the JC1060P470C.
typedef esp_err_t (*usb_screen_draw_fn_t)(const void *rgb565, uint16_t width, uint16_t height);
/// Wait until no draw is reading a buffer (so another owner may use the panel).
typedef esp_err_t (*usb_screen_draw_idle_fn_t)(void);

typedef struct {
    uint16_t width;               ///< panel size in pixels, landscape (the driver does not do portrait)
    uint16_t height;
    usb_screen_draw_fn_t draw;    ///< required
    usb_screen_draw_idle_fn_t draw_idle; ///< required
    esp_lcd_touch_handle_t touch; ///< NULL: display only (product id 0x2987, no HID interface)
    uint8_t jpeg_quality;         ///< 1..10, what the driver encodes with (it lowers it for frames over frame_limit)
    uint8_t max_fps;              ///< the driver's frame cap
    uint32_t frame_limit;         ///< bytes: largest compressed frame accepted; sent to the driver in KB
    const char *serial;           ///< USB serial string; NULL: the MAC address
    usb_screen_event_cb_t on_event;
    void *event_ctx;
} usb_screen_config_t;

// Defaults of the reference example on the P4 (1024x600 at 60 fps): quality 6, frames up to ~293 KB
// (a busy 1024x600 desktop at quality 6 is well under it; the driver re-encodes a larger one at lower quality).
#define USB_SCREEN_CONFIG_DEFAULT() \
    {                               \
        .jpeg_quality = 6,          \
        .max_fps = 60,              \
        .frame_limit = 300000,      \
    }

/// Start the USB device on the high-speed port. The output starts off. Call once.
esp_err_t usb_screen_start(const usb_screen_config_t *cfg);

/// Present the device to the host (true, the state after usb_screen_start) or remove it from the bus (false:
/// a soft disconnect, the host sees the cable pulled and its display disappears). Reconnecting makes the host
/// enumerate again. Safe from any task. While disconnected the link is USB_SCREEN_DETACHED.
void usb_screen_connect(bool on);
bool usb_screen_connected(void);

usb_screen_link_t usb_screen_link(void);
const char *usb_screen_link_name(usb_screen_link_t link);

/// Output on: frames are decoded and drawn, and touches go to the host. Off: frames are still received, the
/// newest one is kept (and decoded and drawn the moment the output comes back), and the touch controller is
/// left alone. Turning it off returns only once no draw and no touch read is in progress, so the caller may
/// hand the panel and the touch controller to someone else right after. Safe from any task.
void usb_screen_set_output(bool on);
bool usb_screen_output(void);

/// Draw the latest decoded frame again (after another owner drew over the panel). No-op with the output off.
void usb_screen_redraw(void);

/// A small tab painted over the host's picture (dark, with a four-square "dashboard" mark) that the user taps to
/// leave the host: a single-finger tap that starts and ends on it (plus a finger's width around it) raises
/// USB_SCREEN_EVENT_TAB and is never sent to the host. NULL removes it; frames already decoded keep it until the
/// host sends the next one. Safe from any task.
void usb_screen_set_tab(const usb_screen_rect_t *tab);

/// With a menu set, a tap on the tab opens it instead of raising USB_SCREEN_EVENT_TAB: a strip of icon buttons
/// painted over the host's picture at the left edge (80 px squares, `count` <= USB_SCREEN_MENU_MAX, in order,
/// top to bottom). While it is open no touch reaches the host. Tapping an icon closes it and raises
/// USB_SCREEN_EVENT_MENU (CANCEL only closes it); a tap anywhere else, or 6 s without touch, closes it too.
/// count 0 removes the menu. Needs a tab and a touch controller.
void usb_screen_set_menu(const usb_screen_icon_t *items, int count);
/// The icon behind the last USB_SCREEN_EVENT_MENU.
usb_screen_icon_t usb_screen_menu_pick(void);

typedef struct {
    uint32_t received;     ///< frames that arrived complete
    uint32_t decoded;
    uint32_t drawn;
    uint32_t dropped;      ///< no free buffer, oversized, wrong area, unsupported type
    uint32_t errors;       ///< JPEG or draw failures
    float fps;             ///< drawn frames per second, recent
    int64_t last_frame_us; ///< esp_timer time of the last complete frame; 0 = none since start
} usb_screen_stats_t;

void usb_screen_get_stats(usb_screen_stats_t *out);

#ifdef __cplusplus
}
#endif
