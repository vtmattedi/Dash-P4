#pragma once

// The resting screen: after the idle timeout the panel dims and shows a large clock and the date on the left and
// the calendar's agenda on the right (today's events that have not ended, then tomorrow's; the one happening now in
// the accent colour). The corners keep what is worth seeing from across a room: anything wrong at top-left, the air
// conditioner top-right, a chosen resource and what is switched on bottom-left, and bottom-right the first task or
// alert from the feed with a button that dismisses it on this panel (see App/Calendar.h, App/Feed.h). Any other
// touch wakes it (and is swallowed, so waking never presses a button underneath); the dismiss button does not wake.

/// Build the overlay and start the idle watch. Call once with the LVGL lock held.
void Rest_begin();

bool Rest_active();
/// Go to rest immediately, regardless of the timeout. The next touch wakes it.
void Rest_now();
void Rest_wake();
/// Rest now, and when a touch wakes it (and only a touch) run `afterTouchWake` once, on the LVGL task. Any other
/// way out of rest (a critical notification, Rest_wake()) drops it.
void Rest_nowThen(void (*afterTouchWake)());
