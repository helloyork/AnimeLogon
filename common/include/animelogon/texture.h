// A scene's picture as it is kept in the library: made once, when the scene is imported, so that
// loading it later is only a copy to the graphics card and no image decoder ever runs on what a
// scene brings. A small fixed header, then every mipmap level, largest first, tightly packed.
//
// Checking a file needs nothing but its header and its length, so the elevated side can check
// every texture of a scene without decoding anything.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace animelogon::texture {

enum class Format : uint32_t {
    Bc7 = 1,    // colour with alpha, 16 bytes per 4x4 block
    Bc4 = 2,    // one channel (masks), 8 bytes per 4x4 block
    Rgba8 = 3,  // uncompressed, for pictures marked exact
};
constexpr uint32_t kMagic = 0x58544C41;  // "ALTX"
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxSide = 8192;
constexpr size_t kHeaderBytes = 40;

struct Info {
    Format format = Format::Bc7;
    uint32_t width = 0, height = 0;          // the stored size, each a multiple of 4
    uint32_t usedWidth = 0, usedHeight = 0;  // the picture inside it, from the top left
    uint32_t levels = 1;
    bool premultiplied = true;  // colour already multiplied by alpha
};

struct Level {
    uint32_t width = 0, height = 0;  // in pixels
    uint32_t rowPitch = 0, rows = 0; // bytes per row of blocks (or pixels), and how many rows
    size_t offset = 0, bytes = 0;    // where it is in the file
};

// The number of levels down to 1x1.
uint32_t FullChain(uint32_t width, uint32_t height);
// Where each level of a texture with this info lies in its file.
std::vector<Level> Layout(const Info &info);
std::vector<uint8_t> Header(const Info &info);
// Checks the header and that the file is exactly as long as its levels.
bool Read(const uint8_t *bytes, size_t size, Info *info, std::vector<Level> *levels, std::wstring *why);

}  // namespace animelogon::texture
