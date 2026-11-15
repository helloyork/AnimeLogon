// What the icon in the notification area offers (tray.h), worked out from the library alone so that
// it can be tested: the recently used themes, each marked when it is on the logon screen, and the
// line its tooltip shows.
#pragma once

#include <string>
#include <vector>

#include "animelogon/monitors.h"

#include "library.h"

namespace app {

// Where a display is beside the main one: what tells two displays apart when they have the same
// name, and what the settings page and the icon's menu say after each display's name.
enum class Place { Main, Left, Right, Above, Below };
// Beside the main display where the two share rows of pixels, above or below it where they share
// columns; one off its corner goes by the way it is further, in the main display's widths and heights.
// The main one is the one Windows marks; without one, the first.
Place PlaceOf(const animelogon::MonitorInfo &m, const std::vector<animelogon::MonitorInfo> &monitors);
// main, left, right, above or below, as the page's strings name them (monitor.place.*).
const wchar_t *PlaceName(Place place);
// What a display is called: the name Windows has for it (its model, from the display itself), else
// 显示器 n with the number Windows gives it (\\.\DISPLAYn), else its place in `monitors` from 1.
std::wstring DisplayName(const animelogon::MonitorInfo &m, size_t index);
// The name with where the display is: DELL U2720Q（主显示器右侧）.
std::wstring DisplayLabel(const animelogon::MonitorInfo &m, const std::vector<animelogon::MonitorInfo> &monitors, size_t index);

struct TrayTheme {
    std::wstring id, name;
    std::wstring monitor;  // what Library::Apply takes with it: empty, or the display it goes on
    bool checked = false;  // on the logon screen there now
};

// In per-monitor mode with more than one display, a submenu for each.
struct TrayDisplay {
    std::wstring label;  // DisplayLabel: named and placed as the settings page names and places it
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
