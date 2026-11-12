#pragma once
#include "lvgl.h"

// The quick-actions drawer: pulled down from the status bar (a panel at the top right, under the icons) over whatever page is showing. Tiles for the light and the air
// conditioner (each toggles the real thing and shows its state), rest, a feed sync and Settings, and a brightness slider.
// Closed by a tap outside it, a swipe up on it, or by using a tile that leaves the screen.

/// Build it (hidden) on the top layer. Once, with the LVGL lock held.
void Drawer_create();
void Drawer_open();
void Drawer_close();
bool Drawer_isOpen();
/// The x where the panel starts. The status bar only opens the drawer for a swipe that starts at or right of it.
int Drawer_left();
