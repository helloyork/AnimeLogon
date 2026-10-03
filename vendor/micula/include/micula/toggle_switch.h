// Micula / toggle_switch.h

#pragma once

#include "window.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <functional>
#include <string>
#include <vector>

namespace micula {


// The same boolean as a CheckBox, and the reason both exist is what the two say
// about *when* the change lands. Windows uses a switch for a setting that takes
// effect immediately and a checkbox for one that is collected now and applied on OK:
// a settings page is switches, an installer's "also do this during the install"
// options are checkboxes. Getting that round the wrong way is not a bug anybody
// reports, and it is the difference between a window that feels native and one that
// feels close.
struct ToggleSwitch : Widget {
    std::wstring label;
    std::wstring detail;
    bool on = false;
    std::function<void(bool)> onChange;
    // 0..1, animated. A switch that snaps is the single most obvious tell that a
    // control was drawn rather than inherited -- and it is one of the few things WinUI
    // really does animate, with a storyboard rather than a brush transition.
    //
    // Two of them, for the two things that move and are not the same thing: `knob` is where
    // the knob *is*, which is the pointer's while a hand is on it, and `fill` is what the
    // switch *is*, which is the value's. One value driving both is what a switch looks like
    // when the colour follows the hand -- the moment it changed cannot be seen.
    motion::Track knob;
    motion::Track fill;
    // How much of the hold is a *drag* rather than a press. A click presses the knob, which
    // Fluent draws as wider and shorter; a drag carries it, and a squash says "pushed against
    // something", which is the one thing a knob under a hand is not doing. This is the
    // handover between the two, so that a grab turns the squash into a round knob instead of
    // a shape changing between two frames.
    motion::Track carry;

    ToggleSwitch(std::wstring text, bool value, std::function<void(bool)> f)
        : label(std::move(text)), on(value), onChange(std::move(f)),
          knob(value ? 1.0f : 0.0f), fill(value ? 1.0f : 0.0f) {}

    bool Focusable() const override { return true; }
    void OnClick() override {
        if (!enabled) return;
        // A drag that has already chosen is not a click as well.
        if (moved) { moved = false; return; }
        Set(!on, true);
    }

    // --- dragging the knob ---------------------------------------------------------
    //
    // The track is 40 by 20 and its knob travels the middle 20 of it, here and in Paint:
    // named because the pointer has to be turned into a position with the same numbers.
    static constexpr float kW = 40.0f, kH = 20.0f, kTravel = 20.0f;
    // Where the state flips while the knob is being dragged: two points rather than one, so
    // that a hand resting on the middle of the track -- or a drag crossing it twice in a
    // second -- does not fire `onChange` twice for one gesture. The middle is a place with
    // no answer, which is what it is for, and past either of these the switch has really
    // chosen.
    static constexpr float kFlipOn = 0.6f, kFlipOff = 0.4f;
    // How far from the knob's centre a press still counts as taking hold of it: the knob
    // itself, and a little for a hand. A press anywhere else is a click.
    static constexpr float kGrab = 10.0f;

    bool dragging = false;
    bool moved = false;      // the drag changed the state, so the release is not a click too
    float grabAt = 0.0f;     // the knob's position when it was taken hold of
    float grabX = 0.0f;      // and where the pointer was, so it does not jump under the hand

    bool TracksPointer() const override { return dragging; }
    bool PressedVisual() const override { return pressed || dragging; }
    float KnobX() const { return rect.right - kW + 10.0f + knob.value * kTravel; }

    void Set(bool value, bool tell) {
        if (on == value) return;
        on = value;
        if (!tell) return;
        if (owner) StartAnimation(owner);
        if (onChange) onChange(on);
    }
    // The knob at `k` (0..1), and the state that follows from where it has got to.
    void DragTo(float k) {
        knob.Set(std::clamp(k, 0.0f, 1.0f));
        if (k >= kFlipOn) Set(true, true);
        else if (k <= kFlipOff) Set(false, true);
        if (owner) owner->Invalidate();
    }
    void OnPress(float x, float y) override {
        moved = false;
        if (!enabled) return;
        // Only a press on the knob itself takes hold of it. Anywhere else -- the rest of the
        // track, the words -- is a click, and a click must not move the knob before the
        // release: pressing the empty end of the track used to throw the knob across to it.
        const float cy = rect.top + (Height(rect) - kH) / 2 + kH / 2;
        const float cx = KnobX();
        if (!Inside({ cx - kGrab, cy - kGrab, cx + kGrab, cy + kGrab }, x, y)) return;
        dragging = true;
        grabAt = knob.value;
        grabX = x;
    }
    void OnDrag(float x, float /*y*/) override {
        if (!dragging) return;
        moved = true;          // a hand that moved is not a click, whatever the knob did
        // Followed by the distance the pointer moved rather than jumped to where it is, so
        // that a knob taken hold of by its edge stays where it was taken hold of.
        DragTo(grabAt + (x - grabX) / kTravel);
    }
    void OnRelease() override {
        if (!dragging) return;
        dragging = false;
        // The knob is left where the hand let go of it: Tick takes it to the end the state
        // came out on, which is the animation that says the switch has settled.
        if (owner) owner->Invalidate();
    }
    bool Animating() const override {
        return Widget::Animating() || knob.Wants(on ? 1.0f : 0.0f) ||
               fill.Wants(on ? 1.0f : 0.0f) || carry.Wants(dragging ? 1.0f : 0.0f);
    }
    void Tick(float dt) override {
        Widget::Tick(dt);
        // Was `knob += 0.18f` per tick, which made the switch's speed a function of how
        // busy the message queue was. micula::motion says why that had to go.
        //
        // The fill is the value's, and runs whether or not a hand has the knob: that is what
        // separates "the switch turned on" from "the knob is over there", and what keeps the
        // moment of the change visible while the knob is being dragged.
        fill.To(on ? 1.0f : 0.0f);
        fill.Step(dt, motion::kFast);
        carry.To(dragging ? 1.0f : 0.0f);
        carry.Step(dt, motion::kFaster);
        // The knob is the pointer's while it is held, and Tick must not pull it the other way.
        if (dragging) return;
        knob.To(on ? 1.0f : 0.0f);
        knob.Step(dt, motion::kFast);
    }

    void Paint(const Painter &p) override {
        const Palette &c = *p.pal;
        const float w = kW, h = kH;
        const float y = rect.top + (Height(rect) - h) / 2;
        // The switch sits on the right of the row, which is where Windows' own
        // settings put it -- label left, control right, aligned down the page.
        const D2D1_RECT_F track = { rect.right - w, y, rect.right, y + h };

        // The track's colour and the knob's are the *value's*: `fill` crosses over on its own,
        // at the length the knob's own move takes, so the two are one move of the switch and
        // the flip is visible even while the knob is following a hand.
        const float f = fill.value;
        const D2D1_COLOR_F offBg = Mix(c.controlBg, c.controlBgHover, hoverT);
        const D2D1_COLOR_F onBg  = Mix(Mix(c.accent, c.accentHover, hoverT),
                                       c.accentPressed, pressT);
        p.FillRound(track, h / 2, !enabled ? c.controlBg : Mix(offBg, onBg, f));
        if (!enabled || f < 1.0f)
            p.StrokeRound(track, h / 2, Fade(c.controlStroke, enabled ? 1.0f - f : 1.0f));

        // Fluent's knob grows under the pointer and squashes while it is held -- wider
        // than it is tall, as if it were being pushed against the track. Two radii and
        // two lerps, which is the whole of it. A drag is not a press, though: the squash
        // gives way to a round knob, a little larger, for as long as the hand is on it.
        const float press = pressT * (1.0f - carry.value);
        const float rx = 6.0f + hoverT + press + carry.value * 1.5f;
        const float ry = 6.0f + hoverT - press + carry.value * 1.5f;
        const float cx = KnobX();
        const D2D1_COLOR_F knobColor = !enabled ? c.textDisabled
                                                : Mix(c.textSecondary, c.accentText, f);
        p.rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, y + h / 2), rx, ry),
                          p.Brush(knobColor));

        if (focus && owner && owner->showFocusRing) {
            const D2D1_RECT_F o = { track.left - 3, track.top - 3, track.right + 3, track.bottom + 3 };
            p.StrokeRound(o, h / 2 + 3, c.textPrimary, 2.0f);
        }

        // An empty label means the caller is drawing the words itself -- which is what
        // a settings card does, so that the title, the one-line description and the
        // control all sit on a grid the page owns rather than one each control invents.
        if (label.empty()) return;

        const D2D1_COLOR_F fg = enabled ? c.textPrimary : c.textDisabled;
        const float right = track.left - 16;
        if (detail.empty()) {
            p.Text(label, { rect.left, rect.top, right, rect.bottom }, p.font->body, fg);
        } else {
            const float half = Height(rect) / 2;
            p.Text(label, { rect.left, rect.top, right, rect.top + half }, p.font->body, fg);
            p.TextWrapped(detail, { rect.left, rect.top + half + 1, right, rect.bottom },
                          p.font->caption, c.textSecondary, false, true);
        }
    }
};

}  // namespace micula
