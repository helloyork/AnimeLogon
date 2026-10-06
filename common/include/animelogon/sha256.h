// SHA-256 through Windows CNG, for the hash wallpaper.ini carries: the elevated helper computes
// it from the payload it writes and uses it to find a wallpaper that is already installed.
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace animelogon {

// A hash fed in pieces. Any failure along the way makes Finish return an empty string.
class Sha256 {
public:
    Sha256();
    ~Sha256();
    Sha256(const Sha256 &) = delete;
    Sha256 &operator=(const Sha256 &) = delete;

    void Update(const void *bytes, size_t size);
    // Adds the rest of an open file, from its current position to its end. The position is
    // left at the end. False if the file could not be read.
    bool UpdateFromHandle(HANDLE file);
    // 64 lowercase hex digits, or empty if anything failed. The hash cannot be fed after this.
    std::wstring Finish();

private:
    void *algorithm_ = nullptr;
    void *hash_ = nullptr;
    bool failed_ = false;
};

// The hash of `size` bytes, as 64 lowercase hex digits; empty on failure.
std::wstring Sha256Of(const void *bytes, size_t size);

}  // namespace animelogon
