// A skin drawn off screen over a still picture, for the settings app's preview and for tools.
#pragma once

#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

#include "animelogon/skinview.h"

namespace animelogon {

// A picture as 32-bit BGRA rows, `width` * 4 bytes apart.
struct Picture {
    UINT width = 0, height = 0;
    std::vector<uint8_t> bgra;
};

// Decodes an image file, scaled down to fit `maxWidth` when it is wider. Needs COM.
bool LoadPicture(const std::wstring &path, UINT maxWidth, Picture *out);
bool SavePicture(const std::wstring &path, const Picture &picture);

class SkinPreview {
public:
    // Draws the skin over `background` as on a display of the background's size. `bounds`, if
    // given, says where each panel and text landed.
    bool Render(const Picture &background, const skin::Resolved &skin, const ClockStyle &style,
                const RegionalFormat &format, const SYSTEMTIME &now, Picture *out,
                std::vector<SkinView::Bound> *bounds = nullptr);

private:
    bool Ready();

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context_;
    SkinView view_;
    bool failed_ = false;
};

}  // namespace animelogon
