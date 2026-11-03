#pragma once

// The notification centre: a drawer listing the model's history, with live conditions
// (the sticky ones) pinned on top. Opened from the status-bar bell or the banner.

void NotificationCentre_open();
void NotificationCentre_close();
bool NotificationCentre_isOpen();

/// Keep an open centre in step with the model. Call from a periodic timer.
void NotificationCentre_refresh();
