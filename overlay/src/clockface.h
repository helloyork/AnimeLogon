// Draws the clock over the video with Direct2D, on the presenter's device. The text is drawn
// once a minute into a cached bitmap; each frame only composites it with its shadow.
#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>
#include <vector>

#include "animelogon/clock.h"
#include "clocktext.h"

class ClockFace {
public:
    // False leaves the video without a clock.
    bool Init(ID3D11Device *device, const animelogon::ClockSettings &settings, const clocktext::Pictures &pictures);
    void Release();

    // Reads the time; returns true when the words changed.
    bool Tick();

    // Draws onto `surface`, a window's back buffer. `slot` keeps one cache per window.
    bool Draw(IDXGISurface *surface, UINT width, UINT height, size_t slot);

private:
    struct Slot {
        UINT width = 0, height = 0;
        unsigned version = 0;
        Microsoft::WRL::ComPtr<ID2D1Bitmap1> text;
        D2D1_POINT_2F at{};
        float blur = 0.0f;  // the shadow's, in pixels
    };

    bool Build(Slot &slot, UINT width, UINT height);
    std::wstring FontFamily() const;

    animelogon::ClockSettings settings_;
    clocktext::Pictures pictures_;
    std::wstring time_, date_;
    unsigned version_ = 0;
    SYSTEMTIME shown_{};

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID2D1Factory1> factory_;
    Microsoft::WRL::ComPtr<ID2D1Device> d2d_;
    Microsoft::WRL::ComPtr<ID2D1DeviceContext> dc_;
    Microsoft::WRL::ComPtr<IDWriteFactory> write_;
    Microsoft::WRL::ComPtr<ID2D1Effect> shadow_;
    std::vector<Slot> slots_;
};
