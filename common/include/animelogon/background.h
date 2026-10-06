// The sign-in background behind Windows' credential UI. Its bytes never change between
// versions (any change costs a black frame), hence a fixed encoder rather than a codec.
#pragma once

#include <cstdint>
#include <vector>

namespace animelogon::background {

constexpr int kWidth = 320;
constexpr int kHeight = 180;

// A PNG of a dark vertical gradient, identical on every build and architecture.
std::vector<uint8_t> Render();

// The same gradient at any size, as width * height * 4 bytes, B G R A per pixel (A = 255),
// rows top to bottom: the built-in wallpaper, drawn at the size of the display it fills.
// At kWidth x kHeight it is exactly the picture Render() encodes. Empty for a size below 1.
std::vector<uint8_t> Pixels(int width, int height);

}  // namespace animelogon::background
