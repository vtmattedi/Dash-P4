#pragma once

// Shared between the parts of the USB screen: usb_screen.c (USB stack, link state, ownership),
// usb_screen_video.c (frame reception, JPEG decode, drawing), usb_screen_touch.c (touch -> HID).

#include "usb_screen.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#ifdef __cplusplus
extern "C" {
#endif

extern usb_screen_config_t g_usb_screen_cfg;

/// Held while drawing and while reading the touch controller; usb_screen_set_output(false) takes it, so
/// once it returns neither is in progress.
extern SemaphoreHandle_t g_usb_screen_own;
/// Output flag, read under g_usb_screen_own.
extern volatile bool g_usb_screen_output;
/// The hand-back tab (w == 0: none). Written under g_usb_screen_own, read under it.
extern usb_screen_rect_t g_usb_screen_tab;
/// Extra touch margin around the tab: a fingertip is wider than the mark.
#define USB_SCREEN_TAB_TOUCH_MARGIN 16

// The menu: icon cells down the left edge. Changing anything the overlay shows bumps the version, which makes the
// video task repaint it into the frame it is about to draw. All of these are read and written under g_usb_screen_own.
#define USB_SCREEN_MENU_CELL 80
#define USB_SCREEN_MENU_PAD 8
#define USB_SCREEN_MENU_GAP 8
#define USB_SCREEN_MENU_IDLE_US (6 * 1000 * 1000)
extern usb_screen_icon_t g_usb_screen_menu[USB_SCREEN_MENU_MAX];
extern int g_usb_screen_menu_count;
extern bool g_usb_screen_menu_open;
extern uint32_t g_usb_screen_overlay_version;
/// Panel rectangle and each icon cell's rectangle (valid for g_usb_screen_menu_count cells).
void usb_screen_menu_geometry(usb_screen_rect_t *panel, usb_screen_rect_t items[USB_SCREEN_MENU_MAX]);
/// Take g_usb_screen_own, set the menu open or closed, repaint. Safe from the touch task.
void usb_screen_menu_set_open(bool open);
void usb_screen_menu_note_pick(usb_screen_icon_t icon);

/// A complete, valid frame header arrived (USB task). Moves the link to STREAMING.
void usb_screen_note_frame(void);
void usb_screen_emit(usb_screen_event_t event);

esp_err_t usb_screen_video_start(void);
/// Forget a half-received frame (the host went away or came back). USB task or any task with the stream idle.
void usb_screen_video_reset_stream(void);
void usb_screen_video_stats(usb_screen_stats_t *out);
void usb_screen_video_request_redraw(void);

esp_err_t usb_screen_touch_start(void);

#ifdef __cplusplus
}
#endif
