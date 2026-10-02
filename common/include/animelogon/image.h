// Turns a picture the user chose into the pixels of an image wallpaper, ready for
// bitmap::Build. WIC decodes it: png, jpg, webp, bmp, gif, tiff, and heic or anything else
// for which a codec is installed.
//
// Runs only in the settings app, as the signed-in user. The logon screen (SYSTEM) never
// calls this: an image decoder parsing bytes from anywhere is exactly what must not run
// there. It reads only the canonical bitmap this output becomes (bitmap.h).
//
// What it does:
//   - takes the first frame of an animated or multi-page file; a GIF frame smaller than
//     its canvas is placed on the canvas as the format says;
//   - turns the picture upright by its EXIF orientation (and HEIF's equivalent);
//   - flattens any transparency over black, so every pixel is opaque;
//   - scales down, never up, with a high-quality filter, so the longer side is at most
//     7680 and the shorter at most 4320 (bitmap.h's limits). Colour profiles are not
//     applied: pixels are taken as sRGB.
// What it refuses, before decoding any pixels:
//   - a source file over kMaxSourceBytes;
//   - a frame (or GIF canvas) wider or taller than kMaxSourceSide, or with more than
//     kMaxSourcePixels pixels.
//
// Initialises COM on the calling thread for the duration of the call if it is not
// initialised yet, so it may be called from any thread.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace animelogon::image {

constexpr uint64_t kMaxSourceBytes = 256ull * 1024 * 1024;
constexpr uint32_t kMaxSourceSide = 65535;
constexpr uint64_t kMaxSourcePixels = 128ull * 1024 * 1024;  // for example 16384 x 8192

enum class Status {
    Ok,
    CannotOpen,     // the file is missing, unreadable or in use
    TooLarge,       // over kMaxSourceBytes
    NotAnImage,     // no installed codec recognises it
    TooManyPixels,  // over kMaxSourceSide or kMaxSourcePixels
    Failed,         // recognised, but decoding or scaling failed
};

struct Picture {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> bgra;  // width * height * 4 bytes, rows top to bottom, A = 255
};

// The size a width x height picture is scaled to: the same size when it fits the limits,
// otherwise the largest size with the same aspect ratio (rounded, at least 1) that does.
void FitSize(uint32_t width, uint32_t height, int *outWidth, int *outHeight);

// Decodes the file at `path`. Writers are kept out of it while it is read. On failure
// `out` is left empty and `why` says what happened, in one line for the log.
Status Normalize(const std::wstring &path, Picture *out, std::wstring *why);

// The same for a file already in memory.
Status NormalizeBytes(const uint8_t *bytes, size_t size, Picture *out, std::wstring *why);

}  // namespace animelogon::image
