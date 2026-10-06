// Micula / scroll_bar.h

#pragma once

#include "window.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <functional>
#include <string>
#include <vector>

namespace micula {


// A vertical scroll bar, and it is WinUI's rather than an impression of one: every number
// below is out of WinUI 2's ScrollBar template (ScrollBar_themeresources.xaml) or the
// ScrollViewer and RepeatButton code that drives it.
//
// Twelve DIPs wide (ScrollBarSize), in three states:
//
//   hidden     nothing. (NoIndicator)
//   indicator  a 2-DIP line and nothing else. It appears the moment the pointer moves
//              over the page or the page scrolls, and goes 2 s after both have stopped.
//              (MouseIndicator; ScrollViewerSeparatorContractDelay)
//   expanded   the track, the two arrows and a 6-DIP thumb. 400 ms after the pointer
//              comes onto the bar, and back to the line 500 ms after it leaves.
//              (ConsciousStates; ScrollBarExpandBeginTime, ScrollBarContractBeginTime)
//
// The line and the thumb are one shape. The template's Thumb is a Rectangle with a
// *transparent 6-DIP stroke* round its fill, 8 wide and shifted 2 right when collapsed,
// 12 wide and unshifted when expanded -- so what shows is 2 wide, then 6, growing out of
// a right edge that does not move. That is what Paint draws, rather than two widths.
//
// Unless "Always show scrollbars" is on (Settings > Accessibility > Visual effects):
// then WinUI's ScrollViewer is not "conscious", and the bar is simply always expanded.
//
// What it does is RepeatButton's and Thumb's. An arrow scrolls 16 DIPs
// (ScrollViewerLineDelta) and the track a viewport, once on press, again after 250 ms
// and every 50 ms after that *while the pointer is still on what was pressed* -- which
// is what stops a held track at the pointer instead of at the end of the page. The thumb
// moves the page in proportion to the room it has to travel.
struct ScrollBar : Widget {
    static constexpr float kSize     = 12.0f;   // ScrollBarSize
    static constexpr float kThumbMin = 30.0f;   // ScrollBarVerticalThumbMinHeight
    static constexpr float kLine     = 16.0f;   // ScrollViewerLineDelta
    static constexpr ULONGLONG kExpandDelay = 400, kContractDelay = 500, kHideDelay = 2000;

    enum class Part { None, Up, Down, PageUp, PageDown, Thumb };

    // What the page says on every layout, in DIPs.
    float viewport = 0.0f;   // how much of the page is visible
    float extent   = 0.0f;   // how tall the whole page is
    float value    = 0.0f;   // where it has been scrolled to
    float drawn    = 0.0f;   // where it is drawn, which trails `value` through a glide
    // The scrolling part of the window. The pointer moving anywhere over it shows the bar.
    // Handed to the window as this control's ExternalRegion, which is where a move over
    // the page is delivered from: not a hit-test area, so a click here is a click on the
    // page.
    D2D1_RECT_F area = {};
    // Asked to scroll the page to `to`. `glide` is false for the thumb only: a dragged
    // thumb has to stay under the pointer, not catch up with it.
    std::function<void(float to, bool glide)> onScroll;
    // Read once, when the bar is made.
    bool autoHide = SystemAutoHidesScrollBars();
    // The bar's own two timers, out of the window's pool: the moment one of the three states is
    // next due to change, and the repeat of an arrow button that is held. See Timer.
    Timer stateTimer, repeatTimer;

    Part  grab = Part::None;             // what the button went down on
    float px = -1.0f, py = -1.0f;        // the pointer, from the last message that had it
    float grabY = 0.0f, grabValue = 0.0f;
    bool  repeating = false;             // past RepeatButton's Delay, on to its Interval
    bool  shown = false, expanded = false, held = false;
    ULONGLONG activeAt = 0, heldAt = 0, armedFor = 0;
    float showT = 0.0f;                  // the thumb's opacity
    float fadeT = 0.0f;                  // the track's and the arrows' opacity
    motion::Track sizeT;                 // 0 the line, 1 the expanded thumb

    explicit ScrollBar(std::function<void(float, bool)> f) : onScroll(std::move(f)) {}

    // The page scrolled, or the pointer moved over it: show the line, and start its 2 s
    // again.
    void Wake() { activeAt = GetTickCount64(); shown = true; }

    // --- geometry, in window DIPs ------------------------------------------------
    float MaxValue() const { return (std::max)(0.0f, extent - viewport); }
    float TrackTop() const { return rect.top + kSize; }
    float TrackLength() const { return (std::max)(0.0f, Height(rect) - 2 * kSize); }
    // A track too short for the smallest thumb has none, which is the template's answer too.
    bool  HasThumb() const { return TrackLength() >= kThumbMin; }
    float ThumbLength() const {
        const float t = TrackLength();
        const float l = extent > 0.0f ? t * viewport / extent : t;
        return std::clamp(l, kThumbMin, (std::max)(kThumbMin, t));
    }
    float ThumbTop(float at) const {
        const float m = MaxValue(), room = TrackLength() - ThumbLength();
        return TrackTop() + (m > 0.0f && room > 0.0f ? room * std::clamp(at / m, 0.0f, 1.0f)
                                                     : 0.0f);
    }
    // Against the thumb where it is drawn, because that is what the pointer is aimed at.
    Part PartAt(float y) const {
        if (y < rect.top + kSize) return Part::Up;
        if (y >= rect.bottom - kSize) return Part::Down;
        if (!HasThumb()) return y < TrackTop() + TrackLength() / 2 ? Part::PageUp : Part::PageDown;
        const float t = ThumbTop(drawn);
        if (y < t) return Part::PageUp;
        return y < t + ThumbLength() ? Part::Thumb : Part::PageDown;
    }

    // --- input --------------------------------------------------------------------
    bool TracksPointer() const override { return hover; }   // the arrow under it lights
    D2D1_RECT_F ExternalRegion() const override { return area; }

    void OnPointerMove(float x, float y) override {
        // Both places a move can arrive from -- over the bar itself and over the page it
        // scrolls -- are places the bar should be out for, so there is nothing to test.
        px = x; py = y;
        Wake();
        Poll();
    }
    void OnPress(float x, float y) override {
        px = x; py = y;
        grab = PartAt(y);
        Wake();
        if (grab == Part::Thumb) {
            grabY = y;
            grabValue = drawn;
        } else {
            repeating = false;
            Step(false);
            // RepeatButton.Delay's default; RepeatTick takes over from here at its Interval.
            repeatTimer.Start(owner, 250, [this] { RepeatTick(); });
        }
        Poll();
    }
    void OnDrag(float x, float y) override {
        px = x; py = y;
        const float room = TrackLength() - ThumbLength();
        if (grab != Part::Thumb || room <= 0.0f || !onScroll) return;
        onScroll(std::clamp(grabValue + (y - grabY) / room * MaxValue(), 0.0f, MaxValue()),
                 false);
    }
    void OnRelease() override {
        repeatTimer.Stop();
        grab = Part::None;
        Poll();
    }
    // The held arrow button repeating: an arrow that did nothing would be an arrow that is
    // broken, so the first tick also steps. The template's Interval is 50 ms.
    void RepeatTick() {
        if (grab == Part::None || grab == Part::Thumb) { repeatTimer.Stop(); return; }
        if (!repeating) {
            repeating = true;
            repeatTimer.Start(owner, 50, [this] { RepeatTick(); });
        }
        Step(true);
    }
    // The moment one of the three states was due to change, which is what the bar arms itself
    // a timer for rather than running frames for two seconds.
    void StateTick() {
        stateTimer.Stop();
        armedFor = 0;
        Poll();
        if (owner) owner->Invalidate();
    }

    // One click of whatever was pressed. `repeat` is the timer's, and RepeatButton only
    // repeats while the pointer is on the button it went down on -- for the track, the
    // part of it still above or below the thumb, measured where the page is going.
    void Step(bool repeat) {
        if (!onScroll) return;
        const bool on = Inside(rect, px, py);
        float to = value;
        switch (grab) {
        case Part::Up:
            if (repeat && !(on && py < TrackTop())) return;
            to -= kLine;
            break;
        case Part::Down:
            if (repeat && !(on && py >= rect.bottom - kSize)) return;
            to += kLine;
            break;
        case Part::PageUp:
            if (repeat && !(on && py >= TrackTop() && py < ThumbTop(value))) return;
            to -= viewport;
            break;
        case Part::PageDown:
            if (repeat && !(on && py < rect.bottom - kSize &&
                            py >= ThumbTop(value) + ThumbLength())) return;
            to += viewport;
            break;
        default:
            return;
        }
        Wake();
        onScroll(std::clamp(to, 0.0f, MaxValue()), true);
    }

    // --- the three states ---------------------------------------------------------
    bool Held() const { return visible && (hover || grab != Part::None); }

    // Where the states stand now, and a timer for the next moment one of them is due to
    // change -- so a bar waiting out its 2 s costs one timer rather than two seconds of
    // frames.
    void Poll() {
        const ULONGLONG now = GetTickCount64();
        const bool h = Held();
        if (h != held) {
            held = h;
            heldAt = now;
            if (!h) activeAt = now;   // leaving the bar is the pointer moving over the page
        }
        ULONGLONG next = 0;
        if (!visible) {
            // Nothing to scroll. Put away at once, so that a page which does need the bar
            // does not arrive with this one's fading out on it.
            shown = expanded = false;
            showT = fadeT = 0.0f;
            sizeT.Set(0.0f);
        } else if (!autoHide) {
            if (!expanded) { showT = fadeT = 1.0f; sizeT.Set(1.0f); }
            shown = expanded = true;
        } else {
            auto due = [&](ULONGLONG at) {
                if (at <= now) return true;
                if (next == 0 || at < next) next = at;
                return false;
            };
            if (held) {
                shown = true;
                if (!expanded && due(heldAt + kExpandDelay)) expanded = true;
            } else {
                if (expanded && due(heldAt + kContractDelay)) expanded = false;
                if (shown && due(activeAt + kHideDelay)) shown = expanded = false;
            }
        }
        if (next != armedFor && owner && owner->hwnd) {
            armedFor = next;
            if (next == 0) stateTimer.Stop();
            else stateTimer.Start(owner, (UINT)(next - now), [this] { StateTick(); });
        }
    }

    bool Animating() const override {
        return Widget::Animating() || Held() != held ||
               showT != (shown ? 1.0f : 0.0f) || fadeT != (expanded ? 1.0f : 0.0f) ||
               sizeT.Wants(expanded ? 1.0f : 0.0f);
    }
    void Tick(float dt) override {
        Widget::Tick(dt);
        Poll();
        // Appearing is a setter in MouseIndicator; going is NoIndicator's 83 ms fade.
        if (shown) showT = 1.0f;
        else motion::Ramp(&showT, 0.0f, dt, motion::kFaster);
        // The track and the arrows fade over ScrollBarOpacityChangeDuration, linear. The
        // thumb widens over ScrollBarExpandDuration on KeySpline 0,0,0,1, which is Decel.
        motion::Ramp(&fadeT, expanded ? 1.0f : 0.0f, dt, motion::kFaster);
        sizeT.To(expanded ? 1.0f : 0.0f);
        sizeT.Step(dt, motion::kFast);
    }

    void Paint(const Painter &p) override {
        const Palette &c = *p.pal;
        if (fadeT > 0.0f) {
            // ScrollBarCornerRadius, doubled by the track's converter: a pill.
            p.FillRound(rect, 6.0f, Fade(c.acrylicInApp, fadeT));
            PaintArrow(p, Part::Up);
            PaintArrow(p, Part::Down);
        }
        if (showT <= 0.0f || !HasThumb()) return;
        const float s = sizeT.value;
        const float w = 8.0f + 4.0f * s;                              // Width 8 -> 12
        const float left = rect.left + (kSize - w) / 2 + 2.0f * (1.0f - s);   // X 2 -> 0
        const float top = ThumbTop(drawn);
        // Inside the transparent stroke.
        const D2D1_RECT_F fill = { left + 3, top + 3, left + w - 3, top + ThumbLength() - 3 };
        p.FillRound(fill, (std::min)(3.0f, Width(fill) / 2), Fade(c.controlStrong, showT));
    }

    void PaintArrow(const Painter &p, Part part) {
        const Palette &c = *p.pal;
        const bool up = part == Part::Up;
        // The button is the whole 12x12 end of the bar; the glyph sits in it with the
        // template's 4-DIP padding on the outer side.
        const D2D1_RECT_F box = up ? D2D1_RECT_F{ rect.left, rect.top + 4, rect.right, rect.top + kSize }
                                   : D2D1_RECT_F{ rect.left, rect.bottom - kSize, rect.right, rect.bottom - 4 };
        const bool over = hover && Inside(rect, px, py) &&
                          (up ? py < TrackTop() : py >= rect.bottom - kSize);
        const bool down = over && grab == part;
        const std::wstring g = up ? glyph::kCaretUp : glyph::kCaretDown;
        const float gw = p.MeasureWidth(g, p.font->iconTiny);
        const float cx = (box.left + box.right) / 2, cy = (box.top + box.bottom) / 2;
        // ScrollBarButtonArrowScalePressed, about the glyph's centre.
        D2D1_MATRIX_3X2_F was;
        if (down) {
            p.rt->GetTransform(&was);
            p.rt->SetTransform(D2D1::Matrix3x2F::Scale(0.875f, 0.875f, D2D1::Point2F(cx, cy)) * was);
        }
        p.Text(g, { cx - gw / 2, box.top, cx + gw / 2 + 1, box.bottom }, p.font->iconTiny,
               Fade(over ? c.textSecondary : c.controlStrong, fadeT));
        if (down) p.rt->SetTransform(was);
    }
};

}  // namespace micula
