// Micula / window.h
//
// A Windows 11 window: Mica behind it, Direct2D on top of DirectComposition, and no
// child controls at all.
//
// Why the composition path rather than an HWND render target
// ----------------------------------------------------------
// Because of Mica, and Mica is not a colour -- it is a *material DWM draws behind the
// window*, sampled from the desktop wallpaper. For any of it to be visible the window
// has to hand DWM pixels with alpha in them, and an `ID2D1HwndRenderTarget` cannot:
// HWND render targets are documented as supporting `D2D1_ALPHA_MODE_IGNORE` only, so
// every pixel it produces is opaque and the material is covered by whatever the page
// painted. The first version of this file used one, painted an opaque `#F3F3F3`, and
// looked like a themed dialog rather than like a Windows 11 application.
//
// So the window is composed instead, the same way WinUI 3 composes its own:
//
//   WS_EX_NOREDIRECTIONBITMAP    no redirection surface, so nothing opaque is
//                                allocated for the window at all
//   swap chain for composition   B8G8R8A8 with DXGI_ALPHA_MODE_PREMULTIPLIED
//   DirectComposition visual     the swap chain's content, targeted at the HWND
//   DWMWA_SYSTEMBACKDROP_TYPE    DWMSBT_MAINWINDOW -- Mica
//
// The cost, and it is not small: **`WS_EX_NOREDIRECTIONBITMAP` means child HWNDs do
// not render.** There is no redirection surface for USER32 to draw them into. So the
// text field that used to be a real EDIT control is gone, and widgets.h has a drawn
// one in its place with its own caret, clipboard and IME handling. That is a real
// regression in exactly one control and it is the price of the whole window looking
// native rather than nearly native.
//
// What happens on a machine without Mica
// --------------------------------------
// `DWMWA_SYSTEMBACKDROP_TYPE` arrived in Windows 11 22H2 (22621). On anything older
// DwmSetWindowAttribute returns an error, `micaActive` stays false, and the page
// paints an opaque background instead -- the window still composes, it just has a flat
// colour where the material would be. Checked rather than assumed, because Windows 10
// and the first Windows 11 release are machines this window still has to appear on.
//
// The title bar is drawn too, and it has to be
// --------------------------------------------
// The first version of this file left the caption to DWM, on the reasoning that DWM
// draws a good one and a hand-drawn caption is where snap layouts and the window menu
// go to die. That reasoning was sound and the result was wrong: on a machine with
// "show accent colour on title bars" switched on -- which is a default on many
// installs -- DWM paints the caption a flat accent colour, and **the backdrop stops at
// the bottom of it**. The window body showed Mica and had a solid brick-red bar across
// the top. Half a Mica window looks worse than none.
//
// So the caption is ours: `WM_NCCALCSIZE` gives the whole window to the client area,
// the material then covers every pixel, and the title, the icon and the three buttons
// are painted in the same pass as the page. What that costs is spelled out at each
// handler below, and the two things worth not losing are kept rather than
// reimplemented -- `WM_NCHITTEST` still answers `HTMAXBUTTON` over the maximise
// button, so Windows 11's snap-layout flyout appears on hover, and `HTCAPTION`
// everywhere else, so dragging, double-click-to-maximise and the right-click window
// menu are still DefWindowProc's job.
//
// No accessibility. Every control is drawn and none publishes a UI Automation
// provider, so a screen reader sees one window and nothing in it. This is the largest
// known gap in Micula and it is stated here rather than papered over: a program that
// must be usable without sight should offer the same operations another way -- a
// command line, a config file -- until this is closed.

#pragma once

#include "theme.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dcomp.h>
#include <dwrite.h>
#include <dwmapi.h>
#include <imm.h>
#include <wincodec.h>
#include <windowsx.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "dcomp.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "imm32.lib")
#pragma comment(lib, "windowscodecs.lib")
// And the four a Visual Studio project links by default and a bare `cl` does not.
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

namespace micula {

// ------------------------------------------------------------------ the DPI calls
//
// **Resolved at run time, and the reason is the same one frameclock::Resolve gives.**
//
// These three live in user32 and none of them has always been there:
//
//     GetDpiForWindow           Windows 10 1607 (14393)
//     GetSystemMetricsForDpi    Windows 10 1607
//     SetProcessDpiAwarenessContext  Windows 10 1703 (15063)
//
// Called directly they are bound by the loader, so a machine without one of them does
// not run a program that names it -- it ends the process with STATUS_ENTRYPOINT_NOT_FOUND
// before `wWinMain`, with no window and no message. A header library cannot know which
// Windows its host program promises to support, so it must not quietly raise that
// floor for it. The compositor clock in Window::Run is resolved the same way, for the
// same reason.
//
// The fallbacks are the pre-1607 answers, not stubs: system DPI for the window, the
// plain metrics call, and the process-wide awareness Windows 8.1 and Vista offer. A
// machine old enough to take them is a machine on which per-monitor DPI did not exist,
// so nothing is being taken away from it.
namespace dpiapi {

inline HMODULE User32() {
    static const HMODULE m = GetModuleHandleW(L"user32.dll");
    return m;
}

// Pixels per layout unit for the monitor this window is on; 96 when nothing can say.
inline UINT ForWindow(HWND hwnd) {
    using Fn = UINT(WINAPI *)(HWND);
    static const Fn fn = User32() ? (Fn)GetProcAddress(User32(), "GetDpiForWindow") : nullptr;
    if (fn) return fn(hwnd);
    // The system DPI, which is what every window had before per-monitor existed.
    HDC dc = GetDC(nullptr);
    const UINT d = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(nullptr, dc);
    return d ? d : 96;
}

inline int SystemMetric(int index, UINT dpi) {
    using Fn = int(WINAPI *)(int, UINT);
    static const Fn fn =
        User32() ? (Fn)GetProcAddress(User32(), "GetSystemMetricsForDpi") : nullptr;
    if (fn) return fn(index, dpi);
    return GetSystemMetrics(index);
}

// Per-monitor v2 where it exists, and the best available awareness where it does not.
// Returns what it managed, for nobody in particular: the caller cannot do anything
// with the answer, and the manifest is what takes effect on a normal launch anyway.
inline void EnablePerMonitorV2() {
    using CtxFn = BOOL(WINAPI *)(DPI_AWARENESS_CONTEXT);
    if (User32())
        if (auto set = (CtxFn)GetProcAddress(User32(), "SetProcessDpiAwarenessContext"))
            if (set(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2)) return;
    // Windows 8.1: per-monitor v1, through shcore. Loaded rather than linked, so this
    // does not add a library to a build that would otherwise not need one.
    using AwareFn = HRESULT(WINAPI *)(int);
    if (HMODULE sh = LoadLibraryW(L"shcore.dll"))
        if (auto set = (AwareFn)GetProcAddress(sh, "SetProcessDpiAwareness"))
            if (SUCCEEDED(set(2))) return;             // PROCESS_PER_MONITOR_DPI_AWARE
    SetProcessDPIAware();
}

}  // namespace dpiapi

// DWM attributes that are not in every SDK header. Spelled out so this builds against
// whatever the machine has, and every call is checked -- an older Windows returns
// E_INVALIDARG for an attribute it does not know, which is the signal, not a failure.
constexpr DWORD kDwmImmersiveDarkMode = 20;
constexpr DWORD kDwmCornerPreference  = 33;
constexpr DWORD kDwmSystemBackdrop    = 38;
constexpr DWORD kDwmCornerRound       = 2;
// DWMWA_SYSTEMBACKDROP_TYPE's values, which the SDK this builds against may not name.
constexpr DWORD kDwmBackdropAuto       = 0;   // DWMSBT_AUTO: let DWM choose
constexpr DWORD kDwmBackdropNone       = 1;   // DWMSBT_NONE
constexpr DWORD kDwmBackdropMainWindow = 2;   // DWMSBT_MAINWINDOW == Mica
constexpr DWORD kDwmBackdropAcrylic    = 3;   // DWMSBT_TRANSIENTWINDOW
constexpr DWORD kDwmBackdropTabbed     = 4;   // DWMSBT_TABBEDWINDOW == Mica Alt

// The three system cursors, as wide resource ids. IDC_ARROW and its siblings are
// MAKEINTRESOURCE, which follows UNICODE -- in a program built without it they are
// narrow strings, and LoadCursorW refuses them. Micula calls the W functions
// throughout and must not depend on which way the program sets that macro.
inline const LPCWSTR kCursorArrow = MAKEINTRESOURCEW(32512);   // IDC_ARROW
inline const LPCWSTR kCursorIBeam = MAKEINTRESOURCEW(32513);   // IDC_IBEAM
inline const LPCWSTR kCursorHand  = MAKEINTRESOURCEW(32649);   // IDC_HAND

// The caption, in DIPs. 32 tall and 46 wide per button are Windows 11's own numbers --
// measured off its own windows rather than derived from SM_CYCAPTION, which still
// answers with the Windows 7 caption height plus a frame.
constexpr float kCaptionH    = 32.0f;
constexpr float kCaptionBtnW = 46.0f;
// How close to an edge counts as a resize grip. The frame is gone as far as the client
// area is concerned, so this is synthesised in WM_NCHITTEST.
constexpr float kResizeGrip  = 6.0f;

// ---------------------------------------------------------------- Painter

// Everything drawn goes through one of these. It holds the device context, the fonts
// and the palette, plus a single reusable solid-colour brush.
//
// One brush, recoloured per call, rather than a brush per colour. Creating a brush is
// cheap in Direct2D and the alternative -- a cache keyed by colour -- is a table that
// has to be invalidated on every theme change and on every device loss, which is two
// more places to get wrong for no measurable gain in a window that repaints when
// somebody moves the mouse.
struct Painter {
    ID2D1DeviceContext   *rt = nullptr;
    ID2D1SolidColorBrush *br = nullptr;
    const Fonts   *font = nullptr;
    const Palette *pal  = nullptr;

    ID2D1Brush *Brush(const D2D1_COLOR_F &c) const { br->SetColor(c); return br; }

    void Fill(const D2D1_RECT_F &r, const D2D1_COLOR_F &c) const {
        rt->FillRectangle(r, Brush(c));
    }
    void FillRound(const D2D1_RECT_F &r, float radius, const D2D1_COLOR_F &c) const {
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, radius, radius), Brush(c));
    }
    // Stroked on the *inside* of the rectangle. A 1-DIP stroke centred on the edge
    // straddles the pixel boundary and comes out as two half-covered rows of pixels,
    // which at 100% scaling reads as a blurry grey line instead of a crisp one.
    void StrokeRound(const D2D1_RECT_F &r, float radius, const D2D1_COLOR_F &c,
                     float width = 1.0f) const {
        const D2D1_RECT_F in = { r.left + width / 2, r.top + width / 2,
                                 r.right - width / 2, r.bottom - width / 2 };
        rt->DrawRoundedRectangle(D2D1::RoundedRect(in, radius, radius), Brush(c), width);
    }
    void Line(float x0, float y0, float x1, float y1, const D2D1_COLOR_F &c,
              float width = 1.0f) const {
        rt->DrawLine(D2D1::Point2F(x0, y0), D2D1::Point2F(x1, y1), Brush(c), width);
    }
    // A panel whose four corners are chosen one by one, and whose border runs along any
    // combination of its edges -- WinUI's content layer is one rounded corner, three square ones,
    // a border along the two edges that face the rest of the window, and none along the two that
    // are the window.
    //
    // The shape is a path because it has to be: `FillRoundedRectangle` takes one radius for all
    // four corners, and a page's layer has exactly one of them rounded. Drawing it as a rounded
    // rectangle with the square corners patched on over the top is the same picture for an opaque
    // colour -- and a band of a lighter colour down every edge it passes for a translucent one,
    // which is what a layer colour is: two coats in one place and one everywhere else. Over Mica
    // that is a hint; over Acrylic it is a band you can measure.
    //
    // The path is built per call rather than cached, which is what a page with a handful of
    // panels can afford. A page that draws hundreds of them wants its own cache.
    void Panel(const D2D1_RECT_F &r, const Corners &c, const D2D1_COLOR_F &fill,
               const D2D1_COLOR_F &border = D2D1::ColorF(0, 0.0f),
               unsigned edges = edge::kAll, float width = 1.0f) const {
        ID2D1Factory *factory = nullptr;
        rt->GetFactory(&factory);
        if (!factory) return;
        // One figure per run of neighbouring pieces, and the pieces themselves in drawing order:
        // the top edge, the arc joining it to the right edge, the right edge, and so on round, so
        // that piece `i` runs from `pt[i]` to `pt[i + 1]`. A corner is an arc where two edges meet
        // -- and half an arc is not a corner, which is why a corner needs both of its edges.
        auto shape = [&](const D2D1_RECT_F &box, const Corners &k,
                         unsigned on) -> ID2D1PathGeometry * {
            float tl = k.r[0], tr = k.r[1], br = k.r[2], bl = k.r[3];
            // No two radii on one side may overlap, or the curve crosses itself and the fill comes
            // out inside out. XAML scales the pair down the same way.
            const float w = box.right - box.left, h = box.bottom - box.top;
            if (tl + tr > 0.0f) { const float s = (std::min)(1.0f, w / (tl + tr)); tl *= s; tr *= s; }
            if (bl + br > 0.0f) { const float s = (std::min)(1.0f, w / (bl + br)); bl *= s; br *= s; }
            if (tl + bl > 0.0f) { const float s = (std::min)(1.0f, h / (tl + bl)); tl *= s; bl *= s; }
            if (tr + br > 0.0f) { const float s = (std::min)(1.0f, h / (tr + br)); tr *= s; br *= s; }
            const D2D1_POINT_2F pt[9] = {
                D2D1::Point2F(box.left + tl, box.top),     D2D1::Point2F(box.right - tr, box.top),
                D2D1::Point2F(box.right, box.top + tr),    D2D1::Point2F(box.right, box.bottom - br),
                D2D1::Point2F(box.right - br, box.bottom), D2D1::Point2F(box.left + bl, box.bottom),
                D2D1::Point2F(box.left, box.bottom - bl),  D2D1::Point2F(box.left, box.top + tl),
                D2D1::Point2F(box.left + tl, box.top),
            };
            const float rad[8] = { 0.0f, tr, 0.0f, br, 0.0f, bl, 0.0f, tl };
            const bool lit[8] = {
                (on & edge::kTop) != 0,
                (on & edge::kTop) != 0 && (on & edge::kRight) != 0,
                (on & edge::kRight) != 0,
                (on & edge::kRight) != 0 && (on & edge::kBottom) != 0,
                (on & edge::kBottom) != 0,
                (on & edge::kBottom) != 0 && (on & edge::kLeft) != 0,
                (on & edge::kLeft) != 0,
                (on & edge::kLeft) != 0 && (on & edge::kTop) != 0,
            };
            ID2D1PathGeometry *path = nullptr;
            if (FAILED(factory->CreatePathGeometry(&path)) || !path) return nullptr;
            ID2D1GeometrySink *sink = nullptr;
            if (FAILED(path->Open(&sink)) || !sink) { path->Release(); return nullptr; }
            for (int i = 0; i < 8;) {
                if (!lit[i]) { i++; continue; }
                int j = i;
                while (j < 8 && lit[j]) j++;
                sink->BeginFigure(pt[i], D2D1_FIGURE_BEGIN_FILLED);
                for (int p = i; p < j; p++) {
                    if (rad[p] > 0.0f)
                        sink->AddArc(D2D1::ArcSegment(pt[p + 1], D2D1::SizeF(rad[p], rad[p]), 0.0f,
                                                      D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                                      D2D1_ARC_SIZE_SMALL));
                    else
                        sink->AddLine(pt[p + 1]);
                }
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                i = j;
            }
            sink->Close();
            sink->Release();
            return path;
        };
        // The fill is the whole shape whatever the border covers, and an open figure is filled as
        // if it were closed, which is what makes the two share one builder.
        if (ID2D1PathGeometry *p = shape(r, c, edge::kAll)) {
            rt->FillGeometry(p, Brush(fill));
            p->Release();
        }
        if (border.a > 0.0f && width > 0.0f && edges != 0) {
            // Inset by the stroke and drawn on that edge, which is what StrokeRound does with a
            // rectangle: a stroke centred on the edge is two half-covered rows of pixels.
            const D2D1_RECT_F in = { r.left + width / 2, r.top + width / 2,
                                     r.right - width / 2, r.bottom - width / 2 };
            Corners k = c;
            for (float &rad : k.r) rad = rad > width / 2 ? rad - width / 2 : 0.0f;
            if (ID2D1PathGeometry *p = shape(in, k, edges)) {
                rt->DrawGeometry(p, Brush(border), width);
                p->Release();
            }
        }
        factory->Release();
    }

    // One line, vertically centred in `r`, clipped. CLIP is on because a string that
    // overflows its box should be cut, not painted over the control next to it -- an
    // overflowing label that silently draws across its neighbour is the hardest kind
    // of layout fault to see in a screenshot.
    void Text(const std::wstring &s, const D2D1_RECT_F &r, IDWriteTextFormat *fmt,
              const D2D1_COLOR_F &c) const {
        if (s.empty()) return;
        rt->DrawText(s.c_str(), (UINT32)s.size(), fmt, r, Brush(c),
                     D2D1_DRAW_TEXT_OPTIONS_CLIP);
    }

    // A laid-out paragraph: wraps, and reports how tall it came out so the caller can
    // put the next thing under it.
    //
    // `clip` matters more than it looks. A text layout is drawn from its origin and
    // pays no attention to the height of the rectangle it was measured against, so a
    // two-line string in a one-line box draws its second line over whatever is below.
    float TextWrapped(const std::wstring &s, const D2D1_RECT_F &r, IDWriteTextFormat *fmt,
                      const D2D1_COLOR_F &c, bool measureOnly = false,
                      bool clip = false) const {
        if (s.empty()) return 0.0f;
        IDWriteTextLayout *layout = nullptr;
        if (FAILED(font->dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt,
                                              r.right - r.left, 100000.0f, &layout)) || !layout)
            return 0.0f;
        layout->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        layout->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        DWRITE_TEXT_METRICS m = {};
        layout->GetMetrics(&m);
        if (!measureOnly) {
            if (clip) rt->PushAxisAlignedClip(r, D2D1_ANTIALIAS_MODE_ALIASED);
            rt->DrawTextLayout(D2D1::Point2F(r.left, r.top), layout, Brush(c),
                               D2D1_DRAW_TEXT_OPTIONS_NONE);
            if (clip) rt->PopAxisAlignedClip();
        }
        layout->Release();
        return m.height;
    }

    // How wide a single line wants to be. Buttons size themselves from this rather
    // than from a guess, which is what keeps a Chinese label and an English one both
    // fitting without a magic constant per string.
    float MeasureWidth(const std::wstring &s, IDWriteTextFormat *fmt) const {
        if (s.empty()) return 0.0f;
        IDWriteTextLayout *layout = nullptr;
        if (FAILED(font->dw->CreateTextLayout(s.c_str(), (UINT32)s.size(), fmt,
                                              100000.0f, 100.0f, &layout)) || !layout)
            return 0.0f;
        DWRITE_TEXT_METRICS m = {};
        layout->GetMetrics(&m);
        layout->Release();
        return m.width;
    }
};

inline D2D1_RECT_F Rect(float x, float y, float w, float h) { return { x, y, x + w, y + h }; }
inline float Width(const D2D1_RECT_F &r)  { return r.right - r.left; }
inline float Height(const D2D1_RECT_F &r) { return r.bottom - r.top; }
inline bool  Inside(const D2D1_RECT_F &r, float x, float y) {
    return x >= r.left && x < r.right && y >= r.top && y < r.bottom;
}

// ---------------------------------------------------------------- Widget

struct Window;

// The whole control vocabulary derives from this. Deliberately small: a rectangle,
// three interaction flags, a paint call and a click.
struct Widget {
    D2D1_RECT_F rect = {};
    bool visible = true;
    bool enabled = true;
    bool hover   = false;
    bool pressed = false;
    bool focus   = false;
    // The animated shadows of the three flags above: 0 is off, 1 is on, and anything
    // between is a brush crossing over.
    //
    // **Only the background follows these.** A WinUI control under the pointer moves one
    // property -- `<ContentPresenter.BackgroundTransition>` -- and leaves its border, its
    // text and its focus ring to change between two frames. Fading all of them together
    // is what made the first version of this read as some other platform's idea of
    // Fluent. micula::motion::Ramp is the transition, linear and 83 ms, because that is what
    // BrushTransition is.
    float hoverT = 0.0f, pressT = 0.0f, focusT = 0.0f;
    // Paint order, and hit-test order reversed. Three layers: 0 is the furniture and the
    // page, 1 is raised *within* the page -- a dropdown's list is drawn over the controls
    // below it and takes the click that lands on one of them, and insertion order cannot
    // express that, because the dropdown was added in the middle of the page -- and 2 is
    // over the page altogether, which is what a navigation pane is while it is open over
    // one. Only the drawing order knows the difference between 1 and 2; the hit test asks
    // whether a widget is raised at all, since a click belongs to whatever is on top.
    int  z = 0;
    Window *owner = nullptr;

    virtual ~Widget() {}
    virtual void Paint(const Painter &p) = 0;
    // Only widgets that can be operated from the keyboard join the Tab order.
    virtual bool Focusable() const { return false; }
    virtual void OnClick() {}
    // The mouse went down on this widget, at this point in the widget's own space.
    //
    // For a control that has to know *which part* of itself was grabbed: a colour
    // panel is one rectangle with a square, two strips and a row of swatches in it, and
    // `pressed` does not say which of them the gesture started on.
    //
    // Taken from the message rather than from GetCursorPos, and the difference is not
    // theoretical: the pointer can have moved between the click being queued and this
    // running, so a press position read from the cursor can disagree with the hit test
    // that chose this widget.
    virtual void OnPress(float /*x*/, float /*y*/) {}
    // The mouse moved while this widget holds capture, in the same space as OnPress and
    // taken from the message for the same reason.
    //
    // This and OnPress are the whole of a drag. A control must not update itself out of
    // Paint instead, by reading the cursor each frame: Slider did, and it cost both
    // correctness and testability -- see PressedVisual below for the first and the
    // note on Cursor() for the second.
    virtual void OnDrag(float /*x*/, float /*y*/) {}
    // The pointer moved over this control, or over the region it watches outside itself
    // (see ExternalRegion), in the control's own space.
    //
    // It is sent to the control under the pointer and to nobody else, so a control that
    // has to hear about a move it does not contain says where that is: a scroll bar shows
    // itself when the pointer crosses the page it scrolls, which is a page and not a
    // control. Being told about every move in the window is not the same offer -- it is
    // a coordinate space each control then has to correct by hand.
    virtual void OnPointerMove(float /*x*/, float /*y*/) {}
    // The area outside `rect` -- in the control's own space, so for a scrolling control
    // the page's offset is already off it -- where this control wants OnPointerMove as
    // well. Empty for a control that only answers to itself.
    //
    // Moves only. A press here is a press on whatever is behind, so this widens what a
    // control can see, not what it takes: a scroll bar that could be grabbed by clicking
    // the page it scrolls would be a different control.
    virtual D2D1_RECT_F ExternalRegion() const { return {}; }
    // A wheel turned over this control, in its own space; `notches` is positive away from
    // the user. Return true to keep it from the page.
    //
    // For an open drop-down's list, which scrolls by itself when it is taller than the
    // room it has. Left to the page, the notch scrolled the page instead: the list's own
    // anchor moved out from under it, and on a page that lays itself out in response to a
    // scroll the open list was thrown away with everything else.
    virtual bool OnWheel(float /*x*/, float /*y*/, float /*notches*/) { return false; }
    // The keyboard's way of working this control: Space, and Enter on a control that has one.
    //
    // Distinct from `OnClick`, which is what a mouse press and release means, because a click
    // is the *pointer* doing something and this is not. A drop-down chooses the row the
    // pointer is over, and there is no pointer to read when the choice came from a keyboard:
    // what Space means there is "the one the list has already arrived at". The default is a
    // click, which is what every other control wants.
    virtual void OnActivate() { OnClick(); }
    // Something happened that should put away anything transient this control is
    // showing: a press somewhere else, the window being deactivated. Only a flyout has
    // anything to put away, and the reason this is a window-level broadcast rather than
    // the flyout's own business is that a control cannot see a click it did not get --
    // which is exactly why the drop-down used to stay open until it was clicked again.
    virtual void Dismiss() {}
    // This control draws something that follows the pointer *inside* itself: an open
    // flyout's hovered row, a segmented control's hovered cell, a slider being dragged.
    //
    // The window repaints on hover *changes*, and moving from one row of a list to the
    // next is not one -- the same widget is hovered throughout -- so without this the
    // highlight stays wherever it was when the pointer arrived and only catches up when
    // something else happens to repaint.
    virtual bool TracksPointer() const { return false; }
    // Whether the press shadow should be showing.
    //
    // The window clears `pressed` the moment the pointer leaves the rectangle, which is
    // what makes a button cancellable by dragging off it. A control whose gesture
    // outlives the rectangle overrides this to say it is still held -- a slider dragged
    // out of its own track would otherwise grow its thumb back under a finger that is
    // plainly still on it, and the press is the only cue that the drag has not ended.
    virtual bool PressedVisual() const { return pressed; }
    // The mouse went up on a widget that had capture, wherever the cursor ended up.
    //
    // OnClick is not the same event and cannot stand in for this one: it fires only
    // when the release lands back inside the control, which is exactly what a drag
    // does not do. A slider dragged past its own edge and let go there needs to know
    // the gesture finished, or the value it just produced is never committed.
    virtual void OnRelease() {}
    // Return true to say the key was consumed, so the window does not also treat it
    // as Tab navigation or as the default button.
    virtual bool OnKey(WPARAM /*vk*/) { return false; }
    // A character the keyboard or the IME committed. Only the text field wants these.
    virtual bool OnChar(wchar_t /*ch*/) { return false; }
    // Focus has just left this widget.
    //
    // For a field whose value is only meaningful once it is finished. A text box that
    // wrote its setting on every keystroke would save `#00` on the way to `#0000008C`
    // -- five times, each one a real value that a reader of the file cannot tell from
    // a choice. This is the text field's half of the split Slider already makes
    // between `onChange` and `onCommit`, and it exists for the same reason.
    virtual void OnBlur() {}
    // Where the IME should put its composition window, in DIPs. Only meaningful for a
    // widget that takes text; ignored otherwise.
    virtual bool CaretPoint(D2D1_POINT_2F * /*out*/) const { return false; }
    virtual bool HandCursor() const { return false; }
    virtual bool TextCursor() const { return false; }

    // --- animation ---------------------------------------------------------------
    //
    // Where the 16 ms timer stops. The default is "the three cross-fades have caught up
    // with the three flags", which is the whole of it for most controls; one with a
    // value of its own -- the switch's knob, the segmented control's pill, a flyout
    // opening -- overrides both of these and calls them.
    virtual bool Animating() const {
        return hoverT != Want(hover) || pressT != Want(PressedVisual()) || focusT != Want(focus);
    }
    virtual void Tick(float dt) {
        motion::Ramp(&hoverT, Want(hover), dt, motion::kFaster);
        motion::Ramp(&pressT, Want(PressedVisual()), dt, motion::kFaster);
        // Focus moves a fill too (a text field lightens when the caret is in it) and
        // nothing else: the accent underline and the focus ring are setters, and arrive
        // whole.
        motion::Ramp(&focusT, Want(focus), dt, motion::kFaster);
    }
    // A disabled control animates nothing: its states are all off, so the cross-fades
    // run down to zero and stay there.
    float Want(bool on) const { return on && enabled ? 1.0f : 0.0f; }

    // The cursor, in this widget's own coordinates: window DIPs with the page's paint
    // offset taken back off, so a control that reads the mouse while it is being
    // dragged agrees with where it was drawn. Defined under Window, which is what
    // knows the offset.
    //
    // This is the *physical* pointer, so a control that takes its value from here cannot
    // be driven by posted messages -- a harness sends a WM_MOUSEMOVE at one place and the
    // widget reads the mouse at another. Take a drag's coordinates from OnPress and
    // OnDrag; this is for the few things that genuinely mean "where is the pointer now",
    // such as a hovered row.
    D2D1_POINT_2F Cursor() const;
    // The part of the page that is on screen, in this widget's own coordinates -- the
    // window's ClipRect with the page's paint offset added back on. Empty when the page
    // does not scroll, and empty in the same way ClipRect is.
    //
    // For a control that has to know how much room it really has. A drop-down deciding
    // whether its list fits below it is asking about the visible strip, and on a
    // scrolling page that strip is not the window's: it moves with the page while the
    // control's own rectangle stays where the layout put it.
    D2D1_RECT_F VisibleArea() const;

    // Whether the pointer is on this control, in the control's own coordinates -- the page's
    // offset has already been taken off. This is what the window's hit test asks, rather than
    // `rect` on its own.
    //
    // It exists for the control whose rectangle is not its own to keep: a page hands one a fresh
    // `rect` in every Layout, taking back the edge the control derives from its own state, and
    // deriving it again is a frame's work. Between the two -- a layout with no animation after
    // it -- the control is drawn correctly and cannot be clicked, which is a control that is
    // broken for seconds at a time and comes back when anything else in the window happens to
    // animate. See `SideNav`.
    virtual bool Covers(float x, float y) const { return Inside(rect, x, y); }

    // This widget moves with the page's scroll: its `rect` is in the page's own space --
    // window coordinates with the scroll *not* taken off -- and the offset that puts it
    // on screen comes from ContentTransform() when it is painted and hit-tested. So,
    // painted only inside the window's ClipRect() and taking no clicks outside it.
    //
    // Left false for the furniture -- a navigation list, a title-bar button, the page's
    // own scroll bar -- which does not scroll, and which clipping to the scrolling area
    // would hide.
    bool scrolls = false;
    // Survives ClearWidgets, together with the capture or focus it holds.
    //
    // For a control the page lays out *while it is being operated*: the page is rebuilt,
    // the gesture is not over, and a rebuild would drop the capture -- so it would end on
    // its first pixel. A scroll bar's thumb held through a resize is what this is for, and
    // so is anything else a page repositions on every layout that a person can hold on
    // to. The page makes such a control once and repositions it in each Layout().
    bool persistent = false;
};

// One Windows timer, whose id nobody had to choose: the window hands them out from a pool of its
// own, so a control that needs one does not have to know which numbers the library uses -- or
// which numbers a page picked for itself.
//
// It is also the answer to where the timer's message goes. The window keeps the timers that are
// running and offers every `WM_TIMER` to them by id, so a timer belongs to whatever started it.
// The window used to walk its *widgets* instead and ask each one `OnTimer(id)`, which required
// any owner to be in that list: a scroll bar inside a drop-down is not, so its timers arrived
// nowhere at all -- silently -- and the drop-down carried a forwarder to work around it.
class Timer {
public:
    Timer() = default;
    Timer(const Timer &) = delete;
    Timer &operator=(const Timer &) = delete;
    ~Timer();
    // Starts it, or moves it: `fn` runs `ms` from now, and again every `ms` -- a Windows timer
    // repeats until it is stopped. Called on the window's thread, like everything else here.
    void Start(Window *w, UINT ms, std::function<void()> fn);
    // Ends it, and gives the id back. Safe to call from inside the callback.
    void Stop();
    bool Running() const { return win != nullptr; }
    // One `WM_TIMER`: true when the id was this timer's.
    bool Handle(UINT_PTR which);
private:
    Window *win = nullptr;
    UINT_PTR id = 0;
    std::function<void()> tick;
};

// ---------------------------------------------------------------- Window

struct Window {
    HWND hwnd = nullptr;
    UINT dpi  = 96;
    Palette pal;
    Fonts   fonts;
    // False on Windows 11 before 22H2, and on anything that refuses the attribute.
    // The page paints an opaque background instead of letting the material through.
    bool micaActive = false;
    // Which system backdrop to ask DWM for: one of the kDwmBackdrop* values, Mica unless a page
    // says otherwise -- `kDwmBackdropAcrylic` is the translucent one, `kDwmBackdropTabbed` Mica
    // Alt, `kDwmBackdropNone` a flat window. Set it before `Create`; a page that switches
    // material at run time sets it and calls `ApplyThemeToFrame()`.
    DWORD backdrop = kDwmBackdropMainWindow;
    bool resizable = false;
    // How Create shows the window. SW_HIDE leaves it hidden for the program to show
    // later: after restoring a saved position, say, or without taking the foreground
    // (SetWindowPos with SWP_SHOWWINDOW | SWP_NOACTIVATE). SW_SHOW activates, and the
    // window that had the foreground loses it even when the activation is refused.
    int showCommand = SW_SHOW;
    // How much of the window is not client area, in pixels: the resize border on the
    // left, right and bottom. Read from the window rather than computed, because the
    // caption is client area here and no system metric describes that frame.
    SIZE frameExtra = {};
    void MeasureFrame() {
        if (!hwnd || IsIconic(hwnd) || IsZoomed(hwnd)) return;
        RECT wr, cr;
        GetWindowRect(hwnd, &wr);
        GetClientRect(hwnd, &cr);
        frameExtra = { (wr.right - wr.left) - cr.right, (wr.bottom - wr.top) - cr.bottom };
    }

    // The composition stack, in the order it has to be built and the reverse of the
    // order it has to be torn down.
    IDWriteFactory       *dw    = nullptr;
    ID3D11Device         *d3d   = nullptr;
    IDXGIDevice          *dxgi  = nullptr;
    ID2D1Factory1        *d2d   = nullptr;
    ID2D1Device          *d2dDevice = nullptr;
    ID2D1DeviceContext   *dc    = nullptr;
    IDXGISwapChain1      *swap  = nullptr;
    ID2D1Bitmap1         *target = nullptr;
    IDCompositionDevice  *comp   = nullptr;
    IDCompositionTarget  *compTarget = nullptr;
    IDCompositionVisual  *compVisual = nullptr;
    ID2D1SolidColorBrush *brush = nullptr;

    // The caption we draw. `hot` and `down` are 0/1/2 for minimise, maximise, close.
    HICON         appIcon = nullptr;
    ID2D1Bitmap1 *iconBitmap = nullptr;

    // Pictures loaded from disk, keyed by path. Device-bound, so they live next to
    // the device that owns them and die with it in ReleaseDevice -- a bitmap that
    // outlives its target is a crash rather than a blank tile.
    //
    // A failed load caches a null. A page that shows a grid of pictures asks for all of
    // them on every paint, and a missing file would otherwise be a failed decode per
    // picture per frame of every resize.
    std::map<std::wstring, ID2D1Bitmap1 *> images;
    int  captionHot = -1;
    int  captionDown = -1;
    // The three buttons' hover fades. Windows' own caption tints under the pointer
    // rather than switching, and the close button is the one people notice.
    float captionT[3] = { 0.0f, 0.0f, 0.0f };
    bool active = true;

    // The timers this window is running, and the ids they took -- see Timer, which is where both
    // the ids and the dispatch come from. Declared before `widgets` because a control's timer
    // takes itself out of this list as it stops, which happens while the controls are being
    // destroyed.
    std::vector<Timer *> timers;
    std::vector<UINT_PTR> timerIds;
    UINT_PTR TakeTimerId();
    void GiveTimerId(UINT_PTR id);
    // The window's own two, from the same pool: the caret's blink, and the frame loop's stand-in
    // while Windows is running a modal size or move loop of its own.
    Timer caretTimer, frameTimer;

    std::vector<std::unique_ptr<Widget>> widgets;
    Widget *capture = nullptr;    // the widget the mouse went down on
    Widget *focused = nullptr;
    // Set when the keyboard was used to move focus. Windows only paints focus rings
    // after somebody has pressed Tab, and copying that is the difference between a
    // window that looks calm on arrival and one covered in rectangles.
    bool showFocusRing = false;
    bool caretOn = true;

    // The animation clock, and it is QueryPerformanceCounter rather than GetTickCount64
    // for a measured reason: GetTickCount64's resolution is the system tick, 15.6 ms, so
    // every dt it can report is 0, 15.6 or 31.2 -- the quantisation is the same size as
    // the frame, and the motion inherits it as a stutter. Measured before this changed:
    // frame gaps of 2 to 35 ms, median 24.
    LARGE_INTEGER qpcFreq = {};
    LARGE_INTEGER qpcLast = {};
    // True while the frame loop is the thing running, rather than GetMessage. Read by a
    // page that needs to know whether a value it was handed should animate or land
    // (motion::Track::To or Track::Set) when it lays itself out.
    bool animOn = false;
    // True while Windows is running a modal loop of its own for a drag of the border or the
    // caption, during which the frame loop cannot run and WM_PAINT is the only painting
    // there is. See WM_ENTERSIZEMOVE.
    bool inSizeMove = false;
    bool alive = true;

    float scale() const { return dpi / 96.0f; }
    float ClientW() const { RECT r; GetClientRect(hwnd, &r); return r.right / scale(); }
    float ClientH() const { RECT r; GetClientRect(hwnd, &r); return r.bottom / scale(); }

    virtual ~Window() {}

    // --- to implement -----------------------------------------------------------
    virtual const wchar_t *ClassName() const = 0;
    virtual const wchar_t *Title() const = 0;
    virtual void Layout() {}                       // position widgets, in DIPs
    virtual void PaintPage(const Painter &) {}     // everything that is not a widget
    virtual void OnDefaultAction() {}              // Enter
    virtual void OnCancel() { PostMessageW(hwnd, WM_CLOSE, 0, 0); }   // Esc
    // ~16 ms while anything is moving. `dt` is real elapsed seconds, clamped; a page
    // that animates something of its own advances it by that rather than by a constant.
    virtual void OnTick(float /*dt*/) {}
    // Does the *page* want the timer kept alive -- a progress bar, a spinner, a page
    // arriving? The controls' own animations are Animating() below and are not this
    // question; a subclass overriding this does not have to know about them.
    virtual bool AnimationWanted() const { return false; }
    virtual bool OnAppMessage(UINT, WPARAM, LPARAM) { return false; }
    // The area scrolling widgets live in, in DIPs. An empty rectangle -- the default --
    // means the window does not scroll and nothing is clipped.
    virtual D2D1_RECT_F ClipRect() const { return D2D1_RECT_F{ 0, 0, 0, 0 }; }
    // An offset and an opacity for the scrolling half of the page, applied when it is
    // painted and taken back off when a click is hit-tested against it.
    //
    // This is how a page arrives and how a wheel notch glides: the layout stays where
    // the scroll says it is and the *drawing* lags behind it, which costs one transform
    // per frame instead of a whole re-layout. Because the hit test subtracts the same
    // number, a control clicked mid-glide is the control that was under the pointer.
    virtual void ContentTransform(float *dy, float *opacity) const {
        *dy = 0.0f; *opacity = 1.0f;
    }
    // The smallest the window may be dragged to, in DIPs. Zero means no limit.
    //
    // A resizable window without one is a window somebody can drag to nothing, and a
    // layout that computes its columns by dividing the width it is given goes negative
    // at a small enough size, with the controls coming out inside out. Scrolling answers
    // "too short"; this answers "too narrow".
    virtual void MinSize(int *w, int *h) const { *w = 0; *h = 0; }

    // --- lifetime ---------------------------------------------------------------
    bool Create(int dipW, int dipH, bool canResize, HICON icon);
    int  Run();
    // While the frame loop is animating it draws every frame itself and clears the update
    // region after each one, so invalidating as well buys nothing -- and it costs a frame:
    // the region it sets is handed back by the next PeekMessage as a WM_PAINT, which paints
    // the window a second time in the same frame. See Window::Run and the WM_PAINT case.
    void Invalidate() { if (hwnd && !animOn) InvalidateRect(hwnd, nullptr, FALSE); }

    // Everything the window animates on its own account: every control's pointer states
    // and the three caption buttons. Distinct from AnimationWanted(), which is the
    // page's own answer -- the frame loop runs while either of them says so.
    bool Animating() const {
        for (int i = 0; i < 3; i++)
            if (captionT[i] != (captionHot == i ? 1.0f : 0.0f)) return true;
        for (const auto &w : widgets) if (w->visible && w->Animating()) return true;
        return false;
    }
    void Tick(float dt) {
        for (int i = 0; i < 3; i++)
            motion::Ramp(&captionT[i], captionHot == i ? 1.0f : 0.0f, dt, motion::kFaster);
        for (auto &w : widgets) w->Tick(dt);
    }

    // One frame's worth of time, and what it is spent on. Called from the loop in Run().
    void Frame() {
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt = (float)((double)(now.QuadPart - qpcLast.QuadPart) /
                           (double)qpcFreq.QuadPart);
        qpcLast = now;
        // Clamped only at the top, and generously: a frame that took 100 ms is a frame
        // the machine really did take that long over, and the animation should be 100 ms
        // further along rather than pretending otherwise. What must not happen is a
        // whole animation in one step after the window was behind a modal dialog.
        if (dt < 0.0f) dt = 0.0f;
        if (dt > 0.1f) dt = 0.1f;
        // Hover first: the page may have been rebuilt since the last frame.
        RefreshHover();
        Tick(dt);
        OnTick(dt);
    }

    // Hover, recomputed from where the cursor actually is rather than from the last
    // mouse message.
    //
    // Needed because the widget list is rebuilt more often than the mouse moves: an
    // expander opening, a page switching, a button relabelling itself and every saved
    // setting all call Layout(), and the control under a *stationary* pointer is then a
    // new object with hover false -- which used to make the highlight vanish under the
    // cursor and now would fade it out, which is worse. Called from the tick, where a
    // rebuild has just happened.
    bool RefreshHover();

    template <typename T> T *Add(T *w) {
        w->owner = this;
        widgets.emplace_back(w);
        return w;
    }
    // --- what a running callback is still standing on -----------------------------
    //
    // `Layout()` is routinely called from inside a widget's own callback, because "go
    // to the next page" is a button that replaces the page the button is on. Clearing
    // the list there frees the `std::function` whose body is currently executing --
    // and MSVC reloads a lambda's captures out of that closure after every call the
    // body makes, so the `Invalidate()` such a handler ends with reads `this` back out
    // of freed memory.
    //
    // It is a crash exactly when that block has been reused in between, which is why
    // it reads as "the program usually dies on its busiest page" rather than as a fault
    // in any one page: the busiest page is the one whose layout allocates enough to take
    // the block back. Seen in the field as an access violation at `mov rax,[rbx+8]` --
    // the captured `this`, reloaded from the closure -- on the instruction after the
    // call to `Layout`.
    //
    // The comment over `OnClick` in `Proc` had half of this already: it stopped the
    // *window* touching a widget that a click had replaced. This is the other half,
    // which is the widget touching itself. So a widget replaced while a message is
    // being dispatched is moved here rather than deleted, and this list is emptied
    // when that message has returned. Outside dispatch nothing is held: there is no
    // closure running to protect.
    std::vector<std::unique_ptr<Widget>> retired;
    int dispatchDepth = 0;

    // One window message, and any nested ones -- a modal dialog, a drop-down running
    // its own loop -- that happen inside it. Only the outermost frees, because the
    // closure being protected belongs to the outermost.
    struct Dispatch {
        Window *w;
        explicit Dispatch(Window *win) : w(win) { w->dispatchDepth++; }
        ~Dispatch();
        Dispatch(const Dispatch &) = delete;
        Dispatch &operator=(const Dispatch &) = delete;
    };

    void ClearWidgets() {
        // A persistent control keeps its place, and the capture or focus it holds with it.
        // See Widget::persistent.
        if (capture && !capture->persistent) capture = nullptr;
        if (focused && !focused->persistent) focused = nullptr;
        std::vector<std::unique_ptr<Widget>> kept;
        for (auto &w : widgets) {
            if (w->persistent)          kept.emplace_back(std::move(w));
            else if (dispatchDepth > 0) retired.emplace_back(std::move(w));
        }
        widgets = std::move(kept);
    }

    // --- internals --------------------------------------------------------------
    void ApplyThemeToFrame();
    void ReloadTheme();
    bool CreateDevice();
    bool CreateSizedResources();
    void ReleaseSizedResources();
    void ReleaseDevice();
    void Resize();
    void Paint();
    void PaintCaption(const Painter &p);
    void EnsureIconBitmap();
    // A picture from disk, decoded once and scaled on the way in to `maxW` pixels
    // wide. Null when the file is not there, which callers draw around rather than
    // treat as an error: a preview that has not been generated yet is a normal state,
    // not a fault. Needs COM initialised on this thread, because WIC is COM.
    ID2D1Bitmap1 *Image(const std::wstring &path, UINT maxW);
    void ReleaseImages();
    LRESULT CaptionHitTest(POINT screen) const;
    Widget *HitTest(float x, float y);
    // Everything but `except` puts away what it is showing -- an open list, a peeked pane. From
    // a copy of the list, because a dismissal is allowed to lay the page out again and the list
    // itself may not survive that. See `retired`.
    void DismissOthers(Widget *except);
    void MoveFocus(int delta);
    void SetFocusTo(Widget *w);
    // Ends a gesture the pointer is no longer allowed to finish, and hands the widget
    // the release it will otherwise never see. See the WM_CAPTURECHANGED handler.
    void CancelCapture();
    void PlaceImeAtCaret();

    static LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM w, LPARAM l);
};

inline UINT_PTR Window::TakeTimerId() {
    // From the bottom up, and 1 is left out: `SetTimer` refuses 0, and a program that sets a timer
    // of its own is likelier to have picked 1 than 2. Timers are few, so a scan is a scan.
    for (UINT_PTR id = 2;; id++) {
        bool taken = false;
        for (UINT_PTR used : timerIds) if (used == id) { taken = true; break; }
        if (!taken) { timerIds.push_back(id); return id; }
    }
}

inline void Window::GiveTimerId(UINT_PTR id) {
    for (size_t i = 0; i < timerIds.size(); i++)
        if (timerIds[i] == id) { timerIds.erase(timerIds.begin() + i); return; }
}

inline void Timer::Start(Window *w, UINT ms, std::function<void()> fn) {
    if (!w || !w->hwnd) return;
    if (win && win != w) Stop();   // a timer belongs to one window at a time
    if (!win) {
        win = w;
        id = w->TakeTimerId();
        w->timers.push_back(this);
    }
    this->tick = std::move(fn);
    SetTimer(w->hwnd, id, ms, nullptr);   // an id that is already set is simply re-armed
}

inline void Timer::Stop() {
    if (!win) return;
    if (win->hwnd) KillTimer(win->hwnd, id);
    for (size_t i = 0; i < win->timers.size(); i++)
        if (win->timers[i] == this) { win->timers.erase(win->timers.begin() + i); break; }
    win->GiveTimerId(id);
    win = nullptr;
    id = 0;
    tick = nullptr;
}

inline Timer::~Timer() { Stop(); }

inline bool Timer::Handle(UINT_PTR which) {
    if (which != id) return false;
    // Copied, because the callback may stop this timer -- the scroll bar's state timer does --
    // and stopping clears `tick` out from under the call that is running it.
    const std::function<void()> f = tick;
    if (f) f();
    return true;
}

inline void ApplyBackdrop(HWND hwnd, bool dark, bool *micaOut,
                          DWORD backdrop = kDwmBackdropMainWindow) {
    const BOOL d = dark ? TRUE : FALSE;
    DwmSetWindowAttribute(hwnd, kDwmImmersiveDarkMode, &d, sizeof(d));
    const DWORD round = kDwmCornerRound;
    DwmSetWindowAttribute(hwnd, kDwmCornerPreference, &round, sizeof(round));
    // The one that matters, and the one that can fail. The system backdrops are Windows 11
    // 22H2 and later; before that this returns E_INVALIDARG and the caller paints an opaque
    // background instead.
    const HRESULT hr = DwmSetWindowAttribute(hwnd, kDwmSystemBackdrop, &backdrop,
                                             sizeof(backdrop));
    if (micaOut) *micaOut = SUCCEEDED(hr);
}

inline void Window::ApplyThemeToFrame() {
    ApplyBackdrop(hwnd, pal.dark, &micaActive, backdrop);
}

inline void Window::ReloadTheme() {
    pal = MakePalette(SystemUsesDarkTheme());
    ApplyThemeToFrame();
    Invalidate();
}

// ---------------------------------------------------------------- device

inline bool Window::CreateDevice() {
    if (dc) return true;

    // BGRA support is required by Direct2D and is not on by default. Hardware first,
    // WARP second: this window has to appear on a machine with a broken or absent
    // display driver, because one of the things it may have to say is why.
    const D3D_DRIVER_TYPE types[] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP };
    for (D3D_DRIVER_TYPE t : types) {
        if (SUCCEEDED(D3D11CreateDevice(nullptr, t, nullptr,
                                        D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                        nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr)))
            break;
    }
    if (!d3d) return false;
    if (FAILED(d3d->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi))) return false;

    D2D1_FACTORY_OPTIONS opts = {};
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1),
                                 &opts, (void **)&d2d)))
        return false;
    if (FAILED(d2d->CreateDevice(dxgi, &d2dDevice))) return false;
    if (FAILED(d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc)))
        return false;

    // Grayscale, not ClearType. Subpixel antialiasing needs to know the opaque colour
    // behind the glyph, and on a composed surface with real alpha there is not one --
    // ClearType on a transparent target either fails or fringes. WinUI draws its text
    // this way for the same reason.
    dc->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    if (FAILED(dc->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &brush))) return false;

    IDXGIAdapter *adapter = nullptr;
    IDXGIFactory2 *factory = nullptr;
    if (FAILED(dxgi->GetAdapter(&adapter)) || !adapter) return false;
    const HRESULT fhr = adapter->GetParent(__uuidof(IDXGIFactory2), (void **)&factory);
    adapter->Release();
    if (FAILED(fhr) || !factory) return false;

    RECT rc; GetClientRect(hwnd, &rc);
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    desc.Width  = (UINT)(rc.right  > 0 ? rc.right  : 1);
    desc.Height = (UINT)(rc.bottom > 0 ? rc.bottom : 1);
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    // The line the whole file exists for. Without premultiplied alpha the swap chain
    // is opaque and Mica is behind a solid rectangle.
    desc.AlphaMode   = DXGI_ALPHA_MODE_PREMULTIPLIED;
    const HRESULT shr = factory->CreateSwapChainForComposition(d3d, &desc, nullptr, &swap);
    factory->Release();
    if (FAILED(shr) || !swap) return false;

    if (FAILED(DCompositionCreateDevice(dxgi, __uuidof(IDCompositionDevice), (void **)&comp)))
        return false;
    if (FAILED(comp->CreateTargetForHwnd(hwnd, TRUE, &compTarget))) return false;
    if (FAILED(comp->CreateVisual(&compVisual))) return false;
    compVisual->SetContent(swap);
    compTarget->SetRoot(compVisual);
    comp->Commit();

    return CreateSizedResources();
}

inline bool Window::CreateSizedResources() {
    if (!swap || !dc) return false;
    IDXGISurface *surface = nullptr;
    if (FAILED(swap->GetBuffer(0, __uuidof(IDXGISurface), (void **)&surface)) || !surface)
        return false;
    // The DPI goes on the bitmap, so every coordinate a page or a control uses is a DIP
    // and the scale factor is applied once, by Direct2D, rather than being multiplied
    // into forty rectangles by hand.
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        (float)dpi, (float)dpi);
    const HRESULT hr = dc->CreateBitmapFromDxgiSurface(surface, &props, &target);
    surface->Release();
    if (FAILED(hr)) return false;
    dc->SetTarget(target);
    dc->SetDpi((float)dpi, (float)dpi);
    return true;
}

inline void Window::ReleaseSizedResources() {
    if (dc) dc->SetTarget(nullptr);
    if (target) { target->Release(); target = nullptr; }
}

// The icon bitmap belongs to the device context, so it goes when that does.

inline void Window::Resize() {
    if (!swap) return;
    RECT rc; GetClientRect(hwnd, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    ReleaseSizedResources();
    swap->ResizeBuffers(0, (UINT)rc.right, (UINT)rc.bottom, DXGI_FORMAT_UNKNOWN, 0);
    CreateSizedResources();
}

inline void Window::ReleaseImages() {
    for (std::map<std::wstring, ID2D1Bitmap1 *>::iterator it = images.begin();
         it != images.end(); ++it)
        if (it->second) it->second->Release();
    images.clear();
}

inline void Window::ReleaseDevice() {
    ReleaseSizedResources();
    ReleaseImages();
    if (iconBitmap) { iconBitmap->Release(); iconBitmap = nullptr; }
    if (brush)      { brush->Release();      brush = nullptr; }
    if (compVisual) { compVisual->Release(); compVisual = nullptr; }
    if (compTarget) { compTarget->Release(); compTarget = nullptr; }
    if (comp)       { comp->Release();       comp = nullptr; }
    if (swap)       { swap->Release();       swap = nullptr; }
    if (dc)         { dc->Release();         dc = nullptr; }
    if (d2dDevice)  { d2dDevice->Release();  d2dDevice = nullptr; }
    if (d2d)        { d2d->Release();        d2d = nullptr; }
    if (dxgi)       { dxgi->Release();       dxgi = nullptr; }
    if (d3d)        { d3d->Release();        d3d = nullptr; }
}

inline void Window::Paint() {
    if (!CreateDevice() || !target) return;
    Painter p;
    p.rt = dc; p.br = brush; p.font = &fonts; p.pal = &pal;

    dc->BeginDraw();
    // Transparent when the material is there, opaque when it is not. This one call is
    // the difference between a Mica window and a grey one.
    dc->Clear(micaActive ? D2D1::ColorF(0, 0, 0, 0) : pal.windowBg);
    PaintPage(p);
    // Scrolling widgets are clipped to the page's own area, so a card that has been
    // scrolled up stops at the header instead of drawing across it.
    const D2D1_RECT_F clip = ClipRect();
    const bool clipping = clip.right > clip.left && clip.bottom > clip.top;
    // The page's arrival and its scroll glide, applied to the scrolling half of the
    // page only: a navigation list and a header button are furniture and stay put.
    float dy = 0.0f, op = 1.0f;
    ContentTransform(&dy, &op);
    // The page's widgets go under one clip, one transform and at most one layer --
    // rather than one of each per widget, which is what this was and which put twenty
    // intermediate surfaces into every frame of a page transition.
    //
    // That means painting the furniture (widgets with `scrolls` false) before the page
    // rather than interleaved with it in insertion order. They do not overlap -- the
    // furniture is outside ClipRect by construction, which is the same fact that makes
    // the clip legal -- so the order between the two groups cannot show.
    // A control that has been scrolled clear of the clip is not painted at all. The clip
    // would discard its pixels anyway, but only after it had built everything it draws --
    // and a long page pays for every row above and below the visible strip, on every
    // frame of a scroll. Four DIPs of slack, because a control may draw a little outside
    // its own rectangle: a focus ring, a shadow, a flyout the control has not grown its
    // rect to cover.
    auto reaches = [&](const Widget *w) {
        const D2D1_RECT_F r = { w->rect.left, w->rect.top + dy,
                                w->rect.right, w->rect.bottom + dy };
        return r.right + 4.0f > clip.left && r.left - 4.0f < clip.right &&
               r.bottom + 4.0f > clip.top && r.top - 4.0f < clip.bottom;
    };
    auto shown = [&](const Widget *w) {
        return w->visible && (!clipping || !w->scrolls || reaches(w));
    };
    auto pass = [&](int z) {
        for (auto &w : widgets)
            if (w->visible && w->z == z && !w->scrolls) w->Paint(p);
        bool any = false;
        for (auto &w : widgets)
            if (w->z == z && w->scrolls && shown(w.get())) { any = true; break; }
        if (!any) return;
        // Clip first, transform second. The clip is a fixed window onto the page and
        // must not move with what is being drawn inside it -- pushed the other way round
        // it slides too, and the cards then run off under the header.
        //
        // The clip is the *page's*, so it is the page's contents that take it. Anything
        // with a z is over the page rather than in it -- a lid, a flyout, a pane that
        // covers the content -- and is drawn whole: its shadow reaches outside its own
        // rectangle by design, and a panel whose shadow is sliced off at the header stops
        // reading as something floating above the page at all. What such a control still
        // does is keep *itself* inside the room it has, which is `Bounds()` in the
        // drop-down and the window's own edges in a pane.
        const bool pageArea = z == 0;
        const bool clipped = clipping && pageArea;
        if (clipped) dc->PushAxisAlignedClip(clip, D2D1_ANTIALIAS_MODE_ALIASED);
        if (dy != 0.0f) dc->SetTransform(D2D1::Matrix3x2F::Translation(0.0f, dy));
        const bool layered = op < 1.0f;
        if (layered) {
            const D2D1_RECT_F b = clipped
                ? D2D1_RECT_F{ clip.left, clip.top - 32, clip.right, clip.bottom + 32 }
                : D2D1::InfiniteRect();
            dc->PushLayer(D2D1::LayerParameters(b, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                D2D1::IdentityMatrix(), op), nullptr);
        }
        for (auto &w : widgets)
            if (w->z == z && w->scrolls && shown(w.get())) w->Paint(p);
        if (layered) dc->PopLayer();
        if (dy != 0.0f) dc->SetTransform(D2D1::Matrix3x2F::Identity());
        if (clipped) dc->PopAxisAlignedClip();
    };
    pass(0);
    pass(1);
    pass(2);
    // Last, so a page that draws to the top of its own area cannot run under the
    // caption -- which is now client area like any other, and has nothing but paint
    // order protecting it.
    PaintCaption(p);
    const HRESULT hr = dc->EndDraw();
    if (SUCCEEDED(hr)) {
        swap->Present(1, 0);
    } else if (hr != D2DERR_RECREATE_TARGET) {
        // Nothing to do, and the comment is the point: Direct2D batches, so an illegal
        // call is reported *here* rather than where it was made, and the entire frame
        // is discarded. That reads as "the window drew nothing", which sends you
        // looking at the layout. It has happened once already -- a FillOpacityMask in
        // the wrong antialias mode, returning D2DERR_WRONG_STATE (0x88990001) -- and
        // the way it was found was a temporary log of this HRESULT. Put one back here
        // before looking anywhere else.
    }

    // The GPU went away -- a driver update, a remote session, a virtual display driver
    // being removed. Everything device-bound is thrown away and rebuilt on the next
    // paint.
    if (hr == D2DERR_RECREATE_TARGET) {
        ReleaseDevice();
        Invalidate();
    }
}

// The app icon, as an **opacity mask** rather than as a picture. Converted once, kept.
//
// Two decisions, and the second is the one worth reading.
//
// Through WIC rather than DrawIconEx: there is no GDI device context in this window at
// all -- the surface belongs to DirectComposition -- so the icon has to arrive as a
// bitmap. `CreateBitmapFromHICON` does the AND-mask-to-alpha work that makes an icon
// with soft edges composite correctly.
//
// And converted to A8, so the caption draws it in the caption's own text colour. A
// monochrome mark -- white throughout, with the shape carried entirely in the alpha
// channel -- is fine while DWM paints the caption a dark accent colour and invisible
// the moment the caption is Mica over a light wallpaper. Drawing the alpha as a mask is
// what a monochrome mark is for, and it keeps the icon legible in both themes without
// either of them being special-cased.
//
// The cost is that a *colour* icon is drawn as its silhouette. Give the window a
// monochrome mark, or no icon at all -- the title then moves up to the left edge.
inline void Window::EnsureIconBitmap() {
    if (iconBitmap || !appIcon || !dc) return;
    IWICImagingFactory *wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic))) || !wic)
        return;
    IWICBitmap *src = nullptr;
    IWICFormatConverter *conv = nullptr;
    if (SUCCEEDED(wic->CreateBitmapFromHICON(appIcon, &src)) && src &&
        SUCCEEDED(wic->CreateFormatConverter(&conv)) && conv &&
        SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat8bppAlpha,
                                   WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeMedianCut))) {
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_NONE,
            D2D1::PixelFormat(DXGI_FORMAT_A8_UNORM, D2D1_ALPHA_MODE_STRAIGHT));
        dc->CreateBitmapFromWicBitmap(conv, &props, &iconBitmap);
    }
    if (conv) conv->Release();
    if (src) src->Release();
    wic->Release();
}

// Scaled on the way in, by WIC, not on the way out by Direct2D.
//
// Pictures are routinely far larger than they are drawn: nine screenshots at 2560x1440
// are about seventy megabytes of decoded pixels for a grid of thumbnails a hundred and
// forty pixels wide. An IWICBitmapScaler in the chain decodes straight into the size
// that will be drawn, so what reaches the GPU is the thumbnail rather than the
// wallpaper.
inline ID2D1Bitmap1 *Window::Image(const std::wstring &path, UINT maxW) {
    const std::map<std::wstring, ID2D1Bitmap1 *>::iterator hit = images.find(path);
    if (hit != images.end()) return hit->second;
    images[path] = nullptr;              // remembered even if everything below fails
    if (!dc || path.empty()) return nullptr;

    IWICImagingFactory *wic = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&wic))) || !wic)
        return nullptr;

    IWICBitmapDecoder *dec = nullptr;
    IWICBitmapFrameDecode *frame = nullptr;
    IWICBitmapScaler *scaler = nullptr;
    IWICFormatConverter *conv = nullptr;
    ID2D1Bitmap1 *out = nullptr;
    UINT w = 0, h = 0;
    if (SUCCEEDED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ,
                                                 WICDecodeMetadataCacheOnDemand, &dec)) &&
        dec && SUCCEEDED(dec->GetFrame(0, &frame)) && frame &&
        SUCCEEDED(frame->GetSize(&w, &h)) && w && h &&
        SUCCEEDED(wic->CreateBitmapScaler(&scaler)) && scaler) {
        const UINT tw = maxW && w > maxW ? maxW : w;
        const UINT th = tw == w ? h : (UINT)((unsigned long long)h * tw / w);
        if (SUCCEEDED(scaler->Initialize(frame, tw, th ? th : 1,
                                         WICBitmapInterpolationModeFant)) &&
            SUCCEEDED(wic->CreateFormatConverter(&conv)) && conv &&
            SUCCEEDED(conv->Initialize(scaler, GUID_WICPixelFormat32bppPBGRA,
                                       WICBitmapDitherTypeNone, nullptr, 0.0,
                                       WICBitmapPaletteTypeMedianCut))) {
            const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
                D2D1_BITMAP_OPTIONS_NONE,
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            dc->CreateBitmapFromWicBitmap(conv, &props, &out);
        }
    }
    if (conv)   conv->Release();
    if (scaler) scaler->Release();
    if (frame)  frame->Release();
    if (dec)    dec->Release();
    wic->Release();

    images[path] = out;
    return out;
}

inline void Window::PaintCaption(const Painter &p) {
    const float w = ClientW();
    const Palette &c = *p.pal;
    // Dimmed when the window is not the active one, which is the one caption cue people
    // read without knowing they are reading it.
    const D2D1_COLOR_F fg = active ? c.textPrimary : c.textDisabled;

    EnsureIconBitmap();
    if (iconBitmap) {
        // The four-argument overload, which belongs to ID2D1DeviceContext. The
        // five-argument one with the D2D1_OPACITY_MASK_CONTENT enum is
        // ID2D1RenderTarget's, and it is only legal while the antialias mode is
        // ALIASED -- called otherwise it does not fail at the call, it fails at
        // EndDraw, which throws the *entire frame* away. The window came up as a sheet
        // of bare Mica with nothing on it, and nothing in the code had moved except
        // this line.
        const D2D1_SIZE_F sz = iconBitmap->GetSize();
        const D2D1_RECT_F dst = D2D1::RectF(12, 8, 28, 24);
        const D2D1_RECT_F src = D2D1::RectF(0, 0, sz.width, sz.height);
        // ALIASED for the duration, and it is not optional: FillOpacityMask refuses in
        // per-primitive antialiasing mode and returns D2DERR_WRONG_STATE -- which
        // Direct2D reports at EndDraw, not at the call, so the whole frame vanishes and
        // the window comes up as bare Mica with nothing on it.
        //
        // Nothing is lost by it. The mode governs *geometry* edges, and there is no
        // geometry here: the mask is an A8 bitmap whose own edges are already
        // antialiased, and it is sampled, not rasterised.
        dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
        dc->FillOpacityMask(iconBitmap, p.Brush(fg), &dst, &src);
        dc->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    }

    // Beside the icon when there is one, and at the icon's own inset when there is not.
    p.Text(Title(), { iconBitmap ? 36.0f : 12.0f, 0, w - kCaptionBtnW * 3 - 8, kCaptionH },
           p.font->caption, fg);

    // Minimise, maximise/restore, close. Segoe Fluent Icons' own caption glyphs, which
    // exist at the same code points in Segoe MDL2 Assets -- see theme.h's fallback.
    const bool zoomed = IsZoomed(hwnd) != 0;
    const wchar_t *glyphs[3] = { L"\uE921", zoomed ? L"\uE923" : L"\uE922", L"\uE8BB" };
    for (int i = 0; i < 3; i++) {
        const D2D1_RECT_F r = { w - kCaptionBtnW * (3 - i), 0,
                                w - kCaptionBtnW * (2 - i), kCaptionH };
        D2D1_COLOR_F glyphColour = fg;
        const float t = captionT[i];
        if (t > 0.0f) {
            // Close goes red and its glyph goes white, which is the one caption button
            // Windows colours rather than tints. Everything else takes the subtle fill.
            // Both fade, and the fill carries its own alpha -- so there is nothing to
            // mix against and no need to know what the caption is drawn over.
            if (i == 2) {
                p.Fill(r, Fade(captionDown == i ? Rgb(0xC42B1C, 0.9f) : Rgb(0xC42B1C), t));
                glyphColour = Mix(fg, Rgb(0xFFFFFF), t);
            } else {
                p.Fill(r, Fade(captionDown == i ? c.controlBgPressed : c.subtleHover, t));
            }
        }
        const float gw = p.MeasureWidth(glyphs[i], p.font->iconSmall);
        p.Text(glyphs[i], { (r.left + r.right) / 2 - gw / 2, 0, r.right, kCaptionH },
               p.font->iconSmall, glyphColour);
    }
}

// Which part of the window a point is over, in the codes Windows expects back from
// WM_NCHITTEST.
//
// Two of these answers are load-bearing beyond hit-testing. `HTMAXBUTTON` is what makes
// Windows 11 offer its snap-layout flyout when the pointer rests on the maximise
// button -- a custom caption that returns HTCLIENT there silently loses a feature
// people use. `HTCAPTION` is what makes dragging, double-click-to-maximise and the
// right-click window menu keep working without any of them being reimplemented.
inline LRESULT Window::CaptionHitTest(POINT screen) const {
    RECT wr;
    GetWindowRect(hwnd, &wr);
    const float s = dpi / 96.0f;
    const float x = (screen.x - wr.left) / s;
    const float y = (screen.y - wr.top) / s;
    const float w = (wr.right - wr.left) / s;
    const float h = (wr.bottom - wr.top) / s;

    // The frame is gone from the client area, so the grips are synthesised. Corners
    // first, or a corner would answer as whichever edge was tested first.
    if (resizable && !IsZoomed(hwnd)) {
        const bool l = x < kResizeGrip, r = x >= w - kResizeGrip;
        const bool t = y < kResizeGrip, b = y >= h - kResizeGrip;
        if (t && l) return HTTOPLEFT;
        if (t && r) return HTTOPRIGHT;
        if (b && l) return HTBOTTOMLEFT;
        if (b && r) return HTBOTTOMRIGHT;
        if (t) return HTTOP;
        if (b) return HTBOTTOM;
        if (l) return HTLEFT;
        if (r) return HTRIGHT;
    }
    if (y < kCaptionH) {
        if (x >= w - kCaptionBtnW)     return HTCLOSE;
        if (x >= w - kCaptionBtnW * 2) return HTMAXBUTTON;
        if (x >= w - kCaptionBtnW * 3) return HTMINBUTTON;
        return HTCAPTION;
    }
    return HTCLIENT;
}

inline Widget *Window::HitTest(float x, float y) {
    // A scrolled-away control is not there. Without this the half of a card that has
    // slid under the header still answers the mouse, which is worse than invisible:
    // the click lands on something the person cannot see.
    const D2D1_RECT_F clip = ClipRect();
    const bool clipping = clip.right > clip.left && clip.bottom > clip.top;
    // The same offset the page is drawn with, taken back off. A glide or an arriving
    // page draws the cards a few DIPs from where the layout put them, and the control
    // the pointer is over has to be the one it *looks* like it is over.
    float dy = 0.0f, op = 1.0f;
    ContentTransform(&dy, &op);
    auto reachable = [&](const Widget *w) {
        // The clip is tested against the real pointer, not the shifted one: it is a
        // window onto the page and does not move with the page.
        return w->visible && w->enabled && (!clipping || !w->scrolls || Inside(clip, x, y));
    };
    auto over = [&](const Widget *w) {
        return w->Covers(x, w->scrolls ? y - dy : y);
    };
    // Raised first, then the rest back-to-front: the reverse of the paint order, so
    // whatever is drawn on top is whatever the click reaches.
    for (auto it = widgets.rbegin(); it != widgets.rend(); ++it)
        if (reachable(it->get()) && (*it)->z != 0 && over(it->get()))
            return it->get();
    for (auto it = widgets.rbegin(); it != widgets.rend(); ++it)
        if (reachable(it->get()) && (*it)->z == 0 && over(it->get()))
            return it->get();
    return nullptr;
}

inline void Window::DismissOthers(Widget *except) {
    std::vector<Widget *> shown;
    shown.reserve(widgets.size());
    for (auto &w : widgets) shown.push_back(w.get());
    for (Widget *w : shown)
        if (w != except) w->Dismiss();
}

inline bool Window::RefreshHover() {    POINT pt = {};
    if (!GetCursorPos(&pt)) return false;
    // Whose window the pointer is actually over. A cursor resting on something else
    // must not leave a control lit: this is called from the tick, not from a mouse
    // message, so there is no WM_MOUSELEAVE to lean on.
    const bool mine = WindowFromPoint(pt) == hwnd;
    ScreenToClient(hwnd, &pt);
    const float s = scale();
    Widget *over = capture ? capture : (mine ? HitTest(pt.x / s, pt.y / s) : nullptr);
    bool changed = false;
    for (auto &w : widgets) {
        const bool now = (w.get() == over);
        if (w->hover != now) { w->hover = now; changed = true; }
    }
    return changed;
}

// The gesture that was in progress cannot finish: the capture went to another window,
// the system took it back for a modal state of its own, or this window lost the
// activation. Nothing else here notices. WM_LBUTTONUP is delivered to whoever holds the
// capture, and from that moment on that is no longer this window, so the release is
// synthesised rather than waited for.
//
// Without it a drag has no end at all: a scroll bar with a repeat timer running keeps
// scrolling, a slider keeps its knob grabbed, and a button that was held down stays
// looking held. The capture is dropped before OnRelease runs, because a widget is free
// to lay the page out again there and this must not re-enter on the way.
inline void Window::CancelCapture() {
    Widget *w = capture;
    if (!w) return;
    capture = nullptr;
    w->pressed = false;
    if (w->enabled) w->OnRelease();
    Invalidate();
}

inline D2D1_POINT_2F Widget::Cursor() const {
    POINT pt = {};
    GetCursorPos(&pt);
    if (!owner) return D2D1::Point2F((float)pt.x, (float)pt.y);
    ScreenToClient(owner->hwnd, &pt);
    float dy = 0.0f, op = 1.0f;
    if (scrolls) owner->ContentTransform(&dy, &op);
    const float s = owner->scale();
    return D2D1::Point2F(pt.x / s, pt.y / s - dy);
}

inline D2D1_RECT_F Widget::VisibleArea() const {
    if (!owner) return D2D1_RECT_F{ 0, 0, 0, 0 };
    const D2D1_RECT_F clip = owner->ClipRect();
    if (clip.right <= clip.left || clip.bottom <= clip.top) return clip;
    // The page is drawn `dy` from where it was laid out, so the window's strip becomes a
    // strip of the page by shifting it the other way -- the same offset the pointer is
    // shifted by, in the other direction.
    float dy = 0.0f, op = 1.0f;
    if (scrolls) owner->ContentTransform(&dy, &op);
    return D2D1_RECT_F{ clip.left, clip.top - dy, clip.right, clip.bottom - dy };
}

inline void Window::SetFocusTo(Widget *w) {
    if (focused == w) return;
    if (focused) { focused->focus = false; focused->OnBlur(); }
    focused = w;
    if (focused) focused->focus = true;
    caretOn = true;
    Invalidate();
}

inline void Window::MoveFocus(int delta) {
    std::vector<Widget *> tab;
    for (auto &w : widgets)
        if (w->visible && w->enabled && w->Focusable()) tab.push_back(w.get());
    if (tab.empty()) return;
    int at = -1;
    for (size_t i = 0; i < tab.size(); i++) if (tab[i] == focused) at = (int)i;
    at = (at < 0) ? (delta > 0 ? 0 : (int)tab.size() - 1)
                  : (int)(((size_t)at + tab.size() + delta) % tab.size());
    showFocusRing = true;
    SetFocusTo(tab[at]);
}

// Put the IME's candidate and composition windows at the caret rather than at the
// top-left corner of the window, which is where they land by default. On a machine
// with a Chinese IME that default is the difference between a usable field and one
// that types into a box floating over the title bar.
inline void Window::PlaceImeAtCaret() {
    D2D1_POINT_2F pt;
    if (!focused || !focused->CaretPoint(&pt)) return;
    HIMC imc = ImmGetContext(hwnd);
    if (!imc) return;
    COMPOSITIONFORM cf = {};
    cf.dwStyle = CFS_POINT;
    cf.ptCurrentPos.x = (LONG)(pt.x * scale());
    cf.ptCurrentPos.y = (LONG)(pt.y * scale());
    ImmSetCompositionWindow(imc, &cf);
    CANDIDATEFORM caf = {};
    caf.dwStyle = CFS_CANDIDATEPOS;
    caf.ptCurrentPos = cf.ptCurrentPos;
    ImmSetCandidateWindow(imc, &caf);
    ImmReleaseContext(hwnd, imc);
}

inline bool Window::Create(int dipW, int dipH, bool canResize, HICON icon) {
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown **>(&dw))))
        return false;
    if (!fonts.Create(dw)) return false;
    pal = MakePalette(SystemUsesDarkTheme());

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc   = Proc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = ClassName();
    wc.hCursor       = LoadCursorW(nullptr, kCursorArrow);
    wc.hIcon         = icon;
    wc.hIconSm       = icon;
    // No background brush. Every pixel comes from the composition surface, and letting
    // USER32 erase first is a flash of grey on every resize.
    wc.hbrBackground = nullptr;
    RegisterClassExW(&wc);

    // The window is created at the DPI of the monitor it lands on, which is not known
    // until it exists. So: create it small, ask what DPI it got, then size it.
    // Creating it at 96-DPI pixels and letting WM_DPICHANGED fix it up produces a
    // visible resize on every high-DPI machine, which is most of them.
    // The style bits stay exactly as they were, caption included. They are what DWM
    // reads to decide that this is an ordinary top-level window -- shadow, rounded
    // corners, snap, Alt+Tab. What changes is only where the client area starts, and
    // that is WM_NCCALCSIZE's business, not the style's.
    resizable = canResize;
    appIcon = icon;
    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    if (resizable) style |= WS_THICKFRAME | WS_MAXIMIZEBOX;
    hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP, ClassName(), Title(), style,
                           CW_USEDEFAULT, CW_USEDEFAULT, 100, 100,
                           nullptr, nullptr, wc.hInstance, this);
    if (!hwnd) return false;

    // dipW x dipH is the client area. The frame around it is not the one
    // AdjustWindowRectEx computes for this style -- WM_NCCALCSIZE hands the caption and
    // the top border to the client -- so it is measured from the window instead: size it
    // once, read how much of it is not client, and size it again.
    dpi = dpiapi::ForWindow(hwnd);
    const int cw = MulDiv(dipW, dpi, 96), ch = MulDiv(dipH, dpi, 96);
    SetWindowPos(hwnd, nullptr, 0, 0, cw, ch, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    MeasureFrame();
    const int w = cw + frameExtra.cx, h = ch + frameExtra.cy;
    // Centred on the monitor the window landed on, not on the primary. On a laptop
    // docked to a second screen those are different, and a window that opens on the
    // screen the pointer is not on is a small daily annoyance this can avoid.
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi);
    const int x = mi.rcWork.left + (mi.rcWork.right - mi.rcWork.left - w) / 2;
    const int y = mi.rcWork.top + (mi.rcWork.bottom - mi.rcWork.top - h) / 2;
    SetWindowPos(hwnd, nullptr, x, y, w, h, SWP_NOZORDER);

    ApplyThemeToFrame();
    Layout();
    if (showCommand != SW_HIDE) {
        ShowWindow(hwnd, showCommand);
        UpdateWindow(hwnd);
    }
    return true;
}

// Two loops in one, and which of them is running is the whole of this window's
// relationship with the clock.
//
// **Idle**: block in GetMessage, exactly as before. A settings window with nothing
// moving costs nothing, which is the state it is in for all but a few seconds of its
// life.
//
// **Animating**: drain the queue, draw one frame, and wait for the compositor's own
// clock. `DCompositionWaitForCompositorClock` returns when DWM is about to compose the
// next frame -- it is the same clock the window's visual is being composed against, so
// frames land one refresh apart by construction.
//
// What this replaces is a 16 ms WM_TIMER, and the replacement is not a preference.
// WM_TIMER is generated only when the queue is empty, is quantised to the system tick,
// and competes with the WM_PAINT it posts; measured over a page change and a scroll it
// produced gaps of 2 to 35 ms with a median of 24, which reads as stutter. Present()
// does not pace anything here either -- it was measured at 0.1 ms, because a flip-model
// composition swap chain with two buffers queues the frame and returns.
//
// The timeout is a backstop, not a cadence: if DWM ever stops ticking (it does not on
// Windows 11, but a remote session or a display going away can stall it), the loop
// keeps turning slowly rather than hanging with an animation half-finished.
//
// **Windows 10 has no compositor clock.** The function is build 22000 and later, and a
// static call binds it by ordinal (#1104) through the SDK's dcomp.lib; Windows 10's
// dcomp.dll has nothing in that slot, so the loader stops the process with
// STATUS_ORDINAL_NOT_FOUND (0xC0000138) before wWinMain -- no window, no message, not
// even the program's own "this needs Windows 11" if it has one. So it is resolved at
// run time. Without it the loop waits out the rest of one display refresh on a
// high-resolution waitable timer. Not Sleep(1): Present() returns in 0.1 ms here, so a
// 1 ms yield paces nothing and the window draws as fast as the GPU will queue frames.
namespace frameclock {
using Fn = DWORD(WINAPI *)(UINT, const HANDLE *, DWORD);
inline Fn Resolve() {
    static const Fn fn = [] {
        HMODULE m = GetModuleHandleW(L"dcomp.dll");
        if (!m) m = LoadLibraryW(L"dcomp.dll");
        return m ? (Fn)GetProcAddress(m, "DCompositionWaitForCompositorClock") : nullptr;
    }();
    return fn;
}
// One refresh of the monitor the window is on, in 100 ns; 60 Hz when the display
// reports 0 or 1, which both mean "the hardware default".
inline LONGLONG RefreshPeriod(HWND hwnd) {
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    DEVMODEW dm = {};
    dm.dmSize = sizeof(dm);
    if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi) &&
        EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) &&
        dm.dmDisplayFrequency > 1)
        return 10000000LL / dm.dmDisplayFrequency;
    return 10000000LL / 60;
}
}  // namespace frameclock

// Seconds since the process started, monotonic, off the same performance counter the frame
// loop times its frames with. QPC's frequency is fixed for the life of the process, so it
// is read once.
//
// For a control whose animation is periodic and holds nothing else. Asking the clock where
// in its cycle *now* is gives the same answer to a control that has just been built as to
// the one it replaced, and a control is rebuilt for all sorts of reasons -- a resize, a save
// that changes the shape of the page, a page that lays itself out in response to a scroll.
// A phase counted per frame starts over from the beginning every time. See ProgressBar.
inline double MonotonicSeconds() {
    static const LARGE_INTEGER freq = [] {
        LARGE_INTEGER f = {};
        QueryPerformanceFrequency(&f);
        return f;
    }();
    LARGE_INTEGER now = {};
    QueryPerformanceCounter(&now);
    return freq.QuadPart ? (double)now.QuadPart / (double)freq.QuadPart : 0.0;
}

inline int Window::Run() {
    // GetCaretBlinkTime's own default period. The window owns this timer the way a control owns
    // its own; see Timer.
    caretTimer.Start(this, 530, [this] {
        // Only repaint when there is a caret to blink. A window that invalidates twice a second
        // forever is a window that keeps a laptop's GPU awake.
        if (focused && focused->CaretPoint(nullptr)) {
            caretOn = !caretOn;
            Invalidate();
        }
    });
    QueryPerformanceFrequency(&qpcFreq);
    QueryPerformanceCounter(&qpcLast);
    const frameclock::Fn clock = frameclock::Resolve();
    // Only created where it is needed. Null on a pre-1803 build too, where the wait
    // falls back to Sleep for the same remainder.
    HANDLE pace = clock ? nullptr
                        : CreateWaitableTimerExW(nullptr, nullptr,
                                                 CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                                 TIMER_ALL_ACCESS);
    LARGE_INTEGER paceMark = {};
    LONGLONG period = 0;
    MSG msg = {};
    int exitCode = 0;
    while (alive) {
        const bool moving = Animating() || AnimationWanted();
        if (!moving) {
            animOn = false;
            if (GetMessageW(&msg, nullptr, 0, 0) <= 0) { exitCode = (int)msg.wParam; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            // The clock is only picked up again here: a window that sat idle for a
            // minute must not hand the first frame a minute's worth of dt.
            QueryPerformanceCounter(&qpcLast);
            continue;
        }
        if (!animOn) {
            animOn = true;
            QueryPerformanceCounter(&qpcLast);
            paceMark = qpcLast;
            // Asked once per stretch of animation rather than per frame: a window
            // dragged to a monitor with a different rate mid-animation is paced at the
            // old rate for the rest of a transition that lasts a fraction of a second.
            if (!clock) period = frameclock::RefreshPeriod(hwnd);
        }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { exitCode = (int)msg.wParam; alive = false; break; }
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!alive) break;
        // Inside a Dispatch, because a page's tick is allowed to lay itself out again --
        // a card that grows a frame at a time does -- and that frees the widgets a
        // running callback may be standing on. See Window::retired.
        { Dispatch frame(this); Frame(); }
        Paint();
        // Painted outside WM_PAINT, so the update region has to be cleared by hand or
        // the next PeekMessage hands back a WM_PAINT for a window that was just drawn.
        ValidateRect(hwnd, nullptr);
        if (clock) {
            clock(0, nullptr, 32);
        } else {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            const LONGLONG spent = (now.QuadPart - paceMark.QuadPart) * 10000000LL / qpcFreq.QuadPart;
            const LONGLONG left = period - spent;
            if (left > 0) {
                LARGE_INTEGER due;
                due.QuadPart = -left;                 // relative, 100 ns
                if (pace && SetWaitableTimer(pace, &due, 0, nullptr, nullptr, FALSE))
                    WaitForSingleObject(pace, 32);    // the same backstop as the clock's
                else
                    Sleep((DWORD)((left + 9999) / 10000));
            }
            QueryPerformanceCounter(&paceMark);
        }
    }
    // Usually the loop ends because WM_DESTROY cleared `alive`, before the WM_QUIT it
    // posted has been read -- and then `msg` is whatever was being dispatched, an Alt+F4
    // keystroke for one. The exit code is the quit message's, so take it from the queue.
    MSG quit;
    if (PeekMessageW(&quit, nullptr, WM_QUIT, WM_QUIT, PM_REMOVE)) exitCode = (int)quit.wParam;
    // Nothing is left to fire at, and the window is about to go: the caret's and the frame loop's
    // timers are stopped here rather than in their destructors, which run with no hwnd left.
    caretTimer.Stop();
    frameTimer.Stop();
    if (pace) CloseHandle(pace);
    fonts.Release();
    ReleaseDevice();
    if (dw) { dw->Release(); dw = nullptr; }
    return exitCode;
}

inline LRESULT CALLBACK Window::Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    Window *self = reinterpret_cast<Window *>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (m == WM_NCCREATE) {
        self = reinterpret_cast<Window *>(reinterpret_cast<CREATESTRUCTW *>(lp)->lpCreateParams);
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        self->hwnd = h;
    }
    if (!self) return DefWindowProcW(h, m, wp, lp);

    // Nothing a widget callback replaces is really freed until this message returns.
    // See `Window::retired`.
    Window::Dispatch frame(self);

    const float s = self->scale();
    const float mx = (float)GET_X_LPARAM(lp) / s;
    const float my = (float)GET_Y_LPARAM(lp) / s;

    switch (m) {
    case WM_NCCALCSIZE: {
        // The whole window becomes client area, which is what lets the backdrop reach
        // the top of it. DefWindowProc is still asked first: it computes the left,
        // right and bottom insets, which have to stay or a maximised window hangs off
        // the screen and the resize borders stop working. Only the top is put back.
        if (!wp) break;
        NCCALCSIZE_PARAMS *ncc = reinterpret_cast<NCCALCSIZE_PARAMS *>(lp);
        const LONG top = ncc->rgrc[0].top;
        const LRESULT r = DefWindowProcW(h, m, wp, lp);
        if (r != 0) return r;
        ncc->rgrc[0].top = top;
        // Except when maximised: a maximised window's frame really does extend past
        // the work area by the border width, and keeping the top would put the caption
        // buttons under the edge of the screen.
        if (IsZoomed(h)) {
            const UINT d = dpiapi::ForWindow(h);
            ncc->rgrc[0].top += dpiapi::SystemMetric(SM_CYSIZEFRAME, d) +
                                dpiapi::SystemMetric(SM_CXPADDEDBORDER, d);
        }
        return 0;
    }
    case WM_NCHITTEST: {
        const POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        return self->CaptionHitTest(pt);
    }
    case WM_NCMOUSEMOVE: {
        const int was = self->captionHot;
        self->captionHot = wp == HTMINBUTTON ? 0 : wp == HTMAXBUTTON ? 1
                         : wp == HTCLOSE     ? 2 : -1;
        if (self->captionHot >= 0) {
            TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE | TME_NONCLIENT, h, 0 };
            TrackMouseEvent(&tme);
        }
        if (was != self->captionHot) self->Invalidate();
        break;   // and on to DefWindowProc, which owns the caption's own behaviour
    }
    case WM_NCMOUSELEAVE:
        if (self->captionHot != -1) { self->captionHot = -1; self->Invalidate(); }
        break;
    case WM_NCLBUTTONDOWN:
        // Handled here rather than passed on, for the three buttons only. Left to
        // DefWindowProc they enter its own modal loop, which paints the pre-Windows-10
        // caption buttons into a frame that no longer exists.
        if (wp == HTMINBUTTON || wp == HTMAXBUTTON || wp == HTCLOSE) {
            self->captionDown = wp == HTMINBUTTON ? 0 : wp == HTMAXBUTTON ? 1 : 2;
            self->Invalidate();
            return 0;
        }
        break;
    case WM_NCLBUTTONUP:
        if (self->captionDown >= 0) {
            const int which = self->captionDown;
            self->captionDown = -1;
            self->Invalidate();
            const bool same = (wp == HTMINBUTTON && which == 0) ||
                              (wp == HTMAXBUTTON && which == 1) ||
                              (wp == HTCLOSE && which == 2);
            if (same) {
                if (which == 0) ShowWindow(h, SW_MINIMIZE);
                else if (which == 1) ShowWindow(h, IsZoomed(h) ? SW_RESTORE : SW_MAXIMIZE);
                else PostMessageW(h, WM_CLOSE, 0, 0);
            }
            return 0;
        }
        break;
    case WM_ACTIVATE:
        self->active = LOWORD(wp) != WA_INACTIVE;
        // A window that has just been alt-tabbed away from must not leave a flyout
        // hanging open over its own page, waiting for a click it will never get -- nor a
        // control lit under the pointer it no longer has. Alt-tab moves no mouse, so
        // there is no WM_MOUSELEAVE to do the second; the hover would sit there until the
        // pointer happened to move over this window again.
        //
        // The gesture in progress ends here too. Losing the capture has its own message
        // and usually arrives first, which is why CancelCapture is written to be harmless
        // a second time -- but a window can be deactivated while it still holds the
        // capture, and the button that was down then comes up over whatever took the
        // activation. See WM_CAPTURECHANGED.
        if (!self->active) {
            self->CancelCapture();
            for (auto &w : self->widgets) { w->Dismiss(); w->hover = false; }
        }
        self->Invalidate();
        break;
    case WM_PAINT: {
        // The frame loop paints outside WM_PAINT while it is animating (see Window::Run),
        // directly after the Tick that moved everything. A paint here would be the second
        // one in the same frame, drawing the state from before that Tick -- and every mouse
        // message this window handles invalidates, so during a drag it is every frame, at
        // twice the drawing cost and half the frame rate.
        //
        // Unless Windows has the loop's turn in its own hands -- a drag of the border or the
        // caption -- in which case this is the only paint there is going to be, and it has
        // to happen or the window shows the size it had when the drag started.
        PAINTSTRUCT ps;
        BeginPaint(h, &ps);
        if (!self->animOn || self->inSizeMove) self->Paint();
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ENTERSIZEMOVE:
        // Windows is about to run a modal loop of its own for a drag of the border or of the
        // caption. This window's frame loop -- and the compositor clock it paces itself
        // with -- gets no turn again until that loop ends, so everything the loop does has
        // to happen from a message instead, and the only message that keeps arriving is a
        // timer. Without one the window repainted only when the mouse happened to move, the
        // controls caught up with the new size only when the drag was let go, and whatever
        // was animating -- an indeterminate bar, a page's glide -- stood still until then.
        self->inSizeMove = true;
        // A frame of the loop that cannot run, in the loop's own order: tick, then paint. No
        // Dispatch of its own -- the one at the top of Proc covers the whole message, which is
        // what a tick needs to be able to lay the page out.
        self->frameTimer.Start(self, 16, [self] {
            if (!self->inSizeMove) { self->frameTimer.Stop(); return; }
            self->Frame();
            self->Paint();
            ValidateRect(self->hwnd, nullptr);
        });
        return 0;
    case WM_EXITSIZEMOVE:
        self->inSizeMove = false;
        self->frameTimer.Stop();
        // And the clock is picked up again here, or the frame loop's first frame after the
        // drag carries the whole drag's worth of `dt` -- which the loop clamps to a tenth of
        // a second, but a tenth of a second of an animation in one step is a jump.
        QueryPerformanceCounter(&self->qpcLast);
        return 0;
    case WM_SIZE:
        if (wp == SIZE_RESTORED) self->MeasureFrame();
        self->Resize();
        self->Layout();
        self->Invalidate();
        return 0;
    case WM_DPICHANGED: {
        self->dpi = HIWORD(wp);
        const RECT *r = reinterpret_cast<const RECT *>(lp);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        // After the move, because Resize reads the new client size and the bitmap
        // carries the DPI.
        self->Resize();
        self->Layout();
        self->Invalidate();
        return 0;
    }
    case WM_SETTINGCHANGE:
        // The machine switched between light and dark while the window was open.
        // Windows announces it as a generic settings change with this string, which is
        // the only signal there is short of WinRT.
        if (lp && _wcsicmp(reinterpret_cast<const wchar_t *>(lp), L"ImmersiveColorSet") == 0)
            self->ReloadTheme();
        return 0;
    case WM_MOUSEMOVE: {
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, h, 0 };
        TrackMouseEvent(&tme);
        Widget *over = self->capture ? self->capture : self->HitTest(mx, my);
        bool changed = false;
        for (auto &w : self->widgets) {
            const bool now = (w.get() == over);
            if (w->hover != now) { w->hover = now; changed = true; }
        }
        // The page's offset, read once per move rather than once per control: every
        // control that hears about a pointer below hears about it in the space its own
        // rect is in.
        float pdy = 0.0f, popacity = 1.0f;
        self->ContentTransform(&pdy, &popacity);

        if (self->capture) {
            // The page's paint offset taken back off, so a control dragged while the page
            // is still gliding does not un-press itself.
            const float dragY = self->capture->scrolls ? my - pdy : my;
            // From the message, and the same point OnDrag gets. This used to read
            // GetCursorPos: two coordinates for one event, and the one that decided
            // whether the control was still pressed was not the one it was being dragged
            // with. It also put every drag out of reach of a harness that posts messages,
            // since the widget answered the physical pointer instead of the message.
            //
            // `pressed` follows the rectangle honestly and nothing here overrides it: a
            // control whose gesture outlives its own rectangle says so in PressedVisual.
            const bool down = Inside(self->capture->rect, mx, dragY);
            if (self->capture->pressed != down) { self->capture->pressed = down; changed = true; }
            self->capture->OnDrag(mx, dragY);
        }

        // The control under the pointer, and any control that watches a region outside
        // its own rectangle. See Widget::ExternalRegion. By index, because a drag above
        // may have laid the page out and replaced the list.
        bool tracks = false;
        for (size_t i = 0; i < self->widgets.size(); i++) {
            Widget *wd = self->widgets[i].get();
            const float wy = wd->scrolls ? my - pdy : my;
            if (wd != over &&
                !(wd->visible && wd->enabled && Inside(wd->ExternalRegion(), mx, wy)))
                continue;
            wd->OnPointerMove(mx, wy);
            if (wd->TracksPointer()) tracks = true;
        }
        SetCursor(LoadCursorW(nullptr, !over            ? kCursorArrow
                                     : over->TextCursor() ? kCursorIBeam
                                     : over->HandCursor() ? kCursorHand
                                                          : kCursorArrow));
        if (changed || tracks) self->Invalidate();
        return 0;
    }
    case WM_MOUSELEAVE:
        for (auto &w : self->widgets) w->hover = false;
        self->Invalidate();
        return 0;
    case WM_LBUTTONDOWN: {
        Widget *w = self->HitTest(mx, my);
        // Everything else puts away whatever it was showing. This is what closes an open
        // drop-down when the click lands somewhere else -- including on nothing, which is
        // the case the control itself can never see.
        self->DismissOthers(w);
        // Those dismissals can lay the page out again -- a pane that closes tells the page, and
        // a page that lays itself out is a different list of widgets. What the pointer was over
        // is then a control that has been retired, freed when this message returns, and a press
        // taken on it would leave the capture pointing at memory that is going: the mouse-up
        // after it is the crash. So the hit test is made again, on the page that is there now.
        w = self->HitTest(mx, my);
        // Clicking anywhere takes the focus ring away again: it is a keyboard
        // affordance, and a mouse user who has just clicked a button does not want the
        // rectangle left behind on it.
        self->showFocusRing = false;
        if (w) {
            SetCapture(h);
            self->capture = w;
            w->pressed = true;
            if (w->Focusable()) self->SetFocusTo(w);
            // From the message rather than from GetCursorPos, and the difference is not
            // theoretical: the pointer can have moved between the click being queued and
            // this running, and a press position that disagrees with the hit test by a
            // pixel is a gesture that grabbed the wrong sub-region of its own control.
            float pdy = 0.0f, popacity = 1.0f;
            self->ContentTransform(&pdy, &popacity);
            w->OnPress(mx, w->scrolls ? my - pdy : my);
        } else {
            self->SetFocusTo(nullptr);
        }
        self->Invalidate();
        return 0;
    }
    case WM_LBUTTONUP: {
        Widget *w = self->capture;
        self->capture = nullptr;
        ReleaseCapture();
        if (w) {
            // A release that is a click: the pointer is still on the control, and the
            // control was not being dragged. `pressed` alone used to say both, and stops
            // saying the second the moment a widget keeps it through a drag that has left
            // its rectangle -- a slider let go three rows away is not a click on whatever
            // it was let go over.
            const bool click = w->pressed;
            w->pressed = false;
            self->Invalidate();
            if (w->enabled) w->OnRelease();
            // OnClick last, and after the state is already tidy: a click can replace
            // the entire widget list (that is what "next page" is), and touching `w`
            // after that is a use-after-free. OnRelease goes before it for the same
            // reason -- by the time OnClick has returned, `w` may not exist.
            if (click && w->enabled) w->OnClick();
        }
        return 0;
    }
    // The capture went away without a button-up: alt-tab, a system modal, another
    // application taking the mouse. Windows revokes it and no WM_LBUTTONUP is ever
    // coming, so a gesture left running here is one that never ends. The button stays
    // dark under a window that is not even active any more -- and worse, `capture` still
    // points at the control, so the next time the pointer crosses this window the move
    // goes straight to OnDrag and the slider follows it with no button held.
    //
    // Ended as a release that is not a click. A drag has already put its value on screen
    // and in memory, so OnRelease is what stops a settings file from disagreeing with
    // both; OnClick is the half that must not happen, because the gesture was abandoned
    // rather than finished.
    //
    // No ReleaseCapture here: it is already gone, and calling it inside this message is
    // what the documentation warns against. Nor does this double up with the case above
    // -- that clears `capture` before it releases, so the WM_CAPTURECHANGED it causes
    // arrives to find nothing left to end.
    case WM_CAPTURECHANGED:
        self->CancelCapture();
        return 0;
    case WM_CANCELMODE:
        // The same thing, announced before the capture is taken rather than after.
        // Released here so that the two handlers cannot disagree about who holds it.
        if (self->capture) ReleaseCapture();
        self->CancelCapture();
        return 0;
    case WM_GETMINMAXINFO: {
        int mw = 0, mh = 0;
        self->MinSize(&mw, &mh);
        if (mw <= 0 && mh <= 0) break;
        // The minimum is a *window* size and MinSize speaks in client DIPs. The
        // difference is the frame this window keeps, as measured -- see MeasureFrame.
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        if (mw > 0) mmi->ptMinTrackSize.x = (LONG)(mw * self->scale()) + self->frameExtra.cx;
        if (mh > 0) mmi->ptMinTrackSize.y = (LONG)(mh * self->scale()) + self->frameExtra.cy;
        return 0;
    }
    case WM_MOUSEWHEEL: {
        // Delivered in screen coordinates, unlike every other mouse message.
        POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(h, &pt);
        // The control under the pointer first, in its own space -- see Widget::OnWheel.
        {
            const float x = pt.x / s, y = pt.y / s;
            if (Widget *w = self->HitTest(x, y)) {
                float pdy = 0.0f, popacity = 1.0f;
                if (w->scrolls) self->ContentTransform(&pdy, &popacity);
                if (w->OnWheel(x, y - pdy,
                               (float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA)) {
                    self->Invalidate();
                    return 0;
                }
            }
        }
        // Then anything floating over the page. A popup that is up is what a wheel over it is for,
        // and the page under it is not the thing being turned: a page that scrolled out from under
        // an open list would take the list's own place in it with it. Tried after the control the
        // pointer is actually over, so that an ordinary control's wheel still wins where the two
        // overlap, and only where a floating control says it wants the turn at all.
        {
            const float x = pt.x / s, y = pt.y / s;
            for (auto &w : self->widgets) {
                if (!w->visible || w->z <= 0) continue;
                float pdy = 0.0f, popacity = 1.0f;
                if (w->scrolls) self->ContentTransform(&pdy, &popacity);
                if (w->OnWheel(x, y - pdy,
                               (float)GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA)) {
                    self->Invalidate();
                    return 0;
                }
            }
        }
        // The page scrolls only when the wheel is over the page. `ClipRect` is where the page
        // draws its scrolling content; its *box* is that strip widened up to the top of the page's
        // own furniture, because the page's title and its caption are the page's -- a wheel over
        // them is a wheel over the page. What is left out is everything that belongs to the window
        // rather than to the page: the caption bar and its buttons above, and the pane's rail to
        // the left. A wheel over one of those used to scroll the page underneath it.
        //
        // Where the page names no strip at all -- nothing to scroll -- the box is what is left of
        // the client under the caption bar, which is the same rule with the page's own answer
        // missing.
        const float wx = pt.x / s, wy = pt.y / s;
        D2D1_RECT_F page = self->ClipRect();
        if (page.right <= page.left || page.bottom <= page.top)
            page = D2D1_RECT_F{ 0, 0, self->ClientW(), self->ClientH() };
        const D2D1_RECT_F box = { page.left, kCaptionH, page.right, self->ClientH() };
        if (wx < box.left || wx >= box.right || wy < box.top || wy >= box.bottom) return 0;
        return self->OnAppMessage(m, wp, MAKELPARAM(pt.x, pt.y)) ? 0
                                                                 : DefWindowProcW(h, m, wp, lp);
    }
    case WM_GETDLGCODE:
        return DLGC_WANTALLKEYS | DLGC_WANTCHARS;
    case WM_CHAR:
    case WM_IME_CHAR: {
        // Both, because they arrive by different routes and a field that handles only
        // one is a field that works in English and not in Chinese. WM_CHAR carries
        // keyboard input and, on most IMEs, the committed result as well; WM_IME_CHAR
        // is what some of them send instead.
        const wchar_t ch = (wchar_t)wp;
        if (self->focused && ch >= 0x20 && ch != 0x7F && self->focused->OnChar(ch)) {
            self->caretOn = true;
            self->Invalidate();
            return 0;
        }
        return 0;
    }
    case WM_IME_STARTCOMPOSITION:
        self->PlaceImeAtCaret();
        return DefWindowProcW(h, m, wp, lp);
    case WM_KEYDOWN:
        if (self->focused && self->focused->OnKey(wp)) {
            self->caretOn = true;
            self->Invalidate();
            return 0;
        }
        switch (wp) {
        case VK_TAB:
            self->MoveFocus((GetKeyState(VK_SHIFT) & 0x8000) ? -1 : 1);
            return 0;
        case VK_SPACE:
            if (self->focused) { self->focused->OnActivate(); self->Invalidate(); }
            return 0;
        case VK_RETURN:
            // Enter operates the focused control if it is one that can be operated,
            // and otherwise the page's default action. Without the first half, tabbing
            // to "Browse" and pressing Enter would press the page's default button.
            if (self->focused) self->focused->OnActivate();
            else               self->OnDefaultAction();
            self->Invalidate();
            return 0;
        case VK_ESCAPE:
            self->OnCancel();
            return 0;
        }
        return 0;
    case WM_TIMER:
        // A timer this window is running: see Timer, where the window's own and every control's
        // come from, and which knows whose id this is. By index, with the size asked again each
        // turn, because a callback can stop the timer it is running on.
        for (size_t i = 0; i < self->timers.size(); i++)
            if (self->timers[i]->Handle(wp)) return 0;
        // Anything else is a timer a page set for itself, which falls through to OnAppMessage,
        // which is where a page's messages are answered.
        break;
    case WM_ERASEBKGND:
        return 1;   // every pixel comes from the composition surface
    case WM_DESTROY:
        self->alive = false;
        PostQuitMessage(0);
        return 0;
    }
    if (self->OnAppMessage(m, wp, lp)) return 0;
    return DefWindowProcW(h, m, wp, lp);
}

// Kept for its callers, and now it only has to wake the loop.
//
// There is nothing left to start: Run() asks `Animating() || AnimationWanted()` at the
// top of every turn, so a state change made anywhere is picked up by the next one. What
// it does still have to do is get the loop *to* its next turn -- if the window is
// blocked in GetMessage, invalidating is what returns from it.
inline void StartAnimation(Window *w) {
    if (w) w->Invalidate();
}

// The end of one window message. Nothing a widget callback was standing on is really
// freed until here; see `Window::retired`.
inline Window::Dispatch::~Dispatch() {
    if (--w->dispatchDepth == 0) w->retired.clear();
}

// Per-monitor v2, set from code as well as from the manifest.
//
// The manifest is what actually takes effect for a normally-launched program, and it
// is where this belongs. This call is the belt to those braces: it is what makes the
// window behave when the exe is started by something that supplies its own activation
// context -- an elevated launch through a shell extension, for one.
inline void EnablePerMonitorDpi() {
    dpiapi::EnablePerMonitorV2();
}

// ---------------------------------------------------------------- clipboard

// Both directions, for the one field that takes text. Small enough to live here and
// not worth a file: the alternative is every caller opening the clipboard by hand and
// one of them forgetting to close it, which locks it for every other program on the
// machine.
inline std::wstring ClipboardText(HWND owner) {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(owner)) return L"";
    std::wstring out;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t *p = (const wchar_t *)GlobalLock(h)) {
            out = p;
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    // Exactly what is there. What a *field* should keep of it is the field's decision --
    // see TextBox::Pasted.
    return out;
}

inline void SetClipboardText(HWND owner, const std::wstring &s) {
    if (!OpenClipboard(owner)) return;
    EmptyClipboard();
    const size_t bytes = (s.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void *p = GlobalLock(h)) {
            memcpy(p, s.c_str(), bytes);
            GlobalUnlock(h);
            SetClipboardData(CF_UNICODETEXT, h);
        }
    }
    CloseClipboard();
}

}  // namespace micula
