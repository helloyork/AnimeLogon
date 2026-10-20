#include "picture.h"

#include <windows.h>

#include <algorithm>
#include <cmath>

#include "animelogon/background.h"
#include "animelogon/bitmap.h"
#include "animelogon/secure.h"

namespace picture {
namespace {

bool ReadExactly(HANDLE h, uint8_t *to, size_t size) {
    size_t done = 0;
    while (done < size) {
        const DWORD want = (DWORD)std::min<size_t>(size - done, 1u << 24);
        DWORD got = 0;
        if (!ReadFile(h, to + done, want, &got, nullptr) || !got) return false;
        done += got;
    }
    return true;
}

// One channel of `image` at (x, y) in its pixels, filtered as the sampler filters: texel
// centres at half pixels, edges clamped.
float Bilinear(const Image &image, int c, float x, float y) {
    x = std::clamp(x - 0.5f, 0.0f, (float)(image.width - 1));
    y = std::clamp(y - 0.5f, 0.0f, (float)(image.height - 1));
    const int x0 = (int)x, y0 = (int)y, x1 = std::min(x0 + 1, image.width - 1), y1 = std::min(y0 + 1, image.height - 1);
    const float fx = x - (float)x0, fy = y - (float)y0;
    auto at = [&](int xx, int yy) { return (float)image.bgra[((size_t)yy * image.width + xx) * 4 + c]; };
    const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
    const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
    return top + (bottom - top) * fy;
}

}  // namespace

bool ReadImage(const std::wstring &path, Image *out, std::wstring *why) {
    std::wstring reason;
    // Opened once: refusing links, and with writers kept out until it is closed, so the
    // bytes read are the bytes whose size was checked.
    const HANDLE h = animelogon::secure::OpenPlainFile(path, &reason);
    if (h == INVALID_HANDLE_VALUE) {
        *why = L"image.bmp " + reason;
        return false;
    }
    LARGE_INTEGER size{};
    uint8_t header[animelogon::bitmap::kHeaderBytes];
    Image image;
    bool ok = GetFileSizeEx(h, &size) && ReadExactly(h, header, sizeof(header)) &&
              animelogon::bitmap::ReadSize(header, sizeof(header), (uint64_t)size.QuadPart, &image.width,
                                           &image.height);
    if (!ok) {
        *why = L"image.bmp is not a canonical bitmap";
    } else {
        image.bgra.resize((size_t)image.width * image.height * 4);
        ok = ReadExactly(h, image.bgra.data(), image.bgra.size());
        if (!ok) *why = L"image.bmp could not be read";
    }
    CloseHandle(h);
    if (ok) *out = std::move(image);
    return ok;
}

Image GradientColumn(int height) {
    Image column;
    column.width = 1;
    column.height = std::clamp(height, 1, kMaxGradientRows);
    column.bgra = animelogon::background::Pixels(1, column.height);
    return column;
}

std::vector<uint8_t> Compose(const Image &image, const layout::Mapping &m, int windowW, int windowH, int width,
                             int height) {
    std::vector<uint8_t> bgr((size_t)width * height * 3);
    if (image.width <= 0 || image.height <= 0) return bgr;
    const float kx = (float)windowW / (float)width, ky = (float)windowH / (float)height;
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const float px = ((float)x + 0.5f) * kx, py = ((float)y + 0.5f) * ky;
            const float u = px * m.scaleX + m.offsetX, v = py * m.scaleY + m.offsetY;
            uint8_t *out = &bgr[((size_t)y * width + x) * 3];
            if (u < 0 || v < 0 || u > 1 || v > 1) {
                out[0] = out[1] = out[2] = 0;
                continue;
            }
            for (int c = 0; c < 3; ++c)
                out[c] = (uint8_t)std::lround(
                    std::clamp(Bilinear(image, c, u * image.width, v * image.height), 0.0f, 255.0f));
        }
    }
    return bgr;
}

}  // namespace picture
