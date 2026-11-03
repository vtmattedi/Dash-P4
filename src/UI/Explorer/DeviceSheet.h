#pragma once
#include <string>

// The device sheet: everything the panel can show or do for one device, in a right-hand
// sheet with five tabs.
//
//   Overview   identity, hardware, firmware, health and link; refresh, reboot, delete, bind
//   Resources  every Value / Action / Event, live; tap one for its page (edit, invoke, watch)
//   Console    run console commands on the device and read the answers
//   Ping       round-trip time, N pings, min/avg/max/loss and a chart
//   Topics     the raw messages on `<device>/#`, live
//
// Opened from the Network page. Only one sheet exists at a time.

void DeviceSheet_open(const std::string &device);
void DeviceSheet_close();
bool DeviceSheet_isOpen();
