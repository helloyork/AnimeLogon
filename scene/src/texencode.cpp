#include "texencode.h"
#include <wrl/client.h>
#include <DirectXTex.h>
#include <algorithm>
#include <cstring>
#include "animelogon/texture.h"

namespace animelogon::scene {
namespace {
using Microsoft::WRL::ComPtr;

bool Fail(std::wstring *why, const std::wstring &s) { if (why) *why = s; return false; }

// One level of the chain, before compression: premultiplied RGBA8, or one byte per pixel for a mask.
struct Plane {
    uint32_t width = 0, height = 0, channels = 4;
    std::vector<uint8_t> pixels;
};

// Halves a level by averaging 2x2 blocks; an odd last row or column is averaged with itself.
Plane Half(const Plane &p) {
    Plane h;
    h.width = std::max(1u, p.width / 2);
    h.height = std::max(1u, p.height / 2);
    h.channels = p.channels;
    h.pixels.resize((size_t)h.width * h.height * h.channels);
    for (uint32_t y = 0; y < h.height; ++y)
        for (uint32_t x = 0; x < h.width; ++x)
            for (uint32_t c = 0; c < h.channels; ++c) {
                const uint32_t x0 = std::min(p.width - 1, x * 2), x1 = std::min(p.width - 1, x * 2 + 1);
                const uint32_t y0 = std::min(p.height - 1, y * 2), y1 = std::min(p.height - 1, y * 2 + 1);
                auto at = [&](uint32_t px, uint32_t py) { return (uint32_t)p.pixels[((size_t)py * p.width + px) * p.channels + c]; };
                h.pixels[((size_t)y * h.width + x) * h.channels + c] = (uint8_t)((at(x0, y0) + at(x1, y0) + at(x0, y1) + at(x1, y1) + 2) / 4);
            }
    return h;
}

DirectX::Image View(const Plane &p) {
    DirectX::Image image{};
    image.width = p.width;
    image.height = p.height;
    image.format = p.channels == 4 ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R8_UNORM;
    image.rowPitch = (size_t)p.width * p.channels;
    image.slicePitch = image.rowPitch * p.height;
    image.pixels = const_cast<uint8_t *>(p.pixels.data());
    return image;
}
}  // namespace

bool EncodeTexture(ID3D11Device *device, const uint8_t *bgra, uint32_t width, uint32_t height, size_t stride,
                   TextureKind kind, std::vector<uint8_t> *out, std::wstring *why) {
    if (!bgra || width < 1 || height < 1 || width > texture::kMaxSide || height > texture::kMaxSide || stride < (size_t)width * 4)
        return Fail(why, L"picture size out of range");
    texture::Info info;
    info.format = kind == TextureKind::Color ? texture::Format::Bc7 : kind == TextureKind::Mask ? texture::Format::Bc4 : texture::Format::Rgba8;
    info.usedWidth = width;
    info.usedHeight = height;
    info.width = (width + 3) & ~3u;
    info.height = (height + 3) & ~3u;
    info.levels = texture::FullChain(info.width, info.height);
    info.premultiplied = kind != TextureKind::Mask;

    Plane top;
    top.width = info.width;
    top.height = info.height;
    top.channels = kind == TextureKind::Mask ? 1 : 4;
    top.pixels.assign((size_t)top.width * top.height * top.channels, 0);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t *row = bgra + (size_t)y * stride;
        for (uint32_t x = 0; x < width; ++x) {
            const uint8_t b = row[x * 4], g = row[x * 4 + 1], r = row[x * 4 + 2], a = row[x * 4 + 3];
            uint8_t *dst = &top.pixels[((size_t)y * top.width + x) * top.channels];
            if (kind == TextureKind::Mask) {
                dst[0] = (uint8_t)(((r + g + b) * a + 382) / 765);
            } else {
                dst[0] = (uint8_t)((r * a + 127) / 255);
                dst[1] = (uint8_t)((g * a + 127) / 255);
                dst[2] = (uint8_t)((b * a + 127) / 255);
                dst[3] = a;
            }
        }
    }

    ComPtr<ID3D11Device> warp;
    if (kind == TextureKind::Color && !device) {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &warp, nullptr, nullptr)))
            return Fail(why, L"no device to compress textures on");
        device = warp.Get();
    }

    const auto layout = texture::Layout(info);
    std::vector<uint8_t> file = texture::Header(info);
    file.resize(layout.back().offset + layout.back().bytes);
    Plane level = std::move(top);
    for (uint32_t i = 0; i < info.levels; ++i) {
        const texture::Level &l = layout[i];
        if (level.width != l.width || level.height != l.height) return Fail(why, L"mipmap chain out of step");
        uint8_t *dst = file.data() + l.offset;
        if (kind == TextureKind::Exact) {
            std::memcpy(dst, level.pixels.data(), l.bytes);
        } else {
            DirectX::ScratchImage packed;
            HRESULT hr;
            // The graphics card's encoder is far faster on whole pictures; the small tail of the chain
            // is quicker on the processor than a round trip to the card.
            if (kind == TextureKind::Color && l.width >= 16 && l.height >= 16)
                hr = DirectX::Compress(device, View(level), DXGI_FORMAT_BC7_UNORM, DirectX::TEX_COMPRESS_DEFAULT, 1.0f, packed);
            else
                hr = DirectX::Compress(View(level), kind == TextureKind::Color ? DXGI_FORMAT_BC7_UNORM : DXGI_FORMAT_BC4_UNORM,
                                       kind == TextureKind::Color ? DirectX::TEX_COMPRESS_BC7_QUICK : DirectX::TEX_COMPRESS_DEFAULT,
                                       DirectX::TEX_THRESHOLD_DEFAULT, packed);
            if (FAILED(hr)) return Fail(why, L"texture compression failed");
            const DirectX::Image *p = packed.GetImage(0, 0, 0);
            if (!p || p->rowPitch != l.rowPitch || p->slicePitch != l.bytes) return Fail(why, L"compressed level has an unexpected size");
            std::memcpy(dst, p->pixels, l.bytes);
        }
        if (i + 1 < info.levels) level = Half(level);
    }
    *out = std::move(file);
    return true;
}

}  // namespace animelogon::scene
