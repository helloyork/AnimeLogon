// What one display shows: the theme settings.ini picks for it, with the person's overrides
// applied and every value checked against the component it is for. The overlay resolves once
// per display.
//
// Which theme:
// - In PerMonitor mode, the theme `screen.<monitor>` names, when it names one; otherwise, and in
//   the other modes, the theme `theme` names.
// - A theme that does not load is replaced by the built-in "default" (with its own overrides).
//
// Legacy mode (transitional, until the overlay and the settings app use themes): with no
// `theme` key, and no theme named by the display's `screen.<monitor>`, the result is the theme
// the overlay showed before themes:
// - the wallpaper is the video Settings::VideoFor gives, or none when that is empty or does not
//   load (the overlay then left that display alone);
// - the fit is `scaling`;
// - one instance, "clock", of the skin `skin` (the clock when that skin does not load) with its
//   `skin.<id>.*` values, shown when `clock` is true, on the displays `clock_displays` says.
//
// With a theme:
// - a wallpaper that does not load is replaced by the built-in "default"; "none" gives none;
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
    std::wstring themeId;  // the theme shown; empty in legacy mode
    bool legacy = false;   // transitional: built from the legacy keys
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
    virtual bool ThemeExists(const std::wstring &id) const = 0;
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
