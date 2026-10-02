#include "plan.h"

#include <map>

#include "animelogon/text.h"

namespace plan {
namespace {

using animelogon::ResolvedTheme;

Wallpaper WallpaperOf(const ResolvedTheme &theme) {
    Wallpaper w;
    w.fit = theme.fit;
    if (!theme.wallpaper) return w;
    const animelogon::WallpaperInfo &info = *theme.wallpaper;
    w.id = info.id;
    if (info.builtIn) {
        w.source = Source::Gradient;
    } else if (info.kind == animelogon::WallpaperKind::Image) {
        w.source = Source::Image;
        w.path = info.imagePath;
    } else {
        w.source = Source::Video;
        w.path = info.videoPath;
        if (info.hasAudio) w.audioPath = info.audioPath;
    }
    return w;
}

// A display's theme, with its components ready to draw.
struct Resolution {
    ResolvedTheme theme;
    std::vector<Component> components;
};

}  // namespace

Plan Build(const animelogon::Settings &settings, const std::vector<animelogon::MonitorInfo> &monitors,
           const animelogon::ThemeStore &store) {
    Plan plan;
    if (monitors.empty()) return plan;

    // ResolveTheme sees a display only through its screen.<monitor> key, and only in per-monitor
    // mode, so displays it would see alike are resolved once and share the result.
    std::map<std::wstring, Resolution> resolved;
    auto resolve = [&](const animelogon::MonitorInfo &m) -> const Resolution & {
        std::wstring alike;
        if (settings.monitorMode == animelogon::MonitorMode::PerMonitor) {
            const auto it = settings.screens.find(m.key);
            if (it != settings.screens.end()) alike = L"screen " + it->second;
        }
        const auto found = resolved.find(alike);
        if (found != resolved.end()) return found->second;
        Resolution &r = resolved[alike];
        r.theme = animelogon::ResolveTheme(settings, m.key, store, &plan.problems);
        for (const animelogon::ResolvedComponent &c : r.theme.components)
            r.components.push_back({L"theme " + r.theme.themeId + L"/" + c.instance, c.instance, c.component,
                                    animelogon::skin::Resolve(c.skin, c.values)});
        return r;
    };

    const Resolution &first = resolve(monitors.front());
    plan.componentDisplays = first.theme.componentDisplays;
    const bool span = settings.monitorMode == animelogon::MonitorMode::Span;
    const RECT bounds = animelogon::VirtualBounds(monitors);
    for (const animelogon::MonitorInfo &m : monitors) {
        const Resolution &r = resolve(m);
        Display d;
        d.monitorKey = m.key;
        d.rect = m.rect;
        d.primary = m.primary;
        d.themeId = r.theme.themeId;
        // Spanning shows one wallpaper across the wall: the primary display's.
        d.canvas = span ? bounds : m.rect;
        d.wallpaper = WallpaperOf(span ? first.theme : r.theme);
        d.components = r.components;
        switch (plan.componentDisplays) {
        case animelogon::ComponentDisplays::Auto:
        case animelogon::ComponentDisplays::Primary: d.showComponents = m.primary; break;
        case animelogon::ComponentDisplays::All: d.showComponents = true; break;
        }
        plan.displays.push_back(std::move(d));
    }
    return plan;
}

std::wstring Describe(const Display &d) {
    std::wstring wallpaper;
    switch (d.wallpaper.source) {
    case Source::None: wallpaper = L"no wallpaper (black)"; break;
    case Source::Video: wallpaper = L"video " + d.wallpaper.id; break;
    case Source::Image: wallpaper = L"image " + d.wallpaper.id; break;
    case Source::Gradient: wallpaper = L"the built-in wallpaper"; break;
    }
    return animelogon::Format(L"display %s%s %ldx%ld: theme %s, %s, %s, %zu component(s)%s", d.monitorKey.c_str(),
                              d.primary ? L" (primary)" : L"", d.rect.right - d.rect.left, d.rect.bottom - d.rect.top,
                              d.themeId.c_str(), wallpaper.c_str(),
                              animelogon::ToString(d.wallpaper.fit), d.components.size(),
                              d.components.empty() ? L"" : d.showComponents ? L", drawn here" : L", not drawn here");
}

}  // namespace plan
