// settings.ini: the machine-wide configuration. Written by the settings app (Users may rewrite
// the file but never create it), read by the overlay. One `key = value` per line; unknown keys
// are ignored. It holds only ids, enums and typed values, never a path or free text.
//
// The global keys say what the person wants whatever the theme: which theme, the multi-monitor
// mode, where components are drawn, how the clock reads, the audio. Everything the person changes
// about a theme is an override, `theme.<theme id>.<...>`: editing never needs the elevated helper,
// and putting a theme back as it came is removing its overrides.
#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "animelogon/clock.h"

namespace animelogon {

enum class MonitorMode {
    Duplicate,  // the same theme on every display
    Span,       // one wallpaper across all displays
    PerMonitor, // each display shows its own theme
};

// How a wallpaper fills its display: a theme's fit (and the legacy `scaling`).
enum class Scaling {
    Fill,     // cover the display, cropping the edges
    Fit,      // show the whole wallpaper, with bars
    Stretch,  // fill the display, ignoring the aspect ratio
};

// Which displays components are drawn on. Auto is the display Windows puts its password box on.
using ComponentDisplays = ClockDisplays;

struct AudioSource {
    bool enabled = true;
    int volume = 100;  // 0..100
};

struct AudioSettings {
    bool enabled = false;
    std::wstring device;  // empty: follow the signed-in user's default device
    int volume = 80;      // 0..100
    AudioSource videoTrack;
};

// What settings.ini changes about one component instance of a theme:
// theme.<theme id>.<instance>.ref / .visible / .<setting id> / .<element>.<attribute>.
struct InstanceOverrides {
    std::optional<std::wstring> ref;  // a component id: adds the instance, or swaps its component
    std::optional<bool> visible;      // false hides the instance
    skin::Values values;              // by setting id or adjustment key, e.g. p1.l1.t1.weight
};

// What settings.ini changes about one theme: theme.<theme id>.wallpaper / .fit / .<instance>.*.
struct ThemeOverrides {
    std::optional<std::wstring> wallpaper;  // a wallpaper id, "default" or "none"
    std::optional<Scaling> fit;
    std::map<std::wstring, InstanceOverrides> instances;  // by instance id
};

struct Settings {
    // --- Global --------------------------------------------------------------------------
    MonitorMode monitorMode = MonitorMode::Duplicate;
    // Monitor key -> the theme that display shows in PerMonitor mode. Transitional: the value may
    // be a legacy video id instead; ResolveTheme tells them apart.
    std::map<std::wstring, std::wstring> screens;
    AudioSettings audio;
    // clock.style (clock_hours, clock_ampm, clock_date, clock_language) is global. The rest of
    // ClockSettings is legacy; see below.
    ClockSettings clock;

    // --- Themes --------------------------------------------------------------------------
    // Each of these is written back only when it was read or set, so a settings app that knows
    // nothing of themes leaves the machine in legacy mode.
    std::optional<std::wstring> theme;                   // `theme`: a theme id, or "default"
    std::optional<bool> components;                      // `components`: false hides them all; default true
    std::optional<ComponentDisplays> componentDisplays;  // `component_displays`; default Auto
    std::map<std::wstring, ThemeOverrides> themeOverrides;  // by theme id

    // Empty when the theme has none.
    const ThemeOverrides &OverridesFor(const std::wstring &themeId) const;

    // --- Legacy --------------------------------------------------------------------------
    // Read and written until the overlay and the settings app use themes, then removed. Without
    // a `theme` key ResolveTheme builds, from these, the theme the overlay showed before themes.
    // Also legacy: clock.enabled (`clock`, now `components`), clock.displays (`clock_displays`,
    // now `component_displays`), clock.skin (`skin`) and clock.values (`skin.<id>.*`, now a
    // theme's overrides).
    Scaling scaling = Scaling::Fill;  // `scaling`, now a theme's fit
    std::wstring video;               // `video`: library id; used unless a display has its own

    // The video a display should play, or empty for none.
    std::wstring VideoFor(const std::wstring &monitorKey) const;
};

// Parses the file's text. Malformed lines are reported in `problems` and ignored.
Settings ParseSettings(const std::wstring &text, std::vector<std::wstring> *problems = nullptr);
std::wstring SerializeSettings(const Settings &settings);

// Reads settings.ini. Returns defaults if it is missing, and refuses a file that is not
// administrators-only when `requireTrusted` is set.
Settings LoadSettings(bool requireTrusted, std::wstring *why = nullptr);

// A typed value as settings.ini and theme.xml carry it: at most 128 characters, none of them
// control characters. Whether it suits what it is for is checked where it is used.
bool IsTypedValueText(std::wstring_view value);

const wchar_t *ToString(MonitorMode mode);
const wchar_t *ToString(Scaling scaling);
bool Parse(std::wstring_view text, Scaling *out);  // fill, fit or stretch

}  // namespace animelogon
