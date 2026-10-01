// The attached displays, each with a key that survives reboots and reordering.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace animelogon {

struct MonitorInfo {
    std::wstring key;       // HashKey of the monitor's device path
    std::wstring name;      // the display's friendly name, if Windows has one
    std::wstring gdiName;   // \\.\DISPLAYn
    RECT rect{};            // desktop coordinates, physical pixels
    bool primary = false;
    HMONITOR handle = nullptr;
};

// Requires a per-monitor DPI aware process for physical coordinates.
std::vector<MonitorInfo> EnumerateMonitors();

// The bounding box of every display.
RECT VirtualBounds(const std::vector<MonitorInfo> &monitors);

}  // namespace animelogon
