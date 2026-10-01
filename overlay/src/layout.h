// Where the video sits on a display: the mapping from a window pixel to video UV.
#pragma once

#include <windows.h>

#include <algorithm>

#include "animelogon/settings.h"

namespace layout {

struct Mapping {
    float scaleX = 0, scaleY = 0;
    float offsetX = 0, offsetY = 0;
};

// `canvas` is what the video is fitted to (one display, or the bounds of all of them for
// spanning); `window` is the display this window covers. Both in desktop pixels.
inline Mapping Map(int videoW, int videoH, const RECT &canvas, const RECT &window, animelogon::Scaling scaling) {
    Mapping m;
    const float cw = (float)(canvas.right - canvas.left), ch = (float)(canvas.bottom - canvas.top);
    if (videoW <= 0 || videoH <= 0 || cw <= 0 || ch <= 0) return m;
    float w = cw, h = ch;
    if (scaling != animelogon::Scaling::Stretch) {
        const float sx = cw / (float)videoW, sy = ch / (float)videoH;
        const float s = scaling == animelogon::Scaling::Fill ? std::max(sx, sy) : std::min(sx, sy);
        w = (float)videoW * s;
        h = (float)videoH * s;
    }
    // The video's top-left corner, relative to the window.
    const float left = (float)(canvas.left - window.left) + (cw - w) * 0.5f;
    const float top = (float)(canvas.top - window.top) + (ch - h) * 0.5f;
    m.scaleX = 1.0f / w;
    m.scaleY = 1.0f / h;
    m.offsetX = -left / w;
    m.offsetY = -top / h;
    return m;
}

}  // namespace layout
