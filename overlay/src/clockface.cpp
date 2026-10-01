#include "clockface.h"

#include <d2d1_1helper.h>
#include <d2d1effects.h>
#include <d3d10.h>
#include <dxgi.h>

#include <algorithm>
#include <cmath>

#include "animelogon/log.h"

using Microsoft::WRL::ComPtr;

namespace {

// Time height as a fraction of the display's height, by size.
constexpr float kTimeHeight[] = {0.075f, 0.10f, 0.135f, 0.18f};
constexpr float kDateScale = 0.28f;    // date height relative to the time's
constexpr float kMarginScale = 0.06f;  // distance from the edges, relative to the short side

bool HasFamily(IDWriteFactory *write, const std::wstring &family) {
    ComPtr<IDWriteFontCollection> fonts;
    UINT32 index = 0;
    BOOL exists = FALSE;
    return SUCCEEDED(write->GetSystemFontCollection(&fonts)) &&
           SUCCEEDED(fonts->FindFamilyName(family.c_str(), &index, &exists)) && exists;
}

// Serialises Direct2D's use of the immediate context with the decoder threads'.
class DeviceLock {
public:
    explicit DeviceLock(ID3D11Device *device) {
        if (device && SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&mt_)))) mt_->Enter();
    }
    ~DeviceLock() {
        if (mt_) mt_->Leave();
    }

private:
    ComPtr<ID3D10Multithread> mt_;
};

}  // namespace

bool ClockFace::Init(ID3D11Device *device, const animelogon::ClockSettings &settings,
                     const clocktext::Pictures &pictures) {
    Release();
    settings_ = settings;
    pictures_ = pictures;
    device_ = device;
    ComPtr<IDXGIDevice> dxgi;
    D2D1_FACTORY_OPTIONS options{};
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&dxgi));
    if (SUCCEEDED(hr))
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                               reinterpret_cast<void **>(factory_.GetAddressOf()));
    if (SUCCEEDED(hr)) hr = factory_->CreateDevice(dxgi.Get(), &d2d_);
    if (SUCCEEDED(hr)) hr = d2d_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_);
    if (SUCCEEDED(hr))
        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown **>(write_.GetAddressOf()));
    if (SUCCEEDED(hr)) hr = dc_->CreateEffect(CLSID_D2D1Shadow, &shadow_);
    if (FAILED(hr)) {
        ALOG(L"clock: Direct2D unavailable (0x%08X) -- no clock", hr);
        Release();
        return false;
    }
    dc_->SetUnitMode(D2D1_UNIT_MODE_PIXELS);
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    Tick();
    return true;
}

void ClockFace::Release() {
    slots_.clear();
    shadow_.Reset();
    write_.Reset();
    dc_.Reset();
    d2d_.Reset();
    factory_.Reset();
    device_.Reset();
    time_.clear();
    date_.clear();
    version_ = 0;
}

bool ClockFace::Tick() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    if (version_ && now.wMinute == shown_.wMinute && now.wHour == shown_.wHour && now.wDay == shown_.wDay)
        return false;
    shown_ = now;
    std::wstring time = clocktext::Time(pictures_, now), date = clocktext::Date(pictures_, now);
    if (version_ && time == time_ && date == date_) return false;
    time_ = std::move(time);
    date_ = std::move(date);
    ++version_;
    return true;
}

std::wstring ClockFace::FontFamily() const {
    if (!settings_.font.empty() && HasFamily(write_.Get(), settings_.font)) return settings_.font;
    return HasFamily(write_.Get(), L"Segoe UI Variable Display") ? L"Segoe UI Variable Display" : L"Segoe UI";
}

bool ClockFace::Build(Slot &slot, UINT width, UINT height) {
    slot.text.Reset();
    const int column = (int)settings_.anchor % 3, row = (int)settings_.anchor / 3;
    const DWRITE_TEXT_ALIGNMENT align = column == 0   ? DWRITE_TEXT_ALIGNMENT_LEADING
                                        : column == 1 ? DWRITE_TEXT_ALIGNMENT_CENTER
                                                      : DWRITE_TEXT_ALIGNMENT_TRAILING;
    const float w = (float)width, h = (float)height;
    const float margin = kMarginScale * std::min(w, h);
    const std::wstring family = FontFamily();

    float timePx = h * kTimeHeight[(int)settings_.size];
    ComPtr<IDWriteTextLayout> timeLayout, dateLayout;
    DWRITE_TEXT_METRICS tm{}, dm{};
    for (int pass = 0; pass < 2; ++pass) {
        ComPtr<IDWriteTextFormat> format;
        if (FAILED(write_->CreateTextFormat(family.c_str(), nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, timePx,
                                            pictures_.locale.c_str(), &format)) ||
            FAILED(write_->CreateTextLayout(time_.c_str(), (UINT32)time_.size(), format.Get(), w, h, &timeLayout)))
            return false;
        timeLayout->GetMetrics(&tm);
        // Narrow or portrait displays: shrink until the time fits between the margins.
        const float room = w - 2.0f * margin;
        if (tm.widthIncludingTrailingWhitespace <= room || room <= 0.0f) break;
        timePx *= room / tm.widthIncludingTrailingWhitespace;
    }
    if (!date_.empty()) {
        ComPtr<IDWriteTextFormat> format;
        const float datePx = std::max(12.0f, timePx * kDateScale);
        if (FAILED(write_->CreateTextFormat(family.c_str(), nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, datePx,
                                            pictures_.locale.c_str(), &format)) ||
            FAILED(write_->CreateTextLayout(date_.c_str(), (UINT32)date_.size(), format.Get(), w, h, &dateLayout)))
            return false;
        dateLayout->GetMetrics(&dm);
    }

    const float boxW = std::ceil(std::max(tm.widthIncludingTrailingWhitespace, dm.widthIncludingTrailingWhitespace));
    const float gap = dateLayout ? timePx * 0.02f : 0.0f;
    const float boxH = std::ceil(tm.height + gap + dm.height);
    const float pad = std::ceil(timePx * 0.15f);  // room for the shadow
    timeLayout->SetMaxWidth(boxW);
    timeLayout->SetTextAlignment(align);
    if (dateLayout) {
        dateLayout->SetMaxWidth(boxW);
        dateLayout->SetTextAlignment(align);
    }

    const D2D1_SIZE_U size = D2D1::SizeU((UINT32)(boxW + 2 * pad), (UINT32)(boxH + 2 * pad));
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    ComPtr<ID2D1SolidColorBrush> brush;
    if (FAILED(dc_->CreateBitmap(size, nullptr, 0, &props, &slot.text)) ||
        FAILED(dc_->CreateSolidColorBrush(D2D1::ColorF(settings_.color & 0xFFFFFF), &brush)))
        return false;
    dc_->SetTarget(slot.text.Get());
    dc_->BeginDraw();
    dc_->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc_->DrawTextLayout(D2D1::Point2F(pad, pad), timeLayout.Get(), brush.Get());
    if (dateLayout) dc_->DrawTextLayout(D2D1::Point2F(pad, pad + tm.height + gap), dateLayout.Get(), brush.Get());
    const HRESULT hr = dc_->EndDraw();
    dc_->SetTarget(nullptr);
    if (FAILED(hr)) return false;

    const float bw = (float)size.width, bh = (float)size.height;
    slot.at.x = column == 0 ? margin - pad : column == 1 ? (w - bw) / 2.0f : w - margin - bw + pad;
    slot.at.y = row == 0 ? margin - pad : row == 1 ? (h - bh) / 2.0f : h - margin - bh + pad;
    slot.at.x = std::floor(slot.at.x);
    slot.at.y = std::floor(slot.at.y);
    slot.width = width;
    slot.height = height;
    slot.version = version_;
    slot.blur = timePx * 0.035f;
    return true;
}

bool ClockFace::Draw(IDXGISurface *surface, UINT width, UINT height, size_t slot) {
    if (!dc_ || time_.empty()) return false;
    DeviceLock lock(device_.Get());
    if (slot >= slots_.size()) slots_.resize(slot + 1);
    Slot &s = slots_[slot];
    if ((s.version != version_ || s.width != width || s.height != height || !s.text) && !Build(s, width, height)) {
        ALOG(L"clock: could not lay the clock out");
        return false;
    }
    const D2D1_BITMAP_PROPERTIES1 props =
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    ComPtr<ID2D1Bitmap1> target;
    if (FAILED(dc_->CreateBitmapFromDxgiSurface(surface, &props, &target))) return false;
    dc_->SetTarget(target.Get());
    dc_->BeginDraw();
    shadow_->SetInput(0, s.text.Get());
    shadow_->SetValue(D2D1_SHADOW_PROP_BLUR_STANDARD_DEVIATION, s.blur);
    shadow_->SetValue(D2D1_SHADOW_PROP_COLOR, D2D1::Vector4F(0.0f, 0.0f, 0.0f, 0.55f));
    const float drop = std::max(1.0f, s.blur * 0.4f);
    dc_->DrawImage(shadow_.Get(), D2D1::Point2F(s.at.x + drop, s.at.y + drop));
    dc_->DrawImage(s.text.Get(), s.at);
    const HRESULT hr = dc_->EndDraw();
    dc_->SetTarget(nullptr);
    return SUCCEEDED(hr);
}
