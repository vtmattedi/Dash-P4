#pragma once
#include <stdint.h>
#include <string>

// The notification model. Pure data and rules, no LVGL: the toast, the status-bar
// bell and the notification centre are views over it (UI/Toast, UI/NotificationCentre).
//
//   Notify()        something happened. Lands in the history and pops a toast.
//   NotifySticky()  a condition is true (transport down, door open). One entry per
//                   key, updated in place, shown as a banner until it is cleared.
//   NotifyClear()   the condition ended. The banner goes away, optionally with a
//                   one-line "resolved" notification.
//
// Threading: LVGL task only (decisions D7). Other tasks use NotifyAsync(), which
// queues for the next Notifications_tick().

enum class NotifySeverity : uint8_t
{
    Info,
    Ok,
    Warn,
    Crit
};

struct Notification
{
    uint32_t id = 0;
    NotifySeverity severity = NotifySeverity::Info;
    std::string source; // who raised it: "network", "device:Adler", "explorer" ...
    std::string title;
    std::string body;
    std::string key;     // non-empty for sticky conditions
    uint32_t atMs = 0;   // ms since boot when raised (or last changed)
    int64_t epoch = 0;   // wall clock seconds, 0 when the clock was not valid
    bool read = false;
    bool sticky = false; // a live condition
    bool active = true;  // sticky only: false once cleared
};

enum class NotifyEvent : uint8_t
{
    Added,
    Updated, // a sticky condition changed in place
    Cleared, // a sticky condition ended
    Reset    // history cleared or marked read wholesale; redraw everything
};

using NotifyListener = void (*)(const Notification &notification, NotifyEvent event);

/// Time source seams, so the model can be tested on the host. Defaults: esp_timer / time().
void Notifications_setClock(uint32_t (*nowMs)(), int64_t (*epoch)());

uint32_t Notify(NotifySeverity severity, const std::string &source, const std::string &title,
                const std::string &body = std::string());

/// Raise or update the condition `key`. Returns its id. A repeat with identical
/// severity/title/body is ignored (no toast, no unread bump).
uint32_t NotifySticky(const std::string &key, NotifySeverity severity, const std::string &source,
                      const std::string &title, const std::string &body = std::string());

/// End the condition `key`. If `resolvedTitle` is given, also raise an Ok notification.
void NotifyClear(const std::string &key, const std::string &resolvedTitle = std::string());

/// Thread-safe variant of Notify(): queue now, apply on the LVGL task.
void NotifyAsync(NotifySeverity severity, const std::string &source, const std::string &title,
                 const std::string &body = std::string());

/// Apply anything queued by NotifyAsync(). Call from an lv_timer.
void Notifications_tick();

/// History, newest first. Includes cleared sticky entries (they stay as history).
size_t Notifications_count();
const Notification &Notifications_at(size_t indexNewestFirst);
const Notification *Notifications_find(uint32_t id);

/// Currently active sticky conditions, most severe first.
size_t Notifications_activeCount();
const Notification &Notifications_activeAt(size_t index);

uint32_t Notifications_unread();
void Notifications_markRead(uint32_t id);
void Notifications_markAllRead();
void Notifications_clearHistory(); // keeps active sticky conditions

uint32_t Notifications_revision();
bool Notifications_onChange(NotifyListener listener); // up to 4 listeners

#define NOTIFICATIONS_CAPACITY 128
