// A theme's wallpaper blur: a Gaussian whose standard deviation is a percentage of the display's
// height (settings.h kBlurMax), drawn the same way wherever the wallpaper is shown. The logon
// screen does it on the graphics card (overlay/src/gpublur.h); the sign-in background, the other
// displays' stills and the settings app's pictures do it here. Both take the same three steps, so
// the still that covers the logon screen first and the wallpaper that follows it match:
//   1. Down: the picture is cut into cells of `Factor` x `Factor` pixels, each the mean of the
//      pixels it covers (fewer at the right and bottom edges).
//   2. Across: a Gaussian of sigma / Factor cells, kernel radius ceil(3 sigma), along the rows and
//      then the columns, the cells at the edges held for the ones beyond them.
//   3. Up: each pixel bilinear between the centres of the cells around it, held at the edges.
// Working on cells keeps the cost the same whatever the blur, and the cells' Gaussian of two to
// four cells looks like the full one at the display's size.
#pragma once

#include <cstddef>
#include <cstdint>

namespace animelogon::blur {

// The standard deviation in pixels for a theme's blur (a percentage) on a display `height` tall.
inline float Sigma(float percent, int height) { return percent / 100.0f * (float)height; }

// The cell size for a blur of `sigma` pixels: the largest power of two no more than half of it,
// at least 1. The cells' own Gaussian then has a standard deviation of two to four cells.
int Factor(float sigma);

// Blurs `width` x `height` pixels `stride` bytes apart (3 for B G R, 4 for B G R A, whose fourth
// byte is left alone), rows one after another with no padding, by `sigma` pixels. Nothing happens
// for a sigma of zero or less.
void Apply(uint8_t *pixels, int width, int height, int stride, float sigma);

}  // namespace animelogon::blur
