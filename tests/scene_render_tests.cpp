// The scene importer and renderer that scene.exe runs, here in the test process on WARP: what a
// node tree looks like, pixel by pixel. The sandbox around them is tested in scene_runtime_tests.
#include "check.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <cmath>
#include <cstdio>
#include "animelogon/image.h"
#include "animelogon/scenegraph.h"
#include "animelogon/texture.h"
#include "renderer.h"
#include "texencode.h"
using namespace animelogon;
using Microsoft::WRL::ComPtr;
namespace sg = animelogon::scenegraph;
namespace {
ComPtr<ID3D11Device> Warp() {
    ComPtr<ID3D11Device> device;
    D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                      &device, nullptr, nullptr);
    return device;
}
std::vector<uint8_t> Bytes(const std::string &s) { return {s.begin(), s.end()}; }
// A w x h picture of one colour (0xAARRGGBB), as an exact texture file.
std::vector<uint8_t> Solid(ID3D11Device *device, uint32_t w, uint32_t h, uint32_t argb, scene::TextureKind kind = scene::TextureKind::Exact) {
    std::vector<uint8_t> bgra((size_t)w * h * 4);
    for (size_t i = 0; i < bgra.size(); i += 4) {
        bgra[i] = argb & 255; bgra[i + 1] = (argb >> 8) & 255; bgra[i + 2] = (argb >> 16) & 255; bgra[i + 3] = argb >> 24;
    }
    std::vector<uint8_t> out;
    std::wstring why;
    scene::EncodeTexture(device, bgra.data(), w, h, (size_t)w * 4, kind, &out, &why);
    return out;
}
struct Picture {
    int width = 0, height = 0;
    std::vector<uint8_t> bgra;
    // B, G, R at a pixel.
    std::array<int, 3> At(int x, int y) const {
        const uint8_t *p = &bgra[((size_t)y * width + x) * 4];
        return {p[0], p[1], p[2]};
    }
};
bool Near(const std::array<int, 3> &got, int b, int g, int r, int tolerance = 3) {
    const bool ok = std::abs(got[0] - b) <= tolerance && std::abs(got[1] - g) <= tolerance && std::abs(got[2] - r) <= tolerance;
    if (!ok) std::printf("    got %d,%d,%d want %d,%d,%d\n", got[0], got[1], got[2], b, g, r);
    return ok;
}
// Imports `package` and draws it at `time` into a width x height picture.
struct Stage {
    ComPtr<ID3D11Device> device = Warp();
    scene::Renderer renderer;
    ComPtr<ID3D11Texture2D> output, staging;
    int width = 0, height = 0;
    std::wstring why;
    bool Open(const scene::Files &package, int w, int h, int videoMemory = 0) {
        scene::Files library;
        if (!device || !scene::Import(device.Get(), package, &library, &why) || !renderer.Open(device.Get(), library, w, h, videoMemory, &why)) {
            std::wprintf(L"    %s\n", why.c_str());
            return false;
        }
        width = w; height = h;
        D3D11_TEXTURE2D_DESC d{};
        d.Width = w; d.Height = h; d.MipLevels = d.ArraySize = 1; d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(device->CreateTexture2D(&d, nullptr, &output))) return false;
        d.BindFlags = 0; d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(device->CreateTexture2D(&d, nullptr, &staging));
    }
    Picture Draw(double time, const sg::Values &values = {}, float scale = 1) {
        scene::Inputs in;
        in.data.time = time;
        in.values = values;
        float ms = 0;
        Picture p;
        if (!renderer.Draw(in, nullptr, output.Get(), scale, &ms, &why)) { std::wprintf(L"    %s\n", why.c_str()); return p; }
        ComPtr<ID3D11DeviceContext> context;
        device->GetImmediateContext(&context);
        context->CopyResource(staging.Get(), output.Get());
        D3D11_MAPPED_SUBRESOURCE m{};
        if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &m))) return p;
        p.width = width; p.height = height;
        p.bgra.resize((size_t)width * height * 4);
        for (int y = 0; y < height; ++y) memcpy(&p.bgra[(size_t)y * width * 4], (uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)width * 4);
        context->Unmap(staging.Get(), 0);
        return p;
    }
};
std::string Scene(const std::string &body, int w = 100, int h = 50) {
    return "<scene format=\"2\" name=\"t\" width=\"" + std::to_string(w) + "\" height=\"" + std::to_string(h) + "\">" + body + "</scene>";
}
}  // namespace

TEST(SceneRendererDrawsFillsPicturesAndBlends) {
    Stage s;
    const std::string xml = Scene(
        "<assets><image id=\"green\" file=\"green.tex\" exact=\"true\"/><image id=\"blue\" file=\"blue.tex\" exact=\"true\"/></assets>"
        "<fill color=\"#FF0000\"/>"
        "<layer source=\"green\" x=\"10\" y=\"10\"/>"
        "<layer source=\"blue\" x=\"40\" y=\"10\" opacity=\"0.5\"/>"
        "<layer source=\"blue\" x=\"70\" y=\"10\" blend=\"add\"/>"
        "<fill x=\"0\" y=\"30\" width=\"100\" height=\"20\" colors=\"#000000 0%, #FFFFFF 100%\" gradient=\"horizontal\"/>");
    const scene::Files package{{L"scene.xml", Bytes(xml)}, {L"green.tex", Solid(s.device.Get(), 10, 10, 0xFF00FF00)},
                               {L"blue.tex", Solid(s.device.Get(), 10, 10, 0xFF0000FF)}};
    // The canvas is the stage at twice its size.
    CHECK(s.Open(package, 200, 100));
    const Picture p = s.Draw(0);
    CHECK(p.width == 200);
    if (p.width != 200) return;
    CHECK(Near(p.At(4, 4), 0, 0, 255));       // the red fill
    CHECK(Near(p.At(30, 30), 0, 255, 0));     // the green picture, 10 points from the corner
    CHECK(Near(p.At(90, 30), 128, 0, 128));   // half the blue over the red
    CHECK(Near(p.At(150, 30), 255, 0, 255));  // blue added to red
    // The gradient runs dark to light from left to right.
    CHECK(p.At(10, 80)[0] < 30 && p.At(190, 80)[0] > 225 && Near(p.At(100, 80), 128, 128, 128, 6));
}

TEST(SceneRendererIsAPureFunctionOfTime) {
    Stage s;
    const std::string xml = Scene(
        "<assets><image id=\"dot\" file=\"dot.tex\" exact=\"true\"/></assets><fill color=\"#202020\"/>"
        "<layer source=\"dot\" y=\"20\"><keys property=\"x\" duration=\"4\"><key at=\"0\" value=\"0\"/><key at=\"4\" value=\"90\"/></keys>"
        "<wave property=\"y\" amplitude=\"5\" period=\"3\"/></layer>");
    CHECK(s.Open({{L"scene.xml", Bytes(xml)}, {L"dot.tex", Solid(s.device.Get(), 10, 10, 0xFFFFFFFF)}}, 100, 50));
    const Picture a = s.Draw(1.25), b = s.Draw(3), c = s.Draw(1.25), d = s.Draw(0);
    CHECK(!a.bgra.empty() && a.bgra == c.bgra);
    CHECK(a.bgra != b.bgra && a.bgra != d.bgra);
    // At 2 s the keys are halfway: x = 45.
    const Picture half = s.Draw(2);
    CHECK(Near(half.At(50, 20), 255, 255, 255));
    CHECK(Near(half.At(40, 25), 32, 32, 32));
}

TEST(SceneRendererPlacesAnchorsPivotsAndGroups) {
    Stage s;
    const std::string xml = Scene(
        "<assets><image id=\"bar\" file=\"bar.tex\" exact=\"true\"/></assets><fill color=\"#000000\"/>"
        // The bottom-right corner sits at (100, 50): the picture fills the corner.
        "<layer id=\"corner\" source=\"bar\" x=\"100\" y=\"50\" anchor=\"bottom-right\"/>"
        // Turned a quarter about its centre: 20 x 4 becomes 4 x 20 around (30, 25).
        "<layer id=\"turned\" source=\"bar\" x=\"30\" y=\"25\" anchor=\"center\" rotation=\"90\"/>"
        // A group 10 points tall holding the 4-point-tall bar scales it by 2.5.
        "<group x=\"50\" y=\"0\" height=\"10\"><layer source=\"bar\"/></group>");
    CHECK(s.Open({{L"scene.xml", Bytes(xml)}, {L"bar.tex", Solid(s.device.Get(), 20, 4, 0xFFFFFFFF)}}, 100, 50));
    const Picture p = s.Draw(0);
    if (p.width != 100) { CHECK(false); return; }
    CHECK(Near(p.At(95, 48), 255, 255, 255) && Near(p.At(75, 48), 0, 0, 0) && Near(p.At(95, 44), 0, 0, 0));
    CHECK(Near(p.At(30, 17), 255, 255, 255) && Near(p.At(30, 33), 255, 255, 255) && Near(p.At(25, 25), 0, 0, 0));
    CHECK(Near(p.At(51, 1), 255, 255, 255) && Near(p.At(98, 9), 255, 255, 255) && Near(p.At(75, 12), 0, 0, 0));
}

TEST(SceneRendererMasksAndPinsToTheScreen) {
    Stage s;
    // Half white, half black: the mask lets the left half through.
    std::vector<uint8_t> bgra(20 * 20 * 4, 0);
    for (int y = 0; y < 20; ++y)
        for (int x = 0; x < 20; ++x) {
            uint8_t *q = &bgra[((size_t)y * 20 + x) * 4];
            q[0] = q[1] = q[2] = x < 10 ? 255 : 0; q[3] = 255;
        }
    std::vector<uint8_t> mask;
    std::wstring why;
    CHECK(scene::EncodeTexture(s.device.Get(), bgra.data(), 20, 20, 80, scene::TextureKind::Mask, &mask, &why));
    const std::string xml = Scene(
        "<assets><image id=\"white\" file=\"white.tex\" exact=\"true\"/><mask id=\"half\" file=\"half.tex\"/></assets>"
        "<fill color=\"#000000\"/>"
        "<layer source=\"white\" x=\"10\" y=\"10\" width=\"20\" height=\"20\" mask=\"half\"/>"
        // Pinned to the screen's top-right corner whatever part of the stage is cut away.
        "<layer source=\"white\" space=\"screen\" anchor=\"top-right\" width=\"5\" height=\"5\"/>", 100, 100);
    // A wide canvas cuts the square stage at the top and bottom.
    CHECK(s.Open({{L"scene.xml", Bytes(xml)}, {L"white.tex", Solid(s.device.Get(), 4, 4, 0xFFFFFFFF)}, {L"half.tex", mask}}, 200, 100));
    const Picture p = s.Draw(0);
    if (p.width != 200) { CHECK(false); return; }
    // The stage is 200 wide on the canvas (2 pixels a point) and its middle 100 points tall shows.
    // The masked square spans points 10..30, so pixels 20..60 across and (10 - 25) * 2 + ... above
    // the top: only its lower part shows, from y = 0 to 10.
    CHECK(Near(p.At(25, 5), 255, 255, 255) && Near(p.At(55, 5), 0, 0, 0));
    CHECK(Near(p.At(197, 2), 255, 255, 255) && Near(p.At(185, 2), 0, 0, 0));
}

TEST(SceneRendererKeepsTexturesWithinTheVideoMemoryLimit) {
    Stage s;
    const std::string xml = Scene("<assets><image id=\"big\" file=\"big.tex\" exact=\"true\"/></assets><layer source=\"big\" full=\"true\"/>");
    // 1024 x 1024 uncompressed with its mipmaps is over 5 MB; a 1 MB limit leaves out its finest levels.
    CHECK(s.Open({{L"scene.xml", Bytes(xml)}, {L"big.tex", Solid(s.device.Get(), 1024, 1024, 0xFF336699)}}, 100, 50, 1));
    CHECK(s.renderer.ReducedLevels() >= 2 && s.renderer.TextureBytes() <= 1024 * 1024);
    const Picture p = s.Draw(0);
    CHECK(!p.bgra.empty() && Near(p.At(50, 25), 0x99, 0x66, 0x33));
}

TEST(SceneRendererRunsLayerShadersWithTheirParameters) {
    Stage s;
    const std::string xml = Scene(
        "<settings><color id=\"tint\" label=\"Tint\" default=\"#00FF00\"/></settings>"
        "<assets><shader id=\"paint\" kind=\"layer\" file=\"paint.hlsl\"/></assets>"
        "<layer source=\"paint\" full=\"true\"><param slot=\"0\" value=\"$tint\"/></layer>");
    const std::string hlsl = "float4 main(float2 uv : TEXCOORD0) : SV_Target { return float4(param[0].rgb, 1); }";
    CHECK(s.Open({{L"scene.xml", Bytes(xml)}, {L"paint.hlsl", Bytes(hlsl)}}, 100, 50));
    CHECK(Near(s.Draw(0).At(50, 25), 0, 255, 0));
    CHECK(Near(s.Draw(0, {{L"tint", L"#0000FF"}}).At(50, 25), 255, 0, 0));
}

TEST(SceneImportMakesTexturesAndCompiledShadersUnderDerivedNames) {
    const auto device = Warp();
    image::Picture picture;
    picture.width = picture.height = 8;
    picture.bgra.assign(8 * 8 * 4, 200);
    std::vector<uint8_t> png;
    std::wstring why;
    CHECK(image::EncodePng(8, 8, picture.bgra.data(), picture.bgra.size(), &png, &why));
    const std::string xml = Scene(
        "<assets><image id=\"sky\" file=\"art/Sky.png\"/><mask id=\"m\" file=\"art/sky.png\"/>"
        "<frames id=\"blink\"><frame file=\"art/sky.png\"/><frame file=\"art/Sky.png\"/></frames>"
        "<shader id=\"glow\" kind=\"layer\" file=\"fx/glow.hlsl\"/></assets>"
        "<layer source=\"sky\" full=\"true\"/><layer source=\"blink\"/><layer source=\"glow\" opacity=\"0.5\" mask=\"m\"/>");
    const std::string hlsl = "float4 main() : SV_Target { return float4(time, 0, 0, 1); }";
    scene::Files library;
    CHECK(scene::Import(device.Get(), {{L"scene.xml", Bytes(xml)}, {L"art/Sky.png", png}, {L"art/sky.png", png}, {L"fx/glow.hlsl", Bytes(hlsl)}},
                        &library, &why));
    if (!why.empty()) std::wprintf(L"    %s\n", why.c_str());
    sg::Document stored;
    const auto *text = sg::Find(library, L"scene.xml");
    CHECK(text && sg::Parse(std::string_view((const char *)text->bytes.data(), text->bytes.size()), sg::Mode::Stored, &stored, &why));
    if (!why.empty()) std::wprintf(L"    %s\n", why.c_str());
    const std::vector<std::wstring> expected{L"textures/sky.tex", L"textures/m.tex", L"textures/blink-0.tex", L"textures/blink-1.tex",
                                             L"shaders/glow.hlsl", L"shaders/glow.dxbc"};
    CHECK(sg::LibraryFiles(stored) == expected && library.size() == expected.size() + 1);
    texture::Info info;
    const auto *sky = sg::Find(library, L"textures/sky.tex"), *m = sg::Find(library, L"textures/m.tex");
    CHECK(sky && texture::Read(sky->bytes.data(), sky->bytes.size(), &info, nullptr, &why) && info.format == texture::Format::Bc7 && info.usedWidth == 8);
    CHECK(m && texture::Read(m->bytes.data(), m->bytes.size(), &info, nullptr, &why) && info.format == texture::Format::Bc4);
    // A shader outside the contract, a mask given as a colour texture, and a missing file are refused.
    const std::string bad = "Texture2D<float4> other : register(t3); float4 main() : SV_Target { return other.Load(int3(0,0,0)); }";
    CHECK(!scene::Import(device.Get(), {{L"scene.xml", Bytes(xml)}, {L"art/Sky.png", png}, {L"art/sky.png", png}, {L"fx/glow.hlsl", Bytes(bad)}},
                         &library, &why));
    const std::string asMask = Scene("<assets><mask id=\"m\" file=\"m.tex\"/></assets><fill mask=\"m\"/>");
    CHECK(!scene::Import(device.Get(), {{L"scene.xml", Bytes(asMask)}, {L"m.tex", Solid(device.Get(), 4, 4, 0xFFFFFFFF)}}, &library, &why));
    CHECK(!scene::Import(device.Get(), {{L"scene.xml", Bytes(xml)}, {L"art/Sky.png", png}}, &library, &why));
}
