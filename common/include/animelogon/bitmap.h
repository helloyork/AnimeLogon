// image.bmp: the on-disk form of an image wallpaper. The settings app decodes the user's
// picture (image.h) and stores the pixels in this one fixed layout, so the logon screen,
// which runs as SYSTEM, reads them without any image decoder: it checks the header and
// takes the bytes after it as they are. The same idea as storing sound as canonical WAV.
//
// The layout, and nothing else:
//   BITMAPFILEHEADER, 14 bytes: "BM", bfSize = the file's size, both reserved fields 0,
//                               bfOffBits = 54.
//   BITMAPINFOHEADER, 40 bytes: biSize 40, biWidth = width (> 0), biHeight = -height
//                               (negative: the first row is the top one), biPlanes 1,
//                               biBitCount 32, biCompression BI_RGB, biSizeImage = width *
//                               height * 4, every other field 0.
//   width * height * 4 bytes of pixels, B G R A per pixel, rows top to bottom, no padding
//   (a row of 32-bit pixels is always a multiple of 4 bytes). A is 255 in every pixel the
//   writer produces; a reader draws the image opaque whatever the fourth byte says.
// The file is exactly 54 + width * height * 4 bytes long.
//
// Limits: width and height at least 1, the longer side at most 7680 and the shorter at
// most 4320 (8K UHD, either way round). The largest file is about 127 MiB.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace animelogon::bitmap {

constexpr size_t kHeaderBytes = 54;
constexpr int kMaxLongSide = 7680;
constexpr int kMaxShortSide = 4320;

// True for a width and height the format allows.
bool FitsLimits(int width, int height);

// The 54-byte header for a width x height image. False, with `header` untouched, for a size
// outside the limits.
bool MakeHeader(int width, int height, uint8_t header[kHeaderBytes]);

// The whole file for `bgra` (width * height * 4 bytes, rows top to bottom), with A set to
// 255 in every pixel. Empty for a size outside the limits or the wrong number of bytes.
// The same pixels always give the same bytes.
std::vector<uint8_t> Build(int width, int height, const uint8_t *bgra, size_t bgraBytes);

// Is `header` (the first `headerBytes` bytes of a file `fileBytes` long) exactly the
// layout above, with the file's size matching what the header declares?
bool IsCanonical(const uint8_t *header, size_t headerBytes, uint64_t fileBytes);

// The width and height of a header IsCanonical accepts. False, with both left alone, for
// any other.
bool ReadSize(const uint8_t *header, size_t headerBytes, uint64_t fileBytes, int *width, int *height);

}  // namespace animelogon::bitmap
