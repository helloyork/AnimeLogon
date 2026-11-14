// The wallpaper blur on the graphics card (overlay/src/gpublur.h) draws what the same blur on the
// processor (animelogon/blur.h) draws: the still baked for the sign-in background and the wallpaper
// the logon screen plays after it must match. Run on a WARP device, so it runs everywhere.
#include "check.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <cstdlib>
#include <vector>

#include "animelogon/blur.h"
#include "gpublur.h"

using Microsoft::WRL::ComPtr;

namespace {

// A picture with sharp edges in every colour: blocks of pseudo-random colours with a few lines.
std::vector<uint8_t> Pattern(int w, int h) {
    std::vector<uint8_t> bgra((size_t)w * h * 4);
    uint32_t seed = 12345;
    auto next = [&] { return (seed = seed * 1664525u + 1013904223u) >> 24; };
    std::vector<uint8_t> blocks(64 * 3);
    for (uint8_t &b : blocks) b = (uint8_t)next();
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            uint8_t *p = &bgra[((size_t)y * w + x) * 4];
            const int block = ((y / 7) % 8) * 8 + (x / 9) % 8;
            for (int c = 0; c < 3; ++c) p[c] = blocks[(size_t)block * 3 + c];
            if (x % 23 == 0) p[0] = p[1] = p[2] = 255;
            p[3] = 255;
        }
    return bgra;
}

// The largest difference in any colour between the GPU's blur and the processor's.
int Difference(ID3D11Device *device, ID3D11DeviceContext *context, GpuBlur &blur, int w, int h, float sigma) {
    const std::vector<uint8_t> source = Pattern(w, h);
    ID3D11RenderTargetView *picture = blur.Begin((UINT)w, (UINT)h, sigma);
    if (!picture) return 1000;
    ComPtr<ID3D11Resource> resource;
    picture->GetResource(&resource);
    context->UpdateSubresource(resource.Get(), 0, nullptr, source.data(), (UINT)w * 4, 0);

    D3D11_TEXTURE2D_DESC d{};
    d.Width = (UINT)w;
    d.Height = (UINT)h;
    d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target, staging;
    ComPtr<ID3D11RenderTargetView> rtv;
    if (FAILED(device->CreateTexture2D(&d, nullptr, &target)) ||
        FAILED(device->CreateRenderTargetView(target.Get(), nullptr, &rtv)))
        return 1000;
    d.BindFlags = 0;
    d.Usage = D3D11_USAGE_STAGING;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    if (FAILED(device->CreateTexture2D(&d, nullptr, &staging))) return 1000;
    if (!blur.End(context, rtv.Get())) return 1000;
    context->CopyResource(staging.Get(), target.Get());
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return 1000;

    std::vector<uint8_t> expected = source;
    animelogon::blur::Apply(expected.data(), w, h, 4, sigma);
    int worst = 0;
    for (int y = 0; y < h; ++y) {
        const uint8_t *row = static_cast<const uint8_t *>(m.pData) + (size_t)y * m.RowPitch;
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                worst = std::max(worst, std::abs((int)row[x * 4 + c] - (int)expected[((size_t)y * w + x) * 4 + c]));
    }
    context->Unmap(staging.Get(), 0);
    return worst;
}

}  // namespace

TEST(BlurOnTheGraphicsCardMatchesTheOneOnTheProcessor) {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &device, nullptr, &context))) {
        CHECK(false);  // WARP is part of Windows
        return;
    }
    GpuBlur blur;
    CHECK(blur.Create(device.Get()));
    // Sizes that are not whole cells, and sigmas taking one-pixel cells, two-pixel cells and
    // eight-pixel cells. Half floats and the last rounding allow a level or two.
    CHECK(Difference(device.Get(), context.Get(), blur, 70, 45, 1.5f) <= 2);
    CHECK(Difference(device.Get(), context.Get(), blur, 70, 45, 6.0f) <= 2);
    CHECK(Difference(device.Get(), context.Get(), blur, 211, 117, 20.0f) <= 2);
    // A second size keeps its own textures; the first is still there.
    CHECK(Difference(device.Get(), context.Get(), blur, 70, 45, 6.0f) <= 2);
    CHECK(!blur.Begin(70, 45, 0.0f));
}
