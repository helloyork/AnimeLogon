#include "animelogon/skinview.h"

#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <dwrite_1.h>

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <cmath>
#include <cwchar>

#include "animelogon/log.h"

using Microsoft::WRL::ComPtr;

namespace animelogon {
namespace {

constexpr float kUnbounded = 100000.0f;
constexpr size_t kShadows = 4;

D2D1_COLOR_F ColorOf(uint32_t argb, float opacity) {
    return D2D1::ColorF(((argb >> 16) & 0xFF) / 255.0f, ((argb >> 8) & 0xFF) / 255.0f, (argb & 0xFF) / 255.0f,
                        ((argb >> 24) & 0xFF) / 255.0f * opacity);
}

bool HasFamily(IDWriteFactory *write, const std::wstring &family) {
    ComPtr<IDWriteFontCollection> fonts;
    UINT32 index = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(write->GetSystemFontCollection(&fonts)) &&
           SUCCEEDED(fonts->FindFamilyName(family.c_str(), &index, &exists)) && exists;
}

// A bar among a line's texts. DirectWrite lays it out like a character; Direct2D draws it into
// the panel's bitmap while the line is drawn. `lead` is the space before it.
class Bar : public IDWriteInlineObject {
public:
    Bar(ID2D1DeviceContext *dc, const skin::Text &t, float px, float lead, float axis, float fraction)
        : dc_(dc), t_(t), lead_(lead), length_(t.length * px), height_(std::max(1.0f, t.thickness * px)),
          axis_(axis), fraction_(fraction) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDWriteInlineObject)) {
            *out = static_cast<IDWriteInlineObject *>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (!left) delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE Draw(void *, IDWriteTextRenderer *, FLOAT x, FLOAT y, BOOL, BOOL, IUnknown *) override {
        const float left = std::round(x + lead_), top = std::round(y);
        const float radius = std::min(t_.radius, 0.5f) * height_;
        auto fill = [&](float width, uint32_t color, float opacity) {
            if (width <= 0.0f || opacity <= 0.0f) return;
            ComPtr<ID2D1SolidColorBrush> brush;
            if (FAILED(dc_->CreateSolidColorBrush(ColorOf(color, opacity), &brush))) return;
            const float r = std::min(radius, width / 2.0f);
            dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(left, top, left + width, top + height_), r, r),
                                      brush.Get());
        };
        fill(length_, t_.track, t_.trackOpacity);
        fill(length_ * fraction_, t_.color, t_.opacity);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS *m) override {
        // Centred `axis` above the line's baseline.
        *m = {lead_ + length_, height_, axis_ + height_ / 2.0f, FALSE};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS *o) override {
        *o = {};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBreakConditions(DWRITE_BREAK_CONDITION *before, DWRITE_BREAK_CONDITION *after) override {
        *before = *after = DWRITE_BREAK_CONDITION_NEUTRAL;
        return S_OK;
    }

private:
    virtual ~Bar() = default;
    std::atomic<ULONG> refs_{1};
    ID2D1DeviceContext *dc_;
    skin::Text t_;
    float lead_, length_, height_, axis_, fraction_;
};

// A spectrum's room in a line. DirectWrite lays it out like a character -- as wide as the spectrum
// and as tall, standing on the line's baseline -- and the panel's bitmap keeps it empty: the bands
// move with the sound, so they are drawn every frame apart from the text (SkinView::Draw). Drawing
// the line only says where the room was put. A line's stroke reaches past the room by half its
// width, and `overhang` says so, so that the panel's bitmap is large enough for it.
class Room : public IDWriteInlineObject {
public:
    Room(float lead, float width, float height, float overhang, std::shared_ptr<D2D1_POINT_2F> at)
        : lead_(lead), width_(width), height_(height), overhang_(overhang), at_(std::move(at)) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDWriteInlineObject)) {
            *out = static_cast<IDWriteInlineObject *>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (!left) delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE Draw(void *, IDWriteTextRenderer *, FLOAT x, FLOAT y, BOOL, BOOL, IUnknown *) override {
        *at_ = D2D1::Point2F(x + lead_, y);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS *m) override {
        *m = {lead_ + width_, height_, height_, FALSE};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS *o) override {
        *o = {overhang_, overhang_, overhang_, overhang_};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBreakConditions(DWRITE_BREAK_CONDITION *before, DWRITE_BREAK_CONDITION *after) override {
        *before = *after = DWRITE_BREAK_CONDITION_NEUTRAL;
        return S_OK;
    }

private:
    virtual ~Room() = default;
    std::atomic<ULONG> refs_{1};
    float lead_, width_, height_, overhang_;
    std::shared_ptr<D2D1_POINT_2F> at_;
};

// Bezier segments through `points`, from the first (where the figure already is) to the last: a
// Catmull-Rom curve, kept between `top` and `bottom` so that it never swings past the room.
void Through(ID2D1GeometrySink *sink, const std::vector<D2D1_POINT_2F> &points, bool backwards, float top, float bottom) {
    const size_t n = points.size();
    auto at = [&](ptrdiff_t i) {
        i = std::clamp<ptrdiff_t>(i, 0, (ptrdiff_t)n - 1);
        return points[backwards ? n - 1 - (size_t)i : (size_t)i];
    };
    for (ptrdiff_t i = 0; i + 1 < (ptrdiff_t)n; ++i) {
        const D2D1_POINT_2F p0 = at(i - 1), p1 = at(i), p2 = at(i + 1), p3 = at(i + 2);
        const D2D1_POINT_2F c1 = D2D1::Point2F(p1.x + (p2.x - p0.x) / 6.0f, std::clamp(p1.y + (p2.y - p0.y) / 6.0f, top, bottom));
        const D2D1_POINT_2F c2 = D2D1::Point2F(p2.x - (p3.x - p1.x) / 6.0f, std::clamp(p2.y - (p3.y - p1.y) / 6.0f, top, bottom));
        sink->AddBezier(D2D1::BezierSegment(c1, c2, p2));
    }
}

// A text that sits above or below its line's baseline (`rise`): DirectWrite has no baseline shift,
// so the text is laid out on its own and placed in the line like a character, as a bar is. Its box
// is the text's own, raised, so the line makes room for it; its ink is the text's ink.
class Raised : public IDWriteInlineObject {
public:
    Raised(ID2D1DeviceContext *dc, ComPtr<IDWriteTextLayout> layout, ComPtr<ID2D1Brush> brush, float lead, float rise)
        : dc_(dc), layout_(std::move(layout)), brush_(std::move(brush)), lead_(lead), rise_(rise) {
        DWRITE_TEXT_METRICS m{};
        DWRITE_LINE_METRICS lm{};
        UINT32 lines = 0;
        if (SUCCEEDED(layout_->GetMetrics(&m))) {
            width_ = m.widthIncludingTrailingWhitespace;
            height_ = m.height;
        }
        if (SUCCEEDED(layout_->GetLineMetrics(&lm, 1, &lines)) && lines) baseline_ = lm.baseline;
        // Overhangs are measured against the layout's box, so the box is made the text's own.
        layout_->SetMaxWidth(std::max(1.0f, width_));
        layout_->SetMaxHeight(std::max(1.0f, height_));
        layout_->GetOverhangMetrics(&ink_);
    }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void **out) override {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDWriteInlineObject)) {
            *out = static_cast<IDWriteInlineObject *>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG left = --refs_;
        if (!left) delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE Draw(void *, IDWriteTextRenderer *, FLOAT x, FLOAT y, BOOL, BOOL, IUnknown *) override {
        // The box's top is the text's top: its baseline lands `rise` above the line's.
        dc_->DrawTextLayout(D2D1::Point2F(x + lead_, y), layout_.Get(), brush_.Get());
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetMetrics(DWRITE_INLINE_OBJECT_METRICS *m) override {
        *m = {lead_ + width_, height_, baseline_ + rise_, FALSE};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetOverhangMetrics(DWRITE_OVERHANG_METRICS *o) override {
        *o = {ink_.left - lead_, ink_.top, ink_.right, ink_.bottom};
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetBreakConditions(DWRITE_BREAK_CONDITION *before, DWRITE_BREAK_CONDITION *after) override {
        *before = *after = DWRITE_BREAK_CONDITION_NEUTRAL;
        return S_OK;
    }

private:
    virtual ~Raised() = default;
    std::atomic<ULONG> refs_{1};
    ID2D1DeviceContext *dc_;
    ComPtr<IDWriteTextLayout> layout_;
    ComPtr<ID2D1Brush> brush_;
    float lead_, rise_;
    float width_ = 0.0f, height_ = 0.0f, baseline_ = 0.0f;
    DWRITE_OVERHANG_METRICS ink_{};
};
// The colour a gradient has at `x` (0 to 1): mixed between the stops either side of it.
D2D1_COLOR_F ColorAt(const std::vector<skin::Stop> &stops, float x) {
    if (stops.empty()) return D2D1::ColorF(D2D1::ColorF::White);
    if (x <= stops.front().at) return ColorOf(stops.front().color, 1.0f);
    for (size_t i = 1; i < stops.size(); ++i) {
        if (x > stops[i].at) continue;
        const D2D1_COLOR_F a = ColorOf(stops[i - 1].color, 1.0f), b = ColorOf(stops[i].color, 1.0f);
        const float span = stops[i].at - stops[i - 1].at, t = span > 0.0f ? (x - stops[i - 1].at) / span : 1.0f;
        return D2D1::ColorF(a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t);
    }
    return ColorOf(stops.back().color, 1.0f);
}

// To 1 and back: x as it goes 0..1, then 1..0 as it goes 1..2, and so on.
float Mirrored(float x) {
    x = std::fmod(x, 2.0f);
    if (x < 0.0f) x += 2.0f;
    return x > 1.0f ? 2.0f - x : x;
}

// A text's words: "{media.position}" and "{media.progress}" move every second while playing.
bool FollowsPlayback(const std::wstring &value) {
    return value.find(L"{media.position}") != std::wstring::npos || value.find(L"{media.progress}") != std::wstring::npos;
}

std::wstring Cased(const std::wstring &text, skin::Case textCase, const std::wstring &locale) {
    if (textCase == skin::Case::None || text.empty()) return text;
    const DWORD flags = textCase == skin::Case::Upper ? LCMAP_UPPERCASE : LCMAP_LOWERCASE;
    std::wstring out(text.size() * 2 + 8, L'\0');
    const int n = LCMapStringEx(locale.empty() ? LOCALE_NAME_USER_DEFAULT : locale.c_str(), flags, text.c_str(),
                                (int)text.size(), out.data(), (int)out.size(), nullptr, nullptr, 0);
    if (n <= 0) return text;
    out.resize((size_t)n);
    return out;
}

}  // namespace

float PanelShift(float lo, float hi, float limit) {
    const float size = hi - lo;
    if (size >= limit) return std::round((limit - size) / 2.0f - lo);  // centred
    // The near edge may lie anywhere from three quarters of the box before the display's start to
    // a quarter of the box before its end.
    const float least = -(1.0f - kPanelStaysOn) * size, most = limit - kPanelStaysOn * size;
    if (lo < least) return std::ceil(least - lo);
    if (lo > most) return std::floor(most - lo);
    return 0.0f;
}

bool SkinView::Init(ID2D1Device *device) {
    Release();
    HRESULT hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_);
    if (SUCCEEDED(hr))
        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown **>(write_.GetAddressOf()));
    for (size_t i = 0; i < 2 * kShadows && SUCCEEDED(hr); ++i) {  // for the text, then for the spectra
        ComPtr<ID2D1Effect> shadow;
        hr = dc_->CreateEffect(CLSID_D2D1Shadow, &shadow);
        shadows_.push_back(shadow);
    }
    if (FAILED(hr)) {
        ALOG(L"skin: Direct2D unavailable (0x%08X)", hr);
        Release();
        return false;
    }
    dc_->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    dc_->GetFactory(&factory_);
    if (factory_)
        factory_->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                                                D2D1_CAP_STYLE_ROUND, D2D1_LINE_JOIN_ROUND),
                                    nullptr, 0, &round_);
    return true;
}

void SkinView::Release() {
    slots_.clear();
    shadows_.clear();
    round_.Reset();
    factory_.Reset();
    write_.Reset();
    dc_.Reset();
    words_.clear();
    spectra_.clear();
    version_ = 0;
}

void SkinView::Set(const skin::Resolved &skin, const ClockStyle &style, const RegionalFormat &format) {
    skin_ = skin;
    style_ = style;
    format_ = FormatFor(format, style.locale);
    everySecond_ = false;
    spectra_.clear();
    for (size_t p = 0; p < skin_.panels.size(); ++p)
        for (size_t l = 0; l < skin_.panels[p].lines.size(); ++l) {
            const skin::Line &line = skin_.panels[p].lines[l];
            for (size_t t = 0; t < line.texts.size(); ++t) {
                everySecond_ = everySecond_ || FollowsPlayback(line.texts[t].value);
                if (line.texts[t].spectrum) spectra_.push_back({p, l, t, {}});
            }
        }
    words_.clear();
    ++version_;
    Tick();
}

void SkinView::SetData(const data::Snapshot &snapshot) {
    data_ = snapshot;
    ++dataVersion_;
}

spectrum::Shape SkinView::ShapeOf(const skin::Text &text) {
    const skin::Spectrum &a = text.audio;
    const int bands = std::clamp(a.bands, 1, 256);
    return {a.bass == skin::Bass::Middle ? (bands + 1) / 2 : bands, a.low, a.high, a.sensitivity, a.smoothing};
}

void SkinView::SetAudio(const spectrum::Frame &frame) {
    for (Audio &a : spectra_) a.bands.Follow(frame, ShapeOf(skin_.panels[a.panel].lines[a.line].texts[a.text]));
}

void SkinView::SettleAudio(const spectrum::Frame &frame) {
    for (Audio &a : spectra_) a.bands.Settle(frame, ShapeOf(skin_.panels[a.panel].lines[a.line].texts[a.text]));
}

bool SkinView::Moving() const {
    for (const Audio &a : spectra_)
        if (a.bands.Moving() || skin_.panels[a.panel].lines[a.line].texts[a.text].audio.flow > 0.0f) return true;
    return false;
}

bool SkinView::Tick() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return Tick(now);
}

bool SkinView::Tick(const SYSTEMTIME &now, uint64_t utc) {
    const bool playing = everySecond_ && data_.media.state == data::MediaState::Playing;
    if (!words_.empty() && dataShown_ == dataVersion_ && now.wMinute == shown_.wMinute && now.wHour == shown_.wHour &&
        now.wDay == shown_.wDay && now.wMonth == shown_.wMonth && now.wYear == shown_.wYear &&
        (!playing || now.wSecond == shown_.wSecond))
        return false;
    shown_ = now;
    dataShown_ = dataVersion_;
    if (!utc) {
        FILETIME ft{};
        GetSystemTimeAsFileTime(&ft);
        utc = (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
    }
    std::vector<std::vector<std::vector<std::wstring>>> words;
    for (const skin::Panel &p : skin_.panels) {
        auto &panel = words.emplace_back();
        const data::Context context{&data_, p.until, utc};
        auto holds = [&](const std::optional<data::Condition> &when) {
            return !when || Holds(*when, format_, style_, now, &context);
        };
        const bool panelShown = holds(p.when);
        for (const skin::Line &l : p.lines) {
            auto &line = panel.emplace_back();
            const bool lineShown = panelShown && l.shown && holds(l.when);
            for (const skin::Text &t : l.texts) {
                std::wstring w;
                if (lineShown && t.shown && holds(t.when)) {
                    double f = 0.0;
                    if (t.spectrum) w = L"~";  // shown; what it shows is drawn every frame
                    else if (!t.bar) w = Cased(FillText(t.value, format_, style_, now, &context), t.textCase, format_.locale);
                    else if (t.value.size() > 2 && data::FractionOf(t.value.substr(1, t.value.size() - 2), context, &f))
                        w = std::to_wstring((int)std::lround(f * 10000.0));  // in ten-thousandths
                }
                line.push_back(std::move(w));
            }
        }
    }
    if (words == words_) return false;
    words_ = std::move(words);
    ++version_;
    return true;
}

std::wstring SkinView::Family(const std::wstring &wanted) const {
    if (!wanted.empty() && HasFamily(write_.Get(), wanted)) return wanted;
    // The icon fonts of Windows 11 and Windows 10 put the same icons at the same code points.
    if (wanted == L"Segoe Fluent Icons" && HasFamily(write_.Get(), L"Segoe MDL2 Assets")) return L"Segoe MDL2 Assets";
    if (wanted == L"Segoe MDL2 Assets" && HasFamily(write_.Get(), L"Segoe Fluent Icons")) return L"Segoe Fluent Icons";
    return HasFamily(write_.Get(), L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
}

void SkinView::Figures(IDWriteTextLayout *layout, const skin::Text &text, DWRITE_TEXT_RANGE range) const {
    if (!text.tabular) return;
    ComPtr<IDWriteTypography> typography;
    if (FAILED(write_->CreateTypography(&typography))) return;
    typography->AddFontFeature({DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1});
    layout->SetTypography(typography.Get(), range);
}

ComPtr<IDWriteTextLayout> SkinView::Alone(const skin::Text &t, const std::wstring &words, float px) const {
    ComPtr<IDWriteTextFormat> format;
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(write_->CreateTextFormat(Family(t.font).c_str(), nullptr, (DWRITE_FONT_WEIGHT)t.weight,
                                        t.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, px * t.size, format_.locale.c_str(), &format)) ||
        FAILED(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP)) ||
        FAILED(write_->CreateTextLayout(words.c_str(), (UINT32)words.size(), format.Get(), kUnbounded, kUnbounded,
                                        &layout)))
        return nullptr;
    const DWRITE_TEXT_RANGE all{0, (UINT32)words.size()};
    ComPtr<IDWriteTextLayout1> spaced;
    if (t.tracking != 0.0f && SUCCEEDED(layout.As(&spaced)))
        spaced->SetCharacterSpacing(0.0f, t.tracking * px * t.size, 0.0f, all);
    Figures(layout.Get(), t, all);
    return layout;
}

std::wstring SkinView::Fit(const skin::Text &t, const std::wstring &words, float px) const {
    if (t.max <= 0.0f || words.empty()) return words;
    const float room = t.max * px;
    auto width = [&](const std::wstring &s) {
        DWRITE_TEXT_METRICS m{};
        const ComPtr<IDWriteTextLayout> layout = Alone(t, s, px);
        return layout && SUCCEEDED(layout->GetMetrics(&m)) ? m.widthIncludingTrailingWhitespace : 0.0f;
    };
    if (width(words) <= room) return words;
    // The longest beginning that fits with the ellipsis after it, never ending on half a pair or
    // on a space.
    auto cut = [&](size_t n) {
        if (n && words[n - 1] >= 0xD800 && words[n - 1] <= 0xDBFF) --n;
        std::wstring s = words.substr(0, n);
        while (!s.empty() && s.back() == L' ') s.pop_back();
        return s + L"…";
    };
    size_t lo = 0, hi = words.size();
    while (lo < hi) {
        const size_t mid = (lo + hi + 1) / 2;
        if (width(cut(mid)) <= room) lo = mid;
        else hi = mid - 1;
    }
    return cut(lo);
}

bool SkinView::Lay(Line &line, const skin::Panel &, const skin::Line &source,
                   const std::vector<std::wstring> &given, float px) {
    struct Run {
        const skin::Text *text;
        UINT32 start, length;
        float lead;  // the space before it: none before the first thing the line draws
    };
    // A text may be cut short to its widest; one that rises is laid out apart and held in the
    // line, like a bar, by one character.
    std::vector<std::wstring> words(given.size());
    for (size_t i = 0; i < source.texts.size() && i < given.size(); ++i)
        words[i] = source.texts[i].bar || source.texts[i].spectrum ? given[i] : Fit(source.texts[i], given[i], px);
    auto apart = [](const skin::Text &t) { return t.bar || t.spectrum || t.rise != 0.0f; };
    std::wstring s;
    std::vector<Run> runs;
    float largest = 0.0f;  // the largest text beside a bar, which it is centred on
    for (size_t i = 0; i < source.texts.size() && i < words.size(); ++i) {
        if (words[i].empty()) continue;
        const skin::Text &t = source.texts[i];
        runs.push_back({&t, (UINT32)s.size(), apart(t) ? 1u : (UINT32)words[i].size(), runs.empty() ? 0.0f : t.space * px});
        s += apart(t) ? std::wstring(1, (wchar_t)0xFFFC) : words[i];
        if (!t.bar && !t.spectrum) largest = std::max(largest, t.size);
    }
    line.layout.Reset();
    line.runs.clear();
    line.rooms.clear();
    if (runs.empty()) return true;  // nothing to say on this line
    for (const Run &r : runs) line.runs.push_back({(size_t)(r.text - source.texts.data()), {r.start, r.length}});

    // The layout's own font is the first text's; a bar or a spectrum has none.
    const auto firstText =
        std::find_if(runs.begin(), runs.end(), [](const Run &r) { return !r.text->bar && !r.text->spectrum; });
    const skin::Text &first = firstText != runs.end() ? *firstText->text : *runs.front().text;
    const std::wstring &locale = format_.locale;
    ComPtr<IDWriteTextFormat> format;
    if (FAILED(write_->CreateTextFormat(Family(first.font).c_str(), nullptr, (DWRITE_FONT_WEIGHT)first.weight,
                                        first.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                        DWRITE_FONT_STRETCH_NORMAL, px * first.size, locale.c_str(), &format)) ||
        FAILED(format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP)) ||
        FAILED(write_->CreateTextLayout(s.c_str(), (UINT32)s.size(), format.Get(), kUnbounded, kUnbounded,
                                        &line.layout)))
        return false;
    ComPtr<IDWriteTextLayout1> spaced;
    line.layout.As(&spaced);
    for (const Run &r : runs) {
        const skin::Text &t = *r.text;
        const DWRITE_TEXT_RANGE range{r.start, r.length};
        if (t.spectrum) {
            const float overhang = t.audio.mode == skin::Mode::Bars ? 0.0f : std::ceil(t.audio.stroke * px / 2.0f) + 1.0f;
            auto at = std::make_shared<D2D1_POINT_2F>(D2D1::Point2F(-kUnbounded, -kUnbounded));
            ComPtr<IDWriteInlineObject> room;
            room.Attach(new Room(r.lead, std::max(1.0f, t.length * px), std::max(1.0f, t.audio.height * px), overhang, at));
            line.layout->SetInlineObject(room.Get(), range);
            line.rooms.push_back({(size_t)(r.text - source.texts.data()), std::move(at)});
            continue;
        }
        if (t.bar) {
            const size_t index = (size_t)(r.text - source.texts.data());
            const float fraction = std::clamp((float)std::wcstod(words[index].c_str(), nullptr) / 10000.0f, 0.0f, 1.0f);
            // About the middle of the figures beside it.
            const float axis = 0.35f * px * (largest > 0.0f ? largest : 1.0f);
            ComPtr<IDWriteInlineObject> bar;
            bar.Attach(new Bar(dc_.Get(), t, px, r.lead, axis, fraction));
            line.layout->SetInlineObject(bar.Get(), range);
            continue;
        }
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc_->CreateSolidColorBrush(ColorOf(t.color, t.opacity), &brush))) return false;
        if (t.rise != 0.0f) {
            const size_t index = (size_t)(r.text - source.texts.data());
            const ComPtr<IDWriteTextLayout> own = Alone(t, words[index], px);
            if (!own) return false;
            ComPtr<IDWriteInlineObject> raised;
            raised.Attach(new Raised(dc_.Get(), own, brush, r.lead, t.rise * px));
            line.layout->SetInlineObject(raised.Get(), range);
            continue;
        }
        line.layout->SetFontFamilyName(Family(t.font).c_str(), range);
        line.layout->SetFontWeight((DWRITE_FONT_WEIGHT)t.weight, range);
        line.layout->SetFontStyle(t.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, range);
        line.layout->SetFontSize(px * t.size, range);
        line.layout->SetDrawingEffect(brush.Get(), range);
        Figures(line.layout.Get(), t, range);
        if (spaced) {
            // Tracking is in the text's own em; the space before it in the panel's.
            const float tracking = t.tracking * px * t.size;
            if (tracking != 0.0f) spaced->SetCharacterSpacing(0.0f, tracking, 0.0f, range);
            if (r.lead != 0.0f) spaced->SetCharacterSpacing(r.lead, tracking, 0.0f, {r.start, 1});
        }
    }

    DWRITE_LINE_METRICS lm{};
    UINT32 lines = 0;
    DWRITE_OVERHANG_METRICS ink{};
    if (FAILED(line.layout->GetLineMetrics(&lm, 1, &lines)) || FAILED(line.layout->GetOverhangMetrics(&ink)))
        return false;
    line.baseline = lm.baseline;
    line.inkLeft = -ink.left;
    line.inkRight = kUnbounded + ink.right;
    line.inkTop = -ink.top;
    line.inkBottom = kUnbounded + ink.bottom;
    if (line.inkRight <= line.inkLeft || line.inkBottom <= line.inkTop) {  // only spaces
        line.layout.Reset();
        return true;
    }

    // Texts that hang -- an AM/PM marker beside the numbers -- are left out of the alignment.
    line.coreLeft = line.inkLeft;
    line.coreRight = line.inkRight;
    const auto firstCore = std::find_if(runs.begin(), runs.end(), [](const Run &r) { return !r.text->hang; });
    const auto lastCore = std::find_if(runs.rbegin(), runs.rend(), [](const Run &r) { return !r.text->hang; });
    if (firstCore != runs.end() && (runs.front().text->hang || runs.back().text->hang)) {
        const UINT32 start = firstCore->start, end = lastCore->start + lastCore->length;
        DWRITE_HIT_TEST_METRICS hit{};
        UINT32 count = 0;
        if (SUCCEEDED(line.layout->HitTestTextRange(start, end - start, 0.0f, 0.0f, &hit, 1, &count)) && count == 1) {
            // The range starts with the space before its first text, which belongs to the gap
            // after what hangs, not to what is aligned.
            if (runs.front().text->hang) line.coreLeft = hit.left + firstCore->lead;
            if (runs.back().text->hang) line.coreRight = hit.left + hit.width;
        }
    }
    return true;
}

bool SkinView::Build(Built &built, size_t index, UINT width, UINT height) {
    built = Built{};
    const skin::Panel &panel = skin_.panels[index];
    const auto &words = words_[index];
    const int column = panel.anchor % 3, row = panel.anchor / 3;
    const int alignColumn = panel.align == skin::Align::Auto     ? column
                            : panel.align == skin::Align::Left   ? 0
                            : panel.align == skin::Align::Center ? 1
                                                                 : 2;
    const float w = (float)width, h = (float)height;
    const float margin = panel.margin / 100.0f * std::min(w, h);

    float px = h * panel.size / 100.0f;
    std::vector<Line> lines(panel.lines.size());
    bool any = false;
    for (int pass = 0; pass < 3; ++pass) {
        float widest = 0.0f;
        any = false;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (!Lay(lines[i], panel, panel.lines[i], words[i], px)) return false;
            if (!lines[i].layout) continue;
            any = true;
            widest = std::max(widest, lines[i].inkRight - lines[i].inkLeft);
        }
        // Narrow or portrait displays: shrink until the panel fits between the margins.
        const float room = w - 2.0f * margin;
        if (widest <= room || room <= 0.0f) break;
        px *= room / widest;
    }
    if (!any) return true;  // nothing to draw

    // Vertical positions from the first line's baseline. Each further line starts its ink
    // `gap` below the baseline above it.
    std::vector<float> baselines(lines.size(), 0.0f);
    float top = 0.0f, bottom = 0.0f, lastBaseline = 0.0f, previous = 0.0f;
    bool first = true;
    for (size_t i = 0; i < lines.size(); ++i) {
        const Line &l = lines[i];
        if (!l.layout) continue;
        baselines[i] = first ? 0.0f : previous + panel.lines[i].gap * px + (l.baseline - l.inkTop);
        if (first) top = l.inkTop - l.baseline;
        bottom = std::max(bottom, baselines[i] + l.inkBottom - l.baseline);
        previous = lastBaseline = baselines[i];
        first = false;
    }

    // Each line is placed by a reference point: its left ink edge, its centre or its right edge.
    auto ref = [&](const Line &l) {
        return alignColumn == 0 ? l.coreLeft : alignColumn == 1 ? (l.coreLeft + l.coreRight) / 2.0f : l.coreRight;
    };
    float before = 0.0f, after = 0.0f;
    for (const Line &l : lines) {
        if (!l.layout) continue;
        before = std::max(before, ref(l) - l.inkLeft);
        after = std::max(after, l.inkRight - ref(l));
    }
    float reach = 0.05f;
    for (const skin::Shadow &s : panel.shadows)
        reach = std::max(reach, s.blur * 3.0f + std::max(std::fabs(s.x), std::fabs(s.y)));
    const float pad = std::ceil(px * reach) + 2.0f;

    const D2D1_SIZE_U size =
        D2D1::SizeU((UINT32)std::ceil(before + after + 2 * pad), (UINT32)std::ceil(bottom - top + 2 * pad));
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc_->CreateBitmap(size, nullptr, 0, &props, &built.text)) ||
        FAILED(dc_->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush)))
        return false;
    const float refX = std::round(pad + before), originY = std::round(pad - top);
    std::vector<D2D1_POINT_2F> origins(lines.size());
    dc_->SetTarget(built.text.Get());
    dc_->BeginDraw();
    dc_->Clear(D2D1::ColorF(0, 0, 0, 0));
    for (size_t i = 0; i < lines.size(); ++i) {
        const Line &l = lines[i];
        if (!l.layout) continue;
        origins[i] = D2D1::Point2F(std::round(refX - ref(l)), std::round(originY + baselines[i]) - l.baseline);
        dc_->DrawTextLayout(origins[i], l.layout.Get(), brush.Get());
    }
    const HRESULT hr = dc_->EndDraw();
    dc_->SetTarget(nullptr);
    if (FAILED(hr)) return false;

    // The spectra's rooms, now that drawing the lines has put them somewhere, and a bitmap the
    // size of the text's to draw their bands into each frame.
    for (size_t i = 0; i < lines.size(); ++i)
        for (const Line::Room &room : lines[i].rooms) {
            if (room.at->x <= -kUnbounded) continue;  // not drawn
            const skin::Text &t = panel.lines[i].texts[room.text];
            Built::Spectrum place;
            for (size_t k = 0; k < spectra_.size(); ++k)
                if (spectra_[k].panel == index && spectra_[k].line == i && spectra_[k].text == room.text) place.audio = k;
            place.line = i;
            place.text = room.text;
            place.box = D2D1::RectF(room.at->x, room.at->y, room.at->x + std::max(1.0f, t.length * px),
                                    room.at->y + std::max(1.0f, t.audio.height * px));
            if (Brushes(place, t)) built.spectra.push_back(std::move(place));
        }
    if (!built.spectra.empty() && FAILED(dc_->CreateBitmap(size, nullptr, 0, &props, &built.live))) {
        built.spectra.clear();
        built.live.Reset();
    }

    const float anchorX = (column == 0 ? margin : column == 1 ? w / 2.0f : w - margin) + panel.offsetX / 100.0f * w;
    const float anchorY = (row == 0 ? margin : row == 1 ? h / 2.0f : h - margin) + panel.offsetY / 100.0f * h;
    const float refY = row == 0 ? top : row == 1 ? (top + lastBaseline) / 2.0f : lastBaseline;
    built.at = D2D1::Point2F(std::floor(anchorX - refX), std::floor(anchorY - (originY + refY)));
    // The panel may hang past the edges of its display, cut off there, but no further than
    // PanelShift allows (a drag that went too far, or a theme made on a display of another shape).
    built.at.x += PanelShift(built.at.x + refX - before, built.at.x + refX + after, w);
    built.at.y += PanelShift(built.at.y + originY + top, built.at.y + originY + bottom, h);
    built.px = px;

    const std::wstring pk = L"p" + std::to_wstring(index + 1);
    Bound whole{pk, D2D1::RectF(built.at.x + refX - before, built.at.y + originY + top, built.at.x + refX + after,
                                built.at.y + originY + bottom)};
    whole.holds = true;
    for (int c = 0; c < 3; ++c) {
        if (panel.align != skin::Align::Auto) {
            whole.holdX[c] = built.at.x + refX;  // the lines keep their alignment at every anchor
            continue;
        }
        // How far the ink reaches either side of the point the lines would be aligned by.
        float left = 0.0f, right = 0.0f;
        for (const Line &l : lines) {
            if (!l.layout) continue;
            const float r = c == 0 ? l.coreLeft : c == 1 ? (l.coreLeft + l.coreRight) / 2.0f : l.coreRight;
            left = std::max(left, r - l.inkLeft);
            right = std::max(right, l.inkRight - r);
        }
        const D2D1_RECT_F &b = whole.rect;
        whole.holdX[c] = c == 0 ? b.left + left : c == 2 ? b.right - right : (b.left + b.right) / 2.0f + (left - right) / 2.0f;
    }
    whole.holdY[0] = built.at.y + originY + top;
    whole.holdY[1] = built.at.y + originY + (top + lastBaseline) / 2.0f;
    whole.holdY[2] = built.at.y + originY + lastBaseline;
    built.bounds.push_back(whole);
    for (size_t i = 0; i < lines.size(); ++i) {
        const Line &l = lines[i];
        if (!l.layout) continue;
        for (const Line::Run &r : l.runs) {
            const auto place = std::find_if(built.spectra.begin(), built.spectra.end(), [&](const Built::Spectrum &s) {
                return s.line == i && s.text == r.text;
            });
            if (place != built.spectra.end()) {  // its room, rather than the line's height
                built.bounds.push_back({pk + L".l" + std::to_wstring(i + 1) + L"." + panel.lines[i].texts[r.text].name,
                                        D2D1::RectF(built.at.x + place->box.left, built.at.y + place->box.top,
                                                    built.at.x + place->box.right, built.at.y + place->box.bottom)});
                continue;
            }
            DWRITE_HIT_TEST_METRICS hit{};
            UINT32 count = 0;
            if (FAILED(l.layout->HitTestTextRange(r.range.startPosition, r.range.length, 0.0f, 0.0f, &hit, 1, &count)) ||
                !count)
                continue;
            const float x = built.at.x + origins[i].x + hit.left, y = built.at.y + origins[i].y + hit.top;
            built.bounds.push_back({pk + L".l" + std::to_wstring(i + 1) + L"." + panel.lines[i].texts[r.text].name,
                                    D2D1::RectF(x, y, x + hit.width, y + hit.height)});
        }
    }

    if (panel.backdrop > 0.0f && panel.backdropShape == skin::Shape::Plate) {
        // A rounded rectangle around the ink, as far out from it as the padding says.
        const float padX = std::round(panel.backdropPaddingX * px), padY = std::round(panel.backdropPaddingY * px);
        const D2D1_RECT_F rect = D2D1::RectF(built.at.x + refX - before - padX, built.at.y + originY + top - padY,
                                             built.at.x + refX + after + padX, built.at.y + originY + bottom + padY);
        const float radius = std::min({panel.backdropRadius * px, (rect.right - rect.left) / 2.0f, (rect.bottom - rect.top) / 2.0f});
        built.plate = true;
        built.plateArea = D2D1::RoundedRect(rect, radius, radius);
        ComPtr<ID2D1SolidColorBrush> fill;
        if (SUCCEEDED(dc_->CreateSolidColorBrush(ColorOf(panel.backdropColor, panel.backdrop), &fill))) built.backdrop = fill;
    } else if (panel.backdrop > 0.0f) {
        // An ellipse around the ink, fading to nothing well before its edge.
        const float left = built.at.x + refX - before, right = built.at.x + refX + after;
        const float upper = built.at.y + originY + top, lower = built.at.y + originY + bottom;
        const D2D1_POINT_2F centre = D2D1::Point2F((left + right) / 2.0f, (upper + lower) / 2.0f);
        built.backdropArea = D2D1::Ellipse(centre, (right - left) * 0.8f + 1.5f * px, (lower - upper) * 0.9f + 1.5f * px);
        // Roughly a Gaussian, so that no ring shows where it ends.
        const D2D1_COLOR_F c = ColorOf(panel.backdropColor, panel.backdrop);
        const float falloff[][2] = {{0.0f, 1.0f}, {0.25f, 0.9f}, {0.5f, 0.6f}, {0.7f, 0.3f}, {0.85f, 0.1f}, {1.0f, 0.0f}};
        D2D1_GRADIENT_STOP stops[ARRAYSIZE(falloff)];
        for (size_t k = 0; k < ARRAYSIZE(falloff); ++k)
            stops[k] = {falloff[k][0], D2D1::ColorF(c.r, c.g, c.b, c.a * falloff[k][1])};
        ComPtr<ID2D1GradientStopCollection> gradient;
        ComPtr<ID2D1RadialGradientBrush> glow;
        if (SUCCEEDED(dc_->CreateGradientStopCollection(stops, ARRAYSIZE(stops), &gradient)) &&
            SUCCEEDED(dc_->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(centre, D2D1::Point2F(), built.backdropArea.radiusX,
                                                    built.backdropArea.radiusY),
                gradient.Get(), &glow)))
            built.backdrop = glow;
    }
    return true;
}

bool SkinView::Brushes(Built::Spectrum &place, const skin::Text &text) {
    const skin::Spectrum &a = text.audio;
    const D2D1_RECT_F &box = place.box;
    const float middle = (box.top + box.bottom) / 2.0f;
    // Along the gradient: left to right, or from where the bands start toward where they reach --
    // up from the bottom, down from the top, or out from the middle. Beyond its ends the gradient
    // comes back mirrored: both ways from the middle then look alike, and colours that flow along
    // it never jump. A line or an area has no bars to colour by their height, so is coloured as
    // vertical.
    if (a.gradient == skin::Gradient::Horizontal) {
        place.start = D2D1::Point2F(box.left, middle);
        place.end = D2D1::Point2F(box.right, middle);
    } else if (a.direction == skin::Direction::Down) {
        place.start = D2D1::Point2F(box.left, box.top);
        place.end = D2D1::Point2F(box.left, box.bottom);
    } else if (a.direction == skin::Direction::Both) {
        place.start = D2D1::Point2F(box.left, middle);
        place.end = D2D1::Point2F(box.left, box.top);
    } else {
        place.start = D2D1::Point2F(box.left, box.bottom);
        place.end = D2D1::Point2F(box.left, box.top);
    }
    // Bars coloured by their height take one plain colour each, set as they are drawn.
    place.linear = a.stops.size() > 1 && !(a.gradient == skin::Gradient::Level && a.mode == skin::Mode::Bars);
    std::vector<D2D1_GRADIENT_STOP> stops;
    for (const skin::Stop &s : a.stops) stops.push_back({s.at, ColorOf(s.color, 1.0f)});
    auto make = [&](ComPtr<ID2D1Brush> *out) {
        if (!place.linear) {
            ComPtr<ID2D1SolidColorBrush> solid;
            if (FAILED(dc_->CreateSolidColorBrush(stops.front().color, &solid))) return false;
            *out = solid;
            return true;
        }
        ComPtr<ID2D1GradientStopCollection> collection;
        ComPtr<ID2D1LinearGradientBrush> linear;
        if (FAILED(dc_->CreateGradientStopCollection(stops.data(), (UINT32)stops.size(), D2D1_GAMMA_2_2,
                                                     D2D1_EXTEND_MODE_MIRROR, &collection)) ||
            FAILED(dc_->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(place.start, place.end),
                                                  collection.Get(), &linear)))
            return false;
        *out = linear;
        return true;
    };
    return !stops.empty() && make(&place.fill) && make(&place.peak);
}

void SkinView::DrawSpectrum(const Built::Spectrum &place, const skin::Text &text, float px) {
    const skin::Spectrum &a = text.audio;
    const spectrum::Bands &bands = spectra_[place.audio].bands;
    // While nothing is heard it fades to its idle opacity.
    const float idle = std::clamp(a.idleOpacity, 0.0f, 1.0f);
    const float alpha = std::clamp(text.opacity, 0.0f, 1.0f) * (idle + (1.0f - idle) * bands.presence());
    if (alpha <= 0.0f) return;
    const int n = std::clamp(a.bands, 1, 256);
    const std::vector<float> &levels = bands.levels(), &peaks = bands.peaks();
    // The band shown at position j: in order from the left, reversed with the bass at the right,
    // and with the bass in the middle each band twice, mirrored about it.
    auto bandAt = [&](int j) {
        if (a.bass == skin::Bass::Right) return (size_t)(n - 1 - j);
        if (a.bass == skin::Bass::Middle) return (size_t)(std::abs(2 * j - (n - 1)) / 2);
        return (size_t)j;
    };
    // How far a band reaches, as a share of the height: its rest, and above it its level.
    const float rest = std::clamp(a.floor, 0.0f, 1.0f);
    auto reach = [&](const std::vector<float> &of, size_t k) {
        return rest + (1.0f - rest) * std::clamp(k < of.size() ? of[k] : 0.0f, 0.0f, 1.0f);
    };
    const D2D1_RECT_F &box = place.box;
    const float width = box.right - box.left, height = box.bottom - box.top, slot = width / (float)n;
    const float middle = (box.top + box.bottom) / 2.0f;
    // Where a band reaching `share` of the height spans, top to bottom.
    auto span = [&](float share, float *top, float *bottom) {
        const float h = share * height;
        if (a.direction == skin::Direction::Down) *top = box.top, *bottom = box.top + h;
        else if (a.direction == skin::Direction::Both) *top = middle - h / 2.0f, *bottom = middle + h / 2.0f;
        else *top = box.bottom - h, *bottom = box.bottom;
    };
    place.fill->SetOpacity(alpha);
    // Flowing colours: the gradient slides along itself, to its end and (mirrored) back, once in
    // `flow` seconds.
    const float phase = a.flow > 0.0f ? (float)std::fmod((double)GetTickCount64() / 1000.0, (double)a.flow) / a.flow : 0.0f;
    if (place.linear)
        for (ID2D1Brush *brush : {place.fill.Get(), place.peak.Get()}) {
            ComPtr<ID2D1LinearGradientBrush> linear;
            if (!brush || FAILED(brush->QueryInterface(IID_PPV_ARGS(&linear)))) continue;
            const float dx = (place.end.x - place.start.x) * 2.0f * phase, dy = (place.end.y - place.start.y) * 2.0f * phase;
            linear->SetStartPoint(D2D1::Point2F(place.start.x + dx, place.start.y + dy));
            linear->SetEndPoint(D2D1::Point2F(place.end.x + dx, place.end.y + dy));
        }
    // Bars coloured by their height: the colour the gradient has that far along (moved by the flow).
    const bool byHeight = a.gradient == skin::Gradient::Level && a.stops.size() > 1;
    auto paint = [&](ID2D1Brush *brush, float share) {
        ComPtr<ID2D1SolidColorBrush> solid;
        if (byHeight && SUCCEEDED(brush->QueryInterface(IID_PPV_ARGS(&solid))))
            solid->SetColor(ColorAt(a.stops, Mirrored(share + 2.0f * phase)));
        return brush;
    };

    if (a.mode == skin::Mode::Bars) {
        const float bar = std::max(1.0f, slot * (1.0f - std::clamp(a.gap, 0.0f, 0.95f)));
        const float corner = std::clamp(text.radius, 0.0f, 0.5f) * bar;
        for (int j = 0; j < n; ++j) {
            const float x = box.left + (float)j * slot + (slot - bar) / 2.0f;
            const float share = reach(levels, bandAt(j));
            float top = 0.0f, bottom = 0.0f;
            span(share, &top, &bottom);
            const float r = std::min(corner, (bottom - top) / 2.0f);
            dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, top, x + bar, bottom), r, r), paint(place.fill.Get(), share));
        }
        if (a.peak <= 0.0f || !place.peak) return;
        // A short mark just beyond each bar's highest recent reach, kept inside the room.
        place.peak->SetOpacity(alpha * std::clamp(a.peak, 0.0f, 1.0f));
        const float mark = std::max(1.5f, std::min(bar * 0.45f, height * 0.05f)), lift = std::max(1.0f, mark * 0.8f);
        const float r = std::min(corner, mark / 2.0f);
        auto draw = [&](float x, float y0, ID2D1Brush *brush) {
            y0 = std::clamp(y0, box.top, box.bottom - mark);
            dc_->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(x, y0, x + bar, y0 + mark), r, r), brush);
        };
        for (int j = 0; j < n; ++j) {
            const float x = box.left + (float)j * slot + (slot - bar) / 2.0f;
            const float share = reach(peaks, bandAt(j));
            float top = 0.0f, bottom = 0.0f;
            span(share, &top, &bottom);
            ID2D1Brush *brush = paint(place.peak.Get(), share);
            if (a.direction != skin::Direction::Down) draw(x, top - lift - mark, brush);
            if (a.direction != skin::Direction::Up) draw(x, bottom + lift, brush);
        }
        return;
    }

    // A line through the bands' tops (and, both ways, their bottoms), flat to the room's ends.
    if (!factory_) return;
    std::vector<D2D1_POINT_2F> upper((size_t)n + 2), lower((size_t)n + 2);
    for (int j = 0; j < n; ++j) {
        float top = 0.0f, bottom = 0.0f;
        span(reach(levels, bandAt(j)), &top, &bottom);
        const float x = box.left + ((float)j + 0.5f) * slot;
        upper[(size_t)j + 1] = D2D1::Point2F(x, a.direction == skin::Direction::Down ? bottom : top);
        lower[(size_t)j + 1] = D2D1::Point2F(x, bottom);
    }
    for (std::vector<D2D1_POINT_2F> *edge : {&upper, &lower}) {
        (*edge)[0] = D2D1::Point2F(box.left, (*edge)[1].y);
        edge->back() = D2D1::Point2F(box.right, (*edge)[(size_t)n].y);
    }
    const bool both = a.direction == skin::Direction::Both, area = a.mode == skin::Mode::Area;
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(factory_->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return;
    if (area) {
        // The area between the line and where the bands start, or between the two lines.
        const float base = a.direction == skin::Direction::Down ? box.top : box.bottom;
        if (both) {
            sink->BeginFigure(upper.front(), D2D1_FIGURE_BEGIN_FILLED);
            Through(sink.Get(), upper, false, box.top, box.bottom);
            sink->AddLine(lower.back());
            Through(sink.Get(), lower, true, box.top, box.bottom);
        } else {
            sink->BeginFigure(D2D1::Point2F(box.left, base), D2D1_FIGURE_BEGIN_FILLED);
            sink->AddLine(upper.front());
            Through(sink.Get(), upper, false, box.top, box.bottom);
            sink->AddLine(D2D1::Point2F(box.right, base));
        }
        sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    } else {
        for (const std::vector<D2D1_POINT_2F> *edge : {&upper, &lower}) {
            if (edge == &lower && !both) break;
            sink->BeginFigure(edge->front(), D2D1_FIGURE_BEGIN_HOLLOW);
            Through(sink.Get(), *edge, false, box.top, box.bottom);
            sink->EndFigure(D2D1_FIGURE_END_OPEN);
        }
    }
    if (FAILED(sink->Close())) return;
    if (area) dc_->FillGeometry(path.Get(), place.fill.Get());
    else dc_->DrawGeometry(path.Get(), place.fill.Get(), std::max(1.0f, a.stroke * px), round_.Get());
}

std::vector<SkinView::Bound> SkinView::Bounds(size_t slot) const {
    std::vector<Bound> out;
    if (slot < slots_.size())
        for (const Built &b : slots_[slot].panels) out.insert(out.end(), b.bounds.begin(), b.bounds.end());
    return out;
}

namespace {

// How far in a panel is, 0 to 1, `seconds` after the wallpaper came up.
float Arrived(const skin::Panel &panel, float seconds) {
    if (seconds < 0.0f) return 1.0f;
    const float t = seconds - panel.enterDelay;
    if (panel.enter == skin::Enter::None || panel.enterDuration <= 0.0f) return t >= 0.0f ? 1.0f : 0.0f;
    return std::clamp(t / panel.enterDuration, 0.0f, 1.0f);
}

}  // namespace

bool SkinView::Entering() const {
    if (entrance_ < 0.0f) return false;
    for (const skin::Panel &p : skin_.panels)
        if (Arrived(p, entrance_) < 1.0f) return true;
    return false;
}

D2D1_RECT_F SkinView::AreaOf(const Built &b, const skin::Panel &panel) const {
    D2D1_RECT_F area = D2D1::RectF(FLT_MAX, FLT_MAX, -FLT_MAX, -FLT_MAX);
    auto add = [&](float left, float top, float right, float bottom) {
        area = D2D1::RectF(std::min(area.left, left), std::min(area.top, top), std::max(area.right, right),
                           std::max(area.bottom, bottom));
    };
    const D2D1_SIZE_U size = b.text->GetPixelSize();
    add(b.at.x, b.at.y, b.at.x + (float)size.width, b.at.y + (float)size.height);
    for (const skin::Shadow &sh : panel.shadows) {
        const float x = b.at.x + std::round(sh.x * b.px), y = b.at.y + std::round(sh.y * b.px);
        const float blur = std::ceil(3.0f * std::max(0.5f, sh.blur * b.px)) + 1.0f;
        add(x - blur, y - blur, x + (float)size.width + blur, y + (float)size.height + blur);
    }
    if (b.backdrop && b.plate) {
        const D2D1_RECT_F &r = b.plateArea.rect;
        add(r.left, r.top, r.right, r.bottom);
    } else if (b.backdrop) {
        const D2D1_ELLIPSE &e = b.backdropArea;
        add(e.point.x - e.radiusX, e.point.y - e.radiusY, e.point.x + e.radiusX, e.point.y + e.radiusY);
    }
    return area;
}

bool SkinView::Reach(size_t slot, D2D1_RECT_F *area) const {
    if (slot >= slots_.size()) return false;
    const Slot &s = slots_[slot];
    bool any = false;
    for (size_t i = 0; i < s.panels.size() && i < skin_.panels.size(); ++i) {
        if (!s.panels[i].text) continue;
        const D2D1_RECT_F a = AreaOf(s.panels[i], skin_.panels[i]);
        *area = any ? D2D1::RectF(std::min(area->left, a.left), std::min(area->top, a.top), std::max(area->right, a.right),
                                  std::max(area->bottom, a.bottom))
                    : a;
        any = true;
    }
    return any;
}

bool SkinView::Draw(ID2D1Bitmap1 *target, UINT width, UINT height, size_t slot, float opacity) {
    if (!dc_) return false;
    if (slot >= slots_.size()) slots_.resize(slot + 1);
    Slot &s = slots_[slot];
    if (s.version != version_ || s.width != width || s.height != height) {
        s.panels.assign(skin_.panels.size(), Built{});
        for (size_t i = 0; i < skin_.panels.size(); ++i) {
            if (!Build(s.panels[i], i, width, height)) {
                ALOG(L"skin: could not lay panel %zu out", i);
                s.panels[i] = Built{};
            }
        }
        s.version = version_;
        s.width = width;
        s.height = height;
    }
    // The spectra's bands as they are now, each panel's into its own bitmap, so that the panel's
    // shadows and its way in apply to them as to its text.
    for (size_t i = 0; i < s.panels.size(); ++i) {
        const Built &b = s.panels[i];
        if (!b.live) continue;
        dc_->SetTarget(b.live.Get());
        dc_->BeginDraw();
        dc_->Clear(D2D1::ColorF(0, 0, 0, 0));
        for (const Built::Spectrum &place : b.spectra)
            DrawSpectrum(place, skin_.panels[i].lines[place.line].texts[place.text], b.px);
        if (FAILED(dc_->EndDraw())) ALOG(L"skin: could not draw the spectrum of panel %zu", i);
    }
    dc_->SetTarget(target);
    dc_->BeginDraw();
    // A layer covers only what a panel can touch: one the size of the target costs a full-screen
    // blend per frame while a panel fades.
    auto areaOf = [&](size_t i) { return AreaOf(s.panels[i], skin_.panels[i]); };
    for (size_t i = 0; i < s.panels.size(); ++i) {
        const Built &b = s.panels[i];
        if (!b.text) continue;
        const skin::Panel &panel = skin_.panels[i];
        // On its way in: faded (smoothly), and for rise and zoom also moved, easing out so that it
        // settles rather than stops.
        const float k = Arrived(panel, entrance_);
        const float shown =
            opacity * std::clamp(panel.opacity, 0.0f, 1.0f) * (panel.enter == skin::Enter::None ? k : k * k * (3.0f - 2.0f * k));
        if (shown <= 0.0f) continue;
        const float eased = 1.0f - (1.0f - k) * (1.0f - k) * (1.0f - k);
        const D2D1_RECT_F area = areaOf(i);
        D2D1_MATRIX_3X2_F move = D2D1::IdentityMatrix();
        if (k < 1.0f && panel.enter == skin::Enter::Rise) {
            move = D2D1::Matrix3x2F::Translation(0.0f, std::round((1.0f - eased) * 0.35f * b.px));
        } else if (k < 1.0f && panel.enter == skin::Enter::Zoom) {
            const float scale = 0.9f + 0.1f * eased;
            move = D2D1::Matrix3x2F::Scale(scale, scale,
                                           D2D1::Point2F((area.left + area.right) / 2.0f, (area.top + area.bottom) / 2.0f));
        }
        dc_->SetTransform(move);
        const bool layered = shown < 1.0f && area.right > area.left && area.bottom > area.top;
        if (layered)
            dc_->PushLayer(D2D1::LayerParameters1(area, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                                  D2D1::IdentityMatrix(), shown),
                           nullptr);
        if (b.backdrop && b.plate) dc_->FillRoundedRectangle(b.plateArea, b.backdrop.Get());
        else if (b.backdrop) dc_->FillEllipse(b.backdropArea, b.backdrop.Get());
        // The text and the spectra each have shadow effects of their own.
        for (size_t layer = 0; layer < 2; ++layer) {
            ID2D1Bitmap1 *image = layer ? b.live.Get() : b.text.Get();
            if (!image) continue;
            for (size_t n = 0; n < panel.shadows.size() && n < kShadows && layer * kShadows + n < shadows_.size(); ++n) {
                const skin::Shadow &sh = panel.shadows[n];
                ID2D1Effect *effect = shadows_[layer * kShadows + n].Get();
                const D2D1_COLOR_F c = ColorOf(sh.color, sh.opacity);
                effect->SetInput(0, image);
                effect->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, std::max(0.5f, sh.blur * b.px));
                effect->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(c.r, c.g, c.b, c.a));
                dc_->DrawImage(effect, D2D1::Point2F(b.at.x + std::round(sh.x * b.px), b.at.y + std::round(sh.y * b.px)));
            }
        }
        dc_->DrawImage(b.text.Get(), b.at);
        if (b.live) dc_->DrawImage(b.live.Get(), b.at);
        if (layered) dc_->PopLayer();
        dc_->SetTransform(D2D1::IdentityMatrix());
    }
    const HRESULT hr = dc_->EndDraw();
    dc_->SetTarget(nullptr);
    return SUCCEEDED(hr);
}

}  // namespace animelogon
