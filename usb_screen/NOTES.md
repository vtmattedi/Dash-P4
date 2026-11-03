# usb_screen: the panel as a Windows USB monitor

A reusable IDF component. Windows sees the board as an additional display over USB 2.0 High-Speed,
with five-finger touch. It is product-agnostic (no LVGL, no NightMare). The caller hands it a panel
draw function and, optionally, a touch controller (`usb_screen.h`).

Derived from Espressif's [usb_extend_screen example](https://github.com/espressif/esp-iot-solution/tree/master/examples/usb/device/usb_extend_screen)
(esp-iot-solution, Apache-2.0; TinyUSB descriptor files are MIT). The upstream example depends on the
ESP Board Manager. Here the board comes from the caller instead.

## On the PC (Windows 10/11)

1. Install Espressif's signed driver:
   [xfz1986_usb_graphic_250224_rc_sign.exe](https://dl.espressif.com/AE/esp-iot-solution/xfz1986_usb_graphic_250224_rc_sign.exe).
   Before running it, check the signature (right-click > Properties > Digital Signatures). Do not turn
   off driver signature enforcement for it.
2. Connect the PC to the board's **high-speed** USB-C port (number 9 in Guition's spec sheet). The other
   USB-C port stays the flashing/log port.
3. *Settings > System > Display*: the new display appears; choose *Extend these displays* and place it.
4. Touch: Windows maps every new touchscreen to the **primary** monitor, so out of the box touches and
   gestures land on the main screen (seen 2026-10-24). Fix once per PC: Win+R, `MultiDigiMon.exe -touch`
   (= *Control Panel > Tablet PC Settings > Display > Setup... > Touch input*). Press Enter while the
   white prompt is on the main monitor, tap the panel when the prompt is on it. Windows keeps the mapping
   per device; the USB serial is the MAC, so it survives replugging. Redo it after a driver reinstall or
   if it flips back.

The device identifies as `303A:2986` (display + touch, composite) or `303A:2987` (display only, when
started without a touch controller). Those are the IDs the driver's INF binds to. The vendor interface
string tells the driver the panel size, JPEG quality, frame cap and buffer limit:
`esp32p4udisp0_R1024x600_Ejpg6_Fps60_Bl292`.

## How it works

| Part | File | From the example |
| --- | --- | --- |
| USB stack (TinyUSB on the P4's HS/UTMI PHY, root port 1), link state, output ownership | `usb_screen.c` | `app_usb.c` |
| Frame reception (vendor bulk OUT), frame pool, HW JPEG decode, draw | `usb_screen_video.c` | `app_vendor.c`, `usb_frame.c`, `app_lcd.c` |
| GT911 -> HID multi-touch, hand-back gesture | `usb_screen_touch.c` | `app_touch.c`, `app_hid.c` |
| Descriptors, TinyUSB config | `tusb/` | `tusb/` (audio removed) |

Changes from the example, and why:
- **Buffer limit in KB.** The driver reads `Bl<n>` as kilobytes (`blimit *= 1024` in its
  `parse_usb_dev_info`). The example sends the byte count, so its limit never takes effect.
- **Reads never cross a frame boundary.** TinyUSB 0.21 collects bulk packets in a FIFO. The example
  reads in 512-byte chunks and could swallow the next frame's header when two frames are queued. Here
  the 16-byte header is read on its own, then exactly the payload it announces.
- **Only the newest frame is decoded**, and none at all while the output is off (another owner has the
  panel). Each frame is the whole desktop, so a backlog has no value.
- **The output can be switched** (`usb_screen_set_output`) so the panel can have a second owner. Off
  returns only after the last copy and the last touch read, and on redraws the newest frame at once.
- **Hand-back gesture:** three or more fingers held for two seconds raises `USB_SCREEN_EVENT_GESTURE`
  (the fingers are lifted on the PC first). The caller decides what that means.
- **Hand-back tab** (`usb_screen_set_tab()`): a small translucent tab with a 2x2 mark painted into each
  decoded frame (once per frame, before the copy). A one-finger tap that starts and ends on it (16 px margin)
  raises `USB_SCREEN_EVENT_TAB` and is never sent to the PC. Dash-P4 puts it at the left edge, centred
  (30x96 px), and can switch it off (Settings > PC screen, pref `pc_tab`).
- **Menu on the picture** (`usb_screen_set_menu()`): with a menu set, a tap on the tab grows it into a strip of 80 px
  icon cells (Rest, Settings, Disconnect, Cancel) painted over the PC's picture; nothing reaches the PC while it is
  open. Lifting on an icon raises `USB_SCREEN_EVENT_MENU` (`usb_screen_menu_pick()`), anywhere else or 6 s idle closes
  it. Overlays are painted into the decoded buffer after saving the pixels under them, so the tab and menu change
  without waiting for a new frame from Windows.
- The HID endpoint reset uses the device's own root port (the example used port 0, the full-speed controller).
- No audio (UAC). The board's codec pins are not confirmed (`board_pins.h` `[E]`).

The decoded frame is 1024x608: libjpeg in the driver encodes 4:2:0, and the hardware decoder pads to
16-row MCUs. So it cannot decode straight into the panel's 1024x600 frame buffer. Two decode buffers
take turns, and `board_display_draw_frame()` copies the visible 600 rows with DMA2D.

## Closing the connection

`usb_screen_connect(false)` removes the device from the USB bus (a soft disconnect: the DWC2 controller's
soft-disconnect bit, so Windows sees the cable pulled and the display goes away; windows on it move to the main
monitor). `usb_screen_connect(true)` puts it back and Windows enumerates again. The link is DETACHED while closed.
Dash-P4 closes it when the PC view is left for good (Settings in the tab menu, the three-finger hold, the Close
buttons) and reopens it from the PC tab, the status bar's PC button or `PCSCREEN SHOW`. Rest keeps it open.

## The link states

`usb_screen_link()`: **detached** (no host, or the host suspended the bus: PC asleep, display off,
cable pulled) -> **connected** (enumerated, no frame yet: driver missing, or the display is disabled in
Windows) -> **streaming** (a valid frame arrived since the mount). Windows sends a frame only when the
desktop changes, so a still desktop sends nothing. Silence is not a disconnect.

## In Dash-P4

`src/PcLink/` decides who owns the panel (tab tap and gesture both hand it back; the dashboard's status bar shows a **PC** button while the PC streams but is not on the panel). It pauses LVGL (`board_lvgl_pause()`) and turns the USB
output on when the PC streams, and does the reverse when the link drops or the gesture is made. The
env `jc1060p470c_usbscreen` uses it alone, with a status screen. `jc1060p470c_usbscreen_nm` uses it
under the dashboard (`src/App/PcScreen.cpp`: Settings > PC screen, console `PCSCREEN`, resource
`pc_screen`).

## Not verified yet

Compiled only. Nothing has been seen on the board. To check on the first run:
- Enumeration at high speed, and whether port 9 alone powers the board while streaming (the panel
  draws more than 500 mA at full brightness).
- Colour order. The decoder outputs BGR-ordered RGB565, which is what the example uses for the same
  little-endian panels. If red and blue are swapped, flip `rgb_order` in `usb_screen_video.c`.
- Touch orientation against the picture, and Windows' touch-to-display mapping.
- Tearing. One panel frame buffer is copied while it scans out. A second buffer is the fix if it shows.
- Real frame rate. The serial log shows the counters; `PCSCREEN` shows them on the console.
