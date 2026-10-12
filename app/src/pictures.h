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

namespace app {

class Library;

// Where a panel or a text of an instance landed, in the picture's pixels.
struct Bound {
    std::wstring instance, key;  // key as adjustments name it: p1, p1.l1.t1
    D2D1_RECT_F rect{};
};

class Pictures {
public:
    explicit Pictures(const Library &library) : library_(library) {}

    // The wallpaper alone, filling a display of `width` x `height` as `fit` says.
    bool Wallpaper(const std::wstring &ref, animelogon::Scaling fit, UINT width, UINT height, animelogon::Picture *out);
    // The theme as a display of that size shows it. With `components` false, the wallpaper only.
    bool Theme(const animelogon::theme::Theme &theme, UINT width, UINT height, bool components,
               animelogon::Picture *out, std::vector<Bound> *bounds = nullptr);

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

// The signed-in person's regional format, as the logon screen will read it from their profile.
animelogon::RegionalFormat UserFormat();

}  // namespace app
