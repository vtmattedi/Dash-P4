#pragma once
#include "lvgl.h"

// The quick-actions drawer: pulled down from the status bar (a panel at the top right, under the icons) over whatever page is showing. Tiles for the light and the air
// conditioner (each toggles the real thing and shows its state), rest (wake, on the rest screen), screen off (this panel only), PC off (the linked PC's monitors, or the PC itself to sleep) and Settings, and a brightness slider.
// A downward drag that starts in the top-right strip (90 px tall, as wide as the panel, over the status bar and the top of
// the page) pulls it out under the finger and it settles open or shut on release. Closed by a tap outside it, a swipe up on it, or by using a tile that leaves the screen.

/// Build it (hidden) on the top layer. Once, with the LVGL lock held.
void Drawer_create();
void Drawer_open();
void Drawer_close();
bool Drawer_isOpen();
