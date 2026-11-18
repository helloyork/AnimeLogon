#include "animelogon/texture.h"
#include <algorithm>

namespace animelogon::texture {
namespace {
void Put(std::vector<uint8_t> &out, uint32_t n) {
    for (int i = 0; i < 4; ++i) out.push_back((uint8_t)(n >> (8 * i)));
}
uint32_t U32(const uint8_t *p) { return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
bool Fail(std::wstring *why, const wchar_t *s) { if (why) *why = s; return false; }
}  // namespace

uint32_t FullChain(uint32_t width, uint32_t height) {
    uint32_t n = 1, side = std::max(width, height);
    while (side > 1) { side >>= 1; ++n; }
    return n;
}

std::vector<Level> Layout(const Info &info) {
    std::vector<Level> out;
    size_t offset = kHeaderBytes;
    for (uint32_t i = 0; i < info.levels; ++i) {
        Level l;
        l.width = std::max(1u, info.width >> i);
        l.height = std::max(1u, info.height >> i);
        if (info.format == Format::Rgba8) {
            l.rowPitch = l.width * 4;
            l.rows = l.height;
        } else {
            l.rowPitch = std::max(1u, (l.width + 3) / 4) * (info.format == Format::Bc7 ? 16 : 8);
            l.rows = std::max(1u, (l.height + 3) / 4);
        }
        l.offset = offset;
        l.bytes = (size_t)l.rowPitch * l.rows;
        offset += l.bytes;
        out.push_back(l);
    }
    return out;
}

std::vector<uint8_t> Header(const Info &info) {
    std::vector<uint8_t> out;
    Put(out, kMagic);
    Put(out, kVersion);
    Put(out, (uint32_t)info.format);
    Put(out, info.width);
    Put(out, info.height);
    Put(out, info.usedWidth);
    Put(out, info.usedHeight);
    Put(out, info.levels);
    Put(out, info.premultiplied ? 1u : 0u);
    Put(out, 0);
    return out;
}

bool Read(const uint8_t *bytes, size_t size, Info *info, std::vector<Level> *levels, std::wstring *why) {
    if (!bytes || size < kHeaderBytes) return Fail(why, L"texture too short");
    if (U32(bytes) != kMagic || U32(bytes + 4) != kVersion) return Fail(why, L"not a texture");
    Info in;
    const uint32_t format = U32(bytes + 8);
    if (format < 1 || format > 3) return Fail(why, L"unknown texture format");
    in.format = (Format)format;
    in.width = U32(bytes + 12);
    in.height = U32(bytes + 16);
    in.usedWidth = U32(bytes + 20);
    in.usedHeight = U32(bytes + 24);
    in.levels = U32(bytes + 28);
    const uint32_t flags = U32(bytes + 32);
    if (in.width < 4 || in.height < 4 || in.width > kMaxSide || in.height > kMaxSide || in.width % 4 || in.height % 4)
        return Fail(why, L"bad texture size");
    if (in.usedWidth < 1 || in.usedHeight < 1 || in.usedWidth > in.width || in.usedHeight > in.height ||
        in.width - in.usedWidth >= 4 || in.height - in.usedHeight >= 4)
        return Fail(why, L"bad picture size");
    if (in.levels < 1 || in.levels > FullChain(in.width, in.height)) return Fail(why, L"bad level count");
    if (flags > 1 || U32(bytes + 36) != 0) return Fail(why, L"unknown texture flags");
    in.premultiplied = flags == 1;
    auto layout = Layout(in);
    if (layout.back().offset + layout.back().bytes != size) return Fail(why, L"texture length does not match");
    if (info) *info = in;
    if (levels) *levels = std::move(layout);
    return true;
}

}  // namespace animelogon::texture
