#include "animelogon/spectrum.h"

#include <algorithm>
#include <cmath>

namespace animelogon::spectrum {
namespace {

constexpr double kPi = 3.14159265358979323846;
// The range a band's level is read over, in decibels of power per octave relative to a sine wave
// at full scale filling one octave. Measured over anime wallpapers' sound tracks, music mastered
// at an ordinary loudness spends half its time above -28 dB in most bands and peaks near -13;
// quiet ambient tracks sit 15 dB lower. Below the floor a band is at rest.
constexpr float kFloorDb = -54.0f, kCeilingDb = -14.0f;
// Music has less power the higher it goes. Above 1 kHz each octave is read this much louder, so
// that the treble moves as much as the middle does.
constexpr float kTrebleLiftDb = 3.0f;
// Below this a frame is silence: about -90 dBFS.
constexpr float kSilence = 1e-9f;
// How long silence lasts before a spectrum counts as silent, and how quickly it fades either way.
constexpr double kSilentAfter = 0.8;
constexpr float kPresenceSeconds = 0.25f;
// A peak stays where it was for a moment, then falls at this many levels a second.
constexpr double kPeakHold = 0.35;
constexpr float kPeakFall = 0.9f;
// Changes smaller than this would not move a pixel on any display.
constexpr float kStill = 0.002f;

// In place, radix 2: `re` and `im` hold n values, n a power of two.
void Transform(std::vector<float> &re, std::vector<float> &im) {
    const size_t n = re.size();
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            std::swap(re[i], re[j]);
            std::swap(im[i], im[j]);
        }
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double angle = -2.0 * kPi / (double)len;
        const float wr = (float)std::cos(angle), wi = (float)std::sin(angle);
        for (size_t i = 0; i < n; i += len) {
            float cr = 1.0f, ci = 0.0f;
            for (size_t k = 0; k < len / 2; ++k) {
                const size_t a = i + k, b = a + len / 2;
                const float br = re[b] * cr - im[b] * ci, bi = re[b] * ci + im[b] * cr;
                re[b] = re[a] - br;
                im[b] = im[a] - bi;
                re[a] += br;
                im[a] += bi;
                const float next = cr * wr - ci * wi;
                ci = cr * wi + ci * wr;
                cr = next;
            }
        }
    }
}

// The bands' edges in Hz, given the frame: from low to high, both kept below half the sample
// rate, and high above low.
void Edges(const Frame &frame, const Shape &shape, float *low, float *high) {
    const float nyquist = frame.binHz * (float)(frame.power.empty() ? 0 : frame.power.size() - 1);
    *low = std::clamp(shape.low, 10.0f, std::max(10.0f, nyquist / 2.0f));
    *high = std::clamp(shape.high, *low * 1.5f, std::max(*low * 1.5f, nyquist));
}

}  // namespace

size_t Analyzer::SizeFor(float rate) {
    size_t n = 1024;
    while (n < 16384 && (float)n < rate * 0.085f) n <<= 1;
    return n;
}

void Analyzer::Run(const float *samples, size_t count, float rate, double seconds, Frame *out) {
    const size_t n = SizeFor(rate);
    if (window_.size() != n) {
        window_.resize(n);
        for (size_t i = 0; i < n; ++i) window_[i] = (float)(0.5 - 0.5 * std::cos(2.0 * kPi * (double)i / (double)n));
    }
    re_.assign(n, 0.0f);
    im_.assign(n, 0.0f);
    const size_t take = std::min(count, n);
    for (size_t i = 0; i < take; ++i) {
        const size_t at = n - take + i;
        re_[at] = samples[count - take + i] * window_[at];
    }
    Transform(re_, im_);
    // A Hann window passes half a sine's amplitude into its bin, and spreads its power over an
    // equivalent 1.5 bins: a sine of amplitude a sums to a*a.
    const float scale = 1.0f / (1.5f * (float)n * (float)n / 16.0f);
    out->power.resize(n / 2 + 1);
    float total = 0.0f;
    for (size_t k = 0; k <= n / 2; ++k) {
        out->power[k] = (re_[k] * re_[k] + im_[k] * im_[k]) * scale;
        total += out->power[k];
    }
    out->binHz = rate / (float)n;
    out->sound = total > kSilence;
    out->seconds = seconds;
}

const Frame &Sample() {
    static const Frame frame = [] {
        Frame f;
        constexpr size_t n = 4096;
        f.binHz = 48000.0f / (float)n;
        f.power.assign(n / 2 + 1, 0.0f);
        f.sound = true;
        for (size_t k = 1; k <= n / 2; ++k) {
            const double hz = k * (double)f.binHz, octave = std::log2(hz);
            // Power per octave, in decibels: a bass bump, a slope down through the treble, a lift
            // where voices are, and some unevenness so neighbouring bands differ.
            double db = -21.0 - 3.0 * std::max(0.0, std::log2(hz / 150.0)) - 7.0 * std::max(0.0, std::log2(60.0 / hz));
            db += 4.0 * std::exp(-std::pow(std::log2(hz / 2000.0), 2.0) / 0.8);
            db += 4.5 * std::sin(octave * 7.1 + 1.3) + 2.5 * std::sin(octave * 13.7 + 0.4) + 1.5 * std::sin(octave * 29.3);
            // Into each bin's share: an octave at hz spans hz * ln 2 / binHz bins.
            f.power[k] = (float)(std::pow(10.0, db / 10.0) * f.binHz / (hz * std::log(2.0)));
        }
        return f;
    }();
    return frame;
}

void Targets(const Frame &frame, const Shape &shape, std::vector<float> *levels) {
    const int bands = std::clamp(shape.bands, 1, 256);
    levels->assign((size_t)bands, 0.0f);
    if (frame.power.size() < 2 || frame.binHz <= 0.0f || !frame.sound) return;
    float low = 0.0f, high = 0.0f;
    Edges(frame, shape, &low, &high);
    // The power between two frequencies: the bins joined by straight lines, and the area under
    // them. A band narrower than a bin -- the bass, mostly -- so takes its share of the bins it
    // falls between, rather than all of one bin's or none.
    const std::vector<float> &p = frame.power;
    std::vector<double> before(p.size(), 0.0);  // the area from bin 0 to bin k
    for (size_t k = 1; k < p.size(); ++k) before[k] = before[k - 1] + 0.5 * ((double)p[k - 1] + (double)p[k]);
    auto area = [&](double x) {
        x = std::clamp(x, 0.0, (double)(p.size() - 1));
        const size_t i = std::min((size_t)x, p.size() - 2);
        const double t = x - (double)i;
        return before[i] + t * p[i] + 0.5 * t * t * ((double)p[i + 1] - (double)p[i]);
    };
    const double span = std::log2((double)high / (double)low), octaves = span / bands;
    const double gain = 20.0 * std::log10(std::max(0.01f, shape.sensitivity));
    for (int i = 0; i < bands; ++i) {
        const double f0 = low * std::exp2(span * i / bands), f1 = low * std::exp2(span * (i + 1) / bands);
        const double power = std::max(0.0, area(f1 / frame.binHz) - area(f0 / frame.binHz));
        const double lift = kTrebleLiftDb * std::max(0.0, std::log2(std::sqrt(f0 * f1) / 1000.0));
        const double db = 10.0 * std::log10(power / octaves + 1e-30) + gain + lift;
        (*levels)[(size_t)i] = std::clamp((float)((db - kFloorDb) / (kCeilingDb - kFloorDb)), 0.0f, 1.0f);
    }
}

void Bands::Reset() {
    levels_.clear();
    peaks_.clear();
    targets_.clear();
    peakAt_.clear();
    at_ = heardAt_ = -1.0;
    presence_ = 0.0f;
    moving_ = false;
}

void Bands::Settle(const Frame &frame, const Shape &shape) {
    Targets(frame, shape, &targets_);
    levels_ = targets_;
    peaks_.resize(levels_.size());
    for (size_t i = 0; i < levels_.size(); ++i) peaks_[i] = std::min(1.0f, levels_[i] * 1.05f + 0.05f);
    peakAt_.assign(levels_.size(), frame.seconds);
    at_ = frame.seconds;
    heardAt_ = frame.sound ? frame.seconds : -1.0;
    presence_ = frame.sound ? 1.0f : 0.0f;
    moving_ = false;
}

void Bands::Follow(const Frame &frame, const Shape &shape) {
    Targets(frame, shape, &targets_);
    if (levels_.size() != targets_.size()) {
        levels_.assign(targets_.size(), 0.0f);
        peaks_.assign(targets_.size(), 0.0f);
        peakAt_.assign(targets_.size(), frame.seconds);
    }
    // A frame long after the last -- the display was off -- moves as far as a quarter second would.
    const double now = frame.seconds;
    const float dt = at_ < 0.0 ? 0.0f : (float)std::clamp(now - at_, 0.0, 0.25);
    at_ = now;
    const float s = std::clamp(shape.smoothing, 0.0f, 0.99f);
    const float rise = 1.0f - std::exp(-dt / (0.01f + 0.12f * s * s));
    const float fall = 1.0f - std::exp(-dt / (0.03f + 0.9f * s * std::sqrt(s)));
    bool moving = false;
    for (size_t i = 0; i < levels_.size(); ++i) {
        float &level = levels_[i];
        const float target = targets_[i];
        level += (target - level) * (target > level ? rise : fall);
        if (std::fabs(target - level) < kStill) level = target;
        else moving = true;
        float &peak = peaks_[i];
        if (level >= peak) {
            peak = level;
            peakAt_[i] = now;
        } else if (now - peakAt_[i] > kPeakHold) {
            peak = std::max(level, peak - kPeakFall * dt);
        }
        if (peak > level) moving = true;
    }
    if (frame.sound) heardAt_ = now;
    const float wanted = heardAt_ >= 0.0 && now - heardAt_ < kSilentAfter ? 1.0f : 0.0f;
    presence_ += (wanted - presence_) * (1.0f - std::exp(-dt / kPresenceSeconds));
    if (std::fabs(wanted - presence_) < kStill) presence_ = wanted;
    else moving = true;
    // Silence that has not yet counted as silence still has to be watched for.
    moving_ = moving || (wanted > 0.0f && !frame.sound);
}

}  // namespace animelogon::spectrum
