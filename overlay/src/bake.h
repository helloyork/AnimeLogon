// The sign-in background follows the video: the first frame the primary display plays, laid
// out as it is played, is baked into the image Windows draws behind its credential screen.
// Lazily, while the session is in use: every change costs one black credential screen, so
// it is paid once, when nobody is looking, and only when the result would differ.
#pragma once

#include <string>

namespace bake {

// Bakes and writes the background if the settings now call for a different one. Returns
// true if it wrote. With `preview`, writes the image there instead, unconditionally.
bool Refresh(const std::wstring &preview = {});

}  // namespace bake
