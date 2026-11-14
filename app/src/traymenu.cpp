#include "traymenu.h"

#include <algorithm>
#include <cmath>
#include <cwchar>

#include "animelogon/i18n.h"
#include "animelogon/text.h"

using namespace animelogon;

namespace app {
namespace {

// The theme a display shows in per-monitor mode: its own, or `theme` when it has none installed.
std::wstring ThemeOn(const Library &library, const MonitorInfo &m) {
    const auto it = library.settings().screens.find(m.key);
    return it != library.settings().screens.end() && library.Entry(it->second) ? it->second : library.Current();
}

std::vector<TrayTheme> Choices(const Library &library, const std::vector<std::wstring> &ids, const std::wstring &shown,
                               const std::wstring &monitor) {
    std::vector<TrayTheme> list;
    for (const std::wstring &id : ids)
        if (const ThemeEntry *e = library.Entry(id)) list.push_back({id, e->name, monitor, id == shown});
    return list;
}

}  // namespace

Place PlaceOf(const MonitorInfo &m, const std::vector<MonitorInfo> &monitors) {
    if (m.primary || monitors.empty()) return Place::Main;
    const MonitorInfo *main = &monitors.front();
    for (const MonitorInfo &other : monitors)
        if (other.primary) {
            main = &other;
            break;
        }
    if (main == &m || main->key == m.key) return Place::Main;
    const RECT &a = m.rect, &p = main->rect;
    const bool rows = a.top < p.bottom && a.bottom > p.top, columns = a.left < p.right && a.right > p.left;
    const double dx = (a.left + a.right - p.left - p.right) / 2.0, dy = (a.top + a.bottom - p.top - p.bottom) / 2.0;
    const Place across = dx < 0 ? Place::Left : Place::Right, along = dy < 0 ? Place::Above : Place::Below;
    if (rows && !columns) return across;
    if (columns && !rows) return along;
    const double wide = std::max<LONG>(1, p.right - p.left), tall = std::max<LONG>(1, p.bottom - p.top);
    return std::fabs(dx) / wide >= std::fabs(dy) / tall ? across : along;
}

const wchar_t *PlaceName(Place place) {
    switch (place) {
    case Place::Left: return L"left";
    case Place::Right: return L"right";
    case Place::Above: return L"above";
    case Place::Below: return L"below";
    default: return L"main";
    }
}

std::wstring DisplayName(const MonitorInfo &m, size_t index) {
    if (!m.name.empty()) return m.name;
    // \\.\DISPLAY2 is display 2 to Windows, whatever order the displays are listed in.
    constexpr wchar_t kDevice[] = L"\\\\.\\DISPLAY";
    size_t number = index + 1;
    if (m.gdiName.compare(0, wcslen(kDevice), kDevice) == 0) {
        const unsigned long n = wcstoul(m.gdiName.c_str() + wcslen(kDevice), nullptr, 10);
        if (n > 0 && n < 1000) number = n;
    }
    return i18n::Tr("monitor.name", {{L"n", std::to_wstring(number)}});
}

std::wstring DisplayLabel(const MonitorInfo &m, const std::vector<MonitorInfo> &monitors, size_t index) {
    const std::string place = "monitor.place." + ToUtf8(PlaceName(PlaceOf(m, monitors)));
    return i18n::Tr("monitor.with_place", {{L"name", DisplayName(m, index)}, {L"place", i18n::Tr(place)}});
}

TrayMenu MenuFor(const Library &library, const std::vector<MonitorInfo> &monitors) {
    const Settings &s = library.settings();
    const std::vector<std::wstring> recent = library.Recent((size_t)s.trayRecent);
    TrayMenu menu;
    if (s.monitorMode != MonitorMode::PerMonitor) {
        menu.themes = Choices(library, recent, library.Current(), L"");
    } else if (monitors.size() <= 1) {
        // One display, which may have a theme of its own: the choice goes on it.
        const MonitorInfo *m = monitors.empty() ? nullptr : &monitors.front();
        menu.themes = Choices(library, recent, m ? ThemeOn(library, *m) : library.Current(), m ? m->key : L"");
    } else {
        for (size_t n = 0; n < monitors.size(); ++n)
            menu.displays.push_back(
                {DisplayLabel(monitors[n], monitors, n), Choices(library, recent, ThemeOn(library, monitors[n]), monitors[n].key)});
    }
    return menu;
}

std::wstring ShowingNow(const Library &library, const std::vector<MonitorInfo> &monitors) {
    std::wstring id = library.Current();
    if (library.settings().monitorMode == MonitorMode::PerMonitor && !monitors.empty()) {
        id = ThemeOn(library, monitors.front());
        for (const MonitorInfo &m : monitors)
            if (ThemeOn(library, m) != id) return i18n::Tr("tray.per_display");
    }
    const ThemeEntry *e = library.Entry(id);
    return e ? e->name : std::wstring();
}

}  // namespace app
