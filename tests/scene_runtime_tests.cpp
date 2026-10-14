// Real shared textures and the sandbox renderer: an acknowledged paused frame must contain
// the video input, including when it was published while the renderer was still starting.
#include "check.h"
#include "animelogon/bitmap.h"
#include "animelogon/scenehost.h"
#include <algorithm>
#include <cstdio>

using namespace animelogon;
using Microsoft::WRL::ComPtr;

TEST(SceneAcknowledgesVideoOnItsFirstFrameAndWhilePaused) {
    ComPtr<ID3D11Device> device;
    std::wstring why;
    if (!scene::Device(&device, &why)) {
        std::printf("  skipped: no hardware adapter for sandbox scene textures\n");
        return;
    }
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<IDXGIAdapter1> adapter1;
    DXGI_ADAPTER_DESC1 description{};
    const bool identified = SUCCEEDED(device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
        SUCCEEDED(adapter.As(&adapter1)) && SUCCEEDED(adapter1->GetDesc1(&description));
    CHECK(identified);
    if (!identified) return;
    // D3D_DRIVER_TYPE_HARDWARE can still select the Basic Render Driver on hosted runners.
    // This test, like scene.exe, needs a real display adapter for AppContainer shared textures.
    if ((description.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) ||
        (description.VendorId == 0x1414 && description.DeviceId == 0x8c)) {
        std::printf("  skipped: software adapter cannot run the hardware scene texture check\n");
        return;
    }
    ComPtr<ID3D11DeviceContext> context;
    device->GetImmediateContext(&context);
    const std::string xml = "<scene format=\"1\" name=\"Video input\"><base video=\"video.mp4\"/></scene>";
    std::vector<uint8_t> pixels(64 * 64 * 4, 0);
    for (size_t i = 3; i < pixels.size(); i += 4) pixels[i] = 255;
    const scene::Files files{{L"scene.xml", {xml.begin(), xml.end()}},
        {L"images/video.bmp", bitmap::Build(64, 64, pixels.data(), pixels.size())}};
    scene::Host host;
    CHECK(host.Start(device.Get(), files, 64, 64, false, &why, {true, 0, 1}));
    if (host.Failed(&why)) return;
    auto publish = [&](float r, float g, float b) {
        bool drawn = false;
        const bool uploaded = host.UpdateVideo([&](ID3D11Texture2D *texture) {
            ComPtr<ID3D11RenderTargetView> view;
            if (FAILED(device->CreateRenderTargetView(texture, nullptr, &view))) return;
            const float colour[]{r, g, b, 1};
            context->ClearRenderTargetView(view.Get(), colour);
            drawn = true;
        });
        CHECK(uploaded && drawn);
        scene::Inputs in;
        return host.SetRunning(false, in, 60, true);
    };
    auto acknowledged = [&](LONG requested, uint8_t r, uint8_t g, uint8_t b) {
        const auto deadline = GetTickCount64() + 10000;
        while (GetTickCount64() < deadline && !host.Failed(&why)) {
            host.Poll();
            if (host.Ready() && host.FrameSequence() >= requested) {
                const auto bmp = host.Bitmap(&why);
                CHECK(bmp.size() == bitmap::kHeaderBytes + 64 * 64 * 4);
                if (bmp.size() != bitmap::kHeaderBytes + 64 * 64 * 4) return;
                // A black poster, an old input or a blend with it must not count as this frame.
                for (size_t i = bitmap::kHeaderBytes; i < bmp.size(); i += 4) {
                    if (bmp[i] != b || bmp[i+1] != g || bmp[i+2] != r || bmp[i+3] != 255) {
                        CHECK(false);
                        return;
                    }
                }
                return;
            }
            Sleep(2);
        }
        std::wprintf(L"  scene frame failed: %s\n", why.c_str());
        CHECK(false);
    };
    // Publish immediately, before waiting for the initial poster or child readiness.
    acknowledged(publish(1, 0, 0), 255, 0, 0);
    acknowledged(publish(0, 0, 1), 0, 0, 255);
    acknowledged(publish(1, 0, 0), 255, 0, 0);
}
