// A theme's wallpaper blur on the graphics card, for the logon screen's windows: the same three
// steps as animelogon/blur.h on the processor (shaders/blur.hlsl), so the blurred wallpaper matches
// the still baked from it. The wallpaper is drawn into the target Begin hands out, at the window's
// size; End draws it blurred into the window. Its textures are kept for each size it is asked for.
#pragma once

#include <d3d11.h>
#include <wrl/client.h>

#include <map>
#include <tuple>

class GpuBlur {
public:
    // Its shaders, constants and sampler on `device`. False if any cannot be made.
    bool Create(ID3D11Device *device);

    // Where to draw the picture to blur, `width` x `height`; null if its textures cannot be made.
    ID3D11RenderTargetView *Begin(UINT width, UINT height, float sigma);
    // Draws what was drawn since Begin into `target` (the same size), blurred by the sigma given to
    // Begin, in pixels. Leaves no render target or shader resource bound.
    bool End(ID3D11DeviceContext *context, ID3D11RenderTargetView *target);

private:
    struct Surface {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
        Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
    };
    // The picture at the window's size, and two of cells at a factor-th of it.
    struct Surfaces {
        Surface picture, cells, across;
        UINT cellsW = 0, cellsH = 0;
    };

    bool Make(Surface *s, UINT width, UINT height, DXGI_FORMAT format);
    void Pass(ID3D11DeviceContext *context, ID3D11PixelShader *shader, ID3D11ShaderResourceView *from,
              ID3D11RenderTargetView *to, UINT width, UINT height);

    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs_;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> box_, gauss_, up_;
    Microsoft::WRL::ComPtr<ID3D11Buffer> constants_;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler_;
    std::map<std::tuple<UINT, UINT, int>, Surfaces> surfaces_;  // by width, height and factor
    Surfaces *now_ = nullptr;
    UINT width_ = 0, height_ = 0;
    int factor_ = 1;
    float sigma_ = 0.0f;
};
