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

// How the logon screen gives way to Windows' sign-in screen when it is woken: a theme's wake
// transition, over its duration in seconds (kWakeSeconds, between kWakeSecondsMin and Max).
enum class WakeStyle {
    Fade,   // fades out
    Zoom,   // grows a little as it fades
    Slide,  // slides up off the display, as Windows' own lock screen does
    Cut,    // goes at once
};
constexpr float kWakeSeconds = 0.5f, kWakeSecondsMin = 0.1f, kWakeSecondsMax = 2.0f;

// Which displays components are drawn on. Auto is the display Windows puts its password box on.
enum class ComponentDisplays { Auto, Primary, All };

// What the displays other than the primary show from the wake until sign-in. Windows draws
// nothing there while it asks for the password, so without a picture of ours they are black.
enum class OtherDisplays {
    Blur,     // a blurred, slightly darkened still of the display's own wallpaper
    Picture,  // the same still, as it is
    Black,    // nothing: black, as Windows leaves them
};

// The icon in the notification area (animelogon.exe --tray) offers the themes most recently put on
// the logon screen: `tray_recent` of them, between kTrayRecentMin and Max. settings.ini remembers
// up to kRecentMax.
constexpr int kTrayRecent = 5, kTrayRecentMin = 1, kTrayRecentMax = 10;
constexpr size_t kRecentMax = kTrayRecentMax;

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
    std::optional<WakeStyle> wake;     // theme.<theme id>.wake
    std::optional<float> wakeSeconds;  // theme.<theme id>.wake-duration
    skin::Values wallpaperValues;  // scene settings and pN layer adjustments
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
    OtherDisplays otherDisplays = OtherDisplays::Blur;              // `other_displays`
    // `media`: whether components may show what is playing (titles and artists, from the
    // signed-in person's own session, on their own lock screen only).
    bool media = true;
    // `clock_hours`, `clock_ampm`, `clock_date`, `clock_language`: what the clock says,
    // whichever theme and component show it.
    ClockStyle clock;
    AudioSettings audio;
    // `tray`: whether AnimeLogon shows its icon in the notification area while someone is signed in.
    bool tray = true;
    int trayRecent = kTrayRecent;  // `tray_recent`: how many recent themes the icon's menu offers
    // `recent`: the themes most recently put on the logon screen, newest first and comma-separated,
    // at most kRecentMax. Nothing on the logon screen reads it.
    std::vector<std::wstring> recent;

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

// The most settings.ini may hold, in bytes as written (UTF-8). A larger file is not read at all,
// so the logon screen would show the built-in theme: whoever writes it keeps within this.
constexpr size_t kMaxSettingsBytes = 256 * 1024;

// Reads settings.ini. Returns defaults if it is missing, and refuses a file that is not
// administrators-only when `requireTrusted` is set.
Settings LoadSettings(bool requireTrusted, std::wstring *why = nullptr);

// A typed value as settings.ini and theme.xml carry it: at most 128 characters, none of them
// control characters. Whether it suits what it is for is checked where it is used.
bool IsTypedValueText(std::wstring_view value);

const wchar_t *ToString(MonitorMode mode);
const wchar_t *ToString(Scaling scaling);
bool Parse(std::wstring_view text, Scaling *out);  // fill, fit or stretch
const wchar_t *ToString(WakeStyle style);
bool Parse(std::wstring_view text, WakeStyle *out);  // fade, zoom, slide or cut
// A wake transition's duration in seconds, within its range.
bool ParseWakeSeconds(std::wstring_view text, float *out);
const wchar_t *ToString(ComponentDisplays displays);
bool Parse(std::wstring_view text, ComponentDisplays *out);  // auto, primary or all
const wchar_t *ToString(OtherDisplays displays);
bool Parse(std::wstring_view text, OtherDisplays *out);  // blur, picture or black

}  // namespace animelogon
