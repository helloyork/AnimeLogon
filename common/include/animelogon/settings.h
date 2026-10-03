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

// How a wallpaper fills its display: a theme's fit.
enum class Scaling {
    Fill,     // cover the display, cropping the edges
    Fit,      // show the whole wallpaper, with bars
    Stretch,  // fill the display, ignoring the aspect ratio
};

// Which displays components are drawn on. Auto is the display Windows puts its password box on.
enum class ComponentDisplays { Auto, Primary, All };

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

// Every global key has a value; a key that is missing or malformed in the file keeps the
// default given here.
struct Settings {
    // --- Global --------------------------------------------------------------------------
    std::wstring theme = L"default";  // `theme`: a theme id, or the built-in "default"
    MonitorMode monitorMode = MonitorMode::Duplicate;
    // `screen.<monitor>`: monitor key -> the theme that display shows in PerMonitor mode. A
    // display without one shows `theme`.
    std::map<std::wstring, std::wstring> screens;
    bool components = true;  // `components`: false hides every component of every theme
    ComponentDisplays componentDisplays = ComponentDisplays::Auto;  // `component_displays`
    // `clock_hours`, `clock_ampm`, `clock_date`, `clock_language`: what the clock says,
    // whichever theme and component show it.
    ClockStyle clock;
    AudioSettings audio;

    // --- Themes --------------------------------------------------------------------------
    std::map<std::wstring, ThemeOverrides> themeOverrides;  // by theme id

    // Empty when the theme has none.
    const ThemeOverrides &OverridesFor(const std::wstring &themeId) const;
};

// Parses the file's text. Malformed lines are reported in `problems` and ignored, and so is a
// file with no well-formed `theme` line: it shows the built-in theme.
Settings ParseSettings(const std::wstring &text, std::vector<std::wstring> *problems = nullptr);
// Every global key, then the overrides.
std::wstring SerializeSettings(const Settings &settings);
// Whether the text has a well-formed `theme` line. A settings.ini from before themes has none;
// the installer replaces such a file with the defaults.
bool NamesATheme(const std::wstring &text);

// Reads settings.ini. Returns defaults if it is missing, and refuses a file that is not
// administrators-only when `requireTrusted` is set.
Settings LoadSettings(bool requireTrusted, std::wstring *why = nullptr);

// A typed value as settings.ini and theme.xml carry it: at most 128 characters, none of them
// control characters. Whether it suits what it is for is checked where it is used.
bool IsTypedValueText(std::wstring_view value);

const wchar_t *ToString(MonitorMode mode);
const wchar_t *ToString(Scaling scaling);
bool Parse(std::wstring_view text, Scaling *out);  // fill, fit or stretch
const wchar_t *ToString(ComponentDisplays displays);
bool Parse(std::wstring_view text, ComponentDisplays *out);  // auto, primary or all

}  // namespace animelogon
