#include "traymenu.h"

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

// Displays are numbered in the order Windows lists them, the main one marked, as on the settings page.
std::wstring Label(const MonitorInfo &m, int number) {
    const std::wstring name = i18n::Tr("monitor.name", {{L"n", std::to_wstring(number)}});
    return m.primary ? i18n::Tr("monitor.primary", {{L"name", name}}) : name;
}

std::vector<TrayTheme> Choices(const Library &library, const std::vector<std::wstring> &ids, const std::wstring &shown,
                               const std::wstring &monitor) {
    std::vector<TrayTheme> list;
    for (const std::wstring &id : ids)
        if (const ThemeEntry *e = library.Entry(id)) list.push_back({id, e->name, monitor, id == shown});
    return list;
}

}  // namespace

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
        int number = 0;
        for (const MonitorInfo &m : monitors)
            menu.displays.push_back({Label(m, ++number), Choices(library, recent, ThemeOn(library, m), m.key)});
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
