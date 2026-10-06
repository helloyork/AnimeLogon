// Micula / progress_bar.h

#pragma once

#include "window.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <functional>
#include <string>
#include <vector>

namespace micula {


struct ProgressBar : Widget {
    float value = 0.0f;      // 0..1
    bool  indeterminate = false;

    // WinUI's indeterminate bar, to the numbers -- and it is not "a segment sweeps across".
    // It is **two** bars of different lengths crossing on a 2 s loop, the second staggered
    // three quarters of a second behind the first, so that for half of the cycle one is
    // leaving at the right while the other arrives at the left. That overlap is the whole
    // of the look, and the two lengths are why the bar is not the same shape on the way out
    // as on the way in.
    //
    // From ProgressBar.xaml's Indeterminate storyboard -- 2 s, RepeatBehavior Forever, two
    // TranslateX animations on KeySpline 0.4, 0, 0.6, 1 -- and the widths and positions
    // ProgressBar.cpp hands it through TemplateSettings:
    //
    //   the first   40% of the bar wide, -100% to 300% of its own width, 0 to 1.5 s* , held
    //   the second  60% of the bar wide, -150% to 166% of its own width, 0.75 to 2 s
    //
    // (*held to the end of the cycle, which is what the discrete key frame at 2 s is for.)
    // Multiplied out, they come in at -0.40 and 1.20 of the bar, and -0.90 and 0.996.
    static constexpr float kSweep = 2.0f;
    struct Sweep { float wide, in, out, begin, end; };
    static constexpr Sweep kSweeps[2] = {
        { 0.4f, -1.00f, 3.00f, 0.00f, 1.50f },
        { 0.6f, -1.50f, 1.66f, 0.75f, 2.00f },
    };

    // One bar's left edge at `t` seconds into the cycle, as a fraction of the control's
    // width: held at `in` until its turn comes round, then on cubic-bezier(0.4, 0, 0.6, 1)
    // to `out`.
    static float SweepLeft(const Sweep &s, float t) {
        const float u = std::clamp((t - s.begin) / (s.end - s.begin), 0.0f, 1.0f);
        return s.wide * (s.in + (s.out - s.in) * motion::InOut(u));
    }

    // Self-driving, because the alternative was a page remembering to advance it: the
    // indeterminate bar's sweep was a field nothing ever wrote, so the segment sat
    // still at the left-hand end wherever one was used.
    bool Animating() const override { return Widget::Animating() || indeterminate; }

    // Where in the cycle *now* is, in seconds.
    //
    // Read from the clock rather than counted across frames, and that is the whole of what
    // keeps the sweep from restarting. A ProgressBar is rebuilt for all sorts of reasons --
    // a resize, another control changing the shape of the page, a page that lays itself out
    // in response to a scroll -- and a phase accumulated per frame began the sweep again
    // from the left on each of them, while the operation it was reporting carried on. A
    // periodic animation has nothing to lose by this: the clock belongs to the window, not
    // to the control, and two bars on one page are in step rather than racing.
    static float Cycle() {
        return (float)std::fmod(micula::MonotonicSeconds(), (double)kSweep);
    }

    // No Tick: there is no phase to advance. Plain Widget::Tick runs the pointer fades.

    void Paint(const Painter &p) override {
        const Palette &c = *p.pal;
        const float cy = rect.top + Height(rect) / 2;
        const D2D1_RECT_F track = { rect.left, cy - 1.5f, rect.right, cy + 1.5f };
        p.rt->FillRoundedRectangle(D2D1::RoundedRect(track, 1.5f, 1.5f), p.Brush(c.controlStroke));
        if (indeterminate) {
            const float w = Width(rect);
            const float t = Cycle();
            for (const Sweep &s : kSweeps) {
                const float left = rect.left + SweepLeft(s, t) * w;
                // Cut at the ends of the track: most of each bar's travel is off it, and a
                // bar that is off the end must not be drawn past the corner.
                const D2D1_RECT_F seg = { (std::max)(rect.left, left), track.top,
                                          (std::min)(rect.right, left + s.wide * w), track.bottom };
                if (seg.right > seg.left)
                    p.rt->FillRoundedRectangle(D2D1::RoundedRect(seg, 1.5f, 1.5f), p.Brush(c.accent));
            }
        } else {
            const D2D1_RECT_F fill = { rect.left, track.top,
                                       rect.left + Width(rect) * std::clamp(value, 0.0f, 1.0f),
                                       track.bottom };
            if (fill.right > fill.left)
                p.rt->FillRoundedRectangle(D2D1::RoundedRect(fill, 1.5f, 1.5f), p.Brush(c.accent));
        }
    }
};

}  // namespace micula
