#include "presenter.h"

#include <d3d10.h>

#include <cstring>

#include "animelogon/log.h"
#include "layout.h"
#include "video_ps.h"
#include "video_vs.h"

using Microsoft::WRL::ComPtr;

namespace {

struct Params {
    float scale[2];
    float offset[2];
    float uvMax[2];
    float dim;
    float hasVideo;
};
static_assert(sizeof(Params) % 16 == 0, "constant buffer size");

bool IsLost(HRESULT hr) {
    return hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG;
}

}  // namespace

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

void Presenter::ReleaseDevice() {
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
    // Black until the first frame arrives.
    std::vector<Picture> none(targets_.size());
    if (!Render(none, animelogon::Scaling::Fill, 1.0f, 1.0f)) return false;
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
    if (dcomp_) dcomp_->Commit();
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
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

bool Presenter::Render(const std::vector<Picture> &pictures, animelogon::Scaling scaling, float dim, float opacity) {
    if (!device_ || lost_) return false;
    for (size_t i = 0; i < windows_.size(); ++i) {
        Window &w = windows_[i];
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
        p.hasVideo = pic.frame && pic.videoW > 0 ? 1.0f : 0.0f;
        if (p.hasVideo > 0.0f) {
            const layout::Mapping m = layout::Map(pic.videoW, pic.videoH, targets_[i].canvas, targets_[i].rect, scaling);
            p.scale[0] = m.scaleX;
            p.scale[1] = m.scaleY;
            p.offset[0] = m.offsetX;
            p.offset[1] = m.offsetY;
            p.uvMax[0] = pic.frame->uMax;
            p.uvMax[1] = pic.frame->vMax;
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
        ID3D11ShaderResourceView *views[] = {pic.frame ? pic.frame->luma : nullptr,
                                             pic.frame ? pic.frame->chroma : nullptr};
        context_->PSSetShaderResources(0, 2, views);
        context_->Draw(3, 0);
        ID3D11ShaderResourceView *none[2] = {};
        context_->PSSetShaderResources(0, 2, none);
        context_->OMSetRenderTargets(0, nullptr, nullptr);
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
