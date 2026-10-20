// Pictures of themes for the settings app's page, drawn by the logon screen's own component
// renderer (render/), so that what the page shows is what the logon screen will show. The page
// only lays its marks (selection, anchors) over them.
#pragma once

#include <windows.h>

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "animelogon/settings.h"
#include "animelogon/skinpreview.h"
#include "animelogon/theme.h"

#include "wall.h"

namespace app {

class Library;

// Where a panel or a text of an instance landed, in the picture's pixels.
struct Bound {
    std::wstring instance, key;  // key as adjustments name it: p1, p1.l1.t1
    D2D1_RECT_F rect{};
    size_t screen = 0;  // which of the picture's screens it is on
    // A panel's points held at a left, centre or right anchor and at a top, middle or bottom
    // one (SkinView::Bound).
    bool holds = false;
    float holdX[3]{}, holdY[3]{};
};

// What a picture of a theme shows.
enum class Draw {
    Everything,
    WallpaperOnly,
    // The components alone over nothing, premultiplied: a layer over a wallpaper drawn elsewhere.
    ComponentsOnly,
};

class Pictures {
public:
    explicit Pictures(const Library &library) : library_(library) {}

    // The wallpaper alone, filling a display of `width` x `height` as `fit` says.
    bool Wallpaper(const std::wstring &ref, animelogon::Scaling fit, UINT width, UINT height, animelogon::Picture *out);
    // The theme as a display of that size shows it. With `screens`, the picture is a wall of
    // displays, as spanning shows it: the wallpaper fitted to all of it, the components drawn on
    // each screen that shows them as on a display of that screen's size.
    bool Theme(const animelogon::theme::Theme &theme, UINT width, UINT height, Draw draw, animelogon::Picture *out,
               std::vector<Bound> *bounds = nullptr, const std::vector<Screen> *screens = nullptr);

    // Drops what was read from the stores, after they changed.
    void Forget() { stills_.clear(); }

private:
    // The wallpaper's own picture at no more than 1920 pixels wide: an image wallpaper's
    // pixels, a video's first frame. False for the built-in ones and for one that cannot be read.
    const animelogon::Picture *Still(const std::wstring &id);

    const Library &library_;
    std::map<std::wstring, animelogon::Picture> stills_;  // by wallpaper id; empty when unreadable
    animelogon::SkinPreview preview_;
};

// The picture as a BMP file: 32-bit, top-down, opaque.
std::vector<uint8_t> Bmp(const animelogon::Picture &picture);
// A picture with premultiplied alpha as a PNG file, which browsers show with its alpha. Empty
// if it cannot be encoded.
std::vector<uint8_t> Png(const animelogon::Picture &premultiplied);

// The signed-in person's regional format, as the logon screen will read it from their profile.
animelogon::RegionalFormat UserFormat();

}  // namespace app
