// Draws a skin with Direct2D. Each panel's text is laid out when its words change -- once a
// minute for a clock -- into a cached bitmap; drawing composites the bitmaps with their shadows.
#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "animelogon/clock.h"
#include "animelogon/skin.h"

namespace animelogon {

class SkinView {
public:
    // Makes its own device context on `device`. False leaves nothing to draw.
    bool Init(ID2D1Device *device);
    void Release();

    // `format` is the regional format; the style may put the words in another language.
    void Set(const skin::Resolved &skin, const ClockStyle &style, const RegionalFormat &format);

    // Reads the time; returns true when the words changed.
    bool Tick();
    bool Tick(const SYSTEMTIME &now);

    // Draws onto `target` as the skin appears on a display of that size. `slot` keeps one
    // cache per display.
    bool Draw(ID2D1Bitmap1 *target, UINT width, UINT height, size_t slot, float opacity = 1.0f);

    ID2D1DeviceContext *context() const { return dc_.Get(); }

    // Where each panel and text was last drawn in `slot`, in the target's pixels, named as
    // adjustments name them (p1, p1.l1.t1).
    struct Bound {
        std::wstring key;
        D2D1_RECT_F rect{};
    };
    std::vector<Bound> Bounds(size_t slot) const;

private:
    // A panel laid out for one display.
    struct Built {
        std::vector<Bound> bounds;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> text;
        D2D1_POINT_2F at{};
        float px = 0.0f;  // 1 em
        Microsoft::WRL::ComPtr<ID2D1RadialGradientBrush> backdrop;
        D2D1_ELLIPSE backdropArea{};
    };
    struct Slot {
        UINT width = 0, height = 0;
        unsigned version = 0;
        std::vector<Built> panels;
    };
    // A line of text, measured by its glyphs rather than its line box.
    struct Line {
        Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
        float baseline = 0.0f;  // from the layout's top
        float inkLeft = 0.0f, inkRight = 0.0f, inkTop = 0.0f, inkBottom = 0.0f;  // from its top left
        float coreLeft = 0.0f, coreRight = 0.0f;  // without the texts that hang
        struct Run {
            size_t text;  // its index in the line
            DWRITE_TEXT_RANGE range;
        };
        std::vector<Run> runs;
    };

    bool Build(Built &built, size_t panel, UINT width, UINT height);
    bool Lay(Line &line, const skin::Panel &panel, const skin::Line &source, const std::vector<std::wstring> &words,
             float px);
    std::wstring Family(const std::wstring &wanted) const;

    skin::Resolved skin_;
    ClockStyle style_;
    RegionalFormat format_;  // in the style's language
    std::vector<std::vector<std::vector<std::wstring>>> words_;  // panel, line, text
    unsigned version_ = 0;
    SYSTEMTIME shown_{};

    Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc_;
    Microsoft::WRL::ComPtr<IDWriteFactory> write_;
    std::vector<Microsoft::WRL::ComPtr<ID2D1Effect>> shadows_;
    std::vector<Slot> slots_;
};

}  // namespace animelogon
