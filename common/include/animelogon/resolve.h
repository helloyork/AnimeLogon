// What one display shows: the theme settings.ini picks for it, with the person's overrides
// applied and every value checked against the component it is for. The overlay resolves once
// per display.
//
// Which theme:
// - In PerMonitor mode, the theme the display's `screen.<monitor>` names, when it has one;
//   otherwise, and in the other modes, the theme `theme` names (ParseSettings leaves "default"
//   there when the file has no valid one).
// - A theme that is not installed or does not load is replaced by the built-in "default" (with
//   its own overrides). So every display has a theme, and the overlay covers every display.
//
// Within the theme:
// - a wallpaper that does not load is replaced by the built-in "default"; "none" gives none,
//   which the overlay draws as black;
// - a component that does not load leaves its instance out;
// - `components = false` leaves every instance out;
// - an instance's values are its sets in theme.xml, then its overrides in settings.ini; values
//   the component does not take are dropped.
// Everything replaced or dropped is reported in `problems`.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "animelogon/settings.h"
#include "animelogon/skin.h"
#include "animelogon/theme.h"
#include "animelogon/wallpaper.h"

namespace animelogon {

struct ResolvedComponent {
    std::wstring instance;   // the instance's id within the theme, e.g. "clock"
    std::wstring component;  // the component it places, e.g. "clock"
    skin::Skin skin;
    skin::Values values;  // for skin::Resolve: only values the component takes
};

struct ResolvedTheme {
    std::wstring themeId;  // the theme shown: the one named, or "default" in its place
    std::optional<WallpaperInfo> wallpaper;  // with its file paths; none: nothing behind the components
    Scaling fit = Scaling::Fill;
    std::vector<ResolvedComponent> components;  // the visible instances, in the theme's order
    // The displays the components are drawn on. Which display Auto means is the overlay's to find.
    ComponentDisplays componentDisplays = ComponentDisplays::Auto;
};

// Where resolution finds themes, wallpapers and components. DiskStore() reads the stores on
// disk with LoadTheme, LoadWallpaper and LoadComponent; tests give their own.
class ThemeStore {
public:
    virtual ~ThemeStore() = default;
    virtual bool LoadTheme(const std::wstring &id, theme::Theme *theme, std::wstring *why) const = 0;
    virtual bool LoadWallpaper(const std::wstring &id, WallpaperInfo *info, std::wstring *why) const = 0;
    virtual bool LoadComponent(const std::wstring &id, skin::Skin *component, std::wstring *why) const = 0;
};
const ThemeStore &DiskStore();

ResolvedTheme ResolveTheme(const Settings &settings, const std::wstring &monitorKey,
                           std::vector<std::wstring> *problems = nullptr);
ResolvedTheme ResolveTheme(const Settings &settings, const std::wstring &monitorKey, const ThemeStore &store,
                           std::vector<std::wstring> *problems = nullptr);

}  // namespace animelogon
