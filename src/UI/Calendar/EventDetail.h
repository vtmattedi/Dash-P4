#pragma once
#include <stdint.h>

/// Open the detail sheet of a feed item (an event, a task or an alert): when, where it came from, its
/// description, and for anything that is not an event a Dismiss button (local to this panel).
/// Does nothing when the server already removed the item. LVGL task only.
void EventDetail_open(int64_t numericId);
