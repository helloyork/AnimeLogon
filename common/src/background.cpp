#include "animelogon/background.h"

#include <cstring>

namespace animelogon::background {
namespace {

uint32_t Crc32(const uint8_t *p, size_t n, uint32_t crc = 0) {
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

void PutU32(std::vector<uint8_t> &out, uint32_t v) {
    out.push_back((uint8_t)(v >> 24));
    out.push_back((uint8_t)(v >> 16));
    out.push_back((uint8_t)(v >> 8));
    out.push_back((uint8_t)v);
}

void Chunk(std::vector<uint8_t> &out, const char type[4], const std::vector<uint8_t> &data) {
    PutU32(out, (uint32_t)data.size());
    const size_t at = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    PutU32(out, Crc32(out.data() + at, out.size() - at));
}

// zlib stream of stored (uncompressed) deflate blocks: no compressor, so no variation.
std::vector<uint8_t> Stored(const std::vector<uint8_t> &raw) {
    std::vector<uint8_t> z = {0x78, 0x01};
    size_t at = 0;
    do {
        const size_t n = raw.size() - at > 65535 ? 65535 : raw.size() - at;
        z.push_back(at + n == raw.size() ? 1 : 0);
        z.push_back((uint8_t)n);
        z.push_back((uint8_t)(n >> 8));
        z.push_back((uint8_t)~n);
        z.push_back((uint8_t)(~n >> 8));
        z.insert(z.end(), raw.begin() + at, raw.begin() + at + n);
        at += n;
    } while (at < raw.size());
    uint32_t a = 1, b = 0;
    for (uint8_t c : raw) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    PutU32(z, (b << 16) | a);
    return z;
}

}  // namespace

std::vector<uint8_t> Render() {
    // Integer arithmetic only, so every compiler produces the same pixels.
    static const int top[3] = {18, 20, 28};
    static const int bottom[3] = {6, 7, 10};
    std::vector<uint8_t> raw;
    raw.reserve((size_t)(kWidth * 3 + 1) * kHeight);
    for (int y = 0; y < kHeight; ++y) {
        raw.push_back(0);  // filter: none
        uint8_t px[3];
        for (int c = 0; c < 3; ++c)
            px[c] = (uint8_t)(top[c] + (bottom[c] - top[c]) * y / (kHeight - 1));
        for (int x = 0; x < kWidth; ++x) raw.insert(raw.end(), px, px + 3);
    }

    std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<uint8_t> ihdr;
    PutU32(ihdr, kWidth);
    PutU32(ihdr, kHeight);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});  // 8-bit RGB, no interlace
    Chunk(png, "IHDR", ihdr);
    Chunk(png, "IDAT", Stored(raw));
    Chunk(png, "IEND", {});
    return png;
}

std::vector<uint8_t> Pixels(int width, int height) {
    if (width < 1 || height < 1) return {};
    // Render()'s colours and arithmetic, with the display's height in place of kHeight.
    static const int top[3] = {18, 20, 28};
    static const int bottom[3] = {6, 7, 10};
    std::vector<uint8_t> out((size_t)width * height * 4);
    for (int y = 0; y < height; ++y) {
        uint8_t px[4] = {0, 0, 0, 255};
        for (int c = 0; c < 3; ++c)
            px[2 - c] = (uint8_t)(height > 1 ? top[c] + (bottom[c] - top[c]) * y / (height - 1) : top[c]);
        uint8_t *row = out.data() + (size_t)y * width * 4;
        for (int x = 0; x < width; ++x) std::memcpy(row + (size_t)x * 4, px, 4);
    }
    return out;
}

}  // namespace animelogon::background
