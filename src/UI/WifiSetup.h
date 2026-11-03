#pragma once

// Pick the Wi-Fi network from a scan and enter its password. The result is applied to
// the running station and persisted by the library once it works. The creds.h profile
// stays the fallback when nothing is stored.

/// Scan, list, ask for the password, apply. LVGL task only.
void WifiSetup_open();
