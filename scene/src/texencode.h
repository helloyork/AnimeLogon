// Turns a decoded picture into the library's texture file (animelogon/texture.h): runs only inside
// the sandboxed renderer while a scene is imported.
#pragma once

#include <d3d11.h>
#include <cstdint>
#include <string>
#include <vector>

namespace animelogon::scene {

enum class TextureKind {
    Color,  // BC7, premultiplied alpha
    Mask,   // BC4: how strongly an effect applies, from the picture's brightness times its alpha
    Exact,  // uncompressed RGBA, premultiplied alpha
};

// `bgra` is straight-alpha B8G8R8A8, `stride` bytes per row, as WIC decodes pictures. The size is
// padded with transparent pixels to a multiple of 4 and every mipmap level down to 1x1 is made.
// BC7 is encoded on `device`'s graphics card; with no device, on WARP.
bool EncodeTexture(ID3D11Device *device, const uint8_t *bgra, uint32_t width, uint32_t height, size_t stride,
                   TextureKind kind, std::vector<uint8_t> *out, std::wstring *why);

}  // namespace animelogon::scene
