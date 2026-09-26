// Micula / theme.h
//
// The colours, the metrics and the font stack, resolved from the machine rather
// than chosen here.
//
// Micula draws its own controls -- see window.h for why -- and the moment you
// do that you own every decision Windows used to make for you. Getting one of them
// wrong is not a cosmetic problem: a hardcoded white background on a machine set to
// dark mode is the difference between "this looks like part of Windows" and "this
// looks like something that got in". So every value below is either read from the
// system or taken from Microsoft's own published Fluent tokens, and the ones that
// are guessed say so.
//
// Three things come from the machine:
//
//   the theme     HKCU ... \Themes\Personalize\AppsUseLightTheme. Not the *system*
//                 theme (SystemUsesLightTheme) -- that one is the taskbar and the
//                 Start menu; apps follow AppsUseLightTheme, and a machine with a
//                 light taskbar and dark apps is a combination people actually run.
//   the accent    HKCU ... \Explorer\Accent\AccentPalette, which is eight RGBA
//                 entries from light to dark. Windows does not use the same one in
//                 both themes: a light window uses a darker shade so text on it
//                 stays legible, a dark window a lighter one. Using the raw accent
//                 in both is the most common way a third-party window looks slightly
//                 wrong without anybody being able to say why.
//   the DPI       per-monitor, per-window, and it changes while the window is open.
//                 Everything here is in DIPs; window.h sets the render target's DPI
//                 so Direct2D does the scaling, which is the only way to keep layout
//                 code free of a scale factor multiplied in by hand in forty places.
//
// No dependency on uxtheme, none on WinRT, nothing outside the Windows SDK. A program
// built with /MT runs on a machine that has installed nothing yet.

#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

// Before <d2d1.h>, and that ordering is the whole of it.
//
// windows.h defines `DrawText` as a macro rewriting it to DrawTextA or DrawTextW, and
// d2d1.h declares `ID2D1RenderTarget::DrawText` with no guard against that. So the
// method's real name depends on whether UNICODE happened to be defined when d2d1.h
// was parsed -- an ANSI build gets `DrawTextA`, a Unicode one `DrawTextW`, and either
// way the error at the call site names a method that "is not a member", pointing at
// the wrong thing entirely.
//
// Undoing the macro here makes the method called DrawText in every configuration, so
// the call sites in window.h are correct rather than correct-by-coincidence. Nothing
// in Micula draws text with GDI; the Win32 function is still reachable by its real
// name, DrawTextW, which is what the macro existed to hide.
#undef DrawText

#include <d2d1.h>
#include <dwrite.h>

#include <cmath>

namespace micula {

// ---------------------------------------------------------------- system state

// AppsUseLightTheme, defaulting to light.
//
// Absent means light: the value is written when somebody changes the setting, and a
// machine nobody has touched is light. Defaulting to dark on a missing value would
// give a fresh install of Windows a dark window, which is the wrong way round.
inline bool SystemUsesDarkTheme() {
    DWORD light = 1, size = sizeof(light);
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size) != ERROR_SUCCESS)
        return false;
    return light == 0;
}

// The accent shade Windows itself would use on a surface of this theme.
//
// AccentPalette is 32 bytes: eight **RGBA** entries, [0] lightest through [7] darkest.
//
// RGBA is worth stating because the obvious guess is wrong. Everything else Windows
// stores a colour in around here is BGR -- COLORREF is, DWM\ColorizationColor is -- so
// this was read as BGRA, and the result was not a crash or a warning but a window whose
// every accented surface was the *red-blue mirror* of the machine's real accent. It
// looked deliberate. It was caught by sampling a screenshot: the title bar came out
// #9B4432 and the checkbox beside it #374699, which are the same three bytes in
// opposite orders. Cross-checked against the two values Windows keeps separately for
// the same colour -- Explorer\Accent\StartColorMenu and AccentColorMenu -- both of
// which decode to the palette entry when its bytes are read R, G, B.
// The system's own naming for them is AccentLight3, Light2, Light1, Accent, Dark1,
// Dark2, Dark3 and then a near-black. Windows 11's own controls use Dark1 (index 4)
// on a light surface and Light2 (index 1) on a dark one; taking index 3 in both --
// the "real" accent -- is what makes a hand-rolled window's buttons look one notch
// too saturated in dark mode and slightly too pale in light.
//
// Falls back to #0078D4, the Windows default accent, when the value is missing or
// the wrong size. Deliberately *not* to DwmGetColorizationColor: that returns the
// title-bar colour, which on a machine with "show accent on title bars" off is a
// neutral grey -- and a grey accent makes the primary button indistinguishable from
// the secondary one, which is the one distinction this window cannot afford to lose.
inline D2D1_COLOR_F SystemAccent(bool dark) {
    BYTE pal[32] = {};
    DWORD size = sizeof(pal);
    const int want = dark ? 1 : 4;
    if (RegGetValueW(HKEY_CURRENT_USER,
                     L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, pal, &size) == ERROR_SUCCESS &&
        size == sizeof(pal)) {
        const BYTE *e = pal + want * 4;   // RGBA
        return D2D1::ColorF(e[0] / 255.0f, e[1] / 255.0f, e[2] / 255.0f, 1.0f);
    }
    return D2D1::ColorF(0x0078D4);
}

// ---------------------------------------------------------------- the palette

// Fluent's own token names, because when one of these looks wrong the way to find out
// is to compare it against Microsoft's published table, and that table uses these
// names.
//
// **Most of these carry real alpha, and that is not decoration.** The window is drawn
// over Mica (see window.h), which is a material DWM samples from the wallpaper -- a
// card painted opaque white would punch a flat hole in it, which is exactly what a
// window looks like when somebody has bolted a Fluent palette onto an opaque surface.
// WinUI's own tokens are alpha over the backdrop and these are the same values:
// `CardBackgroundFillColorDefault` is white at 70% in light and white at 5.12% in
// dark, `ControlStrokeColorDefault` is black at 5.78% and white at 8.37%. Those
// percentages look arbitrary and are not -- they are Microsoft's, and matching them is
// the difference between "looks native" and "looks close".
//
// `windowBg` is the exception: it is opaque, and it is used only on a machine where
// the backdrop was refused (Windows 11 before 22H2). Everything else composites over
// whatever is behind it.
struct Palette {
    bool dark = false;

    D2D1_COLOR_F accent;
    D2D1_COLOR_F accentHover;
    D2D1_COLOR_F accentPressed;
    D2D1_COLOR_F accentText;      // what reads on top of `accent`

    D2D1_COLOR_F windowBg;        // opaque fallback, used only when Mica was refused
    D2D1_COLOR_F layerBg;         // a large region of page over the backdrop
    D2D1_COLOR_F cardBg;          // a raised surface on the page
    D2D1_COLOR_F cardStroke;
    // A flyout is not a card. It is *over* the page rather than part of it, and Windows
    // gives it an opaque surface of its own -- acrylic, with a solid fallback -- plus a
    // heavier border. Painting one in the card's colour (white at 5% in dark, at 70% in
    // light) leaves it see-through, so the page's own text reads through the list of
    // options; that is what it looked like until somebody said so.
    D2D1_COLOR_F flyoutBg;
    D2D1_COLOR_F flyoutStroke;
    D2D1_COLOR_F controlBg;       // button / input rest state
    D2D1_COLOR_F controlBgHover;
    D2D1_COLOR_F controlBgPressed;
    D2D1_COLOR_F controlBgInput;      // a text field with the caret in it
    D2D1_COLOR_F controlStroke;
    D2D1_COLOR_F controlStrokeBottom;  // the 1px darker bottom edge Fluent controls have
    D2D1_COLOR_F subtleHover;          // background a borderless button takes on hover
    // A scroll bar's two colours, both named by WinUI's ScrollBar template: the thumb and
    // the arrows are ControlStrongFillColorDefault, and the track that comes up under
    // them is AcrylicInAppFillColorDefault. Acrylic is a composition effect this window
    // cannot draw, so that one is the brush's own FallbackColor -- what WinUI shows too
    // when transparency effects are off.
    D2D1_COLOR_F controlStrong;
    D2D1_COLOR_F acrylicInApp;

    D2D1_COLOR_F textPrimary;
    D2D1_COLOR_F textSecondary;
    D2D1_COLOR_F textDisabled;

    D2D1_COLOR_F ok;
    D2D1_COLOR_F warn;
    D2D1_COLOR_F bad;
    D2D1_COLOR_F okBg;
    D2D1_COLOR_F warnBg;
    D2D1_COLOR_F badBg;
};

inline D2D1_COLOR_F Rgb(UINT32 hex, float a = 1.0f) { return D2D1::ColorF(hex, a); }

// Nudge a colour towards white or black, for the hover and pressed states of the
// accent button. Windows generates those from the accent the same way rather than
// storing three separate values, and the palette only carries the one shade.
inline D2D1_COLOR_F Shade(const D2D1_COLOR_F &c, float towardsWhite) {
    const float t = towardsWhite;
    if (t >= 0)
        return D2D1::ColorF(c.r + (1.0f - c.r) * t, c.g + (1.0f - c.g) * t,
                            c.b + (1.0f - c.b) * t, c.a);
    return D2D1::ColorF(c.r * (1.0f + t), c.g * (1.0f + t), c.b * (1.0f + t), c.a);
}

// ---------------------------------------------------------------- motion

// Time, and what a control is allowed to do with it.
//
// **Everything here is Microsoft's published number, the same rule the palette above
// follows.** The first version of this was written from an impression of how Windows 11
// feels, and an impression is how a window ends up moving like something else: it
// cross-faded every state of every control, scaled buttons on press, and grew a text
// field's underline out of its centre -- which is Material Design's signature, not
// Fluent's. What WinUI actually does is narrower and is written down.
//
// Two mechanisms, because XAML has two and they behave differently:
//
//   **BrushTransition** -- a *linear* cross-fade of one brush into another over
//   ControlFasterAnimationDuration. This is what a Button's background does under the
//   pointer: `<ContentPresenter.BackgroundTransition><BrushTransition
//   Duration="0:0:0.083"/>`. Nothing else about the control moves -- the border and the
//   text are plain visual-state setters and change between two frames. `Ramp` below is
//   this, and the linearity is not an approximation: BrushTransition takes no easing.
//
//   **Storyboard** -- a duration and a KeySpline, for a control that is *going
//   somewhere*: the switch's knob, the expander's height, a flyout arriving, a page
//   arriving. `Track` below is this.
//
// Durations: WinUI's three published ThemeResources, in seconds.
// Easing: Fluent's own two curves, quoted as cubic-bezier from the design guidance --
// "Fast Out, Slow In" for anything entering or settling, "Slow Out, Fast In" for
// anything leaving. Both are extreme on purpose; that extremity is most of what makes
// Windows 11 feel quick rather than floaty.
//
// **Seconds, not frames**, and the clock is the compositor's -- see Window::Run. A
// per-tick increment (the toggle switch used to do `knob += 0.18f`) runs at whatever
// rate the message queue manages, which was measured at anything between 2 and 35 ms
// per frame.
namespace motion {

// WinUI: ControlFasterAnimationDuration, ControlFastAnimationDuration,
// ControlNormalAnimationDuration.
constexpr float kFaster = 0.083f;   // a pointer state crossing over
constexpr float kFast   = 0.167f;   // a control taking a new value
constexpr float kNormal = 0.250f;   // a page, a panel, a flyout

// cubic-bezier(0, 0, 0, 1) -- "Fast Out, Slow In", for anything arriving.
//
// Closed form rather than solved, because this particular curve has one: with both x
// control points at zero, x(s) = s^3, so s = cbrt(u), and y = 3s^2 - 2s^3 collapses to
// the line below. Half the duration covers 89% of the distance, which is what the
// guidance means by "extreme friction".
inline float Decel(float u) {
    if (u <= 0.0f) return 0.0f;
    if (u >= 1.0f) return 1.0f;
    const float s = std::cbrt(u);
    return s * s * (3.0f - 2.0f * s);
}

// cubic-bezier(1, 0, 1, 1) -- "Slow Out, Fast In", for anything leaving. The mirror of
// the above: x(s) = 3s - 3s^2 + s^3 has no useful inverse, and this identity is exact.
inline float Accel(float u) { return 1.0f - Decel(1.0f - u); }

// cubic-bezier(0.4, 0, 0.6, 1.0) -- "ease in and out", for something crossing from
// one end of its track to the other. It is the shape `KeySpline="0.4, 0.0, 0.6, 1.0"`
// draws in XAML, which is what WinUI's indeterminate progress bar travels on.
//
// Solved rather than closed-form, unlike the two above: with both control points
// live, x(s) = 0.4s^3 - 0.6s^2 + 1.2s is a cubic with no inverse worth writing down.
// Newton from a linear first guess, which is all this curve needs -- its slope is
// between 0.9 and 1.2 everywhere. The y(s) that falls out is smoothstep(s).
inline float InOut(float u) {
    if (u <= 0.0f) return 0.0f;
    if (u >= 1.0f) return 1.0f;
    float s = u;
    for (int i = 0; i < 3; i++)
        s -= (((0.4f * s - 0.6f) * s + 1.2f) * s - u) / ((1.2f * s - 1.2f) * s + 1.2f);
    return s * s * (3.0f - 2.0f * s);
}

// A brush crossing over: linear, at a fixed rate, reversible at any point.
//
// Returns true while it is still moving. Reversing mid-fade walks back at the same
// rate from wherever it got to, which is what BrushTransition does and is the reason
// this is a rate rather than a storyboard -- a pointer that crosses a control and
// leaves again has no "from" worth remembering.
inline bool Ramp(float *now, float want, float dt, float seconds) {
    if (*now == want) return false;
    const float step = dt / seconds;
    if (*now < want) *now = (*now + step > want) ? want : *now + step;
    else             *now = (*now - step < want) ? want : *now - step;
    return *now != want;
}

// A value on its way somewhere: a start, a target, a duration and a curve. One
// Storyboard.
//
// Retargeting mid-flight restarts the curve from where the value actually is, which is
// what XAML does when a new Storyboard takes over a property -- and is why `from` is
// kept rather than assumed to be 0 or 1. Without it a switch flicked twice quickly
// jumps back to the far end before setting off.
struct Track {
    float value = 0.0f;          // what to draw
    float from = 0.0f, to = 0.0f;
    float t = 1.0f;              // progress through the duration, 0..1

    explicit Track(float at = 0.0f) : value(at), from(at), to(at) {}

    void To(float target) {
        if (target == to) return;
        from = value;
        to = target;
        t = 0.0f;
    }
    // Straight there, no animation. For a control being rebuilt with a new value by a
    // Layout() that is not the result of the person changing it -- a page being built
    // for the first time has nothing to animate from.
    void Set(float at) { value = from = to = at; t = 1.0f; }
    bool Moving() const { return t < 1.0f; }
    // Is there anything left to do to reach `target`? Either the storyboard is still
    // running, or **nobody has told it about this target yet** -- which is the state a
    // control is in between the click that changed it and the first frame after that,
    // and is exactly the moment the window is deciding whether to run frames at all.
    //
    // Asking only `Moving()` there is a deadlock with no symptom but a stuck control:
    // the loop sees nothing moving and goes to sleep, so `Tick` never runs, so `To` is
    // never called, so nothing ever moves. The drop-down closed with Escape stayed on
    // screen that way -- closed as far as every click was concerned, still drawn.
    bool Wants(float target) const { return t < 1.0f || to != target; }
    bool Step(float dt, float seconds, float (*ease)(float) = Decel) {
        if (t >= 1.0f) return false;
        t += dt / seconds;
        if (t >= 1.0f) { t = 1.0f; value = to; return false; }
        value = from + (to - from) * ease(t);
        return true;
    }
};

// A slot that a marker moves between, and the interval that stretches between where it has
// got to and where it is going.
//
// `lead` sets off and `trail` follows it, so `Lo()` and `Hi()` are the same slot at rest and
// a slot apart while it travels: whatever is drawn between them is one item wide when it
// lands and longer on the way. That is how Windows draws a selection moving between items --
// the segmented control's block, a navigation bar's accent, a list's indicator -- and it is
// what makes a step read as a move rather than as a jump between two frames.
//
// Two followers rather than a `Track`, and followers rather than a curve each: something
// aimed at a new slot several times inside one frame must not restart a curve every time,
// and a follower that is retargeted is continuous by construction. `slide` equal to `close`
// welds the two edges into one, which is what a marker that must not stretch wants.
struct Span {
    float lead = 0.0f, trail = 0.0f;   // in slots
    float to = 0.0f;                   // the slot both of them belong on
    float slide = 0.05f;               // seconds: time constant of the leading edge
    float close = 0.028f;              // and of the trailing edge, which follows it

    explicit Span(float at = 0.0f) : lead(at), trail(at), to(at) {}

    // The slot it belongs on. Aimed, not started: call it every frame, from `Tick`.
    void To(float at) { to = at; }
    // Straight there, no animation. For a control being built for the first time -- a
    // first layout has nothing to move from.
    void Set(float at) { lead = trail = to = at; }
    // Anything left to do to reach `at`? Either edge still moving, or nobody having told
    // it about this slot yet. Use this in `Animating()` and `AnimationWanted()`.
    bool Wants(float at) const { return lead != at || trail != lead; }
    bool Step(float dt) {
        if (!Wants(to)) return false;
        lead += (to - lead) * (1.0f - std::exp(-dt / slide));
        trail += (lead - trail) * (1.0f - std::exp(-dt / close));
        if (std::fabs(to - lead) < 0.002f) lead = to;
        if (std::fabs(lead - trail) < 0.002f) trail = lead;
        return true;
    }
    // The two edges, the lower one first.
    float Lo() const { return lead < trail ? lead : trail; }
    float Hi() const { return lead < trail ? trail : lead; }
};

}  // namespace motion

// Two colours mixed, for a control cross-fading between two of its states.
//
// Straight alpha, component by component, and here that is correct rather than
// approximate: every pair this is called with is one of Fluent's fill tokens and the
// next one up, which differ in *alpha over the same white or black*. Mixing them is
// mixing one colour's opacity, which is exactly what cross-fading it over the backdrop
// would have produced.
inline D2D1_COLOR_F Mix(const D2D1_COLOR_F &a, const D2D1_COLOR_F &b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t,
                        a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
}

// The same colour, fainter. For fading one thing in or out where a layer would be the
// heavier answer -- a line of text, a badge, a knob.
inline D2D1_COLOR_F Fade(const D2D1_COLOR_F &c, float mul) {
    return D2D1::ColorF(c.r, c.g, c.b, c.a * mul);
}

inline Palette MakePalette(bool dark) {
    Palette p;
    p.dark   = dark;
    p.accent = SystemAccent(dark);
    // Fluent moves the accent button *away* from the text colour on hover and back
    // towards it when pressed, which reads as lifting and then settling. In dark
    // mode the text on it is black, so the directions swap.
    p.accentHover   = Shade(p.accent, dark ? +0.08f : -0.10f);
    p.accentPressed = Shade(p.accent, dark ? -0.10f : -0.20f);
    // Black text on a dark-mode accent (which is a light shade), white on a
    // light-mode one (which is dark). This is what Windows does, and it is the whole
    // reason the two shades are picked apart above.
    p.accentText = dark ? Rgb(0x000000) : Rgb(0xFFFFFF);

    if (dark) {
        p.windowBg            = Rgb(0x202020);                 // SolidBackgroundFillColorBase
        p.layerBg             = Rgb(0x3A3A3A, 0.30f);          // LayerFillColorDefault
        p.cardBg              = Rgb(0xFFFFFF, 0.0512f);        // CardBackgroundFillColorDefault
        p.cardStroke          = Rgb(0x000000, 0.10f);          // CardStrokeColorDefault
        p.flyoutBg            = Rgb(0x2C2C2C);                 // SolidBackgroundFillColorTertiary
        p.flyoutStroke        = Rgb(0x000000, 0.20f);          // SurfaceStrokeColorFlyout
        p.controlBg           = Rgb(0xFFFFFF, 0.0605f);        // ControlFillColorDefault
        p.controlBgHover      = Rgb(0xFFFFFF, 0.0837f);        // ...Secondary
        p.controlBgPressed    = Rgb(0xFFFFFF, 0.0326f);        // ...Tertiary
        p.controlBgInput      = Rgb(0x1E1E1E, 0.70f);          // ControlFillColorInputActive
        p.controlStroke       = Rgb(0xFFFFFF, 0.0837f);        // ControlStrokeColorDefault
        p.controlStrokeBottom = Rgb(0x000000, 0.14f);          // ...Secondary, the bottom edge
        p.subtleHover         = Rgb(0xFFFFFF, 0.0605f);        // SubtleFillColorSecondary
        p.controlStrong       = Rgb(0xFFFFFF, 0.5451f);        // ControlStrongFillColorDefault, #8B
        p.acrylicInApp        = Rgb(0x2C2C2C);                 // AcrylicInAppFillColorDefault's fallback
        p.textPrimary         = Rgb(0xFFFFFF);
        p.textSecondary       = Rgb(0xFFFFFF, 0.786f);         // TextFillColorSecondary
        p.textDisabled        = Rgb(0xFFFFFF, 0.3628f);        // TextFillColorDisabled
        p.ok                  = Rgb(0x6CCB5F);
        p.warn                = Rgb(0xFCE100);
        p.bad                 = Rgb(0xFF99A4);
        p.okBg                = Rgb(0x393D1B);
        p.warnBg              = Rgb(0x433519);
        p.badBg               = Rgb(0x442726);
    } else {
        p.windowBg            = Rgb(0xF3F3F3);
        p.layerBg             = Rgb(0xFFFFFF, 0.50f);
        p.cardBg              = Rgb(0xFFFFFF, 0.70f);
        p.cardStroke          = Rgb(0x000000, 0.0578f);
        p.flyoutBg            = Rgb(0xF9F9F9);                 // SolidBackgroundFillColorTertiary
        p.flyoutStroke        = Rgb(0x000000, 0.0578f);
        p.controlBg           = Rgb(0xFFFFFF, 0.70f);
        p.controlBgHover      = Rgb(0xF9F9F9, 0.50f);
        p.controlBgPressed    = Rgb(0xF9F9F9, 0.30f);
        p.controlBgInput      = Rgb(0xFFFFFF, 1.00f);          // opaque, and Fluent's own
        p.controlStroke       = Rgb(0x000000, 0.0578f);
        p.controlStrokeBottom = Rgb(0x000000, 0.16f);
        p.subtleHover         = Rgb(0x000000, 0.0373f);
        p.controlStrong       = Rgb(0x000000, 0.4471f);        // ControlStrongFillColorDefault, #72
        p.acrylicInApp        = Rgb(0xF9F9F9);                 // AcrylicInAppFillColorDefault's fallback
        p.textPrimary         = Rgb(0x000000, 0.8956f);        // TextFillColorPrimary
        p.textSecondary       = Rgb(0x000000, 0.6063f);
        p.textDisabled        = Rgb(0x000000, 0.3614f);
        p.ok                  = Rgb(0x0F7B0F);
        p.warn                = Rgb(0x9D5D00);
        p.bad                 = Rgb(0xC42B1C);
        p.okBg                = Rgb(0xDFF6DD);
        p.warnBg              = Rgb(0xFFF4CE);
        p.badBg               = Rgb(0xFDE7E9);
    }
    return p;
}

// ---------------------------------------------------------------- metrics

// Everything in DIPs. The render target is told the window's DPI, so these are the
// numbers that reach Direct2D unchanged and the scale factor appears exactly once,
// in window.h, rather than in every rectangle.
namespace metric {
constexpr float kRadiusControl = 4.0f;
constexpr float kRadiusCard    = 8.0f;
constexpr float kControlH      = 32.0f;
constexpr float kButtonMinW    = 100.0f;
}  // namespace metric

// ---------------------------------------------------------------- typography

// Segoe UI Variable is Windows 11's UI face and its two optical sizes are separate
// family names -- Display for anything large, Text for body copy. Asking for the
// wrong one is not fatal, it just looks slightly off at that size, which is the kind
// of thing nobody can name and everybody sees.
//
// The fallback is plain Segoe UI, for Windows 10, which has neither optical size. The
// check is made rather than assumed, because DirectWrite substitutes a missing family
// silently -- see HasFamily.
struct Fonts {
    IDWriteFactory *dw = nullptr;

    IDWriteTextFormat *title      = nullptr;   // 28 Display Semibold
    IDWriteTextFormat *subtitle   = nullptr;   // 20 Display Semibold
    IDWriteTextFormat *bodyStrong = nullptr;   // 14 Text Semibold
    IDWriteTextFormat *body       = nullptr;   // 14 Text
    IDWriteTextFormat *caption    = nullptr;   // 12 Text
    IDWriteTextFormat *mono       = nullptr;   // 12 Cascadia/Consolas -- paths, log lines
    IDWriteTextFormat *icon       = nullptr;   // 16 Segoe Fluent Icons
    IDWriteTextFormat *iconLarge  = nullptr;   // 28 of the same, for a page's one badge
    IDWriteTextFormat *iconSmall  = nullptr;   // 10, the size Windows draws a caption at
    IDWriteTextFormat *iconTiny   = nullptr;   // 8, a scroll bar's arrows (ScrollBarButtonArrowIconFontSize)

    bool Create(IDWriteFactory *factory);
    void Release();
};

inline IDWriteTextFormat *MakeFormat(IDWriteFactory *dw, const wchar_t *family,
                                     DWRITE_FONT_WEIGHT weight, float size) {
    IDWriteTextFormat *f = nullptr;
    // The locale name is the empty string on purpose. DirectWrite uses it to pick
    // language-specific glyph forms, and a window may show Chinese or Japanese text on
    // a machine whose UI language is anything; asking for "en-us" here is how CJK text
    // gets the wrong regional variants of a handful of shared characters.
    if (FAILED(dw->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL,
                                    DWRITE_FONT_STRETCH_NORMAL, size, L"", &f)))
        return nullptr;
    f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    return f;
}

// Does the machine actually have this family? CreateTextFormat succeeds for a name
// that does not exist -- DirectWrite substitutes at layout time and never says so --
// so the only way to know is to ask the system font collection.
inline bool HasFamily(IDWriteFactory *dw, const wchar_t *family) {
    IDWriteFontCollection *sys = nullptr;
    if (FAILED(dw->GetSystemFontCollection(&sys)) || !sys) return false;
    UINT32 index = 0; BOOL exists = FALSE;
    const HRESULT hr = sys->FindFamilyName(family, &index, &exists);
    sys->Release();
    return SUCCEEDED(hr) && exists;
}

inline bool Fonts::Create(IDWriteFactory *factory) {
    dw = factory;
    const wchar_t *display = HasFamily(dw, L"Segoe UI Variable Display")
                                 ? L"Segoe UI Variable Display" : L"Segoe UI";
    const wchar_t *text    = HasFamily(dw, L"Segoe UI Variable Text")
                                 ? L"Segoe UI Variable Text" : L"Segoe UI";
    const wchar_t *code    = HasFamily(dw, L"Cascadia Mono") ? L"Cascadia Mono" : L"Consolas";
    // Windows 11's icon font. Windows 10 has the older Segoe MDL2 Assets, and the
    // glyph *code points* Micula uses are in both -- the check is for the
    // machine that has neither, where a missing font would draw the private-use
    // code point as a box.
    const wchar_t *glyphs  = HasFamily(dw, L"Segoe Fluent Icons") ? L"Segoe Fluent Icons"
                           : HasFamily(dw, L"Segoe MDL2 Assets")  ? L"Segoe MDL2 Assets"
                                                                  : L"Segoe UI Symbol";

    title      = MakeFormat(dw, display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 28.0f);
    subtitle   = MakeFormat(dw, display, DWRITE_FONT_WEIGHT_SEMI_BOLD, 20.0f);
    bodyStrong = MakeFormat(dw, text,    DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.0f);
    body       = MakeFormat(dw, text,    DWRITE_FONT_WEIGHT_NORMAL,    14.0f);
    caption    = MakeFormat(dw, text,    DWRITE_FONT_WEIGHT_NORMAL,    12.0f);
    mono       = MakeFormat(dw, code,    DWRITE_FONT_WEIGHT_NORMAL,    12.0f);
    icon       = MakeFormat(dw, glyphs,  DWRITE_FONT_WEIGHT_NORMAL,    16.0f);
    // Separate from `title` rather than reused at a larger size: the icon glyphs live
    // in the private-use area, and a text face has nothing there. Drawing one with the
    // title format produces the substitution box instead of the icon.
    iconLarge  = MakeFormat(dw, glyphs,  DWRITE_FONT_WEIGHT_NORMAL,    28.0f);
    iconSmall  = MakeFormat(dw, glyphs,  DWRITE_FONT_WEIGHT_NORMAL,    10.0f);
    iconTiny   = MakeFormat(dw, glyphs,  DWRITE_FONT_WEIGHT_NORMAL,     8.0f);
    return title && subtitle && bodyStrong && body && caption && mono && icon &&
           iconLarge && iconSmall && iconTiny;
}

inline void Fonts::Release() {
    IDWriteTextFormat *all[] = { title, subtitle, bodyStrong, body, caption, mono, icon,
                                 iconLarge, iconSmall, iconTiny };
    for (IDWriteTextFormat *f : all) if (f) f->Release();
    title = subtitle = bodyStrong = body = caption = mono = nullptr;
    icon = iconLarge = iconSmall = iconTiny = nullptr;
}

// Settings > Accessibility > Visual effects > "Always show scrollbars", which WinUI reads
// as UISettings.AutoHideScrollBars. HKCU\Control Panel\Accessibility\DynamicScrollbars:
// 0 is always show, 1 is hide. Absent means nobody changed it, which is hide.
inline bool SystemAutoHidesScrollBars() {
    DWORD v = 1, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Control Panel\\Accessibility", L"DynamicScrollbars",
                     RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return true;
    return v != 0;
}

// A handful of Segoe Fluent Icons code points -- the ones the controls draw, and a few
// a page commonly wants -- named so a reader does not have to look them up. Every one
// of them also exists in Segoe MDL2 Assets at the same code point, which is what makes
// the fallback above safe. Draw them with Fonts::icon, never with a text format.
//
// Escapes rather than the characters themselves, so this header means the same thing
// whatever code page the compiler reads it in.
namespace glyph {
constexpr const wchar_t *kCheck    = L"\uE73E";  // CheckMark
constexpr const wchar_t *kWarning  = L"\uE7BA";  // Warning
constexpr const wchar_t *kError    = L"\uEA39";  // ErrorBadge
constexpr const wchar_t *kInfo     = L"\uE946";  // Info
constexpr const wchar_t *kFolder   = L"\uE8B7";  // FolderOpen
constexpr const wchar_t *kChevron  = L"\uE70D";  // ChevronDown
constexpr const wchar_t *kRefresh  = L"\uE72C";  // Refresh
constexpr const wchar_t *kSettings = L"\uE713";  // Settings
constexpr const wchar_t *kShield   = L"\uEA18";  // Shield
constexpr const wchar_t *kBusy     = L"\uE895";  // SyncStatus
constexpr const wchar_t *kCaretUp   = L"\uEDDB";  // CaretUpSolid8, a scroll bar's arrow
constexpr const wchar_t *kCaretDown = L"\uEDDC";  // CaretDownSolid8
}  // namespace glyph

}  // namespace micula
