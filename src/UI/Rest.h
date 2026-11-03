#pragma once

// The resting screen: after the idle timeout the panel dims and shows a large clock with
// the few things worth seeing from across a room in the corners -- anything wrong at
// top-left, the air conditioner top-right, a chosen resource bottom-left, what is switched
// on bottom-right. Any touch wakes it (and is swallowed, so waking never presses a button
// underneath).

/// Build the overlay and start the idle watch. Call once with the LVGL lock held.
void Rest_begin();

bool Rest_active();
/// Go to rest immediately, regardless of the timeout. The next touch wakes it.
void Rest_now();
void Rest_wake();
/// Rest now, and when a touch wakes it (and only a touch) run `afterTouchWake` once, on the LVGL task. Any other
/// way out of rest (a critical notification, Rest_wake()) drops it.
void Rest_nowThen(void (*afterTouchWake)());
