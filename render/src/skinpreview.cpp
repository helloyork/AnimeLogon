#include "animelogon/skinpreview.h"

#include <d2d1_1.h>
#include <d2d1_1helper.h>
#include <dxgi.h>
#include <wincodec.h>

#include <algorithm>
#include <cmath>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace animelogon {

bool LoadPicture(const std::wstring &path, UINT maxWidth, Picture *out) {
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapSource> source;
    ComPtr<IWICFormatConverter> converter;
    UINT w = 0, h = 0;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) ||
        FAILED(wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad,
                                              &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&w, &h)) || !w || !h)
        return false;
    source = frame;
    if (maxWidth && w > maxWidth) {
        ComPtr<IWICBitmapScaler> scaler;
        const UINT sh = std::max<UINT>(1, (UINT)((uint64_t)h * maxWidth / w));
        if (FAILED(wic->CreateBitmapScaler(&scaler)) ||
            FAILED(scaler->Initialize(frame.Get(), maxWidth, sh, WICBitmapInterpolationModeHighQualityCubic)))
            return false;
        source = scaler;
        w = maxWidth;
        h = sh;
    }
    if (FAILED(wic->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                     0.0, WICBitmapPaletteTypeCustom)))
        return false;
    out->width = w;
    out->height = h;
    out->bgra.resize((size_t)w * h * 4);
    return SUCCEEDED(converter->CopyPixels(nullptr, w * 4, (UINT)out->bgra.size(), out->bgra.data()));
}

bool SavePicture(const std::wstring &path, const Picture &picture) {
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    const UINT pitch = picture.width * 4;
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic))) &&
           SUCCEEDED(wic->CreateStream(&stream)) &&
           SUCCEEDED(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) &&
           SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) &&
           SUCCEEDED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) &&
           SUCCEEDED(encoder->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) &&
           SUCCEEDED(frame->SetSize(picture.width, picture.height)) && SUCCEEDED(frame->SetPixelFormat(&format)) &&
           format == GUID_WICPixelFormat32bppBGRA &&
           SUCCEEDED(frame->WritePixels(picture.height, pitch, pitch * picture.height,
                                        const_cast<BYTE *>(picture.bgra.data()))) &&
           SUCCEEDED(frame->Commit()) && SUCCEEDED(encoder->Commit());
}

namespace {

// A Direct3D device to draw on, and `view` set up on it. The previews are small and seldom drawn;
// WARP is the fallback where there is no GPU.
bool MakeDevice(ComPtr<ID3D11Device> *device, ComPtr<ID3D11DeviceContext> *context, SkinView *view) {
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Factory1> factory;
    ComPtr<ID2D1Device> d2d;
    const D2D1_FACTORY_OPTIONS options{};
    for (D3D_DRIVER_TYPE type : {D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP}) {
        if (SUCCEEDED(D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                        D3D11_SDK_VERSION, device->ReleaseAndGetAddressOf(), nullptr,
                                        context->ReleaseAndGetAddressOf())))
            break;
    }
    if (*device && SUCCEEDED(device->As(&dxgi)) &&
        SUCCEEDED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &options,
                                    reinterpret_cast<void **>(factory.GetAddressOf()))) &&
        SUCCEEDED(factory->CreateDevice(dxgi.Get(), &d2d)) && view->Init(d2d.Get()))
        return true;
    device->Reset();
    context->Reset();
    return false;
}

}  // namespace

bool SkinPreview::Ready() {
    if (device_) return true;
    if (failed_) return false;
    failed_ = !MakeDevice(&device_, &context_, &view_);
    return !failed_;
}

bool LivePreview::Ready() {
    if (device_) return true;
    if (failed_) return false;
    failed_ = !MakeDevice(&device_, &context_, &view_);
    return !failed_;
}

bool LivePreview::Draw(const std::wstring &signature, const skin::Resolved &skin, const ClockStyle &style,
                       const RegionalFormat &format, const data::Snapshot &data, UINT width, UINT height,
                       const spectrum::Frame &audio, Picture *out, RECT *where) {
    if (!width || !height || !Ready()) return false;
    if (signature != signature_) {
        view_.Set(skin, style, format);
        signature_ = signature;
    }
    if (!target_ || width != width_ || height != height_) {
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<IDXGISurface> surface;
        const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
        target_.Reset();
        bitmap_.Reset();
        if (FAILED(device_->CreateTexture2D(&desc, nullptr, &target_)) || FAILED(target_.As(&surface)) ||
            FAILED(view_.context()->CreateBitmapFromDxgiSurface(surface.Get(), &props, &bitmap_))) {
            target_.Reset();
            bitmap_.Reset();
            return false;
        }
        width_ = width;
        height_ = height;
    }
    view_.SetData(data);
    view_.SetAudio(audio);
    view_.Tick();
    ID2D1DeviceContext *dc = view_.context();
    dc->SetTarget(bitmap_.Get());
    dc->BeginDraw();
    dc->Clear(D2D1::ColorF(0, 0, 0, 0));
    const HRESULT cleared = dc->EndDraw();
    dc->SetTarget(nullptr);
    D2D1_RECT_F reach{};
    if (FAILED(cleared) || !view_.Draw(bitmap_.Get(), width, height, 0) || !view_.Reach(0, &reach)) return false;

    // Only the part the skin can touch is read back.
    const LONG left = std::clamp((LONG)std::floor(reach.left), 0L, (LONG)width);
    const LONG top = std::clamp((LONG)std::floor(reach.top), 0L, (LONG)height);
    const LONG right = std::clamp((LONG)std::ceil(reach.right), left, (LONG)width);
    const LONG bottom = std::clamp((LONG)std::ceil(reach.bottom), top, (LONG)height);
    if (right - left < 1 || bottom - top < 1) return false;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = (UINT)(right - left);
    desc.Height = (UINT)(bottom - top);
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging))) return false;
    const D3D11_BOX box{(UINT)left, (UINT)top, 0, (UINT)right, (UINT)bottom, 1};
    context_->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, target_.Get(), 0, &box);
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    out->width = desc.Width;
    out->height = desc.Height;
    out->bgra.resize((size_t)out->width * out->height * 4);
    for (UINT y = 0; y < out->height; ++y)
        memcpy(out->bgra.data() + (size_t)y * out->width * 4, (const uint8_t *)map.pData + (size_t)y * map.RowPitch,
               (size_t)out->width * 4);
    context_->Unmap(staging.Get(), 0);
    *where = RECT{left, top, right, bottom};
    return true;
}

bool SkinPreview::Render(const Picture &background, const skin::Resolved &skin, const ClockStyle &style,
                         const RegionalFormat &format, const SYSTEMTIME &now, Picture *out,
                         std::vector<SkinView::Bound> *bounds, bool transparent, const data::Snapshot *data,
                         const spectrum::Frame *audio) {
    if (!background.width || !background.height || !Ready()) return false;
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = background.width;
    desc.Height = background.height;
    desc.MipLevels = desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    const D3D11_SUBRESOURCE_DATA init{background.bgra.data(), background.width * 4, 0};
    ComPtr<ID3D11Texture2D> target, staging;
    ComPtr<IDXGISurface> surface;
    ComPtr<ID2D1Bitmap1> bitmap;
    const D2D1_BITMAP_PROPERTIES1 props = D2D1::BitmapProperties1(
        D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, transparent ? D2D1_ALPHA_MODE_PREMULTIPLIED : D2D1_ALPHA_MODE_IGNORE));
    if (FAILED(device_->CreateTexture2D(&desc, &init, &target)) || FAILED(target.As(&surface)) ||
        FAILED(view_.context()->CreateBitmapFromDxgiSurface(surface.Get(), &props, &bitmap)))
        return false;
    view_.Set(skin, style, format);
    view_.SetData(data ? *data : data::Snapshot{});
    view_.SettleAudio(audio ? *audio : spectrum::Sample());
    view_.Tick(now);
    if (!view_.Draw(bitmap.Get(), background.width, background.height, 0)) return false;
    if (bounds) *bounds = view_.Bounds(0);

    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    D3D11_MAPPED_SUBRESOURCE map{};
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &staging))) return false;
    context_->CopyResource(staging.Get(), target.Get());
    if (FAILED(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &map))) return false;
    out->width = background.width;
    out->height = background.height;
    out->bgra.resize((size_t)out->width * out->height * 4);
    for (UINT y = 0; y < out->height; ++y) {
        uint8_t *row = out->bgra.data() + (size_t)y * out->width * 4;
        memcpy(row, (const uint8_t *)map.pData + (size_t)y * map.RowPitch, (size_t)out->width * 4);
        if (!transparent)
            for (UINT x = 0; x < out->width; ++x) row[x * 4 + 3] = 0xFF;  // the target ignores alpha
    }
    context_->Unmap(staging.Get(), 0);
    return true;
}

}  // namespace animelogon
