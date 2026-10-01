#include "animelogon/skinview.h"

#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <dwrite_1.h>

#include <algorithm>
#include <cmath>

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

bool SkinView::Init(ID2D1Device *device) {
    Release();
    HRESULT hr = device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_);
    if (SUCCEEDED(hr))
        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown **>(write_.GetAddressOf()));
    for (size_t i = 0; i < kShadows && SUCCEEDED(hr); ++i) {
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
    return true;
}

void SkinView::Release() {
    slots_.clear();
    shadows_.clear();
    write_.Reset();
    dc_.Reset();
    words_.clear();
    version_ = 0;
}

void SkinView::Set(const skin::Resolved &skin, const ClockStyle &style, const RegionalFormat &format) {
    skin_ = skin;
    style_ = style;
    format_ = FormatFor(format, style.locale);
    words_.clear();
    ++version_;
    Tick();
}

bool SkinView::Tick() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    return Tick(now);
}

bool SkinView::Tick(const SYSTEMTIME &now) {
    if (!words_.empty() && now.wMinute == shown_.wMinute && now.wHour == shown_.wHour && now.wDay == shown_.wDay &&
        now.wMonth == shown_.wMonth && now.wYear == shown_.wYear)
        return false;
    shown_ = now;
    std::vector<std::vector<std::vector<std::wstring>>> words;
    for (const skin::Panel &p : skin_.panels) {
        auto &panel = words.emplace_back();
        for (const skin::Line &l : p.lines) {
            auto &line = panel.emplace_back();
            for (const skin::Text &t : l.texts)
                line.push_back(Cased(FillText(t.value, format_, style_, now), t.textCase, format_.locale));
        }
    }
    if (words == words_) return false;
    words_ = std::move(words);
    ++version_;
    return true;
}

std::wstring SkinView::Family(const std::wstring &wanted) const {
    if (!wanted.empty() && HasFamily(write_.Get(), wanted)) return wanted;
    return HasFamily(write_.Get(), L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
}

bool SkinView::Lay(Line &line, const skin::Panel &, const skin::Line &source,
                   const std::vector<std::wstring> &words, float px) {
    struct Run {
        const skin::Text *text;
        UINT32 start, length;
    };
    std::wstring s;
    std::vector<Run> runs;
    for (size_t i = 0; i < source.texts.size() && i < words.size(); ++i) {
        if (words[i].empty()) continue;
        runs.push_back({&source.texts[i], (UINT32)s.size(), (UINT32)words[i].size()});
        s += words[i];
    }
    line.layout.Reset();
    line.runs.clear();
    if (runs.empty()) return true;  // nothing to say on this line
    for (const Run &r : runs) line.runs.push_back({(size_t)(r.text - source.texts.data()), {r.start, r.length}});

    const skin::Text &first = *runs.front().text;
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
        ComPtr<ID2D1SolidColorBrush> brush;
        if (FAILED(dc_->CreateSolidColorBrush(ColorOf(t.color, t.opacity), &brush))) return false;
        line.layout->SetFontFamilyName(Family(t.font).c_str(), range);
        line.layout->SetFontWeight((DWRITE_FONT_WEIGHT)t.weight, range);
        line.layout->SetFontStyle(t.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, range);
        line.layout->SetFontSize(px * t.size, range);
        line.layout->SetDrawingEffect(brush.Get(), range);
        if (spaced) {
            // Tracking is in the text's own em; the space before it in the panel's.
            const float tracking = t.tracking * px * t.size;
            if (tracking != 0.0f) spaced->SetCharacterSpacing(0.0f, tracking, 0.0f, range);
            if (t.space != 0.0f) spaced->SetCharacterSpacing(t.space * px, tracking, 0.0f, {r.start, 1});
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
            if (runs.front().text->hang) line.coreLeft = hit.left;
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

    const float anchorX = (column == 0 ? margin : column == 1 ? w / 2.0f : w - margin) + panel.offsetX / 100.0f * w;
    const float anchorY = (row == 0 ? margin : row == 1 ? h / 2.0f : h - margin) + panel.offsetY / 100.0f * h;
    const float refY = row == 0 ? top : row == 1 ? (top + lastBaseline) / 2.0f : lastBaseline;
    built.at = D2D1::Point2F(std::floor(anchorX - refX), std::floor(anchorY - (originY + refY)));
    built.px = px;

    const std::wstring pk = L"p" + std::to_wstring(index + 1);
    built.bounds.push_back({pk, D2D1::RectF(built.at.x + refX - before, built.at.y + originY + top,
                                            built.at.x + refX + after, built.at.y + originY + bottom)});
    for (size_t i = 0; i < lines.size(); ++i) {
        const Line &l = lines[i];
        if (!l.layout) continue;
        for (const Line::Run &r : l.runs) {
            DWRITE_HIT_TEST_METRICS hit{};
            UINT32 count = 0;
            if (FAILED(l.layout->HitTestTextRange(r.range.startPosition, r.range.length, 0.0f, 0.0f, &hit, 1, &count)) ||
                !count)
                continue;
            const float x = built.at.x + origins[i].x + hit.left, y = built.at.y + origins[i].y + hit.top;
            built.bounds.push_back({pk + L".l" + std::to_wstring(i + 1) + L".t" + std::to_wstring(r.text + 1),
                                    D2D1::RectF(x, y, x + hit.width, y + hit.height)});
        }
    }

    if (panel.backdrop > 0.0f) {
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
        if (FAILED(dc_->CreateGradientStopCollection(stops, ARRAYSIZE(stops), &gradient)) ||
            FAILED(dc_->CreateRadialGradientBrush(
                D2D1::RadialGradientBrushProperties(centre, D2D1::Point2F(), built.backdropArea.radiusX,
                                                    built.backdropArea.radiusY),
                gradient.Get(), &built.backdrop)))
            built.backdrop.Reset();
    }
    return true;
}

std::vector<SkinView::Bound> SkinView::Bounds(size_t slot) const {
    std::vector<Bound> out;
    if (slot < slots_.size())
        for (const Built &b : slots_[slot].panels) out.insert(out.end(), b.bounds.begin(), b.bounds.end());
    return out;
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
    dc_->SetTarget(target);
    dc_->BeginDraw();
    const bool faded = opacity < 1.0f;
    if (faded)
        dc_->PushLayer(D2D1::LayerParameters1(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE,
                                              D2D1::IdentityMatrix(), std::max(0.0f, opacity)),
                       nullptr);
    for (size_t i = 0; i < s.panels.size(); ++i) {
        const Built &b = s.panels[i];
        if (!b.text) continue;
        if (b.backdrop) dc_->FillEllipse(b.backdropArea, b.backdrop.Get());
        const skin::Panel &panel = skin_.panels[i];
        for (size_t k = 0; k < panel.shadows.size() && k < shadows_.size(); ++k) {
            const skin::Shadow &sh = panel.shadows[k];
            ID2D1Effect *effect = shadows_[k].Get();
            const D2D1_COLOR_F c = ColorOf(sh.color, sh.opacity);
            effect->SetInput(0, b.text.Get());
            effect->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, std::max(0.5f, sh.blur * b.px));
            effect->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(c.r, c.g, c.b, c.a));
            dc_->DrawImage(effect, D2D1::Point2F(b.at.x + std::round(sh.x * b.px), b.at.y + std::round(sh.y * b.px)));
        }
        dc_->DrawImage(b.text.Get(), b.at);
    }
    if (faded) dc_->PopLayer();
    const HRESULT hr = dc_->EndDraw();
    dc_->SetTarget(nullptr);
    return SUCCEEDED(hr);
}

}  // namespace animelogon
