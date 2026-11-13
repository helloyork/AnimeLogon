// The sign-in background follows the wallpaper of the primary display: the first frame of a
// video, an image's own pixels, each laid out as the display shows it, or for the built-in
// wallpaper the very background the installer writes; each at the theme's brightness, the
// built-in one drawn afresh when that is below 1. It is baked into the image Windows draws
// behind its credential screen. Lazily, while the session is in use: every change costs one
// black credential screen, so it is paid once, when nobody is looking, and only when the result
// would differ. A theme whose wallpaper is "none" leaves the background as it is.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "animelogon/settings.h"

namespace bake {

// Bakes and writes the background if the settings now call for a different one. Returns
// true if it wrote. With `preview`, writes the image there instead, unconditionally.
bool Refresh(const std::wstring &preview = {});

// The first frame of `video` as the display `rect` shows it, fitted to `canvas` by `fit`, at
// the display's size: what the background would be if that display were the primary. B G R,
// 3 bytes a pixel, rows top to bottom. The caller has found the file administrators-only.
bool FirstFrameOn(const std::wstring &video, const RECT &canvas, const RECT &rect, animelogon::Scaling fit,
                  std::vector<uint8_t> *bgr);

}  // namespace bake
