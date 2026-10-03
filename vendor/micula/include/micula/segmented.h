// Micula / segmented.h

#pragma once

#include "window.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <functional>
#include <string>
#include <vector>

namespace micula {


// Three or four mutually exclusive words, side by side. Used where a dropdown would
// be heavier than the choice deserves -- a setting that is auto/on/off is a worse
// answer behind a click than with all three showing.
struct Segmented : Widget {
    std::vector<std::wstring> options;
    int selected = 0;
    std::function<void(int)> onChange;

    // The indicator, in cells: 0 is `rect.left`, 1 the far edge of the first option.
    //
    // Two values, because the block's two edges do not move together: `pillPos` is where the
    // move has got to and `pillLag` is a follower that trails behind it -- see Tick. At rest
    // both are `selected`, so a control that is not moving has nothing to remember and one
    // that Layout() has just built is right on its first paint.
    motion::Track pillPos;
    float pillLag = 0.0f;
    bool  dragging = false;

    // How far behind the leading edge the trailing one trails, in seconds. The block is
    // longer than a cell by about the distance covered in that time: 45 ms of a 167 ms move
    // is a quarter of a cell, which is as much as reads as deliberate.
    static constexpr float kPillLag = 0.045f;

    Segmented(std::vector<std::wstring> opts, int sel, std::function<void(int)> f)
        : options(std::move(opts)), selected(sel), onChange(std::move(f)),
          pillPos((float)sel), pillLag((float)sel) {}

    bool Focusable() const override { return true; }
    // The hovered cell follows the pointer across the control, and a drag follows it out of
    // the control altogether, so the window has to keep repainting either way.
    bool TracksPointer() const override { return hover || dragging; }
    float CellW() const { return Width(rect) / (float)(std::max)(size_t(1), options.size()); }

    // The cell under `x`, or -1 past either end. Asked of the cursor where it is needed
    // rather than cached in a field written by Paint: a cached one stays right only as long
    // as the control is repainted on every move, and anything reading it -- a click, an IME
    // position -- then depends on a frame having been drawn since the pointer last moved.
    int IndexAtX(float x) const {
        const int n = (int)options.size();
        if (n <= 0) return -1;
        const int i = (int)((x - rect.left) / CellW());
        return (i < 0 || i >= n) ? -1 : i;
    }

    // The same, kept between the ends -- what a drag wants, since dragging past the last
    // option means the last option.
    int ClampedIndexX(float x) const {
        const int n = (int)options.size();
        if (n <= 0) return -1;
        return std::clamp((int)((x - rect.left) / CellW()), 0, n - 1);
    }

    // The indicator's two edges, in cells. The leading one is where the move has got to and
    // the trailing one is behind it, so the block spans a cell plus however far apart they
    // are: that gap is the stretch, which opens as the move sets off and closes as it lands.
    void PillEdges(float *lo, float *hi) const {
        *lo = (std::min)(pillPos.value, pillLag);
        *hi = (std::max)(pillPos.value, pillLag) + 1.0f;
    }

    // Take the new index. Nothing starts here: the indicator is aimed at whatever `selected`
    // is, once a frame, in Tick. That is what makes a drag that crosses three cells inside one
    // frame one move rather than three, and crossing back again continuous rather than a
    // rewind.
    void Select(int index) {
        if (index < 0 || index >= (int)options.size() || index == selected) return;
        selected = index;
        if (onChange) onChange(selected);
    }

    // A press takes the option under the pointer and a drag carries the selection along;
    // the release only ends the gesture. The same shape as Slider, including following the
    // pointer out of the control -- and out of the window.
    void OnPress(float x, float /*y*/) override { dragging = true; Select(ClampedIndexX(x)); }
    void OnDrag(float x, float /*y*/) override { if (dragging) Select(ClampedIndexX(x)); }
    void OnRelease() override { dragging = false; }

    bool OnKey(WPARAM vk) override {
        if (vk != VK_LEFT && vk != VK_RIGHT) return false;
        const int n = (int)options.size();
        if (n <= 0) return false;
        // Wraps, and the indicator wraps with it: the one move that crosses the control.
        Select((selected + (vk == VK_RIGHT ? 1 : n - 1)) % n);
        return true;
    }

    bool Animating() const override {
        return Widget::Animating() || pillPos.Wants((float)selected) || pillLag != pillPos.value;
    }
    void Tick(float dt) override {
        Widget::Tick(dt);
        // Aimed every frame rather than at the press: a page that assigns `selected` itself is
        // followed too, and the target is always the last cell the pointer crossed.
        pillPos.To((float)selected);
        pillPos.Step(dt, motion::kFaster);
        // The trailing edge is a lag of the leading one. A follower is continuous through a
        // change of target by construction, which is the whole reason it is not a second
        // curve: a drag retargets this several times a frame, and a curve restarted each time
        // would jump to whichever cell it was last pointed at -- the block teleporting from
        // cell to cell while the pointer was dragged across them.
        pillLag += (pillPos.value - pillLag) * (1.0f - std::exp(-dt / kPillLag));
        // Snapped when it is close, or the two are never quite equal and the window animates
        // for the rest of its life, a thousandth of a cell out.
        if (std::fabs(pillPos.value - pillLag) < 0.002f) pillLag = pillPos.value;
    }

    void Paint(const Painter &p) override {
        const Palette &c = *p.pal;
        // Read here rather than in a mouse handler: this control never sees a move, only
        // its own hover flag, so the cell to light comes from the cursor at paint time --
        // which is also why TracksPointer is true. Nothing is stored: which cell the
        // pointer is over is a question about the pointer, not state this control keeps.
        const int hot = hover ? IndexAtX(Cursor().x) : -1;
        p.FillRound(rect, metric::kRadiusControl, c.controlBg);
        p.StrokeRound(rect, metric::kRadiusControl, c.controlStroke);
        // The hover fill goes under the indicator, so a block on its way to a cell the
        // pointer is already on is not covered by that cell's own highlight.
        if (hot >= 0 && enabled) {
            const float x = rect.left + CellW() * hot;
            p.FillRound({ x + 2, rect.top + 2, x + CellW() - 2, rect.bottom - 2 },
                        metric::kRadiusControl - 1, c.controlBgHover);
        }
        float lo = 0.0f, hi = 0.0f;
        PillEdges(&lo, &hi);
        const float w = CellW();
        const float blockLo = rect.left + lo * w, blockHi = rect.left + hi * w;
        p.FillRound({ blockLo + 2, rect.top + 2, blockHi - 2, rect.bottom - 2 },
                    metric::kRadiusControl - 1, enabled ? c.accent : c.controlBgHover);
        // **The label colour follows the block rather than the selection.** Each label is drawn
        // in the colour of what is behind it, and one the block is halfway across is drawn on
        // either side of the block's edge -- which cuts the glyph in two exactly where the fill
        // changes. Turning the whole word over the moment the option is chosen instead reads as
        // the text arriving before the block does, which is what it is.
        for (size_t i = 0; i < options.size(); i++) {
            const float x = rect.left + w * i;
            const D2D1_RECT_F cell = { x, rect.top, x + w, rect.bottom };
            const float tw = p.MeasureWidth(options[i], p.font->body);
            const D2D1_RECT_F text = { cell.left + (w - tw) / 2, cell.top, cell.right, cell.bottom };
            if (!enabled) { p.Text(options[i], text, p.font->body, c.textDisabled); continue; }
            // Where the block's edges fall inside this cell: three slices, of which the middle
            // one is empty whenever the block misses the cell entirely or covers all of it.
            const float cutLo = std::clamp(blockLo, cell.left, cell.right);
            const float cutHi = std::clamp(blockHi, cell.left, cell.right);
            Label(p, options[i], text, cell.left, cutLo, c.textPrimary);
            Label(p, options[i], text, cutLo, cutHi, c.accentText);
            Label(p, options[i], text, cutHi, cell.right, c.textPrimary);
        }
        if (focus && owner && owner->showFocusRing) {
            const D2D1_RECT_F o = { rect.left - 2, rect.top - 2, rect.right + 2, rect.bottom + 2 };
            p.StrokeRound(o, metric::kRadiusControl + 2, c.textPrimary, 2.0f);
        }
    }

    // One slice of a label: the text drawn through its own box, so that it stays where the
    // layout put it, and clipped to `[from, to)` across that box. The clip is never the text's
    // box -- narrowing that would move the glyphs instead of cutting them.
    static void Label(const Painter &p, const std::wstring &s, const D2D1_RECT_F &text,
                      float from, float to, const D2D1_COLOR_F &c) {
        if (to <= from) return;
        p.rt->PushAxisAlignedClip({ from, text.top, to, text.bottom },
                                  D2D1_ANTIALIAS_MODE_ALIASED);
        p.Text(s, text, p.font->body, c);
        p.rt->PopAxisAlignedClip();
    }
};

}  // namespace micula
