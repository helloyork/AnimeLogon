// settings.ini: the machine-wide configuration. Written by the settings app (Users may rewrite
// the file but never create it), read by the overlay. One `key = value` per line; unknown keys
// are ignored. It holds only ids, enums and typed values, never a path or free text.
//
// The global keys say what the person wants whatever the theme: which theme, the multi-monitor
// mode, where components are drawn, how the clock reads, the audio. Everything the person changes
// about a theme is an override, `theme.<theme id>.<...>`: editing never needs the elevated helper,
// and putting a theme back as it came is removing its overrides.
#pragma once

#include <cstddef>
#include <cstdint>
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

// How bright a theme's wallpaper is drawn on the logon screen, as a fraction of its own
// brightness: 1 as it is, down to kBrightnessMin. Darkening a light wallpaper keeps the
// components over it readable. Each colour channel is scaled by it.
constexpr float kBrightness = 1.0f, kBrightnessMin = 0.1f;

// How much a theme's wallpaper is blurred on the logon screen: a Gaussian whose standard deviation
// is this percentage of the display's height (blur.h), 0 for none. Windows blurs its own sign-in
// background by about 2.8.
constexpr float kBlurMax = 5.0f;

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

// The logon screen can move through a list of themes (rotation.h). When it moves on:
enum class RotationTrigger {
    Interval,  // every `rotation_minutes`, counted while somebody is signed in
    Lock,      // after each logon screen: the next lock or boot shows the next theme
};
// In which order it moves through them.
enum class RotationOrder {
    Sequential,  // as listed, starting again after the last
    Shuffle,     // every theme once in a random order, then a new order
};

// How scenes are drawn (and web wallpapers, once there are any): what the user trades between
// looks and cost. Every scene shares these.
enum class RenderQuality {
    Auto,     // adapts: lowers the frame rate, then the inner resolution, then stops on the still picture
    Highest,  // always full resolution with full-resolution effects; never lowered, frames are dropped instead
    High,     // full resolution, soft effects (blur, glow, rays) at half resolution
    Medium,   // three quarters of the resolution, soft effects at a quarter
    Low,      // half the resolution, soft effects at a quarter, half the particles
};
// What a scene does while the PC runs on its battery.
enum class BatteryRender {
    Same,   // as on mains power
    Half,   // half the frame rate
    Still,  // the still picture only
};
constexpr int kRenderFps = 60, kRenderFpsMin = 15, kRenderFpsMax = 240;
constexpr int kVideoMemoryMin = 128, kVideoMemoryMax = 16384;  // MB
constexpr int kParticlesMin = 10, kParticlesMax = 100;         // percent
struct RenderSettings {
    RenderQuality quality = RenderQuality::Auto;    // `render_quality`
    int fps = kRenderFps;      // `render_fps`; 0 is `display`, the display's own refresh rate
    int videoMemory = 0;       // `render_video_memory` in MB; 0 is `auto`
    int particles = kParticlesMax;                  // `render_particles`: percent of each emitter's particles
    BatteryRender onBattery = BatteryRender::Half;  // `render_on_battery`
    bool stillOnSaver = true;  // `render_battery_saver`: `still` while Windows saves power, or `same`
};
// `rotation_minutes`, between kRotationMinutesMin and Max. Every move to a theme not shown lately
// has Windows prepare its sign-in picture again, so the shortest interval is a few minutes.
constexpr int kRotationMinutes = 30, kRotationMinutesMin = 5, kRotationMinutesMax = 1440;
// The most themes `rotation_themes` may list.
constexpr size_t kRotationMax = 64;

struct AudioSource {
    bool enabled = true;
    int volume = 100;  // 0..100
};

struct AudioSettings {
    bool enabled = false;
    std::wstring device;  // empty: follow the signed-in user's default device
    int volume = 80;      // 0..100
    AudioSource videoTrack;
    // `video_audio_yield`: the video's sound track is silent while another program makes sound on
    // the same device, and comes back once it has been quiet for a while.
    bool videoYields = true;
};

// What settings.ini changes about one component instance of a theme:
// theme.<theme id>.<instance>.ref / .visible / .<setting id> / .<element>.<attribute>.
struct InstanceOverrides {
    std::optional<std::wstring> ref;  // a component id: adds the instance, or swaps its component
    std::optional<bool> visible;      // false hides the instance
    skin::Values values;              // by setting id or adjustment key, e.g. p1.l1.t1.weight
};

// What settings.ini changes about one theme: theme.<theme id>.wallpaper / .fit / .wake /
// .wake-duration / .brightness / .blur / .<instance>.*.
struct ThemeOverrides {
    std::optional<std::wstring> wallpaper;  // a wallpaper id, "default" or "none"
    std::optional<Scaling> fit;
    std::optional<WakeStyle> wake;     // theme.<theme id>.wake
    std::optional<float> wakeSeconds;  // theme.<theme id>.wake-duration
    std::optional<float> brightness;   // theme.<theme id>.brightness
    std::optional<float> blur;         // theme.<theme id>.blur
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
    // `rotation`: whether the logon screen moves through `rotation_themes` (comma-separated theme
    // ids, at most kRotationMax) instead of always showing `theme`; `rotation_trigger` (interval or
    // lock), `rotation_minutes` and `rotation_order` (sequential or shuffle) say how. `theme` is then
    // the theme last chosen by hand: choosing one that is in the list moves the rotation to it.
    bool rotation = false;
    std::vector<std::wstring> rotationThemes;
    RotationTrigger rotationTrigger = RotationTrigger::Interval;
    int rotationMinutes = kRotationMinutes;
    RotationOrder rotationOrder = RotationOrder::Sequential;
    // `render_quality`, `render_fps`, `render_video_memory`, `render_particles`, `render_on_battery`,
    // `render_battery_saver`: how every scene is drawn.
    RenderSettings render;

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
// A wallpaper's brightness, from kBrightnessMin to 1.
bool ParseBrightness(std::wstring_view text, float *out);
// A wallpaper's blur, from 0 to kBlurMax.
bool ParseBlur(std::wstring_view text, float *out);
// Scales the colour of `count` pixels `stride` bytes apart (3 for B G R, 4 for B G R A, whose
// fourth byte is left alone) by `brightness`, rounding to nearest: as the logon screen's shader
// draws a wallpaper of that brightness. Nothing happens at 1.
void ApplyBrightness(uint8_t *pixels, size_t count, int stride, float brightness);
const wchar_t *ToString(ComponentDisplays displays);
bool Parse(std::wstring_view text, ComponentDisplays *out);  // auto, primary or all
const wchar_t *ToString(OtherDisplays displays);
bool Parse(std::wstring_view text, OtherDisplays *out);  // blur, picture or black
const wchar_t *ToString(RotationTrigger trigger);
bool Parse(std::wstring_view text, RotationTrigger *out);  // interval or lock
const wchar_t *ToString(RotationOrder order);
bool Parse(std::wstring_view text, RotationOrder *out);  // sequential or shuffle
const wchar_t *ToString(RenderQuality quality);
bool Parse(std::wstring_view text, RenderQuality *out);  // auto, highest, high, medium or low
const wchar_t *ToString(BatteryRender render);
bool Parse(std::wstring_view text, BatteryRender *out);  // same, half or still

}  // namespace animelogon
