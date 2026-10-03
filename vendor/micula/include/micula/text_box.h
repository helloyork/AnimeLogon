// Micula / text_box.h

#pragma once

#include "window.h"

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <functional>
#include <string>
#include <vector>

namespace micula {


// A single-line text field, drawn.
//
// This was a real child EDIT control, and it had to stop being one: the window carries
// `WS_EX_NOREDIRECTIONBITMAP` so that Mica is visible behind it (window.h says why),
// and a window with no redirection surface has nowhere for USER32 to draw a child
// control. The choice was Mica or the EDIT, and the EDIT lost.
//
// So this reimplements the part of an edit control a settings field actually uses: a
// caret, a selection made with the keyboard or the mouse (press, drag, Shift+click,
// double-click), the six navigation keys, the four clipboard commands, and IME
// input. It does not reimplement undo, drag-and-drop, right-click, spell checking,
// multiple lines or accessibility, and it should not grow them -- a dialog that needs
// those wants a redirected window with a real edit control, not a bigger version of
// this.
//
// The layout object is kept rather than rebuilt per paint. Both things this needs --
// where the caret goes for a character index, and which character index a click landed
// on -- are `IDWriteTextLayout` queries, and creating a layout per query turns a
// 60-character path into 60 layouts on every mouse move.
struct TextBox : Widget {
    std::wstring text;
    size_t caret  = 0;      // index into `text`
    size_t anchor = 0;      // the other end of the selection; equal to caret when none
    float  scroll = 0.0f;   // how far the text is scrolled left, in DIPs
    std::wstring placeholder;
    // The field holds a file-system path. Paste then also drops the quotes that
    // Explorer's "Copy as path" puts round what it copies, and any trailing spaces --
    // neither is part of the path, and both are what somebody would have to delete by
    // hand. Off for ordinary text, where a quoted word is meant.
    bool pathField = false;
    std::function<void(const std::wstring &)> onChange;
    // Fired when the field is finished with -- Enter, or focus leaving it -- and not
    // on every keystroke. See Widget::OnBlur for why a text field needs both.
    std::function<void(const std::wstring &)> onCommit;

    // The layout is a cache of what the text, the format and the theme already say, so
    // it is mutable: the queries below are const and are called from places that have no
    // business rebuilding text layout, the IME among them. Built on demand, dropped by
    // Dirty().
    mutable IDWriteTextLayout *layout = nullptr;

    ~TextBox() override { if (layout) layout->Release(); }

    bool Focusable() const override { return true; }
    bool TextCursor() const override { return true; }
    void OnBlur() override { if (onCommit) onCommit(text); }

    // Fluent's text field padding: 11 DIPs each side.
    float InnerLeft() const  { return rect.left + 11.0f; }
    float InnerWidth() const { return Width(rect) - 22.0f; }

    void SetText(const std::wstring &s) {
        text = s;
        caret = anchor = text.size();
        Dirty();
    }
    void Dirty() {
        if (layout) { layout->Release(); layout = nullptr; }
    }
    void Ensure() const {
        if (layout || !owner || !owner->dw || !owner->fonts.body) return;
        owner->dw->CreateTextLayout(text.c_str(), (UINT32)text.size(), owner->fonts.body,
                                    100000.0f, 100.0f, &layout);
        // The shared body format is vertically centred, because every other call site
        // hands DirectWrite a rectangle the size of its control and wants the text in
        // the middle of it. A *layout* is different: it centres within its own
        // `maxHeight`, which is the 100 above, so the glyphs land forty pixels below
        // the origin they are drawn at -- outside a 32-DIP field, and clipped away.
        // The field came up empty and the box around it did not, which is a fault that
        // looks like the text was never set.
        if (layout) layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    }
    float XOf(size_t index) const {
        Ensure();
        if (!layout) return 0.0f;
        FLOAT x = 0, y = 0;
        DWRITE_HIT_TEST_METRICS m = {};
        layout->HitTestTextPosition((UINT32)index, FALSE, &x, &y, &m);
        return x;
    }
    // The gap nearest `localX`, which is where a caret goes. `under`, if asked for, is
    // the character the point is actually over -- not the same thing in the right half
    // of a letter, and the one a double-click has to start from.
    size_t IndexAt(float localX, size_t *under = nullptr) const {
        Ensure();
        Ensure();
        if (under) *under = 0;
        if (!layout) return 0;
        BOOL trailing = FALSE, inside = FALSE;
        DWRITE_HIT_TEST_METRICS m = {};
        layout->HitTestPoint(localX, 0.0f, &trailing, &inside, &m);
        if (under) *under = (size_t)m.textPosition;
        return (size_t)m.textPosition + (trailing ? 1u : 0u);
    }

    bool HasSelection() const { return caret != anchor; }
    size_t SelLo() const { return caret < anchor ? caret : anchor; }
    size_t SelHi() const { return caret < anchor ? anchor : caret; }
    std::wstring Selected() const {
        return HasSelection() ? text.substr(SelLo(), SelHi() - SelLo()) : std::wstring();
    }
    void DeleteSelection() {
        if (!HasSelection()) return;
        text.erase(SelLo(), SelHi() - SelLo());
        caret = anchor = SelLo();
        Dirty();
    }
    void Changed() {
        Dirty();
        if (onChange) onChange(text);
    }

    // Keep the caret in view. Without this a path longer than the field types itself
    // off the right-hand edge and the person is editing something they cannot see.
    void ScrollToCaret() {
        const float x = XOf(caret);
        if (x - scroll > InnerWidth() - 2) scroll = x - InnerWidth() + 2;
        if (x - scroll < 0)                scroll = x;
        const float full = XOf(text.size());
        if (full - scroll < InnerWidth() && scroll > 0)
            scroll = full > InnerWidth() ? full - InnerWidth() : 0.0f;
        if (scroll < 0) scroll = 0;
    }

    // --- the mouse ---------------------------------------------------------------
    //
    // The caret moves on the press, not on the release. It used to move in OnClick,
    // which is the release, and that left a drag nowhere to start from: the field could
    // not select anything with the mouse, and a user reported it as a text box that was
    // only a picture of one.
    bool  selecting = false;       // between a press and its release
    // The window class has no CS_DBLCLKS -- with it, the second click of a quick pair
    // arrives as WM_LBUTTONDBLCLK, and every button in the window would miss it -- so a
    // double-click reaches this as two presses and is recognised here.
    DWORD lastPressTime = 0;
    float lastPressX = -1.0e6f;

    bool TracksPointer() const override { return selecting; }

    void OnPress(float x, float /*y*/) override {
        size_t under = 0;
        const size_t at = IndexAt(x - InnerLeft() + scroll, &under);
        const DWORD now = (DWORD)GetMessageTime();
        const float slop = (float)GetSystemMetrics(SM_CXDOUBLECLK) / 2.0f /
                           (owner ? owner->scale() : 1.0f);
        const bool twice = lastPressTime != 0 && now - lastPressTime <= GetDoubleClickTime() &&
                           std::fabs(x - lastPressX) <= slop;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        lastPressX = x;
        if (twice && !shift) {
            SelectPieceAt(under);
            lastPressTime = 0;     // a third click is a fresh one, not a second pair
            selecting = false;
        } else {
            lastPressTime = now;
            caret = at;
            if (!shift) anchor = at;
            selecting = true;
        }
        ScrollToCaret();
    }
    void OnDrag(float x, float /*y*/) override {
        if (!selecting) return;
        caret = IndexAt(x - InnerLeft() + scroll);
        ScrollToCaret();
    }
    void OnRelease() override { selecting = false; }

    // Nothing left to do: the press has placed the caret. This used to place it from
    // GetCursorPos, and Space reaches OnClick from the keyboard -- so every space typed
    // into `Program Files` moved the caret to wherever the pointer was resting and went
    // in there. OnKey now keeps Space from coming here at all; this is the other half.
    void OnClick() override {}

    // A double-click selects one piece of the text: the run between two separators,
    // where a separator is a backslash, a slash or a space. Spaces alone would select
    // `Files\Micula` out of `C:\Program Files\Micula`, and the piece somebody
    // double-clicks in a path to replace is one folder name.
    static bool Separator(wchar_t ch) {
        return ch == L'\\' || ch == L'/' || ch == L' ' || ch == L'\t';
    }
    void SelectPieceAt(size_t at) {
        if (text.empty()) { caret = anchor = 0; return; }
        if (at >= text.size()) at = text.size() - 1;
        if (Separator(text[at])) { anchor = at; caret = at + 1; return; }
        size_t lo = at, hi = at;
        while (lo > 0 && !Separator(text[lo - 1])) lo--;
        while (hi < text.size() && !Separator(text[hi])) hi++;
        anchor = lo;
        caret = hi;
    }

    // What of the clipboard this field keeps. One line: text copied out of a document
    // or a terminal routinely ends in a line break, and a single-line field that took it
    // would hold a character nobody can see or delete. A break in the middle ends the
    // paste there rather than being joined up, because joining guesses at what the lines
    // were meant to be.
    std::wstring Pasted(std::wstring in) const {
        const size_t br = in.find_first_of(L"\r\n");
        if (br != std::wstring::npos) in.erase(br);
        if (pathField) {
            while (!in.empty() && in.back() == L' ') in.pop_back();
            if (in.size() >= 2 && in.front() == L'"' && in.back() == L'"')
                in = in.substr(1, in.size() - 2);
        }
        return in;
    }

    bool OnChar(wchar_t ch) override {
        if (!enabled) return false;
        DeleteSelection();
        text.insert(caret, 1, ch);
        caret = anchor = caret + 1;
        Changed();
        ScrollToCaret();
        return true;
    }

    bool OnKey(WPARAM vk) override {
        if (!enabled) return false;
        const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        const bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;

        if (ctrl) {
            switch (vk) {
            case 'A': anchor = 0; caret = text.size(); return true;
            case 'C': if (HasSelection()) micula::SetClipboardText(owner->hwnd, Selected());
                      return true;
            case 'X': if (HasSelection()) {
                          micula::SetClipboardText(owner->hwnd, Selected());
                          DeleteSelection();
                          Changed();
                      }
                      return true;
            case 'V': {
                const std::wstring in = Pasted(micula::ClipboardText(owner->hwnd));
                if (in.empty()) return true;
                DeleteSelection();
                text.insert(caret, in);
                caret = anchor = caret + in.size();
                Changed();
                ScrollToCaret();
                return true;
            }
            default: return false;
            }
        }

        switch (vk) {
        case VK_RETURN:
            // Consumed rather than left to the window, which would otherwise read it
            // as the default button while the person was still in the field.
            if (onCommit) onCommit(text);
            return true;
        case VK_SPACE:
            // The space itself arrives as WM_CHAR. The key is consumed so the window does
            // not also read it as "operate the focused control" -- see OnClick.
            return true;
        case VK_LEFT:
            if (caret > 0) caret--;
            if (!shift) anchor = caret;
            break;
        case VK_RIGHT:
            if (caret < text.size()) caret++;
            if (!shift) anchor = caret;
            break;
        case VK_HOME:
            caret = 0;
            if (!shift) anchor = caret;
            break;
        case VK_END:
            caret = text.size();
            if (!shift) anchor = caret;
            break;
        case VK_BACK:
            if (HasSelection()) DeleteSelection();
            else if (caret > 0) { text.erase(caret - 1, 1); caret = anchor = caret - 1; }
            else return true;
            Changed();
            break;
        case VK_DELETE:
            if (HasSelection()) DeleteSelection();
            else if (caret < text.size()) text.erase(caret, 1);
            else return true;
            Changed();
            break;
        default:
            return false;
        }
        ScrollToCaret();
        return true;
    }

    // Where the IME should open: at the caret, in the field's own space.
    //
    // Measured now rather than read from a position cached by the last paint. This is
    // called from the message loop when a composition starts, which can arrive before
    // the WM_PAINT for a key that has just moved the caret, and the candidate window
    // then opened wherever the caret used to be. Nothing here depends on a paint having
    // happened: the layout is created on demand -- see Ensure.
    bool CaretPoint(D2D1_POINT_2F *out) const override {
        if (out) *out = D2D1::Point2F(InnerLeft() + XOf(caret) - scroll, rect.bottom);
        return true;
    }

    void Paint(const Painter &p) override {
        const Palette &c = *p.pal;
        const bool active = focus;

        // Three states crossing over: the resting fill, the hover fill, and the lighter
        // one Fluent gives a field with the caret in it (ControlFillColorInputActive,
        // which is opaque white in light and 70% #1E1E1E in dark). The focus fill wins
        // over hover, which is why it is the outer mix.
        p.FillRound(rect, metric::kRadiusControl,
                    !enabled ? c.controlBg
                             : Mix(Mix(c.controlBg, c.controlBgHover, hoverT),
                                   c.controlBgInput, focusT));
        p.StrokeRound(rect, metric::kRadiusControl, c.controlStroke);
        // The accent underline, **whole, the moment the field takes focus.**
        //
        // It used to grow out of the centre, which is Material Design's text field and
        // not Fluent's: WinUI changes `BorderThickness` to 0,0,0,2 in a visual-state
        // setter, and a setter has no duration. The fill behind it is the part that
        // crosses over, and it does that above.
        p.Line(rect.left + metric::kRadiusControl, rect.bottom - 1,
               rect.right - metric::kRadiusControl, rect.bottom - 1,
               active && enabled ? c.accent : c.controlStrokeBottom,
               active && enabled ? 2.0f : 1.0f);

        Ensure();

        const D2D1_RECT_F inner = { InnerLeft(), rect.top + 1, rect.right - 11, rect.bottom - 1 };
        p.rt->PushAxisAlignedClip(inner, D2D1_ANTIALIAS_MODE_ALIASED);

        if (text.empty() && !placeholder.empty()) {
            p.Text(placeholder, inner, p.font->body, c.textDisabled);
        } else if (layout) {
            if (HasSelection()) {
                const float a = XOf(SelLo()) - scroll, b = XOf(SelHi()) - scroll;
                // The accent at a third, which is what Fluent's selection highlight
                // is: the text stays its own colour and reads through it.
                p.Fill({ inner.left + a, rect.top + 5, inner.left + b, rect.bottom - 5 },
                       D2D1::ColorF(c.accent.r, c.accent.g, c.accent.b, 0.35f));
            }
            // Vertically centred by hand: the layout is drawn from its origin and its
            // own height is the line height, not the box's.
            DWRITE_TEXT_METRICS tm = {};
            layout->GetMetrics(&tm);
            const float ty = rect.top + (Height(rect) - tm.height) / 2;
            p.rt->DrawTextLayout(D2D1::Point2F(inner.left - scroll, ty), layout,
                                 p.Brush(enabled ? c.textPrimary : c.textDisabled),
                                 D2D1_DRAW_TEXT_OPTIONS_NONE);
        }

        if (active && owner && owner->caretOn && enabled) {
            const float x = inner.left + XOf(caret) - scroll;
            p.Line(x, rect.top + 6, x, rect.bottom - 6, c.textPrimary, 1.0f);
        }
        p.rt->PopAxisAlignedClip();
    }
};

}  // namespace micula
