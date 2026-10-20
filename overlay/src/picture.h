// The pixels of still wallpapers, as the logon screen reads them: an image wallpaper's
// image.bmp, and the built-in gradient. No image decoder runs here: image.bmp is the fixed
// layout of bitmap.h, checked and taken as it is.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "layout.h"

namespace picture {

struct Image {
    int width = 0, height = 0;
    std::vector<uint8_t> bgra;  // width * height * 4 bytes, B G R A, rows top to bottom
};

// Reads an image wallpaper's image.bmp. The caller has already found the file
// administrators-only (LoadWallpaper). It is opened refusing links and keeping writers out
// while it is read, and must be exactly the canonical layout (bitmap::IsCanonical) for its
// real size. False, with `why` for the log, otherwise.
bool ReadImage(const std::wstring &path, Image *out, std::wstring *why);

// The built-in wallpaper for a canvas `height` pixels tall: the gradient only changes down the
// canvas, so one column of it is enough, stretched across. Taller canvases get a column of
// kMaxGradientRows, which the stretch fills just as smoothly.
constexpr int kMaxGradientRows = 8192;
Image GradientColumn(int height);

// The picture a window shows of `image` placed by `m` (layout::Map), at `width` x `height`
// pixels for a window of `windowW` x `windowH`: sampled as the shader samples it, black
// where the image does not reach. B G R, 3 bytes per pixel, rows top to bottom. The same
// input always gives the same bytes.
std::vector<uint8_t> Compose(const Image &image, const layout::Mapping &m, int windowW, int windowH, int width,
                             int height);

}  // namespace picture
