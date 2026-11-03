#pragma once

// Transient pop-ups for notifications, top-right, above everything including modals.
// A view over the notification model: it listens for Added/Updated and shows a card.
//
// Info/Ok disappear after 4 s, Warn after 8 s, Crit stays until tapped (30 s at most).
// At most 3 are visible; a fourth pushes the oldest out. Tapping a toast dismisses it.

/// Subscribe to the notification model and build the host container.
/// Call once, with the LVGL lock held, after the model exists.
void Toast_begin();

/// Dismiss everything now (e.g. when the panel goes to rest).
void Toast_clear();

/// Hold new toasts back (the model still records them). Used while resting.
void Toast_setSuppressed(bool suppressed);
