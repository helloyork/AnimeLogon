// What the icon in the notification area offers (tray.h), worked out from the library alone so that
// it can be tested: the recently used themes, each marked when it is on the logon screen, and the
// line its tooltip shows.
#pragma once

#include <string>
#include <vector>

#include "animelogon/monitors.h"

#include "library.h"

namespace app {

struct TrayTheme {
    std::wstring id, name;
    std::wstring monitor;  // what Library::Apply takes with it: empty, or the display it goes on
    bool checked = false;  // on the logon screen there now
};

// In per-monitor mode with more than one display, a submenu for each.
struct TrayDisplay {
    std::wstring label;  // 显示器 1（主）, numbered as the settings page numbers them
    std::vector<TrayTheme> themes;
};

struct TrayMenu {
    std::vector<TrayTheme> themes;      // when one choice puts a theme on every display
    std::vector<TrayDisplay> displays;  // otherwise
};

// `monitors` in the order EnumerateMonitors lists them. The themes are Library::Recent's, as many
// as settings.ini's `tray_recent` says.
TrayMenu MenuFor(const Library &library, const std::vector<animelogon::MonitorInfo> &monitors);

// What the logon screen shows, for the tooltip: the theme's name, or that each display has its own.
std::wstring ShowingNow(const Library &library, const std::vector<animelogon::MonitorInfo> &monitors);

}  // namespace app
