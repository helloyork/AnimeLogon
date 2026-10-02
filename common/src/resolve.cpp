#include "animelogon/resolve.h"

#include "animelogon/components.h"

namespace animelogon {
namespace {

class Disk : public ThemeStore {
public:
    bool ThemeExists(const std::wstring &id) const override { return ::animelogon::ThemeExists(id); }
    bool LoadTheme(const std::wstring &id, theme::Theme *theme, std::wstring *why) const override {
        return ::animelogon::LoadTheme(id, theme, why);
    }
    bool LoadWallpaper(const std::wstring &id, WallpaperInfo *info, std::wstring *why) const override {
        return ::animelogon::LoadWallpaper(id, info, why);
    }
    bool LoadComponent(const std::wstring &id, skin::Skin *component, std::wstring *why) const override {
        return ::animelogon::LoadComponent(id, component, why);
    }
};

void Report(std::vector<std::wstring> *problems, const std::wstring &what) {
    if (problems) problems->push_back(what);
}

// Adds the values the component takes; reports and drops the rest.
void Merge(const skin::Skin &component, const skin::Values &from, const std::wstring &where, skin::Values *into,
           std::vector<std::wstring> *problems) {
    for (const auto &[key, value] : from) {
        if (skin::Accepts(component, key, value)) (*into)[key] = value;
        else Report(problems, where + L"." + key + L" = " + value + L" does not suit the component -- dropped");
    }
}

// The theme the overlay showed before themes, from the legacy keys.
ResolvedTheme Legacy(const Settings &s, const std::wstring &monitorKey, const ThemeStore &store,
                     std::vector<std::wstring> *problems) {
    ResolvedTheme r;
    r.legacy = true;
    r.fit = s.scaling;
    r.componentDisplays = s.clock.displays;
    const std::wstring video = s.VideoFor(monitorKey);
    std::wstring why;
    WallpaperInfo wallpaper;
    if (!video.empty()) {
        if (store.LoadWallpaper(video, &wallpaper, &why)) r.wallpaper = std::move(wallpaper);
        else Report(problems, L"video " + video + L": " + why + L" -- not shown");
    }
    if (s.clock.enabled) {
        ResolvedComponent clock;
        clock.instance = kClockComponent;
        std::wstring id = s.clock.skin;
        if (!store.LoadComponent(id, &clock.skin, &why)) {
            Report(problems, L"skin " + id + L": " + why + L" -- using the clock");
            id = L"default";
            clock.skin = skin::Default();
        }
        clock.component = id == L"default" ? std::wstring(kClockComponent) : id;
        Merge(clock.skin, s.clock.ValuesFor(id), L"skin." + id, &clock.values, problems);
        r.components.push_back(std::move(clock));
    }
    return r;
}

ResolvedTheme Themed(const Settings &s, std::wstring id, const ThemeStore &store, std::vector<std::wstring> *problems) {
    ResolvedTheme r;
    theme::Theme own;
    std::wstring why;
    if (!store.LoadTheme(id, &own, &why)) {
        Report(problems, L"theme " + id + L": " + why + L" -- showing the default theme");
        id = kDefaultTheme;
        own = theme::Default();
    }
    r.themeId = id;
    const ThemeOverrides &overrides = s.OverridesFor(id);
    std::vector<std::wstring> editProblems;
    const theme::Theme edited = theme::Edited(own, overrides, &editProblems);
    for (const std::wstring &p : editProblems) Report(problems, L"theme." + id + L": " + p);
    r.fit = edited.fit;
    r.componentDisplays = s.componentDisplays.value_or(ComponentDisplays::Auto);

    if (edited.wallpaper != kNoWallpaper) {
        WallpaperInfo wallpaper;
        bool loaded = store.LoadWallpaper(edited.wallpaper, &wallpaper, &why);
        if (!loaded) {
            Report(problems, L"theme " + id + L": wallpaper " + edited.wallpaper + L": " + why +
                                 L" -- showing the default wallpaper");
            loaded = store.LoadWallpaper(kDefaultWallpaper, &wallpaper, &why);
        }
        if (loaded) r.wallpaper = std::move(wallpaper);
    }

    if (!s.components.value_or(true)) return r;
    for (const theme::Instance &instance : edited.components) {
        if (!instance.visible) continue;
        ResolvedComponent c;
        c.instance = instance.id;
        c.component = instance.ref;
        if (!store.LoadComponent(instance.ref, &c.skin, &why)) {
            Report(problems, L"theme " + id + L": " + instance.id + L": component " + instance.ref + L": " + why +
                                 L" -- left out");
            continue;
        }
        // The theme's own sets count only while the instance places the component they were
        // written for.
        const theme::Instance *declared = own.Find(instance.id);
        if (declared && declared->ref == instance.ref)
            Merge(c.skin, declared->sets, L"theme " + id + L": " + instance.id, &c.values, problems);
        const auto o = overrides.instances.find(instance.id);
        if (o != overrides.instances.end())
            Merge(c.skin, o->second.values, L"theme." + id + L"." + instance.id, &c.values, problems);
        r.components.push_back(std::move(c));
    }
    return r;
}

}  // namespace

const ThemeStore &DiskStore() {
    static const Disk disk{};
    return disk;
}

ResolvedTheme ResolveTheme(const Settings &settings, const std::wstring &monitorKey,
                           std::vector<std::wstring> *problems) {
    return ResolveTheme(settings, monitorKey, DiskStore(), problems);
}

ResolvedTheme ResolveTheme(const Settings &s, const std::wstring &monitorKey, const ThemeStore &store,
                           std::vector<std::wstring> *problems) {
    std::wstring screen;
    if (s.monitorMode == MonitorMode::PerMonitor) {
        const auto it = s.screens.find(monitorKey);
        if (it != s.screens.end()) screen = it->second;
    }
    // Transitional: the value may still be a video of the old library.
    const bool screenTheme = !screen.empty() && store.ThemeExists(screen);
    if (!screenTheme && !s.theme) return Legacy(s, monitorKey, store, problems);
    if (!screen.empty() && !screenTheme)
        Report(problems, L"screen." + monitorKey + L" = " + screen + L" names no theme -- showing theme " + *s.theme);
    return Themed(s, screenTheme ? screen : *s.theme, store, problems);
}

}  // namespace animelogon
