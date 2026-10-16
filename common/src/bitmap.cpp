#include "animelogon/bitmap.h"

#include <cstring>

namespace animelogon::bitmap {
namespace {

uint32_t U16(const uint8_t *p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8); }
uint32_t U32(const uint8_t *p) { return U16(p) | (U16(p + 2) << 16); }

void Put16(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

void Put32(uint8_t *p, uint32_t v) {
    Put16(p, v & 0xFFFF);
    Put16(p + 2, v >> 16);
}

uint64_t PixelBytes(int width, int height) { return (uint64_t)width * (uint64_t)height * 4; }

}  // namespace

bool FitsLimits(int width, int height) {
    if (width < 1 || height < 1) return false;
    const int longer = width > height ? width : height;
    const int shorter = width > height ? height : width;
    return longer <= kMaxLongSide && shorter <= kMaxShortSide;
}

bool MakeHeader(int width, int height, uint8_t header[kHeaderBytes]) {
    if (!header || !FitsLimits(width, height)) return false;
    const uint32_t pixels = (uint32_t)PixelBytes(width, height);  // at most 132,710,400
    std::memset(header, 0, kHeaderBytes);
    header[0] = 'B';
    header[1] = 'M';
    Put32(header + 2, (uint32_t)kHeaderBytes + pixels);  // bfSize
    Put32(header + 10, (uint32_t)kHeaderBytes);          // bfOffBits
    Put32(header + 14, 40);                              // biSize
    Put32(header + 18, (uint32_t)width);                 // biWidth
    Put32(header + 22, (uint32_t)-height);               // biHeight: negative, top-down
    Put16(header + 26, 1);                               // biPlanes
    Put16(header + 28, 32);                              // biBitCount
    Put32(header + 30, 0);                               // biCompression: BI_RGB
    Put32(header + 34, pixels);                          // biSizeImage
    return true;                                         // resolution and palette fields stay 0
}

std::vector<uint8_t> Build(int width, int height, const uint8_t *bgra, size_t bgraBytes) {
    uint8_t header[kHeaderBytes];
    if (!bgra || !MakeHeader(width, height, header) || bgraBytes != PixelBytes(width, height)) return {};
    std::vector<uint8_t> out(kHeaderBytes + bgraBytes);
    std::memcpy(out.data(), header, kHeaderBytes);
    std::memcpy(out.data() + kHeaderBytes, bgra, bgraBytes);
    for (size_t at = kHeaderBytes + 3; at < out.size(); at += 4) out[at] = 255;
    return out;
}

bool ReadSize(const uint8_t *h, size_t headerBytes, uint64_t fileBytes, int *width, int *height) {
    if (!h || headerBytes < kHeaderBytes || fileBytes <= kHeaderBytes) return false;
    // Signed fields are read as two's complement; only a negative height is accepted.
    const int32_t w = (int32_t)U32(h + 18);
    const int32_t rawHeight = (int32_t)U32(h + 22);
    if (rawHeight >= 0 || rawHeight < -kMaxLongSide) return false;
    const int32_t ht = -rawHeight;
    if (!FitsLimits(w, ht)) return false;
    const uint64_t pixels = PixelBytes(w, ht);
    const bool ok = h[0] == 'B' && h[1] == 'M' && U32(h + 2) == fileBytes && U16(h + 6) == 0 && U16(h + 8) == 0 &&
                    U32(h + 10) == kHeaderBytes && U32(h + 14) == 40 && U16(h + 26) == 1 && U16(h + 28) == 32 &&
                    U32(h + 30) == 0 && U32(h + 34) == pixels && U32(h + 38) == 0 && U32(h + 42) == 0 &&
                    U32(h + 46) == 0 && U32(h + 50) == 0 && fileBytes == kHeaderBytes + pixels;
    if (!ok) return false;
    if (width) *width = w;
    if (height) *height = ht;
    return true;
}

bool IsCanonical(const uint8_t *header, size_t headerBytes, uint64_t fileBytes) {
    return ReadSize(header, headerBytes, fileBytes, nullptr, nullptr);
}

}  // namespace animelogon::bitmap
