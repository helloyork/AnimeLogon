#include "animelogon/blur.h"

#include <algorithm>
#include <cmath>
#include <vector>

namespace animelogon::blur {
namespace {

// One Gaussian pass over the cells, along the rows (`across`) or the columns, edges held.
void Pass(std::vector<float> &cells, int gw, int gh, float sigma, bool across) {
    const int radius = (int)std::ceil(3.0f * sigma);
    std::vector<float> kernel((size_t)(2 * radius + 1));
    float total = 0.0f;
    for (int k = -radius; k <= radius; ++k) total += kernel[(size_t)(k + radius)] = std::exp(-0.5f * (float)(k * k) / (sigma * sigma));
    for (float &k : kernel) k /= total;
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

int Factor(float sigma) {
    int f = 1;
    while ((float)(f * 2) <= sigma / 2.0f && f < 1024) f *= 2;
    return f;
}

void Apply(uint8_t *pixels, int width, int height, int stride, float sigma) {
    if (!(sigma > 0.0f) || width <= 0 || height <= 0 || (stride != 3 && stride != 4)) return;
    const int f = Factor(sigma);
    const int gw = (width + f - 1) / f, gh = (height + f - 1) / f;

    // Down.
    std::vector<float> cells((size_t)gw * gh * 3, 0.0f);
    for (int y = 0; y < height; ++y) {
        const uint8_t *row = pixels + (size_t)y * width * stride;
        float *line = &cells[(size_t)(y / f) * gw * 3];
        for (int x = 0; x < width; ++x)
            for (int c = 0; c < 3; ++c) line[(x / f) * 3 + c] += row[(size_t)x * stride + c];
    }
    for (int gy = 0; gy < gh; ++gy) {
        const int rows = std::min(f, height - gy * f);
        for (int gx = 0; gx < gw; ++gx) {
            const float n = (float)(rows * std::min(f, width - gx * f));
            for (int c = 0; c < 3; ++c) cells[((size_t)gy * gw + gx) * 3 + c] /= n;
        }
    }

    // Across.
    const float cellSigma = sigma / (float)f;
    Pass(cells, gw, gh, cellSigma, true);
    Pass(cells, gw, gh, cellSigma, false);

    // Up.
    for (int y = 0; y < height; ++y) {
        const float fy = std::clamp(((float)y + 0.5f) / (float)f - 0.5f, 0.0f, (float)(gh - 1));
        const int y0 = (int)fy, y1 = std::min(y0 + 1, gh - 1);
        const float ty = fy - (float)y0;
        uint8_t *row = pixels + (size_t)y * width * stride;
        for (int x = 0; x < width; ++x) {
            const float fx = std::clamp(((float)x + 0.5f) / (float)f - 0.5f, 0.0f, (float)(gw - 1));
            const int x0 = (int)fx, x1 = std::min(x0 + 1, gw - 1);
            const float tx = fx - (float)x0;
            for (int c = 0; c < 3; ++c) {
                const float a = cells[((size_t)y0 * gw + x0) * 3 + c], b = cells[((size_t)y0 * gw + x1) * 3 + c];
                const float p = cells[((size_t)y1 * gw + x0) * 3 + c], q = cells[((size_t)y1 * gw + x1) * 3 + c];
                const float top = a + (b - a) * tx, bottom = p + (q - p) * tx;
                row[(size_t)x * stride + c] = (uint8_t)std::clamp((int)std::lround(top + (bottom - top) * ty), 0, 255);
            }
        }
    }
}

}  // namespace animelogon::blur
