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
    Gradient,  // the built-in wallpaper, drawn by AnimeLogon itself
};

struct Wallpaper {
    Source source = Source::None;
    std::wstring id;         // the wallpaper's id; "default" for the gradient
    std::wstring path;       // video.mp4 or image.bmp, found administrators-only by LoadWallpaper
    std::wstring audioPath;  // a video's audio.wav, when it has sound
    animelogon::Scaling fit = animelogon::Scaling::Fill;
};

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
