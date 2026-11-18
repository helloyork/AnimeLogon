#include "particles.h"
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include "animelogon/text.h"
using Microsoft::WRL::ComPtr;
namespace animelogon::scene {
namespace sg = scenegraph;
namespace {

const wchar_t *const kEmits[] = {L"top", L"bottom", L"left", L"right", L"point", L"line", L"area"};

// One instance per candidate particle. Candidates are numbered and born evenly, as many a second
// as can be alive at once over a lifetime (max / lifetime); a share of them, rate / that, is kept,
// so the rate and the density setting thin the same stream instead of renumbering it. A candidate
// that is not kept, not born yet or already gone collapses to nothing off the target.
//
// Each particle drifts with the wind, carries its own velocity (slowed by drag towards the speed
// gravity alone would give it, or under plain gravity without drag), sways, spins, and changes
// size, opacity and colour over its life.
constexpr char kShader[] = R"(
cbuffer Emitter : register(b0) {
    float4 toTarget0, toTarget1;  // target pixel = (dot(toTarget0.xyz, (x, y, 1)), dot(toTarget1.xyz, (x, y, 1)))
    float4 target;                // xy: the target's size in pixels; zw: the box's size in its units
    float4 timing;                // x: age of the first candidate; y: candidates a second; z: share kept; w: lifetime
    float4 motion;                // x: speed; y: angle (radians); z: spread (radians); w: gravity
    float4 motion2;               // x: wind; y: drag; z: spin (degrees a second); w: sway
    float4 look;                  // x: size; y: size at the end; z: opacity at the end; w: fade
    float4 color, colorEnd;       // straight
    float4 extra;                 // x: twinkle; y: stretch; z: where they are born (0 top .. 6 area); w: frames
    float4 art;                   // xy: the used part of the texture; z: 1 with a texture; w: 1 when it is premultiplied
    float4 maskRect;              // xy: the used part of the mask; z: 1 with a mask; w: the node's opacity
    float4 frameNow;              // x: the frame this draw covers; y: frames a second
    uint4 numbers;                // x: the first candidate's number; y: seed
};
Texture2D<float4> picture : register(t0);
Texture2D<float4> maskPicture : register(t1);
SamplerState linearClamp : register(s0);
static const float kTau = 6.28318531;
struct V {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float2 box : TEXCOORD1;
    float4 color : COLOR0;
};
float Rand(uint n, uint salt) {
    uint h = n * 747796405u + salt * 2891336453u + numbers.y * 1181783497u;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h * (1.0 / 4294967296.0);
}
V Vertex(uint corner : SV_VertexID, uint id : SV_InstanceID) {
    V o;
    o.position = float4(-10, -10, 0, 1); o.uv = 0; o.box = 0; o.color = 0;
    uint n = numbers.x + id;
    if (Rand(n, 1) >= timing.z) return o;
    float life = timing.w * (.8 + .4 * Rand(n, 2));
    float age = timing.x - (id + Rand(n, 3)) / timing.y;
    if (age < 0 || age >= life) return o;
    float f = age / life;
    if (extra.w > 1 && fmod(floor(age * frameNow.y), extra.w) != frameNow.x) return o;
    float W = target.z, H = target.w;
    float size = lerp(look.x, look.y, f) * (.7 + .6 * Rand(n, 4));
    float e = extra.z, r5 = Rand(n, 5), r6 = Rand(n, 6);
    float2 at = e < .5 ? float2(r5 * W, -size) : e < 1.5 ? float2(r5 * W, H + size) : e < 2.5 ? float2(-size, r5 * H) :
                e < 3.5 ? float2(W + size, r5 * H) : e < 4.5 ? float2(W, H) * .5 : e < 5.5 ? float2(r5 * W, H * .5) : float2(r5 * W, r6 * H);
    float theta = motion.y + (Rand(n, 7) - .5) * motion.z;
    float2 u0 = motion.x * (.8 + .4 * Rand(n, 8)) * float2(cos(theta), sin(theta));
    float drag = motion2.y, g = motion.w;
    float2 pos, vel;
    if (drag > 1e-4) {
        float2 terminal = float2(0, g / drag);
        float d = exp(-drag * age);
        pos = terminal * age + (u0 - terminal) * (1 - d) / drag;
        vel = terminal + (u0 - terminal) * d;
    } else {
        pos = u0 * age + float2(0, .5 * g * age * age);
        vel = u0 + float2(0, g * age);
    }
    pos.x += motion2.x * age;
    vel.x += motion2.x;
    float period = 2 + 3 * Rand(n, 9), phase = Rand(n, 10) * kTau;
    pos += motion2.w * float2(sin(kTau * age / period + phase), .5 * sin(kTau * age / (period * 1.3) + phase * 2));
    pos += at;
    float alpha = lerp(1, look.z, f);
    if (look.w > 0) alpha *= saturate(f / look.w) * saturate((1 - f) / look.w);
    alpha *= 1 - extra.x * (.5 + .5 * sin(kTau * age * (.5 + 1.5 * Rand(n, 11)) + Rand(n, 12) * kTau));
    // Streaks lie along the motion; anything else turns by its spin.
    float speedNow = length(vel), len = size;
    float2 down;
    if (extra.y > 0 && speedNow > 1e-3) {
        down = vel / speedNow;
        len = max(size, speedNow * extra.y);
    } else {
        float turn = radians(Rand(n, 13) * 360 + motion2.z * (.5 + Rand(n, 14)) * (Rand(n, 15) < .5 ? -1 : 1) * age);
        down = float2(-sin(turn), cos(turn));
    }
    float2 right = float2(down.y, -down.x);
    float2 q = float2(corner & 1, corner >> 1) - .5;
    float2 p = pos + right * q.x * size + down * q.y * len;
    float2 px = float2(dot(toTarget0.xyz, float3(p, 1)), dot(toTarget1.xyz, float3(p, 1)));
    o.position = float4(px / target.xy * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = q + .5;
    o.box = p / float2(W, H);
    float4 c = lerp(color, colorEnd, f);
    c.a *= alpha * maskRect.w;
    o.color = float4(c.rgb * c.a, c.a);
    return o;
}
float4 Pixel(V v) : SV_Target {
    float4 c;
    if (art.z > .5) {
        c = picture.Sample(linearClamp, v.uv * art.xy);
        if (art.w < .5) c.rgb *= c.a;
    } else if (extra.y > 0) {
        // A streak: soft across, tapering at both ends.
        float2 d = abs(v.uv - .5) * 2;
        c = saturate(1 - d.x) * saturate((1 - d.y) * 3);
    } else {
        // A soft dot.
        c = saturate((1 - length(v.uv - .5) * 2) * 1.5);
    }
    c *= v.color;
    if (maskRect.z > .5) c *= any(v.box < 0) || any(v.box > 1) ? 0 : maskPicture.Sample(linearClamp, v.box * maskRect.xy).r;
    return c;
}
)";
struct Constants {
    float toTarget0[4]{}, toTarget1[4]{}, target[4]{}, timing[4]{}, motion[4]{}, motion2[4]{}, look[4]{};
    float color[4]{}, colorEnd[4]{}, extra[4]{}, art[4]{}, maskRect[4]{}, frameNow[4]{};
    uint32_t numbers[4]{};
};

bool Fail(std::wstring *why, const std::wstring &s) { if (why) *why = s; return false; }
bool Compile(const char *entry, const char *profile, ComPtr<ID3DBlob> *blob, std::wstring *why) {
    ComPtr<ID3DBlob> error;
    if (FAILED(D3DCompile(kShader, strlen(kShader), "particles", nullptr, nullptr, entry, profile,
                          D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob->ReleaseAndGetAddressOf(), &error)))
        return Fail(why, error ? FromUtf8(std::string_view((const char *)error->GetBufferPointer(), std::min<size_t>(error->GetBufferSize(), 1024)))
                               : L"cannot compile the particles");
    return true;
}
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
    ComPtr<ID3DBlob> v, p;
    if (!Compile("Vertex", "vs_5_0", &v, why) || !Compile("Pixel", "ps_5_0", &p, why)) return false;
    if (FAILED(device->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &s->vertex)) ||
        FAILED(device->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &s->pixel)))
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
