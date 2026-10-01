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

}  // namespace animelogon::background
