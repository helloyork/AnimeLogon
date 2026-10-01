// The clock skin over the video, drawn with Direct2D on the presenter's device straight into
// each window's back buffer.
#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>

#include "animelogon/skinview.h"

class ClockFace {
public:
    // False leaves the video without a clock.
    bool Init(ID3D11Device *device, const animelogon::skin::Resolved &skin, const animelogon::RegionalFormat &format);
    void Release();

    // Reads the time; returns true when the words changed.
    bool Tick() { return view_.Tick(); }

    // Draws onto `surface`, a window's back buffer. `slot` keeps one cache per window.
    bool Draw(IDXGISurface *surface, UINT width, UINT height, size_t slot);

private:
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID2D1Factory1> factory_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2d_;
    animelogon::SkinView view_;
};
