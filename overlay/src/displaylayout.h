// The arrangement of the displays, for noticing that it changed: every display's rectangle, the
// primary first and then by position (the order EnumerateMonitors gives). Some changes -- a
// display that powers down, a dock -- post no WM_DISPLAYCHANGE, so the overlay and the other
// displays' stills compare it every half second as well.
#pragma once

#include <windows.h>

#include <algorithm>
#include <utility>
#include <vector>

namespace displaylayout {

struct Display {
    RECT rect{};
    bool primary = false;
};

// The rectangles, the primary first, then left to right, then top to bottom.
inline std::vector<RECT> Ordered(std::vector<Display> displays) {
    std::stable_sort(displays.begin(), displays.end(), [](const Display &a, const Display &b) {
        if (a.primary != b.primary) return a.primary;
        return a.rect.left != b.rect.left ? a.rect.left < b.rect.left : a.rect.top < b.rect.top;
    });
    std::vector<RECT> out;
    for (const Display &d : displays) out.push_back(d.rect);
    return out;
}

inline bool Same(const std::vector<RECT> &a, const std::vector<RECT> &b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (!EqualRect(&a[i], &b[i])) return false;
    return true;
}

// Whether `rect` is exactly one of the displays in `layout`.
inline bool Holds(const std::vector<RECT> &layout, const RECT &rect) {
    return std::any_of(layout.begin(), layout.end(), [&](const RECT &r) { return EqualRect(&r, &rect) != FALSE; });
}

inline BOOL CALLBACK CollectDisplay(HMONITOR monitor, HDC, LPRECT, LPARAM param) {
    MONITORINFO mi{sizeof(mi)};
    if (GetMonitorInfoW(monitor, &mi))
        reinterpret_cast<std::vector<Display> *>(param)->push_back({mi.rcMonitor, (mi.dwFlags & MONITORINFOF_PRIMARY) != 0});
    return TRUE;
}

// The displays as they are now. Cheap: no display configuration query.
inline std::vector<RECT> Current() {
    std::vector<Display> displays;
    EnumDisplayMonitors(nullptr, nullptr, CollectDisplay, reinterpret_cast<LPARAM>(&displays));
    return Ordered(std::move(displays));
}

}  // namespace displaylayout
