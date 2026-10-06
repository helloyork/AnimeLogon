// Micula / button.h

#pragma once

#include "window.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <functional>
#include <string>
#include <vector>

namespace micula {


enum class ButtonStyle {
    Accent,     // the one thing the page wants you to do. One per page, at most.
    Standard,   // everything else with a box around it
    Subtle,     // no box until hovered -- toolbar actions, "open log folder"
    Link,       // accent-coloured text, no box at all
};

struct Button : Widget {
    std::wstring label;
    std::wstring glyph;              // optional Segoe Fluent Icons code point, drawn left
    // Left-aligned rather than centred. For a navigation list, where the labels are
    // different lengths and centring each one in its own row makes the column look
    // ragged -- Windows' own Settings left-aligns them against the icon.
    bool         leftAlign = false;
    ButtonStyle  style = ButtonStyle::Standard;
    std::function<void()> onClick;

    Button(std::wstring text, ButtonStyle s, std::function<void()> f)
        : label(std::move(text)), style(s), onClick(std::move(f)) {}

    bool Focusable() const override { return true; }
    bool HandCursor() const override { return style == ButtonStyle::Link; }
    void OnClick() override { if (enabled && onClick) onClick(); }

    void Paint(const Painter &p) override {
        const Palette &c = *p.pal;
        D2D1_COLOR_F fg = enabled ? c.textPrimary : c.textDisabled;

        // **A button does not move.** The first version of this shrank it by 2% on press,
        // on the strength of PointerDownThemeAnimation -- which is real, and belongs to
        // ListViewItem and GridViewItem, not to Button. WinUI's Button template changes
        // exactly one thing over time, its background, and changes it with a
        // BrushTransition; the border and the label are setters. A button that shrinks
        // under the pointer is a button from another platform.
        const D2D1_RECT_F box = rect;

        if (style == ButtonStyle::Accent) {
            const D2D1_COLOR_F bg =
                !enabled ? c.controlBg
                         : Mix(Mix(c.accent, c.accentHover, hoverT), c.accentPressed, pressT);
            p.FillRound(box, metric::kRadiusControl, bg);
            fg = enabled ? c.accentText : c.textDisabled;
        } else if (style == ButtonStyle::Standard) {
            const D2D1_COLOR_F bg =
                !enabled ? c.controlBg
                         : Mix(Mix(c.controlBg, c.controlBgHover, hoverT),
                               c.controlBgPressed, pressT);
            p.FillRound(box, metric::kRadiusControl, bg);
            p.StrokeRound(box, metric::kRadiusControl, c.controlStroke);
            // Fluent's controls have a darker line along the bottom edge only. It is
            // one pixel and it is most of what makes a rectangle read as a button
            // rather than as a box; without it the standard button looks flat next to
            // Windows' own. It goes as the button is pressed, which is what the real
            // one does -- a pressed control is level with the page, not raised over it.
            if (enabled && pressT < 1.0f)
                p.Line(box.left + metric::kRadiusControl, box.bottom - 0.5f,
                       box.right - metric::kRadiusControl, box.bottom - 0.5f,
                       Fade(c.controlStrokeBottom, 1.0f - pressT));
        } else if (style == ButtonStyle::Subtle) {
            // Nothing at all at rest, so the fill fades in from transparent rather than
            // from a colour: its own alpha is what carries it.
            if (enabled && hoverT > 0.0f)
                p.FillRound(box, metric::kRadiusControl,
                            Fade(Mix(c.subtleHover, c.controlBgPressed, pressT), hoverT));
        } else {
            fg = !enabled ? c.textDisabled : Mix(c.accent, Shade(c.accent, -0.15f), pressT);
        }

        if (focus && owner && owner->showFocusRing) {
            // Fluent's focus ring is two strokes: a thick one in the text colour and
            // a thin one in the background colour inside it, so it stays visible
            // whatever the button is filled with. One stroke in a single colour
            // disappears against the accent button, which is the button people tab
            // to first.
            const D2D1_RECT_F o = { rect.left - 2, rect.top - 2, rect.right + 2, rect.bottom + 2 };
            p.StrokeRound(o, metric::kRadiusControl + 2, c.textPrimary, 2.0f);
            p.StrokeRound(rect, metric::kRadiusControl, c.windowBg, 1.0f);
        }

        D2D1_RECT_F text = box;
        if (!glyph.empty()) {
            const D2D1_RECT_F g = { box.left + 12, box.top, box.left + 32, box.bottom };
            p.Text(glyph, g, p.font->icon, fg);
            text.left += 32;
        }
        // Centred by measuring, not by a centring text format: the glyph above eats
        // into the box asymmetrically, and a format-centred label would sit off to
        // the right of the space that is actually left for it.
        if (leftAlign) {
            text.left += glyph.empty() ? 12.0f : 0.0f;
        } else {
            const float w = p.MeasureWidth(label, p.font->body);
            text.left += (std::max)(0.0f, (Width(text) - w) / 2);
        }
        p.Text(label, text, p.font->body, fg);
    }

    // The width this button wants: its label plus Fluent's 12-DIP side padding, and
    // never narrower than the 100-DIP minimum that keeps a row of buttons even.
    float PreferredWidth(const Painter &p) const {
        return (std::max)(metric::kButtonMinW,
                        p.MeasureWidth(label, p.font->body) + 24.0f + (glyph.empty() ? 0.0f : 32.0f));
    }
};

}  // namespace micula
