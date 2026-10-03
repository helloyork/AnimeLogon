#include "presenter.h"

#include <d3d10.h>
#include <wincodec.h>

#include <cstring>
#include <mutex>
#include <thread>

#include "animelogon/log.h"
#include "animelogon/text.h"
#include "componentlayer.h"
#include "layout.h"
#include "picture.h"
#include "video_ps.h"
#include "video_vs.h"

using Microsoft::WRL::ComPtr;

namespace {

struct Params {
    float scale[2];
    float offset[2];
    float uvMax[2];
    float dim;
    float source;  // 0 black, 1 the video, 2 a picture: the still, an image wallpaper or the gradient
};
static_assert(sizeof(Params) % 16 == 0, "constant buffer size");

bool IsLost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG;
}

// An immutable BGRA texture of `bgra` (rows `width` * 4 bytes apart) and its view. The device
// is free-threaded, so this runs on any thread.
HRESULT MakeView(ID3D11Device *device, const void *bgra, int width, int height,
                 ComPtr<ID3D11ShaderResourceView> *view) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = (UINT)width;
    d.Height = (UINT)height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_IMMUTABLE;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA data{bgra, (UINT)width * 4, 0};
    ComPtr<ID3D11Texture2D> texture;
    HRESULT hr = device->CreateTexture2D(&d, &data, &texture);
    if (SUCCEEDED(hr)) hr = device->CreateShaderResourceView(texture.Get(), nullptr, view->ReleaseAndGetAddressOf());
    return hr;
}

}  // namespace

// An image being read on its own thread. The thread fills it in and sets `done`.
struct Presenter::ImageJob {
    std::mutex lock;
    bool done = false;
    Texture texture;
    std::wstring why;
};

Presenter::~Presenter() {
    DestroyWindows();
    ReleaseDevice();
}

bool Presenter::CreateDevice() {
    ReleaseDevice();
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    struct Attempt {
        D3D_DRIVER_TYPE type;
        UINT flags;
        const wchar_t *name;
    };
    const Attempt attempts[] = {
        {D3D_DRIVER_TYPE_HARDWARE, D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, L"hardware"},
        {D3D_DRIVER_TYPE_HARDWARE, D3D11_CREATE_DEVICE_BGRA_SUPPORT, L"hardware, no video"},
        {D3D_DRIVER_TYPE_WARP, D3D11_CREATE_DEVICE_BGRA_SUPPORT, L"WARP"},
    };
    const wchar_t *used = nullptr;
    bool video = false;
    for (const Attempt &a : attempts) {
        D3D_FEATURE_LEVEL got{};
        if (SUCCEEDED(D3D11CreateDevice(nullptr, a.type, nullptr, a.flags, levels, ARRAYSIZE(levels),
                                        D3D11_SDK_VERSION, &device_, &got, &context_))) {
            used = a.name;
            video = (a.flags & D3D11_CREATE_DEVICE_VIDEO_SUPPORT) != 0;
            break;
        }
    }
    if (!device_) {
        ALOG(L"present: no Direct3D 11 device");
        return false;
    }
    // Media Foundation's decoder threads share this device with the render thread.
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(device_.As(&mt))) mt->SetMultithreadProtected(TRUE);

    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(device_.As(&dxgi_)) || FAILED(dxgi_->GetAdapter(&adapter)) ||
        FAILED(adapter->GetParent(IID_PPV_ARGS(&factory_))))
        return false;
    DXGI_ADAPTER_DESC ad{};
    adapter->GetDesc(&ad);
    ALOG(L"present: Direct3D device on %s (%s)", ad.Description, used);

    if (video && SUCCEEDED(MFCreateDXGIDeviceManager(&managerToken_, &manager_)) &&
        FAILED(manager_->ResetDevice(device_.Get(), managerToken_)))
        manager_.Reset();

    HRESULT hr = DCompositionCreateDevice(dxgi_.Get(), IID_PPV_ARGS(&dcomp_));
    if (FAILED(hr)) {
        ALOG(L"present: DirectComposition unavailable (0x%08X)", hr);
        return false;
    }
    return CreateShaders();
}

bool Presenter::CreateShaders() {
    if (FAILED(device_->CreateVertexShader(g_vs, sizeof(g_vs), nullptr, &vs_)) ||
        FAILED(device_->CreatePixelShader(g_ps, sizeof(g_ps), nullptr, &ps_)))
        return false;
    D3D11_BUFFER_DESC b{};
    b.ByteWidth = sizeof(Params);
    b.Usage = D3D11_USAGE_DEFAULT;
    b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device_->CreateBuffer(&b, nullptr, &constants_))) return false;
    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    return SUCCEEDED(device_->CreateSamplerState(&s, &sampler_));
}

bool Presenter::LoadStill(const std::wstring &path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) {
        stillView_.Reset();
        return false;
    }
    if (stillView_ && CompareFileTime(&a.ftLastWriteTime, &stillStamp_) == 0) return true;
    stillView_.Reset();
    if (!device_) return false;
    ComPtr<IWICImagingFactory> wic;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> bgra;
    UINT w = 0, h = 0;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic));
    if (SUCCEEDED(hr))
        hr = wic->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                            &decoder);
    if (SUCCEEDED(hr)) hr = decoder->GetFrame(0, &frame);
    if (SUCCEEDED(hr)) hr = wic->CreateFormatConverter(&bgra);
    if (SUCCEEDED(hr))
        hr = bgra->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom);
    if (SUCCEEDED(hr)) hr = bgra->GetSize(&w, &h);
    if (SUCCEEDED(hr) && (!w || !h || w > 16384 || h > 16384)) hr = E_FAIL;
    std::vector<BYTE> pixels;
    if (SUCCEEDED(hr)) {
        pixels.resize((size_t)w * h * 4);
        hr = bgra->CopyPixels(nullptr, w * 4, (UINT)pixels.size(), pixels.data());
    }
    if (SUCCEEDED(hr)) hr = MakeView(device_.Get(), pixels.data(), (int)w, (int)h, &stillView_);
    if (FAILED(hr)) {
        ALOG(L"present: the sign-in background could not be loaded (0x%08X)", hr);
        stillView_.Reset();
        return false;
    }
    stillW_ = w;
    stillH_ = h;
    stillStamp_ = a.ftLastWriteTime;
    return true;
}

void Presenter::RequestImage(const std::wstring &id, const std::wstring &path) {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    const bool there = GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) != FALSE;
    const uint64_t bytes = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    const auto it = images_.find(id);
    if (it != images_.end() && there && it->second.path == path && it->second.bytes == bytes &&
        CompareFileTime(&it->second.written, &a.ftLastWriteTime) == 0 && (it->second.texture.view || it->second.job))
        return;  // held, or on its way

    ImageEntry &e = images_[id];
    e = ImageEntry{};
    e.path = path;
    e.written = a.ftLastWriteTime;
    e.bytes = bytes;
    if (!device_) {
        e.why = L"there is no device";
        return;
    }
    // Read off the render thread: an 8K image.bmp is over 100 MB.
    auto job = std::make_shared<ImageJob>();
    const ComPtr<ID3D11Device> device = device_;
    try {
        std::thread([job, device, path] {
            picture::Image image;
            std::wstring why;
            Texture texture;
            if (picture::ReadImage(path, &image, &why)) {
                const HRESULT hr = MakeView(device.Get(), image.bgra.data(), image.width, image.height, &texture.view);
                if (SUCCEEDED(hr)) {
                    texture.width = image.width;
                    texture.height = image.height;
                } else {
                    texture.view.Reset();
                    why = animelogon::Format(L"its %dx%d texture could not be made (0x%08X)", image.width,
                                             image.height, hr);
                }
            }
            const std::lock_guard<std::mutex> hold(job->lock);
            job->texture = std::move(texture);
            job->why = std::move(why);
            job->done = true;
        }).detach();
        e.job = std::move(job);
    } catch (const std::exception &) {
        e.why = L"no thread could read it";
    }
}

Presenter::ImageState Presenter::ImageStatus(const std::wstring &id, std::wstring *why) {
    const auto it = images_.find(id);
    if (it == images_.end()) {
        if (why) *why = L"it was never asked for";
        return ImageState::Failed;
    }
    ImageEntry &e = it->second;
    if (const std::shared_ptr<ImageJob> job = e.job) {
        {
            const std::lock_guard<std::mutex> hold(job->lock);
            if (!job->done) return ImageState::Reading;
            e.texture = std::move(job->texture);
            e.why = std::move(job->why);
        }
        e.job.reset();
    }
    if (e.texture.view) return ImageState::Ready;
    if (why) *why = e.why;
    return ImageState::Failed;
}

void Presenter::KeepImages(const std::set<std::wstring> &ids) {
    for (auto it = images_.begin(); it != images_.end();) it = ids.count(it->first) ? std::next(it) : images_.erase(it);
}

const Presenter::Texture *Presenter::Gradient(int rows) {
    const picture::Image column = picture::GradientColumn(rows);
    const auto it = gradients_.find(column.height);
    if (it != gradients_.end()) return &it->second;
    Texture t;
    if (!device_ || FAILED(MakeView(device_.Get(), column.bgra.data(), column.width, column.height, &t.view))) return nullptr;
    t.width = column.width;
    t.height = column.height;
    return &(gradients_[column.height] = std::move(t));
}

const Presenter::Texture *Presenter::WallpaperTexture(const Target &target, animelogon::Scaling *fit) {
    switch (target.wallpaper.source) {
    case plan::Source::Image: {
        const auto it = images_.find(target.wallpaper.id);
        if (it == images_.end() || !it->second.texture.view) return nullptr;
        *fit = target.wallpaper.fit;
        return &it->second.texture;
    }
    case plan::Source::Gradient:
        // A column as tall as the canvas, stretched across it: what every fit makes of a
        // picture the canvas's own size.
        *fit = animelogon::Scaling::Stretch;
        return Gradient(target.canvas.bottom - target.canvas.top);
    default:
        return nullptr;
    }
}

void Presenter::ReleaseDevice() {
    images_.clear();
    gradients_.clear();
    stillView_.Reset();
    sampler_.Reset();
    constants_.Reset();
    ps_.Reset();
    vs_.Reset();
    manager_.Reset();
    dcomp_.Reset();
    factory_.Reset();
    dxgi_.Reset();
    if (context_) context_->ClearState();
    context_.Reset();
    device_.Reset();
    lost_ = false;
}

bool Presenter::DeviceLost() {
    if (!device_) return false;
    if (!lost_ && device_->GetDeviceRemovedReason() != S_OK) lost_ = true;
    return lost_;
}

bool Presenter::CreateWindows(const std::vector<Target> &targets, const wchar_t *windowClass, HINSTANCE instance) {
    DestroyWindows();
    if (!device_) return false;
    targets_ = targets;
    for (const Target &t : targets_) {
        Window w;
        w.width = (UINT)(t.rect.right - t.rect.left);
        w.height = (UINT)(t.rect.bottom - t.rect.top);
        if (!w.width || !w.height) continue;
        w.hwnd = CreateWindowExW(WS_EX_NOREDIRECTIONBITMAP | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW,
                                 windowClass, L"AnimeLogon", WS_POPUP, t.rect.left, t.rect.top, (int)w.width,
                                 (int)w.height, nullptr, nullptr, instance, nullptr);
        if (!w.hwnd) {
            ALOG(L"present: CreateWindowEx failed (%lu)", GetLastError());
            DestroyWindows();
            return false;
        }
        DXGI_SWAP_CHAIN_DESC1 d{};
        d.Width = w.width;
        d.Height = w.height;
        d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        d.SampleDesc.Count = 1;
        d.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        d.BufferCount = 2;
        d.Scaling = DXGI_SCALING_STRETCH;
        d.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
        d.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        HRESULT hr = factory_->CreateSwapChainForComposition(device_.Get(), &d, nullptr, &w.swapChain);
        if (SUCCEEDED(hr)) hr = dcomp_->CreateTargetForHwnd(w.hwnd, TRUE, &w.target);
        if (SUCCEEDED(hr)) hr = dcomp_->CreateVisual(&w.visual);
        if (SUCCEEDED(hr)) hr = dcomp_->CreateEffectGroup(&w.effect);
        if (SUCCEEDED(hr)) hr = w.visual->SetContent(w.swapChain.Get());
        if (SUCCEEDED(hr)) hr = w.effect->SetOpacity(1.0f);
        if (SUCCEEDED(hr)) hr = w.visual->SetEffect(w.effect.Get());
        if (SUCCEEDED(hr)) hr = w.target->SetRoot(w.visual.Get());
        if (FAILED(hr)) {
            ALOG(L"present: composition setup failed (0x%08X)", hr);
            if (IsLost(hr)) lost_ = true;
            DestroyWindow(w.hwnd);
            DestroyWindows();
            return false;
        }
        windows_.push_back(std::move(w));
    }
    opacity_ = 1.0f;
    // Black until there is a picture to show.
    std::vector<Picture> none(targets_.size());
    if (!Render(none, 1.0f, 1.0f)) return false;
    return !windows_.empty() && SUCCEEDED(dcomp_->Commit());
}

void Presenter::DestroyWindows() {
    for (Window &w : windows_) {
        w.effect.Reset();
        w.visual.Reset();
        w.target.Reset();
        w.swapChain.Reset();
        if (w.hwnd) DestroyWindow(w.hwnd);
    }
    windows_.clear();
    targets_.clear();
    visible_ = false;
    components_ = nullptr;
    componentsPerTarget_.clear();
    componentsFailed_ = false;
    componentOpacity_ = 1.0f;
    if (dcomp_) dcomp_->Commit();
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
}

void Presenter::SetWallpaper(size_t target, const plan::Wallpaper &wallpaper) {
    if (target < targets_.size()) targets_[target].wallpaper = wallpaper;
}

void Presenter::SetComponents(ComponentLayer *layer, const std::vector<std::vector<size_t>> &perTarget) {
    components_ = layer;
    componentsPerTarget_ = perTarget;
    componentsFailed_ = false;
}

bool Presenter::MoveComponentsTo(const RECT &monitor) {
    bool found = false;
    for (const Target &t : targets_) found = found || EqualRect(&t.rect, &monitor);
    if (!found) return false;
    for (Target &t : targets_) t.showComponents = EqualRect(&t.rect, &monitor) != FALSE;
    return true;
}

bool Presenter::Owns(HWND hwnd) const {
    for (const Window &w : windows_)
        if (w.hwnd == hwnd) return true;
    return false;
}

void Presenter::Show() {
    for (const Window &w : windows_) {
        ShowWindow(w.hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(w.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    }
    visible_ = !windows_.empty();
}

void Presenter::Hide() {
    for (const Window &w : windows_) ShowWindow(w.hwnd, SW_HIDE);
    visible_ = false;
}

void Presenter::KeepOnTop() {
    if (!visible_) return;
    for (HWND w = GetTopWindow(nullptr); w; w = GetWindow(w, GW_HWNDNEXT)) {
        if (!IsWindowVisible(w)) continue;
        if (Owns(w)) return;
        RECT r{};
        GetWindowRect(w, &r);
        if (r.right - r.left < 64 || r.bottom - r.top < 64) continue;
        for (const Window &ours : windows_)
            SetWindowPos(ours.hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        return;
    }
}

bool Presenter::SecondsToNextComposition(double *seconds) const {
    DCOMPOSITION_FRAME_STATISTICS st{};
    if (!dcomp_ || FAILED(dcomp_->GetFrameStatistics(&st)) || !st.timeFrequency.QuadPart ||
        !st.currentCompositionRate.Numerator)
        return false;
    const double s = (double)(st.nextEstimatedFrameTime.QuadPart - st.currentTime.QuadPart) /
                     (double)st.timeFrequency.QuadPart;
    if (s <= 0.0 || s > 0.1) return false;
    *seconds = s;
    return true;
}

double Presenter::CompositionRate() const {
    DCOMPOSITION_FRAME_STATISTICS st{};
    if (!dcomp_ || FAILED(dcomp_->GetFrameStatistics(&st)) || !st.currentCompositionRate.Denominator) return 0.0;
    return (double)st.currentCompositionRate.Numerator / st.currentCompositionRate.Denominator;
}

bool Presenter::Render(const std::vector<Picture> &pictures, float dim, float opacity) {
    if (!device_ || lost_) return false;
    for (size_t i = 0; i < windows_.size(); ++i) {
        Window &w = windows_[i];
        const Target &t = targets_[i];
        const Picture pic = i < pictures.size() ? pictures[i] : Picture{};
        ComPtr<ID3D11Texture2D> back;
        ComPtr<ID3D11RenderTargetView> rtv;
        HRESULT hr = w.swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
        if (SUCCEEDED(hr)) hr = device_->CreateRenderTargetView(back.Get(), nullptr, &rtv);
        if (FAILED(hr)) {
            if (IsLost(hr) || DeviceLost()) lost_ = true;
            return false;
        }
        Params p{};
        p.dim = dim;
        // t0 and t1 the video's planes, t2 a picture.
        ID3D11ShaderResourceView *views[3] = {};
        bool placed = false;
        layout::Mapping m;
        switch (pic.kind) {
        case Picture::Kind::Video:
            if (pic.frame && pic.videoW > 0 && pic.videoH > 0) {
                m = layout::Map(pic.videoW, pic.videoH, t.canvas, t.rect, t.wallpaper.fit);
                p.source = 1.0f;
                p.uvMax[0] = pic.frame->uMax;
                p.uvMax[1] = pic.frame->vMax;
                views[0] = pic.frame->luma;
                views[1] = pic.frame->chroma;
                placed = true;
            }
            break;
        case Picture::Kind::Wallpaper: {
            animelogon::Scaling fit = animelogon::Scaling::Fill;
            if (const Texture *texture = WallpaperTexture(t, &fit)) {
                m = layout::Map(texture->width, texture->height, t.canvas, t.rect, fit);
                p.source = 2.0f;
                views[2] = texture->view.Get();
                placed = true;
            }
            break;
        }
        case Picture::Kind::Still:
            if (stillView_) {
                // As Windows draws it: filling each display.
                m = layout::Map((int)stillW_, (int)stillH_, t.rect, t.rect, animelogon::Scaling::Fill);
                p.source = 2.0f;
                views[2] = stillView_.Get();
                placed = true;
            }
            break;
        case Picture::Kind::Black:
            break;
        }
        if (placed) {
            p.scale[0] = m.scaleX;
            p.scale[1] = m.scaleY;
            p.offset[0] = m.offsetX;
            p.offset[1] = m.offsetY;
        }
        context_->UpdateSubresource(constants_.Get(), 0, nullptr, &p, 0, 0);
        D3D11_VIEWPORT vp{0.0f, 0.0f, (float)w.width, (float)w.height, 0.0f, 1.0f};
        context_->RSSetViewports(1, &vp);
        ID3D11RenderTargetView *targets[] = {rtv.Get()};
        context_->OMSetRenderTargets(1, targets, nullptr);
        context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context_->IASetInputLayout(nullptr);
        context_->VSSetShader(vs_.Get(), nullptr, 0);
        context_->PSSetShader(ps_.Get(), nullptr, 0);
        ID3D11Buffer *cb[] = {constants_.Get()};
        context_->PSSetConstantBuffers(0, 1, cb);
        ID3D11SamplerState *samplers[] = {sampler_.Get()};
        context_->PSSetSamplers(0, 1, samplers);
        context_->PSSetShaderResources(0, 3, views);
        context_->Draw(3, 0);
        ID3D11ShaderResourceView *none[3] = {};
        context_->PSSetShaderResources(0, 3, none);
        context_->OMSetRenderTargets(0, nullptr, nullptr);
        if (components_ && t.showComponents && !componentsFailed_ && componentOpacity_ > 0.0f &&
            i < componentsPerTarget_.size() && !componentsPerTarget_[i].empty()) {
            ComPtr<IDXGISurface> surface;
            if (FAILED(back.As(&surface)) ||
                !components_->Draw(surface.Get(), w.width, w.height, i, componentsPerTarget_[i], componentOpacity_)) {
                // The wallpaper goes on without them.
                ALOG(L"present: the components could not be drawn -- continuing without them");
                componentsFailed_ = true;
            }
        }
        hr = w.swapChain->Present(0, 0);
        if (FAILED(hr)) {
            if (IsLost(hr)) lost_ = true;
            return false;
        }
    }
    if (opacity != opacity_) {
        opacity_ = opacity;
        for (Window &w : windows_) w.effect->SetOpacity(opacity);
        if (FAILED(dcomp_->Commit())) return false;
    }
    return true;
}
