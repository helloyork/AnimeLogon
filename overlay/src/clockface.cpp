#include "clockface.h"

#include <d2d1_1helper.h>
#include <d3d10.h>

#include "animelogon/log.h"

using Microsoft::WRL::ComPtr;

namespace {

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

bool ClockFace::Init(ID3D11Device *device, const animelogon::skin::Resolved &skin,
                     const animelogon::ClockStyle &style, const animelogon::RegionalFormat &format) {
    Release();
    device_ = device;
    ComPtr<IDXGIDevice> dxgi;
    const D2D1_FACTORY_OPTIONS options{};
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&dxgi));
    if (SUCCEEDED(hr))
        hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                               reinterpret_cast<void **>(factory_.GetAddressOf()));
    if (SUCCEEDED(hr)) hr = factory_->CreateDevice(dxgi.Get(), &d2d_);
    if (FAILED(hr) || !view_.Init(d2d_.Get())) {
        ALOG(L"clock: Direct2D unavailable (0x%08X) -- no clock", hr);
        Release();
        return false;
    }
    view_.Set(skin, style, format);
    return true;
}

void ClockFace::Release() {
    view_.Release();
    d2d_.Reset();
    factory_.Reset();
    device_.Reset();
}

bool ClockFace::Draw(IDXGISurface *surface, UINT width, UINT height, size_t slot, float opacity) {
    if (!view_.context()) return false;
    DeviceLock lock(device_.Get());
    const D2D1_BITMAP_PROPERTIES1 props =
        D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
    ComPtr<ID2D1Bitmap1> target;
    if (FAILED(view_.context()->CreateBitmapFromDxgiSurface(surface, &props, &target))) return false;
    return view_.Draw(target.Get(), width, height, slot, opacity);
}
