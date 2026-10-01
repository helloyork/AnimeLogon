// The windows the video is drawn in: one per display, composed with DirectComposition
// above everything else on the Winlogon desktop.
#pragma once

#include <windows.h>
#include <d3d11_1.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "animelogon/settings.h"
#include "player.h"

class Presenter {
public:
    struct Target {
        RECT rect{};             // the display, in desktop pixels
        RECT canvas{};           // what the video is fitted to: the display, or all of them
        std::wstring videoId;    // empty draws black
    };

    Presenter() = default;
    ~Presenter();
    Presenter(const Presenter &) = delete;
    Presenter &operator=(const Presenter &) = delete;

    // The device, created while parked. Hardware, else WARP.
    bool CreateDevice();
    void ReleaseDevice();
    ID3D11Device *device() const { return device_.Get(); }
    IMFDXGIDeviceManager *videoManager() const { return manager_.Get(); }
    bool DeviceLost();

    // Windows are created hidden, on the calling thread's desktop.
    bool CreateWindows(const std::vector<Target> &targets, const wchar_t *windowClass, HINSTANCE instance);
    void DestroyWindows();
    bool HasWindows() const { return !windows_.empty(); }
    HWND primary() const { return windows_.empty() ? nullptr : windows_.front().hwnd; }
    bool Owns(HWND hwnd) const;
    const std::vector<Target> &targets() const { return targets_; }

    void Show();
    void Hide();
    bool visible() const { return visible_; }
    // Puts the windows back at the top of the topmost band if something is above them.
    void KeepOnTop();

    struct Picture {
        const VideoPlayer::Frame *frame = nullptr;  // null draws black
        int videoW = 0, videoH = 0;
    };
    // One picture per target. `dim` fades the picture to black; `opacity` fades the window.
    bool Render(const std::vector<Picture> &pictures, animelogon::Scaling scaling, float dim, float opacity);

private:
    struct Window {
        HWND hwnd = nullptr;
        UINT width = 0, height = 0;
        Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
        Microsoft::WRL::ComPtr<IDCompositionTarget> target;
        Microsoft::WRL::ComPtr<IDCompositionVisual> visual;
        Microsoft::WRL::ComPtr<IDCompositionEffectGroup> effect;
    };

    bool CreateShaders();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    Microsoft::WRL::ComPtr<IDXGIDevice> dxgi_;
    Microsoft::WRL::ComPtr<IDXGIFactory2> factory_;
    Microsoft::WRL::ComPtr<IDCompositionDevice> dcomp_;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> manager_;
    UINT managerToken_ = 0;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;

    std::vector<Target> targets_;
    std::vector<Window> windows_;
    bool visible_ = false;
    float opacity_ = -1.0f;
    bool lost_ = false;
};
