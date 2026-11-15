// How long a video wallpaper's sound track loops for.
//
// The importer cuts the track out of the source together with the encoder's lead-in and padding
// (AAC adds some 20 to 30 ms), so audio.wav runs a little longer than the picture. Looped at its
// own length the two drift apart by that much every time round: on a 12 s wallpaper the picture
// is most of a second ahead of its sound after five minutes. Looped at the picture's length --
// cut short, or filled out with silence -- they stay together.
#pragma once

#include <cmath>
#include <cstdint>

namespace soundloop {

// Frames in one loop of a track of `fileFrames` frames at `rate`: the video's `videoSeconds`,
// unless that is unknown (0), or so far from the file's own length (over half a second) that the
// two cannot be one recording, when the file's own length stands.
inline uint64_t LoopFrames(uint64_t fileFrames, double videoSeconds, uint32_t rate) {
    if (!(videoSeconds > 0.0)) return fileFrames;
    const double frames = std::round(videoSeconds * rate);
    if (frames < 1.0 || std::fabs(frames - (double)fileFrames) > 0.5 * rate) return fileFrames;
    return (uint64_t)frames;
}

}  // namespace soundloop
