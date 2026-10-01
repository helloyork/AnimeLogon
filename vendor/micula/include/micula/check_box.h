// Micula / check_box.h

#pragma once

#include "window.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <functional>
#include <string>
#include <vector>

namespace micula {


struct CheckBox : Widget {
    std::wstring label;
    std::wstring detail;             // second line, secondary colour, optional
    bool  checked = false;
    std::function<void(bool)> onChange;
    // The box filling and the tick arriving, rather than both appearing between two
    // frames. WinUI's checkbox animates its glyph in; this is that, one storyboard at
    // ControlFastAnimationDuration.
    motion::Track checkT;

    CheckBox(std::wstring text, bool on, std::function<void(bool)> f)
        : label(std::move(text)), checked(on), onChange(std::move(f)),
          checkT(on ? 1.0f : 0.0f) {}

    bool Focusable() const override { return true; }
    void OnClick() override {
        if (!enabled) return;
        checked = !checked;
        if (onChange) onChange(checked);
    }
    bool Animating() const override {
        return Widget::Animating() || checkT.Wants(checked ? 1.0f : 0.0f);
    }
    void Tick(float dt) override {
        Widget::Tick(dt);
        checkT.To(checked ? 1.0f : 0.0f);
        checkT.Step(dt, motion::kFast);
    }

    void Paint(const Painter &p) override {
        const Palette &c = *p.pal;
        const float side = 20.0f;
        const float y = rect.top + (Height(rect) - side) / 2;
        const D2D1_RECT_F box = { rect.left, y, rect.left + side, y + side };

        const D2D1_COLOR_F off = Mix(c.controlBg, c.controlBgHover, hoverT);
        const D2D1_COLOR_F on  = Mix(Mix(c.accent, c.accentHover, hoverT),
                                     c.accentPressed, pressT);
        const float k = checkT.value;
        p.FillRound(box, 3.0f, !enabled ? c.controlBg : Mix(off, on, k));
        // The empty box's border goes as the accent comes up behind it, or the two are
        // both visible halfway through and the box reads as having gained a second edge.
        if (k < 1.0f)
            p.StrokeRound(box, 3.0f, Fade(enabled ? c.controlStroke : c.controlBg, 1.0f - k));
        if (k > 0.0f)
            p.Text(glyph::kCheck, { box.left + 2, box.top, box.right, box.bottom },
                   p.font->icon, Fade(enabled ? c.accentText : c.textDisabled, k));
        if (focus && owner && owner->showFocusRing) {
            const D2D1_RECT_F o = { rect.left - 2, rect.top - 1, rect.right + 2, rect.bottom + 1 };
            p.StrokeRound(o, 5.0f, c.textPrimary, 2.0f);
        }

        const D2D1_COLOR_F fg = enabled ? c.textPrimary : c.textDisabled;
        const float tx = rect.left + side + 12;
        if (detail.empty()) {
            p.Text(label, { tx, rect.top, rect.right, rect.bottom }, p.font->body, fg);
        } else {
            const float half = Height(rect) / 2;
            p.Text(label, { tx, rect.top, rect.right, rect.top + half }, p.font->body, fg);
            p.Text(detail, { tx, rect.top + half, rect.right, rect.bottom },
                   p.font->caption, c.textSecondary);
        }
    }
};

}  // namespace micula
