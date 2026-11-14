// Draws a skin with Direct2D. Each panel's text is laid out when its words change -- once a
// minute for a clock, when the data changes, and every second for a playback position -- into a
// cached bitmap; drawing composites the bitmaps with their shadows. Bars are laid out with the
// text, as inline objects. A spectrum takes its room in the line the same way, but its bands move
// with the sound, so they are drawn again every frame, into a second bitmap beside the text's.
#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <memory>
#include <string>
#include <vector>

#include "animelogon/clock.h"
#include "animelogon/skin.h"
#include "animelogon/spectrum.h"

namespace animelogon {

class SkinView {
public:
    // Makes its own device context on `device`. False leaves nothing to draw.
    bool Init(ID2D1Device *device);
    void Release();

    // `format` is the regional format; the style may put the words in another language.
    void Set(const skin::Resolved &skin, const ClockStyle &style, const RegionalFormat &format);
    // The battery and what is playing, for the next Tick.
    void SetData(const data::Snapshot &snapshot);

    // What the output device is playing, for the spectra: once a frame on the logon screen. Each
    // spectrum's bands follow it as quickly as its smoothing allows. Until the first frame they
    // lie at rest.
    void SetAudio(const spectrum::Frame &frame);
    // Every spectrum straight at `frame`'s levels, as previews show them.
    void SettleAudio(const spectrum::Frame &frame);
    // Whether a panel holds a spectrum; whether one is still moving, so must be drawn again.
    bool Listens() const { return !spectra_.empty(); }
    bool Moving() const;

    // Reads the time; returns true when the words changed. `utc` is the same moment as UTC
    // FILETIME ticks, for playback positions; 0 reads the clock.
    bool Tick();
    bool Tick(const SYSTEMTIME &now, uint64_t utc = 0);

    // Draws onto `target` as the skin appears on a display of that size. `slot` keeps one
    // cache per display.
    bool Draw(ID2D1Bitmap1 *target, UINT width, UINT height, size_t slot, float opacity = 1.0f);

    // The logon screen brings each panel in as its `enter`, `enter-delay` and `enter-duration`
    // say, counted from the moment the wallpaper is up: `seconds` is how long ago that was.
    // Negative (the default, and what previews use) draws every panel in place.
    void SetEntrance(float seconds) { entrance_ = seconds; }
    // Whether some panel is still on its way in at the time last set.
    bool Entering() const;

    ID2D1DeviceContext *context() const { return dc_.Get(); }

    // Where each panel and text was last drawn in `slot`, in the target's pixels, named as
    // adjustments name them (p1, p1.l1.t1).
    struct Bound {
        std::wstring key;
        D2D1_RECT_F rect{};
        // A panel's point that is held at its anchor: at a left, centre or right anchor (x) and at
        // a top, middle or bottom one (y). Texts that hang and the last line's baseline mean this
        // is not the rectangle's edge or middle. Another column can line the lines up differently,
        // so only the panel's own column is exact; the other two keep that edge, or the middle, of
        // the ink where it is. Texts have none.
        bool holds = false;
        float holdX[3]{}, holdY[3]{};
    };
    std::vector<Bound> Bounds(size_t slot) const;

private:
    // A panel laid out for one display.
    struct Built {
        std::vector<Bound> bounds;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> text;
        D2D1_POINT_2F at{};
        float px = 0.0f;  // 1 em
        // A glow fills the ellipse, a plate the rounded rectangle.
        Microsoft::WRL::ComPtr<ID2D1Brush> backdrop;
        bool plate = false;
        D2D1_ELLIPSE backdropArea{};
        D2D1_ROUNDED_RECT plateArea{};
        // The spectra, where they are in `text`'s pixels, and `live`, a bitmap of its size that
        // they are drawn into each frame.
        struct Spectrum {
            size_t audio = 0;        // in spectra_
            size_t line = 0, text = 0;
            D2D1_RECT_F box{};
            Microsoft::WRL::ComPtr<ID2D1Brush> fill, peak;
        };
        std::vector<Spectrum> spectra;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> live;
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
        // Where each spectrum's room was drawn, once the line has been: its top left, in the
        // target's pixels.
        struct Room {
            size_t text;
            std::shared_ptr<D2D1_POINT_2F> at;
        };
        std::vector<Room> rooms;
    };
    // A spectrum in the skin, and its bands as they move.
    struct Audio {
        size_t panel = 0, line = 0, text = 0;
        spectrum::Bands bands;
    };

    bool Build(Built &built, size_t panel, UINT width, UINT height);
    bool Lay(Line &line, const skin::Panel &panel, const skin::Line &source, const std::vector<std::wstring> &words,
             float px);
    std::wstring Family(const std::wstring &wanted) const;
    // `words` laid out alone as `text` says, on one line, for measuring or for drawing apart from
    // its line.
    Microsoft::WRL::ComPtr<IDWriteTextLayout> Alone(const skin::Text &text, const std::wstring &words, float px) const;
    // `words`, cut short with an ellipsis where they are wider than the text's `max`.
    std::wstring Fit(const skin::Text &text, const std::wstring &words, float px) const;
    void Figures(IDWriteTextLayout *layout, const skin::Text &text, DWRITE_TEXT_RANGE range) const;
    // What a spectrum asks of each frame: its own bands, or for one with the bass in the middle,
    // each band shown twice.
    static spectrum::Shape ShapeOf(const skin::Text &text);
    bool Brushes(Built::Spectrum &place, const skin::Text &text);
    void DrawSpectrum(const Built::Spectrum &place, const skin::Text &text, float px);

    skin::Resolved skin_;
    ClockStyle style_;
    RegionalFormat format_;  // in the style's language
    // Panel, line, text: the words, or for a bar its fraction; empty where nothing is drawn.
    std::vector<std::vector<std::vector<std::wstring>>> words_;
    unsigned version_ = 0;
    float entrance_ = -1.0f;
    SYSTEMTIME shown_{};
    data::Snapshot data_;
    unsigned dataVersion_ = 0, dataShown_ = 0;
    bool everySecond_ = false;  // a text or bar follows a playback position
    std::vector<Audio> spectra_;

    Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc_;
    Microsoft::WRL::ComPtr<ID2D1Factory> factory_;
    Microsoft::WRL::ComPtr<ID2D1StrokeStyle> round_;  // round caps and joins, for lines
    Microsoft::WRL::ComPtr<IDWriteFactory> write_;
    std::vector<Microsoft::WRL::ComPtr<ID2D1Effect>> shadows_;
    std::vector<Slot> slots_;
};

}  // namespace animelogon
