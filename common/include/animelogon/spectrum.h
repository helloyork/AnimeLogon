// What an audio spectrum component shows: the sound the output device is playing, as the levels
// of frequency bands.
//
// The logon screen listens to its output device (overlay/src/audiocapture.h) and once a frame
// takes the spectrum of the latest samples (Analyzer -> Frame). Each <spectrum> in a component
// reads its own bands from that frame, spaced evenly in pitch from its low to its high frequency,
// as loud as its sensitivity says (Targets), and moves toward them as fast as its smoothing allows
// (Bands). Nothing here knows about drawing; previews take a made-up frame (Sample).
#pragma once

#include <cstddef>
#include <vector>

namespace animelogon::spectrum {

// One frame's spectrum: the power at evenly spaced frequencies from 0 up to half the sample rate,
// scaled so that a sine wave at full scale adds up to 1 over the bins it spreads across.
struct Frame {
    std::vector<float> power;  // bin k is at k * binHz
    float binHz = 0.0f;
    bool sound = false;    // something above the noise floor
    double seconds = 0.0;  // when the samples were taken, on a steady clock
};

// Takes the spectrum of a stretch of mono samples, about 85 ms of them: long enough to tell the
// lowest bass notes apart, short enough to follow a beat.
class Analyzer {
public:
    // The samples it wants at `rate` Hz: a power of two.
    static size_t SizeFor(float rate);
    // `samples` are the latest `count` samples, oldest first; fewer than SizeFor(rate) are read as
    // silence before them, more are cut from the front.
    void Run(const float *samples, size_t count, float rate, double seconds, Frame *out);

private:
    std::vector<float> window_, re_, im_;
};

// A made-up frame for previews, shaped like music: strong bass, a lively middle and falling
// treble, uneven from band to band. The same every time.
const Frame &Sample();

// What one spectrum asks of a frame.
struct Shape {
    int bands = 64;
    float low = 40.0f, high = 16000.0f;  // Hz
    float sensitivity = 1.0f;            // a gain: 2 reads every band 6 dB louder
    float smoothing = 0.5f;              // 0 follows at once, 0.95 rises and falls very slowly
};

// Each band's level, 0 (silent, or quieter than the range shown) to 1 (as loud as is shown), in
// a frame. Bands are spaced evenly in octaves, so each covers the same share of pitch; a band's
// level is its power per octave in decibels, read over a fixed range: pink noise, and so roughly
// music, reads level across the bands.
void Targets(const Frame &frame, const Shape &shape, std::vector<float> *levels);

// One spectrum's bands as they move from frame to frame.
class Bands {
public:
    // Moves toward the frame's levels: rising quickly and falling back slowly, both slower with
    // more smoothing. A peak above each band holds a moment, then falls.
    void Follow(const Frame &frame, const Shape &shape);
    // Straight to the frame's levels, peaks a little above them: for previews.
    void Settle(const Frame &frame, const Shape &shape);
    void Reset();

    const std::vector<float> &levels() const { return levels_; }
    const std::vector<float> &peaks() const { return peaks_; }
    // 0 after a moment of silence, 1 while there is sound, easing between them.
    float presence() const { return presence_; }
    // Whether the next frame would look different from this one, so must be drawn.
    bool Moving() const { return moving_; }

private:
    std::vector<float> levels_, peaks_, targets_;
    std::vector<double> peakAt_;
    double at_ = -1.0, heardAt_ = -1.0;
    float presence_ = 0.0f;
    bool moving_ = false;
};

}  // namespace animelogon::spectrum
