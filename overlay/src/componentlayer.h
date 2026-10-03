// The components over the wallpaper -- every instance a theme places, each with its own values
// -- drawn with Direct2D on the presenter's device straight into each window's back buffer.
#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include <memory>
#include <vector>

#include "animelogon/skinview.h"

class ComponentLayer {
public:
    // False leaves the wallpaper without components.
    bool Init(ID3D11Device *device);
    void Release();

    // Adds one instance as it is drawn, with the global clock style and regional format;
    // `index` is how Draw names it. False if it cannot be drawn.
    bool Add(const animelogon::skin::Resolved &drawing, const animelogon::ClockStyle &style,
             const animelogon::RegionalFormat &format, size_t *index);
    size_t size() const { return views_.size(); }

    // Reads the time; returns true when the words of any instance changed.
    bool Tick();

    // Draws the instances `which` names, in that order, onto `surface`, a window's back
    // buffer. `slot` keeps one cache per window.
    bool Draw(IDXGISurface *surface, UINT width, UINT height, size_t slot, const std::vector<size_t> &which,
              float opacity = 1.0f);

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID2D1Factory1> factory_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2d_;
    // Each instance has its own device context; Direct2D shares the target between them, as
    // they come from one device.
    std::vector<std::unique_ptr<animelogon::SkinView>> views_;
};
