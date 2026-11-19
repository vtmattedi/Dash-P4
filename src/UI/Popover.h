#pragma once
#include <functional>
#include <string>
#include <vector>

#include "Theme.h"

// A small floating list that opens next to the control that opened it (the shadcn "popover" /
// "dropdown menu"): it sits beside its anchor, on its right when there is room and on its
// left otherwise, and is kept on screen. Unlike a Modal it does not dim the page; tapping anywhere outside it
// closes it, and so does picking an entry.
//
// One popover at a time: opening another replaces the first. LVGL task only.

struct PopoverItem
{
    std::string label;
    std::string detail; // second line, dimmer
    const lv_image_dsc_t *icon = nullptr; // a picture before the text, drawn at half size
    std::string section; // a heading shown before the first item of each run with the same text ("Open", "Recent")
    std::string badge;  // a green pill on the right ("open"); empty draws none
};

/// Open a popover beside `anchor`. `onPick(index)` runs after it has closed.
void Popover_open(lv_obj_t *anchor, const std::vector<PopoverItem> &items, std::function<void(size_t)> onPick,
                  const std::string &emptyText = "Nothing here.");
void Popover_close();
bool Popover_isOpen();
