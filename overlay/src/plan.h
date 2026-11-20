// What each display shows, from the settings and the attached displays: the theme
// ResolveTheme gives the display, turned into a wallpaper to draw and the components to draw
// over it.
//
// The three monitor modes:
// - Duplicate: each display shows the theme it resolves to (the same one, as only per-monitor
//   mode gives a display a theme of its own).
// - Span: the primary display's wallpaper is fitted to the bounds of all the displays; the
//   components are still placed on each display by itself.
// - Per-monitor: each display shows the theme `screen.<monitor>` names, or `theme`.
//
// Every display is covered: a theme that cannot be shown gives way to the built-in one.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

#include "animelogon/blur.h"
#include "animelogon/monitors.h"
#include "animelogon/resolve.h"
#include "animelogon/settings.h"
#include "animelogon/skin.h"

namespace plan {

// What fills a display.
enum class Source {
    None,      // nothing: the display is black
    Video,     // video.mp4, decoded as it plays
    Image,     // image.bmp, read once
    Scene,     // its still (still.bmp, or the theme's own), then isolated scene frames
    Gradient,  // the built-in wallpaper, drawn by AnimeLogon itself
};

struct Wallpaper {
    Source source = Source::None;
    std::wstring id;         // the wallpaper's id; "default" for the gradient
    // video.mp4 or image.bmp, found administrators-only by LoadWallpaper; a scene's still: still.bmp,
    // or `ownStill` once the overlay has drawn it. Images are held on the device by it.
    std::wstring path;
    std::wstring audioPath;  // a video's audio.wav, when it has sound
    std::wstring sceneDir, videoPath;
    animelogon::skin::Values values;
    int sceneTrialWidth = 0, sceneTrialHeight = 0, sceneFps = 60, sceneScale = 100;
    int sceneWidth = 0, sceneHeight = 0;  // the scene's own size, still.bmp's
    // A scene whose theme gives its settings values: its still drawn with them (wallpaper.h
    // SceneStillPath), which scenestills.h draws. Empty while the scene is shown as it comes.
    std::wstring ownStill;
    animelogon::Scaling fit = animelogon::Scaling::Fill;
    // How bright it is drawn (settings.h kBrightness): the theme's, whatever stands in for it.
    float brightness = animelogon::kBrightness;
    float blur = 0.0f;  // as a percentage of the display's height (blur.h)
    // The height `blur` is a percentage of: 0 for the display's own. When spanning, the primary
    // display's, on every display of the wall, so that the displays match each other -- and the
    // settings app's pictures of the wall (app/src/wall.h WallSigma), which measure it the same way.
    int blurHeight = 0;
};

// The blur's standard deviation in pixels on a display `displayHeight` tall (blur.h).
inline float BlurSigma(const Wallpaper &w, int displayHeight) {
    return animelogon::blur::Sigma(w.blur, w.blurHeight > 0 ? w.blurHeight : displayHeight);
}

struct Component {
    // Instances with the same key are drawn alike wherever they appear: one theme's instance.
    std::wstring key;
    std::wstring instance;   // its id within the theme
    std::wstring component;  // the component it places
    animelogon::skin::Resolved drawing;
};

struct Display {
    std::wstring monitorKey;
    RECT rect{};    // the display, in desktop pixels
    RECT canvas{};  // what the wallpaper is fitted to: the display, or all of them when spanning
    bool primary = false;
    std::wstring themeId;
    Wallpaper wallpaper;
    std::vector<Component> components;  // in the theme's order
    // The components are drawn here. Auto starts on the primary display; the overlay moves
    // them to wherever Windows puts its password box.
    bool showComponents = false;
};

struct Plan {
    std::vector<Display> displays;  // in EnumerateMonitors' order, the primary display first
    animelogon::ComponentDisplays componentDisplays = animelogon::ComponentDisplays::Auto;
    // How every display gives way when woken: as the primary display's theme says, since a wake
    // anywhere takes all of them away together.
    animelogon::WakeStyle wake = animelogon::WakeStyle::Fade;
    float wakeSeconds = animelogon::kWakeSeconds;
    // How scenes are drawn: quality, frame rate caps per power state, video memory.
    animelogon::RenderSettings render;
    std::vector<std::wstring> problems;  // what ResolveTheme replaced or dropped, for the log
};

Plan Build(const animelogon::Settings &settings, const std::vector<animelogon::MonitorInfo> &monitors,
           const animelogon::ThemeStore &store);

// One line for the log: the display, its theme, the wallpaper's kind and id, and how many
// components it has.
std::wstring Describe(const Display &display);

// What the overlay does when wallpaper `id` -- a video or an image -- turns out not to be
// showable after all: every one of `shown` that is that wallpaper becomes the built-in one,
// keeping its fit, so the displays stay covered. Returns the indices it changed.
std::vector<size_t> ShowBuiltInInstead(std::vector<Wallpaper> *shown, const std::wstring &id);

}  // namespace plan
