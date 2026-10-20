// The shape of the settings app's pictures of a theme, from the displays and the settings. When
// spanning several displays the picture is the whole wall of them, as the logon screen shows it
// (overlay/src/plan.cpp): the wallpaper is fitted to the bounds of every display, and the
// components are drawn on each display that shows them as on a display of its own. Otherwise
// the picture is one display.
#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "animelogon/monitors.h"
#include "animelogon/settings.h"

namespace app {

// A display in a picture of several: where it is, in the picture's pixels, and whether the
// logon screen draws components on it.
struct Screen {
    RECT rect{};
    bool components = true;
};

inline bool Spanning(const animelogon::Settings &settings, const std::vector<animelogon::MonitorInfo> &monitors) {
    return settings.monitorMode == animelogon::MonitorMode::Span && monitors.size() > 1;
}

// How tall a picture `width` pixels wide is: the wall's proportions when spanning, otherwise
// those of `shown`, the display the theme is shown on (16:9 without one).
inline UINT PictureHeight(const animelogon::Settings &settings, const std::vector<animelogon::MonitorInfo> &monitors,
                          const animelogon::MonitorInfo *shown, UINT width) {
    LONG w = 16, h = 9;
    if (Spanning(settings, monitors)) {
        const RECT wall = animelogon::VirtualBounds(monitors);
        w = wall.right - wall.left;
        h = wall.bottom - wall.top;
    } else if (shown) {
        w = shown->rect.right - shown->rect.left;
        h = shown->rect.bottom - shown->rect.top;
    }
    return std::max<UINT>(1, (UINT)((uint64_t)width * (UINT)std::max<LONG>(1, h) / (UINT)std::max<LONG>(1, w)));
}

// Spanning: each display in a picture of the wall `width` x `height`, in the displays' order,
// with components where the logon screen starts drawing them -- on every display, or on the
// main one (with "auto" they then follow the password box). Empty when not spanning.
inline std::vector<Screen> Wall(const animelogon::Settings &settings, const std::vector<animelogon::MonitorInfo> &monitors,
                                UINT width, UINT height) {
    std::vector<Screen> screens;
    if (!Spanning(settings, monitors)) return screens;
    const RECT wall = animelogon::VirtualBounds(monitors);
    const double sx = (double)width / std::max<LONG>(1, wall.right - wall.left);
    const double sy = (double)height / std::max<LONG>(1, wall.bottom - wall.top);
    const bool everywhere = settings.componentDisplays == animelogon::ComponentDisplays::All;
    for (const animelogon::MonitorInfo &m : monitors) {
        Screen s;
        s.rect = {(LONG)std::lround((m.rect.left - wall.left) * sx), (LONG)std::lround((m.rect.top - wall.top) * sy),
                  (LONG)std::lround((m.rect.right - wall.left) * sx), (LONG)std::lround((m.rect.bottom - wall.top) * sy)};
        s.components = everywhere || m.primary;
        screens.push_back(s);
    }
    return screens;
}

}  // namespace app
