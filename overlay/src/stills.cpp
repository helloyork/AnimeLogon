#include "stills.h"

#include <algorithm>
#include <cmath>

#include "layout.h"
#include "picture.h"

namespace stills {
namespace {

// Displays larger than this are left black rather than holding hundreds of megabytes.
constexpr uint64_t kMaxPixels = 64ull * 1024 * 1024;
// About how many cells tall the blur's grid is, whatever the display's height.
constexpr int kGridRows = 128;

int Width(const RECT &r) { return r.right - r.left; }
int Height(const RECT &r) { return r.bottom - r.top; }

// A value in (-1, 1), most often near 0, the same for the same pixel every time: the sum of two
// uniform values, a grain without a pattern the eye can follow.
float Grain(int x, int y) {
    auto hash = [](uint32_t n) {
        n ^= n >> 16;
        n *= 0x7FEB352Du;
        n ^= n >> 15;
        n *= 0x846CA68Bu;
        n ^= n >> 16;
        return n;
    };
    const uint32_t a = hash((uint32_t)x * 0x9E3779B1u ^ (uint32_t)y * 0x85EBCA77u);
    const uint32_t b = hash(a ^ 0x27D4EB2Fu);
    return (float)(a >> 8) / 16777216.0f + (float)(b >> 8) / 16777216.0f - 1.0f;
}

// One pass of a Gaussian across `cells` (3 floats a cell), along rows when `across`, else down
// columns; the edges are held, so the borders do not darken.
void Pass(std::vector<float> &cells, int gw, int gh, const std::vector<float> &kernel, bool across) {
    const int radius = (int)kernel.size() / 2;
    std::vector<float> out(cells.size());
    const int lines = across ? gh : gw, length = across ? gw : gh;
    for (int line = 0; line < lines; ++line) {
        for (int at = 0; at < length; ++at) {
            float sum[3] = {};
            for (int k = -radius; k <= radius; ++k) {
                const int p = std::clamp(at + k, 0, length - 1);
                const size_t cell = across ? (size_t)line * gw + p : (size_t)p * gw + line;
                const float weight = kernel[(size_t)(k + radius)];
                for (int c = 0; c < 3; ++c) sum[c] += cells[cell * 3 + c] * weight;
            }
            const size_t cell = across ? (size_t)line * gw + at : (size_t)at * gw + line;
            for (int c = 0; c < 3; ++c) out[cell * 3 + c] = sum[c];
        }
    }
    cells.swap(out);
}

}  // namespace

std::vector<Still> Choose(const plan::Plan &plan, animelogon::OtherDisplays mode) {
    std::vector<Still> out;
    if (mode == animelogon::OtherDisplays::Black || plan.displays.size() < 2) return out;
    // EnumerateMonitors puts the primary first; should none say it is, the first is taken for it.
    const bool named = std::any_of(plan.displays.begin(), plan.displays.end(), [](const plan::Display &d) { return d.primary; });
    for (size_t i = 0; i < plan.displays.size(); ++i) {
        const plan::Display &d = plan.displays[i];
        if (d.primary || (!named && i == 0) || d.wallpaper.source == plan::Source::None) continue;
        if (Width(d.rect) <= 0 || Height(d.rect) <= 0 || (uint64_t)Width(d.rect) * Height(d.rect) > kMaxPixels) continue;
        out.push_back({d.monitorKey, d.rect, d.canvas, d.wallpaper});
    }
    return out;
}

std::vector<uint8_t> BuiltIn(const Still &still) {
    const int w = Width(still.rect), h = Height(still.rect);
    // As the overlay draws it: one column as tall as the canvas, stretched across it.
    const picture::Image column = picture::GradientColumn(Height(still.canvas));
    const layout::Mapping m = layout::Map(column.width, column.height, still.canvas, still.rect, animelogon::Scaling::Stretch);
    return picture::Compose(column, m, w, h, w, h);
}

bool Compose(const Still &still, std::vector<uint8_t> *bgr, std::wstring *why) {
    const int w = Width(still.rect), h = Height(still.rect);
    if (w <= 0 || h <= 0) return *why = L"the display has no size", false;
    switch (still.wallpaper.source) {
    case plan::Source::Image:
    case plan::Source::Scene: {
        // A scene's still.bmp is read as an image wallpaper's image.bmp is.
        picture::Image image;
        if (!picture::ReadImage(still.wallpaper.path, &image, why)) return false;
        const layout::Mapping m = layout::Map(image.width, image.height, still.canvas, still.rect, still.wallpaper.fit);
        *bgr = picture::Compose(image, m, w, h, w, h);
        return true;
    }
    case plan::Source::Gradient:
        *bgr = BuiltIn(still);
        return true;
    case plan::Source::Video:
        *why = L"it is a video";
        return false;
    case plan::Source::None:
        break;
    }
    *why = L"it has no wallpaper";
    return false;
}

void Finish(const std::vector<uint8_t> &bgr, int width, int height, animelogon::OtherDisplays mode, uint8_t *bgra) {
    const size_t n = (size_t)width * height;
    if (bgr.size() < n * 3) return;
    for (size_t i = 0; i < n; ++i) {
        bgra[i * 4 + 0] = bgr[i * 3 + 0];
        bgra[i * 4 + 1] = bgr[i * 3 + 1];
        bgra[i * 4 + 2] = bgr[i * 3 + 2];
        bgra[i * 4 + 3] = 255;
    }
    if (mode == animelogon::OtherDisplays::Blur) Soften(width, height, bgra);
}

void Tone(const float bgr[3], float out[3]) {
    const float luma = 0.0722f * bgr[0] + 0.7152f * bgr[1] + 0.2126f * bgr[2];
    for (int c = 0; c < 3; ++c) out[c] = kLevel * (luma + kSaturation * (bgr[c] - luma)) + kLift;
}

void Soften(int width, int height, uint8_t *bgra) {
    if (width <= 0 || height <= 0) return;
    // Down: each cell the mean of the f x f pixels it covers (fewer at the right and bottom).
    const int f = std::max(1, height / kGridRows);
    const int gw = (width + f - 1) / f, gh = (height + f - 1) / f;
    std::vector<float> cells((size_t)gw * gh * 3, 0.0f);
    for (int y = 0; y < height; ++y) {
        const uint8_t *row = bgra + (size_t)y * width * 4;
        float *line = &cells[(size_t)(y / f) * gw * 3];
        for (int x = 0; x < width; ++x)
            for (int c = 0; c < 3; ++c) line[(x / f) * 3 + c] += row[x * 4 + c];
    }
    for (int gy = 0; gy < gh; ++gy) {
        const int rows = std::min(f, height - gy * f);
        for (int gx = 0; gx < gw; ++gx) {
            const float n = (float)(rows * std::min(f, width - gx * f));
            for (int c = 0; c < 3; ++c) cells[((size_t)gy * gw + gx) * 3 + c] /= n;
        }
    }

    // Across: a Gaussian of kBlurSigma times the height, in cells.
    const float sigma = std::max(0.5f, kBlurSigma * (float)height / (float)f);
    const int radius = (int)std::ceil(3.0f * sigma);
    std::vector<float> kernel((size_t)(2 * radius + 1));
    float total = 0.0f;
    for (int k = -radius; k <= radius; ++k) total += kernel[(size_t)(k + radius)] = std::exp(-0.5f * k * k / (sigma * sigma));
    for (float &k : kernel) k /= total;
    Pass(cells, gw, gh, kernel, true);
    Pass(cells, gw, gh, kernel, false);

    // Up: bilinear between cell centres, toned, with the grain.
    for (int y = 0; y < height; ++y) {
        const float fy = std::clamp(((float)y + 0.5f) / (float)f - 0.5f, 0.0f, (float)(gh - 1));
        const int y0 = (int)fy, y1 = std::min(y0 + 1, gh - 1);
        const float ty = fy - (float)y0;
        uint8_t *row = bgra + (size_t)y * width * 4;
        for (int x = 0; x < width; ++x) {
            const float fx = std::clamp(((float)x + 0.5f) / (float)f - 0.5f, 0.0f, (float)(gw - 1));
            const int x0 = (int)fx, x1 = std::min(x0 + 1, gw - 1);
            const float tx = fx - (float)x0;
            float v[3], toned[3];
            for (int c = 0; c < 3; ++c) {
                const float a = cells[((size_t)y0 * gw + x0) * 3 + c], b = cells[((size_t)y0 * gw + x1) * 3 + c];
                const float p = cells[((size_t)y1 * gw + x0) * 3 + c], q = cells[((size_t)y1 * gw + x1) * 3 + c];
                const float top = a + (b - a) * tx, bottom = p + (q - p) * tx;
                v[c] = top + (bottom - top) * ty;
            }
            Tone(v, toned);
            const float grain = kGrain * Grain(x, y);
            for (int c = 0; c < 3; ++c) row[x * 4 + c] = (uint8_t)std::clamp((int)std::lround(toned[c] + grain), 0, 255);
            row[x * 4 + 3] = 255;
        }
    }
}

}  // namespace stills
