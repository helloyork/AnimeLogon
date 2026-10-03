// The windows the wallpaper is drawn in: one per display, composed with DirectComposition
// above everything else on the Winlogon desktop.
#pragma once

#include <windows.h>
#include <d3d11_1.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <mfapi.h>
#include <wrl/client.h>

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "animelogon/settings.h"
#include "plan.h"
#include "player.h"

class ComponentLayer;

class Presenter {
public:
    struct Target {
        RECT rect{};              // the display, in desktop pixels
        RECT canvas{};            // what the wallpaper is fitted to: the display, or all of them
        plan::Wallpaper wallpaper;
        bool showComponents = false;  // draw this window's components over it
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
    // Puts another wallpaper on a target: the built-in one, when its own cannot be shown.
    void SetWallpaper(size_t target, const plan::Wallpaper &wallpaper);

    // The components drawn over the targets: for each target, the instances of `layer` it
    // shows, in order. Null for none. The layer is not owned.
    void SetComponents(ComponentLayer *layer, const std::vector<std::vector<size_t>> &perTarget);
    // The components' own opacity, for fading them in over the wallpaper.
    void SetComponentOpacity(float opacity) { componentOpacity_ = opacity; }
    // Draws the components on the target covering `monitor` alone. False if none covers it.
    bool MoveComponentsTo(const RECT &monitor);

    void Show();
    void Hide();
    bool visible() const { return visible_; }
    // Puts the windows back at the top of the topmost band if something is above them.
    void KeepOnTop();

    // Seconds until the compositor's next frame, when it can say.
    bool SecondsToNextComposition(double *seconds) const;
    // The compositor's frame rate, or 0 if it cannot say.
    double CompositionRate() const;

    // The baked sign-in background, decoded while parked so that it can cover the screen the
    // moment it locks. Reloaded only when the file changes; false if there is none to show.
    bool LoadStill(const std::wstring &path);
    bool HasStill() const { return stillView_ != nullptr; }

    // Image wallpapers, kept on the device by wallpaper id between appearances. Asking for one
    // starts reading its image.bmp on another thread, unless the device already holds it and
    // the file has not changed since; ImageStatus says when it is ready, and if it failed, why.
    enum class ImageState { Reading, Ready, Failed };
    void RequestImage(const std::wstring &id, const std::wstring &path);
    ImageState ImageStatus(const std::wstring &id, std::wstring *why = nullptr);
    // Lets go of the images no target asked for this time.
    void KeepImages(const std::set<std::wstring> &ids);

    struct Picture {
        enum class Kind {
            Black,
            Still,      // the baked sign-in background, while the wallpaper is not ready
            Video,      // `frame`
            Wallpaper,  // the target's image wallpaper or the built-in gradient
        };
        Kind kind = Kind::Black;
        const VideoPlayer::Frame *frame = nullptr;
        int videoW = 0, videoH = 0;
    };
    // One picture per target. `dim` fades the picture to black; `opacity` fades the window.
    bool Render(const std::vector<Picture> &pictures, float dim, float opacity);

private:
    struct Window {
        HWND hwnd = nullptr;
        UINT width = 0, height = 0;
        Microsoft::WRL::ComPtr<IDXGISwapChain1> swapChain;
        Microsoft::WRL::ComPtr<IDCompositionTarget> target;
        Microsoft::WRL::ComPtr<IDCompositionVisual> visual;
        Microsoft::WRL::ComPtr<IDCompositionEffectGroup> effect;
    };
    // A picture on the device, ready to sample.
    struct Texture {
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
        int width = 0, height = 0;
    };
    struct ImageJob;
    struct ImageEntry {
        std::wstring path;
        FILETIME written{};
        uint64_t bytes = 0;
        Texture texture;
        std::shared_ptr<ImageJob> job;  // while it is being read
        std::wstring why;               // why it failed
    };

    bool CreateShaders();
    // The built-in wallpaper for a canvas: one column, `rows` tall.
    const Texture *Gradient(int rows);
    // The texture a target's wallpaper is drawn from and how it is placed; null if not ready.
    const Texture *WallpaperTexture(const Target &target, animelogon::Scaling *fit);

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
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> stillView_;
    UINT stillW_ = 0, stillH_ = 0;
    FILETIME stillStamp_{};
    std::map<std::wstring, ImageEntry> images_;  // by wallpaper id
    std::map<int, Texture> gradients_;           // by rows

    std::vector<Target> targets_;
    std::vector<Window> windows_;
    bool visible_ = false;
    float opacity_ = -1.0f;
    bool lost_ = false;
    ComponentLayer *components_ = nullptr;
    std::vector<std::vector<size_t>> componentsPerTarget_;
    bool componentsFailed_ = false;
    float componentOpacity_ = 1.0f;
};
