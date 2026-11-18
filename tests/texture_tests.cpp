// The library's texture files (animelogon/texture.h) and the encoder that makes them at import
// (scene/src/texencode.h), on WARP so the result does not depend on this machine's graphics card.
#include "check.h"
#include <DirectXTex.h>
#include <cmath>
#include "animelogon/texture.h"
#include "texencode.h"
using namespace animelogon;

namespace {
// A picture with a smooth gradient, hard-edged shapes and a soft transparent border.
std::vector<uint8_t> Picture(uint32_t w, uint32_t h) {
    std::vector<uint8_t> p((size_t)w * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint8_t *px = &p[((size_t)y * w + x) * 4];
            const bool disc = (x - w / 2.0) * (x - w / 2.0) + (y - h / 2.0) * (y - h / 2.0) < (h / 3.0) * (h / 3.0);
            px[0] = (uint8_t)(disc ? 40 : 255 * x / w);   // B
            px[1] = (uint8_t)(disc ? 200 : 128);          // G
            px[2] = (uint8_t)(disc ? 230 : 255 * y / h);  // R
            px[3] = (uint8_t)std::min<uint32_t>(255, std::min({x, y, w - 1 - x, h - 1 - y}) * 16);
        }
    return p;
}
double Psnr(const uint8_t *a, const uint8_t *b, uint32_t w, uint32_t h, size_t pitchA, size_t pitchB, int channels) {
    double se = 0;
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w * (uint32_t)channels; ++x) {
            const double d = (double)a[y * pitchA + x] - b[y * pitchB + x];
            se += d * d;
        }
    const double mse = se / ((double)w * h * channels);
    return mse == 0 ? 99 : 10 * std::log10(255.0 * 255.0 / mse);
}
}  // namespace

TEST(TextureFilesAreCheckedFromTheirHeaderAndLength) {
    texture::Info info;
    info.width = 300; info.height = 200; info.usedWidth = 299; info.usedHeight = 197;
    info.levels = texture::FullChain(300, 200);
    CHECK(info.levels == 9);
    const auto layout = texture::Layout(info);
    CHECK(layout[0].rowPitch == 75 * 16 && layout[0].rows == 50 && layout[8].width == 1 && layout[8].bytes == 16);
    std::vector<uint8_t> file = texture::Header(info);
    file.resize(layout.back().offset + layout.back().bytes);
    texture::Info read; std::wstring why;
    CHECK(texture::Read(file.data(), file.size(), &read, nullptr, &why));
    CHECK(read.usedWidth == 299 && read.levels == 9 && read.premultiplied);
    std::vector<uint8_t> shorter(file.begin(), file.end() - 1), longer = file;
    longer.push_back(0);
    CHECK(!texture::Read(shorter.data(), shorter.size(), nullptr, nullptr, &why));
    CHECK(!texture::Read(longer.data(), longer.size(), nullptr, nullptr, &why));
    auto broken = [&](size_t at, uint8_t value) { auto f = file; f[at] = value; return !texture::Read(f.data(), f.size(), nullptr, nullptr, &why); };
    CHECK(broken(0, 'X'));     // magic
    CHECK(broken(8, 9));       // format
    CHECK(broken(12, 2));      // width not a multiple of 4
    CHECK(broken(20, 0));      // used width far below the width (296 + 4)
    CHECK(broken(28, 10));     // more levels than the chain has
    CHECK(broken(32, 2));      // unknown flag
}

TEST(PicturesBecomeBc7AndMasksBc4WithEveryMipmap) {
    const uint32_t w = 301, h = 203;
    const auto pic = Picture(w, h);
    std::vector<uint8_t> file; std::wstring why;
    CHECK(scene::EncodeTexture(nullptr, pic.data(), w, h, w * 4, scene::TextureKind::Color, &file, &why));
    texture::Info info; std::vector<texture::Level> levels;
    CHECK(texture::Read(file.data(), file.size(), &info, &levels, &why));
    CHECK(info.format == texture::Format::Bc7 && info.width == 304 && info.height == 204 && info.usedWidth == w && info.levels == 9);
    if (levels.empty()) return;

    // Decoded, the top level is the premultiplied picture within BC7's usual error.
    DirectX::Image bc{};
    bc.width = info.width; bc.height = info.height; bc.format = DXGI_FORMAT_BC7_UNORM;
    bc.rowPitch = levels[0].rowPitch; bc.slicePitch = levels[0].bytes; bc.pixels = file.data() + levels[0].offset;
    DirectX::ScratchImage decoded;
    CHECK(SUCCEEDED(DirectX::Decompress(bc, DXGI_FORMAT_R8G8B8A8_UNORM, decoded)));
    std::vector<uint8_t> expected((size_t)w * h * 4);
    for (size_t i = 0; i < (size_t)w * h; ++i) {
        const uint8_t *s = &pic[i * 4];
        expected[i * 4 + 0] = (uint8_t)((s[2] * s[3] + 127) / 255);
        expected[i * 4 + 1] = (uint8_t)((s[1] * s[3] + 127) / 255);
        expected[i * 4 + 2] = (uint8_t)((s[0] * s[3] + 127) / 255);
        expected[i * 4 + 3] = s[3];
    }
    const DirectX::Image *d = decoded.GetImage(0, 0, 0);
    CHECK(d && Psnr(expected.data(), d->pixels, w, h, w * 4, d->rowPitch, 4) > 40);
    // The padding is transparent.
    CHECK(d && d->pixels[(size_t)(info.height - 1) * d->rowPitch + (info.width - 1) * 4 + 3] == 0);

    std::vector<uint8_t> mask;
    CHECK(scene::EncodeTexture(nullptr, pic.data(), w, h, w * 4, scene::TextureKind::Mask, &mask, &why));
    CHECK(texture::Read(mask.data(), mask.size(), &info, nullptr, &why) && info.format == texture::Format::Bc4 && !info.premultiplied);

    std::vector<uint8_t> exact;
    CHECK(scene::EncodeTexture(nullptr, pic.data(), w, h, w * 4, scene::TextureKind::Exact, &exact, &why));
    CHECK(texture::Read(exact.data(), exact.size(), &info, &levels, &why) && info.format == texture::Format::Rgba8);
    if (!levels.empty()) CHECK(Psnr(expected.data(), exact.data() + levels[0].offset, w, h, w * 4, levels[0].rowPitch, 4) == 99);

    CHECK(!scene::EncodeTexture(nullptr, pic.data(), 0, h, w * 4, scene::TextureKind::Color, &file, &why));
    CHECK(!scene::EncodeTexture(nullptr, pic.data(), 9000, 4, 9000 * 4, scene::TextureKind::Color, &file, &why));
}
