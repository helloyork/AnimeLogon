#include "particles.h"
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include "animelogon/text.h"
// The particles' shaders (shaders/particles.hlsl), compiled when the program is built.
#include "scene_particles_ps.h"
#include "scene_particles_vs.h"
using Microsoft::WRL::ComPtr;
namespace animelogon::scene {
namespace sg = scenegraph;
namespace {

const wchar_t *const kEmits[] = {L"top", L"bottom", L"left", L"right", L"point", L"line", L"area"};

struct Constants {
    float toTarget0[4]{}, toTarget1[4]{}, target[4]{}, timing[4]{}, motion[4]{}, motion2[4]{}, look[4]{};
    float color[4]{}, colorEnd[4]{}, extra[4]{}, art[4]{}, maskRect[4]{}, frameNow[4]{};
    uint32_t numbers[4]{};
};

bool Fail(std::wstring *why, const std::wstring &s) { if (why) *why = s; return false; }
void Put(float *to, std::initializer_list<float> v) { std::copy(v.begin(), v.end(), to); }
}  // namespace

struct ParticleDrawer::State {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> pixel;
    ComPtr<ID3D11Buffer> constants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11RasterizerState> raster;
};

ParticleDrawer::ParticleDrawer() = default;
ParticleDrawer::~ParticleDrawer() = default;

bool ParticleDrawer::Open(ID3D11Device *device, std::wstring *why) {
    auto s = std::make_unique<State>();
    s->device = device;
    device->GetImmediateContext(&s->context);
    if (FAILED(device->CreateVertexShader(g_particles_vs, sizeof(g_particles_vs), nullptr, &s->vertex)) ||
        FAILED(device->CreatePixelShader(g_particles_ps, sizeof(g_particles_ps), nullptr, &s->pixel)))
        return Fail(why, L"cannot create the particle shaders");
    D3D11_BUFFER_DESC cb{};
    cb.Usage = D3D11_USAGE_DEFAULT; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER; cb.ByteWidth = sizeof(Constants);
    if (FAILED(device->CreateBuffer(&cb, nullptr, &s->constants))) return Fail(why, L"cannot create the particle constants");
    D3D11_SAMPLER_DESC samp{};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&samp, &s->sampler))) return Fail(why, L"cannot create the particle sampler");
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rd, &s->raster))) return Fail(why, L"cannot create the particle rasterizer state");
    s_ = std::move(s);
    return true;
}

void ParticleDrawer::Draw(const sg::Placed &p, const std::vector<ParticlePicture> &frames, const ParticleTarget &t, double time, double density) {
    auto &s = *s_;
    const auto value = [&](const wchar_t *k) {
        const auto it = p.values.find(k);
        return it == p.values.end() ? std::array<float, 4>{} : it->second;
    };
    const auto num = [&](const wchar_t *k) { return (double)value(k)[0]; };
    // Size, opacity and colour at the end of a particle's life default to what it starts with.
    const auto written = [&](const wchar_t *k) { const auto it = p.text.find(k); return it != p.text.end() && !it->second.empty(); };
    const double lifetime = std::max(0.1, num(L"lifetime"));
    const double most = std::clamp(num(L"max"), 1.0, (double)sg::kMaxParticles);
    const double candidates = most / lifetime;
    const double keep = std::min(1.0, std::max(0.0, num(L"rate")) * std::clamp(density, 0.0, 1.0) / candidates);
    if (keep <= 0 || t.boxWidth <= 0 || t.boxHeight <= 0) return;
    // The oldest candidate that can still be alive lived 1.2 lifetimes (lives vary by a fifth).
    const double longest = lifetime * 1.2;
    const auto first = (int64_t)std::floor((time - longest) * candidates);
    Constants c;
    Put(c.toTarget0, {(float)t.a, (float)t.c, (float)t.e, 0});
    Put(c.toTarget1, {(float)t.b, (float)t.d, (float)t.f, 0});
    Put(c.target, {(float)t.width, (float)t.height, (float)t.boxWidth, (float)t.boxHeight});
    Put(c.timing, {(float)(time - first / candidates), (float)candidates, (float)keep, (float)lifetime});
    constexpr double kRadians = 3.14159265358979323846 / 180;
    Put(c.motion, {(float)num(L"speed"), (float)(num(L"angle") * kRadians), (float)(num(L"spread") * kRadians), (float)num(L"gravity")});
    Put(c.motion2, {(float)num(L"wind"), (float)num(L"drag"), (float)num(L"spin"), (float)num(L"sway")});
    const double size = num(L"size");
    Put(c.look, {(float)size, (float)(written(L"size-end") ? num(L"size-end") : size), (float)(written(L"opacity-end") ? num(L"opacity-end") : 1),
                 (float)num(L"fade")});
    const auto color = value(L"color");
    const auto colorEnd = written(L"color-end") ? value(L"color-end") : color;
    std::copy(color.begin(), color.end(), c.color);
    std::copy(colorEnd.begin(), colorEnd.end(), c.colorEnd);
    int emit = 0;
    if (const auto it = p.text.find(L"emit"); it != p.text.end())
        for (int i = 0; i < (int)std::size(kEmits); ++i) if (it->second == kEmits[i]) emit = i;
    Put(c.extra, {(float)num(L"twinkle"), (float)num(L"stretch"), (float)emit, (float)frames.size()});
    Put(c.maskRect, {t.maskU, t.maskV, t.mask ? 1.f : 0.f, t.opacity});
    c.numbers[0] = (uint32_t)first;
    c.numbers[1] = (uint32_t)num(L"seed");
    const auto count = (UINT)std::ceil(longest * candidates) + 2;

    const D3D11_VIEWPORT vp{0, 0, (float)t.width, (float)t.height, 0, 1};
    s.context->RSSetViewports(1, &vp);
    s.context->RSSetState(s.raster.Get());
    ID3D11RenderTargetView *rtv = t.target;
    s.context->OMSetRenderTargets(1, &rtv, nullptr);
    const float factor[4]{};
    s.context->OMSetBlendState(t.blend, factor, 0xFFFFFFFF);
    s.context->IASetInputLayout(nullptr);
    s.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    s.context->VSSetShader(s.vertex.Get(), nullptr, 0);
    s.context->PSSetShader(s.pixel.Get(), nullptr, 0);
    ID3D11Buffer *cb = s.constants.Get();
    s.context->VSSetConstantBuffers(0, 1, &cb);
    s.context->PSSetConstantBuffers(0, 1, &cb);
    ID3D11SamplerState *sampler = s.sampler.Get();
    s.context->PSSetSamplers(0, 1, &sampler);
    // A frame sequence is drawn once per frame, each draw keeping the particles showing that frame.
    Put(c.frameNow, {0, (float)std::max(1.0, num(L"fps"))});
    const size_t draws = std::max<size_t>(1, frames.size());
    for (size_t i = 0; i < draws; ++i) {
        const ParticlePicture *art = frames.empty() ? nullptr : &frames[i];
        Put(c.art, {art ? art->u : 1.f, art ? art->v : 1.f, art && art->view ? 1.f : 0.f, art && art->premultiplied ? 1.f : 0.f});
        c.frameNow[0] = (float)i;
        s.context->UpdateSubresource(s.constants.Get(), 0, nullptr, &c, 0, 0);
        ID3D11ShaderResourceView *views[2]{art ? art->view : nullptr, t.mask};
        s.context->PSSetShaderResources(0, 2, views);
        s.context->DrawInstanced(4, count, 0, 0);
    }
    ID3D11ShaderResourceView *none[2]{};
    s.context->PSSetShaderResources(0, 2, none);
}

}  // namespace animelogon::scene
