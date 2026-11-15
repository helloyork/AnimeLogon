// The overlay's watchdog: the rule that decides the main thread is stuck. No threads here, so it
// is tested directly; overlay.cpp runs it on a thread of its own every half second.
//
// The main thread beats (writes the tick count) at every step while it is timed. A thread that
// has not beaten for longer than its limit is stuck, and the process is ended so the screen goes
// back to Windows. Two limits: the preparation of an appearance -- covering the screen, opening
// the videos, setting up the components -- may take a while on a slow disk or a busy decoder, so
// it gets a looser one than the frame loop, which turns many times a second.
//
// The tick count runs on while the machine sleeps, and so would the time since the last beat:
// on resume, a round of the watchdog that ran before the main thread did would see seconds or
// hours "without a beat" and end a healthy overlay, leaving Windows' own screen instead of the
// wallpaper. Sleep stops every thread, the watchdog's included, so it shows as a long gap between
// two of the watchdog's own rounds. After such a gap the count starts again from that round.
#pragma once

#include <windows.h>

#include <algorithm>

namespace watchdog {

// How long the main thread may go without a beat.
constexpr ULONGLONG kPrepareMs = 12000;  // while an appearance is being prepared
constexpr ULONGLONG kLiveMs = 5000;      // while the screen is up, and while its videos are closed

// How often the watchdog looks, and the gap between two of its looks that means the machine (or
// at least this process) was stopped in between.
constexpr ULONGLONG kRoundMs = 500;
constexpr ULONGLONG kGapMs = 2000;

enum class Verdict {
    Fine,      // the main thread beat recently enough
    AfterGap,  // this round came long after the last: the count starts again from now
    Stuck,     // no beat for longer than the limit: end the process
};

class Rule {
public:
    // One round at `now`, the main thread's last beat at `beat`, allowed `limit` without one.
    // After a gap, `gapMs` (if given) says how long it was, for the log.
    Verdict Round(ULONGLONG now, ULONGLONG beat, ULONGLONG limit, ULONGLONG *gapMs = nullptr) {
        const bool gap = last_ != 0 && now - last_ > kGapMs;
        if (gap && gapMs) *gapMs = now - last_;
        last_ = now;
        if (gap) {
            since_ = now;
            return Verdict::AfterGap;
        }
        // Counted from the later of the last beat and the last restart of the count.
        const ULONGLONG from = std::max(beat, since_);
        return now > from && now - from > limit ? Verdict::Stuck : Verdict::Fine;
    }

private:
    ULONGLONG last_ = 0;   // when the previous round ran
    ULONGLONG since_ = 0;  // when the count last started again after a gap
};

}  // namespace watchdog
