// Where the video sits on a display: the mapping from a window pixel to video UV.
#pragma once

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <vector>

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

// Which of the displays get a window: those with an area. Each window keeps the index of its own
// display, so that it is drawn with that display's picture, canvas and components even when an
// earlier one was skipped.
inline std::vector<size_t> WithArea(const std::vector<RECT> &rects) {
    std::vector<size_t> out;
    for (size_t i = 0; i < rects.size(); ++i)
        if (rects[i].right > rects[i].left && rects[i].bottom > rects[i].top) out.push_back(i);
    return out;
}

// How a window's content moves on its way out when woken, in the window's own pixels: grown by
// `scale` about the middle of the canvas, and moved up by `rise` of the canvas's height (-1 is the
// whole canvas up). The canvas is the display itself, or when spanning the whole wall: then every
// display of the wall grows about one point and moves the same distance, and the wall leaves as
// one picture instead of coming apart at the seams.
struct Leave {
    float scale = 1.0f;
    float centreX = 0.0f, centreY = 0.0f;  // what it grows about
    float dy = 0.0f;                       // how far it has moved down (negative: up)
};

inline Leave LeaveOf(const RECT &window, const RECT &canvas, float scale, float rise) {
    Leave l;
    l.scale = scale;
    l.centreX = (float)(canvas.left + canvas.right) / 2.0f - (float)window.left;
    l.centreY = (float)(canvas.top + canvas.bottom) / 2.0f - (float)window.top;
    const float rows = (float)(canvas.bottom - canvas.top);
    const float d = rise * rows;
    l.dy = d < 0.0f ? -std::floor(-d + 0.5f) : std::floor(d + 0.5f);
    return l;
}

}  // namespace layout
