// The sign-in background follows the wallpaper of the primary display: the first frame of a
// video, an image's own pixels, each laid out as the display shows it, or for the built-in
// wallpaper the very background the installer writes. It is baked into the image Windows draws
// behind its credential screen. Lazily, while the session is in use: every change costs one
// black credential screen, so it is paid once, when nobody is looking, and only when the result
// would differ. A theme whose wallpaper is "none" leaves the background as it is.
#pragma once

#include <string>

namespace bake {

// Bakes and writes the background if the settings now call for a different one. Returns
// true if it wrote. With `preview`, writes the image there instead, unconditionally.
bool Refresh(const std::wstring &preview = {});

}  // namespace bake
