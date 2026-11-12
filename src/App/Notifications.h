#pragma once
#include <stdint.h>
#include <string>

// What is left of the notification system: one line of feedback. The history, the notification centre, the bell, the live
// banner, the toasts and the online/offline alerts are gone; "Reboot sent" and "No transport is connected" still need
// somewhere to go, so a message appears in the status bar for a few seconds. LVGL task only.

enum class NotifySeverity : uint8_t
{
    Info,
    Ok,
    Warn,
    Crit
};

/// Show `title` (and `body` after a dash) in the status bar. `source` is not used; call sites keep passing it so they read
/// the same as before.
void Notify(NotifySeverity severity, const std::string &source, const std::string &title,
            const std::string &body = std::string());
