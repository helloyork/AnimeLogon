#include "effects.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <list>
#include "animelogon/text.h"
using Microsoft::WRL::ComPtr;
namespace animelogon::scene {
namespace sg = scenegraph;
namespace {

// The built-in effects in the order the shader numbers them.
const wchar_t *const kKinds[] = {L"bend", L"breathe", L"wave", L"rings", L"flow", L"pan", L"jitter", L"glow", L"rays",
                                 L"sheen", L"flicker", L"grade", L"tint", L"blur", L"haze", L"split", L"grain"};
enum Kind { kBend, kBreathe, kWave, kRings, kFlow, kPan, kJitter, kGlow, kRays, kSheen, kFlicker, kGrade, kTint, kBlur, kHaze, kSplit, kGrain };

// Every pass draws a full-screen triangle over its viewport. Distances an author writes are
// fractions of the box's height (bend's of the box's length from its root); the shader turns them
// into the picture's pixels. Pictures are premultiplied by alpha throughout.
constexpr char kVertex[] = R"(
struct V { float2 uv : TEXCOORD0; float4 position : SV_Position; };
V main(uint id : SV_VertexID) {
    V v; v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1); return v;
})";
constexpr char kPixel[] = R"(
cbuffer Effect : register(b0) {
    float4 size;      // xy: the picture's used size in pixels; zw: 1 / its texture's size
    float4 box;       // xy: the box's top-left in the picture's pixels; zw: the box's size
    float4 p0, p1;    // the effect's parameters
    float4 tint;      // its colour, straight
    float4 maskRect;  // xy: the used part of the mask's texture; z: 1 when the effect has a mask
    float4 lowSize;   // xy: the used size of the low-resolution picture being read; zw: 1 / its texture's size
    float4 level;     // x: picture pixels per low-resolution pixel; y: blur sigma; zw: blur direction
    float time, kind, extend, seed;
};
Texture2D<float4> picture : register(t0);
Texture2D<float4> maskPicture : register(t1);
Texture2D<float4> low : register(t2);
SamplerState linearClamp : register(s0);
static const float kTau = 6.28318531;
static const float3 kLuma = float3(.2126, .7152, .0722);

// The picture at a position in its pixels: transparent outside it, or for a picture that covers the
// screen, the box's nearest edge pixel.
float4 At(float2 px) {
    if (extend > .5) px = clamp(px, box.xy + .5, box.xy + box.zw - .5);
    else {
        if (any(px < 0) || any(px > size.xy)) return 0;
        px = clamp(px, .5, size.xy - .5);
    }
    return picture.SampleLevel(linearClamp, px * size.zw, 0);
}
// The effect's mask over the box; outside the box nothing is masked in.
float MaskAt(float2 uv) {
    if (maskRect.z < .5) return 1;
    if (any(uv < 0) || any(uv > 1)) return 0;
    return maskPicture.SampleLevel(linearClamp, uv * maskRect.xy, 0).r;
}
// The low-resolution picture under a position in the picture's pixels.
float4 Low(float2 px) {
    float2 q = clamp(px / level.x, .5, lowSize.xy - .5);
    return low.SampleLevel(linearClamp, q * lowSize.zw, 0);
}
float Hash(float2 p) {
    uint2 q = asuint(int2(floor(p)));
    uint h = q.x * 1597334677u ^ q.y * 3812015801u;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h * (1.0 / 4294967296.0);
}
float Noise(float2 p) {
    float2 i = floor(p), f = p - i, u = f * f * (3 - 2 * f);
    return lerp(lerp(Hash(i), Hash(i + float2(1, 0)), u.x), lerp(Hash(i + float2(0, 1)), Hash(i + 1), u.x), u.y);
}

float4 Effect(float2 unused : TEXCOORD0, float4 position : SV_Position) : SV_Target {
    float2 px = position.xy;
    float2 uv = (px - box.xy) / box.zw;
    float m = MaskAt(uv);
    float W = box.z, H = box.w;
    int k = (int)kind;
    [branch] if (k == 0) {  // bend: sways more the farther from its root, the tip lagging by `waves`
        float root = p0.z;
        float s = saturate(root < .5 ? uv.y : root < 1.5 ? 1 - uv.y : root < 2.5 ? uv.x : 1 - uv.x);
        float d = p0.x * .12 * (root < 1.5 ? H : W) * s * s * sin(kTau * (time / p0.y - p0.w * s)) * m;
        return At(px - (root < 1.5 ? float2(d, 0) : float2(0, d)));
    }
    [branch] if (k == 1) {  // breathe: grows about its pivot and back, more upward than sideways
        float g = p0.x * (.5 - .5 * cos(kTau * time / p0.y)) * m;
        float2 pivot = box.xy + p1.xy * box.zw;
        return At(pivot + (px - pivot) / (1 + g * float2(.5, 1)));
    }
    [branch] if (k == 2) {  // wave: a sine travelling across or down
        if (p0.w < .5) return At(px - float2(0, p0.x * H * sin(kTau * ((px.x - box.x) / H / p0.y - time / p0.z)) * m));
        return At(px - float2(p0.x * H * sin(kTau * ((px.y - box.y) / H / p0.y - time / p0.z)) * m, 0));
    }
    [branch] if (k == 3) {  // rings: ripples spreading from a centre
        float2 d = px - (box.xy + p0.xy * box.zw);
        float r = length(d) / H;
        float a = p0.z * H * sin(kTau * (r / p0.w - p1.x * time)) * smoothstep(0, p0.w * .5, r) * m;
        return At(px - (r > 0 ? d / (r * H) : 0) * a);
    }
    [branch] if (k == 4) {  // flow: streaks of disturbance running along a direction
        float a = radians(p0.x);
        float2 dir = float2(cos(a), sin(a)), across = float2(-dir.y, dir.x);
        float2 P = (px - box.xy) / H;
        float2 q = float2((dot(P, dir) - p0.y * time) * 4, dot(P, across) * 16) + seed;
        float2 off = dir * (Noise(q) * 2 - 1) + across * .3 * (Noise(q + 31.7) * 2 - 1);
        return At(px - off * p0.z * H * m);
    }
    [branch] if (k == 5) {  // pan: the picture scrolls within its box and wraps around
        if (any(uv < 0) || any(uv > 1)) return At(px);
        float2 moved = frac(uv - p0.xy * time * H / box.zw);
        return lerp(At(px), At(box.xy + moved * box.zw), m);
    }
    [branch] if (k == 6) {  // jitter: jumps to a new nearby place `rate` times a second
        float step = floor(time * p0.y);
        float2 j = float2(Hash(float2(step, seed)), Hash(float2(seed, step + 7))) * 2 - 1;
        return At(px - j * p0.x * H * m);
    }
    [branch] if (k == 7) {  // glow: the bright parts, blurred, added as light
        float4 c = At(px);
        return float4(c.rgb + Low(px).rgb * tint.rgb * tint.a * p0.x, c.a);
    }
    [branch] if (k == 8) {  // rays: the bright parts streaked away from a centre, added as light
        float4 c = At(px);
        return float4(c.rgb + Low(px).rgb * tint.rgb * tint.a * p0.z, c.a);
    }
    [branch] if (k == 9) {  // sheen: a band of light crossing in the first third of each period
        float a = radians(p0.x);
        float2 dir = float2(cos(a), sin(a));
        float s = dot((px - box.xy - box.zw * .5) / H, dir);
        float reach = .5 * (abs(dir.x) * W + abs(dir.y) * H) / H + p0.y;
        float f = frac(time / p0.z) * 3;
        float band = f < 1 ? 1 - smoothstep(0, p0.y * .5, abs(s - lerp(-reach, reach, f))) : 0;
        float4 c = At(px);
        return float4(c.rgb + tint.rgb * tint.a * p0.w * band * c.a * m, c.a);
    }
    [branch] if (k == 10) {  // flicker: dims and recovers unevenly
        float4 c = At(px);
        return float4(c.rgb * (1 - p0.x * Noise(float2(time * p0.y, seed)) * m), c.a);
    }
    [branch] if (k == 11) {  // grade: brightness, contrast, saturation and warmth
        float4 c = At(px);
        if (c.a <= 0) return c;
        float3 v = (c.rgb / c.a - .5) * p0.y + .5 + p0.x;
        v = lerp(dot(v, kLuma), v, p0.z) + p0.w * float3(.12, .02, -.12);
        return float4(lerp(c.rgb, saturate(v) * c.a, m), c.a);
    }
    [branch] if (k == 12) {  // tint: shades of one colour
        float4 c = At(px);
        return float4(lerp(c.rgb, dot(c.rgb, kLuma) * tint.rgb, p0.x * tint.a * m), c.a);
    }
    [branch] if (k == 13) return lerp(At(px), Low(px), m);  // blur
    [branch] if (k == 14) {  // haze: rising shimmer
        float2 P = (px - box.xy) / H / p0.y;
        P.y += p0.z * time / p0.y;
        float2 d = float2(Noise(P + seed), Noise(P + seed + 19.1)) * 2 - 1;
        return At(px - d * float2(1, .5) * p0.x * H * m);
    }
    [branch] if (k == 15) {  // split: red and blue pulled apart
        float a = radians(p0.y);
        float2 o = float2(cos(a), sin(a)) * p0.x * H * m;
        float4 r = At(px - o), g = At(px), b = At(px + o);
        return float4(r.r, g.g, b.b, max(max(r.a, g.a), b.a));
    }
    [branch] if (k == 16) {  // grain: fresh noise 24 times a second
        float frame = fmod(floor(time * 24), 4096);
        float n = Hash(floor(px / max(p0.y, .5)) + frame * float2(37, 91) + seed) - .5;
        float4 c = At(px);
        return float4(max(0, c.rgb + n * p0.x * c.a * m), c.a);
    }
    return At(px);
}

// The first low-resolution picture: the average of the picture's pixels under each one, keeping
// only the bright parts for a glow (above its threshold) and for rays, where the mask lets them.
float4 Bright(float2 px) {
    float4 c = At(px);
    int k = (int)kind;
    if (k == 13) return c;
    float l = dot(c.rgb, kLuma) / max(c.a, 1e-4);
    float w = k == 7 ? saturate((l - p0.z) / max(1 - p0.z, 1e-3)) : smoothstep(.3, 1, l);
    return c * w * MaskAt((px - box.xy) / box.zw);
}
float4 Prefilter(float2 unused : TEXCOORD0, float4 position : SV_Position) : SV_Target {
    float f = level.x;
    if (f < 1.5) return Bright(position.xy);
    float2 corner = (position.xy - .5) * f + 1;
    int n = (int)(f / 2);
    float4 sum = 0;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) sum += Bright(corner + 2 * float2(x, y));
    return sum / (n * n);
}
// Half the size: each pixel the average of the 2 x 2 below it.
float4 Down(float2 unused : TEXCOORD0, float4 position : SV_Position) : SV_Target {
    float2 q = clamp((position.xy - .5) * 2 + 1, .5, lowSize.xy - .5);
    return low.SampleLevel(linearClamp, q * lowSize.zw, 0);
}
// One direction of a Gaussian blur.
float4 Blur(float2 unused : TEXCOORD0, float4 position : SV_Position) : SV_Target {
    float sigma = max(level.y, .1);
    int r = min(16, (int)ceil(sigma * 3));
    float4 sum = 0;
    float total = 0;
    [loop] for (int i = -r; i <= r; ++i) {
        float w = exp(-.5 * i * i / (sigma * sigma));
        float2 q = clamp(position.xy + level.zw * i, .5, lowSize.xy - .5);
        sum += low.SampleLevel(linearClamp, q * lowSize.zw, 0) * w;
        total += w;
    }
    return sum / total;
}
// Each pixel gathers the bright parts on the way to the centre, the nearer ones counting more.
float4 Rays(float2 unused : TEXCOORD0, float4 position : SV_Position) : SV_Target {
    float2 c = (box.xy + p0.xy * box.zw) / level.x;
    float2 d = (c - position.xy) * saturate(p0.w) / 32;
    float4 sum = 0;
    float weight = 1, total = 0;
    [loop] for (int i = 0; i < 32; ++i) {
        float2 q = clamp(position.xy + d * i, .5, lowSize.xy - .5);
        sum += low.SampleLevel(linearClamp, q * lowSize.zw, 0) * weight;
        total += weight;
        weight *= .97;
    }
    return sum / total;
}
// The mask over a box-sized picture, for an author's effect shader to read at its own uv.
float4 MaskCopy(float2 unused : TEXCOORD0, float4 position : SV_Position) : SV_Target {
    return MaskAt(position.xy / size.xy);
}
)";
struct Constants {
    float size[4]{}, box[4]{}, p0[4]{}, p1[4]{}, tint[4]{}, maskRect[4]{1, 1, 0, 0}, lowSize[4]{}, level[4]{1, 0, 0, 0};
    float time = 0, kind = 0, extend = 0, seed = 0;
};

bool Fail(std::wstring *why, const std::wstring &s) { if (why) *why = s; return false; }
bool Compile(const char *source, const char *entry, const char *profile, ComPtr<ID3DBlob> *blob, std::wstring *why) {
    ComPtr<ID3DBlob> error;
    if (FAILED(D3DCompile(source, strlen(source), "effects", nullptr, nullptr, entry, profile,
                          D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob->ReleaseAndGetAddressOf(), &error)))
        return Fail(why, error ? FromUtf8(std::string_view((const char *)error->GetBufferPointer(), std::min<size_t>(error->GetBufferSize(), 1024)))
                               : L"cannot compile the scene effects");
    return true;
}
int KindOf(const std::wstring &type) {
    for (int i = 0; i < (int)std::size(kKinds); ++i) if (type == kKinds[i]) return i;
    return -1;
}
std::array<float, 4> Get(const sg::PlacedEffect &pe, const wchar_t *name) {
    const auto it = pe.values.find(name);
    return it == pe.values.end() ? std::array<float, 4>{} : it->second;
}
void Put(float *to, std::initializer_list<float> v) { std::copy(v.begin(), v.end(), to); }
}  // namespace

ShaderConstants ShaderInputs(const sg::Inputs &data, double time, int width, int height,
                             const std::map<std::wstring, std::array<float, 4>> &values, const std::wstring &prefix) {
    ShaderConstants c;
    c.time = (float)time; c.resolution[0] = (float)width; c.resolution[1] = (float)height; c.day = (float)data.day;
    c.battery = (float)data.battery; c.progress = (float)data.progress; c.wake = (float)data.wake;
    c.playing = data.mediaState == L"playing" ? 2.f : data.mediaState == L"paused" ? 1.f : 0.f;
    for (int i = 0; i < 4; ++i) c.audio[i] = (float)data.audio[i];
    c.month = (float)data.month;
    for (const auto &[k, v] : values) {
        if (k.size() <= prefix.size() || k.compare(0, prefix.size(), prefix)) continue;
        wchar_t *end = nullptr;
        const unsigned long n = wcstoul(k.c_str() + prefix.size(), &end, 10);
        if (end && !*end && n < 16) std::copy(v.begin(), v.end(), c.param[n]);
    }
    return c;
}

bool Surface::Fit(ID3D11Device *device, int w, int h, bool room) {
    if (texture && (room ? width >= w && height >= h : width == w && height == h)) return true;
    int tw = w, th = h;
    if (room) {
        tw = std::max((w + 63) / 64 * 64, texture ? width : 0);
        th = std::max((h + 63) / 64 * 64, texture ? height : 0);
    }
    *this = Surface{};
    D3D11_TEXTURE2D_DESC d{};
    d.Width = tw; d.Height = th; d.MipLevels = d.ArraySize = 1;
    d.Format = DXGI_FORMAT_B8G8R8A8_UNORM; d.SampleDesc.Count = 1;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
    if (FAILED(device->CreateTexture2D(&d, nullptr, &texture)) || FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &view)) ||
        FAILED(device->CreateRenderTargetView(texture.Get(), nullptr, &target))) {
        *this = Surface{};
        return false;
    }
    width = tw; height = th;
    return true;
}

struct EffectChain::State {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vertex;
    ComPtr<ID3D11PixelShader> effect, prefilter, down, blur, rays, maskCopy;
    ComPtr<ID3D11Buffer> constants, scene;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11BlendState> opaque;
    ComPtr<ID3D11RasterizerState> raster;
    ComPtr<ID3D11ShaderResourceView> white;
    // Low-resolution pictures, taken during one effect and given back after it.
    struct Pooled { Surface surface; bool busy = false; };
    std::list<Pooled> pool;
    // Box-sized copies of a picture and its mask for authors' shaders, which read them at their own uv.
    std::map<std::pair<int, int>, Surface> boxPictures, boxMasks;

    Surface *Take(int w, int h) {
        Pooled *best = nullptr;
        for (auto &p : pool)
            if (!p.busy && p.surface.width >= w && p.surface.height >= h &&
                (!best || (int64_t)p.surface.width * p.surface.height < (int64_t)best->surface.width * best->surface.height)) best = &p;
        if (!best) {
            pool.emplace_back();
            best = &pool.back();
            if (!best->surface.Fit(device.Get(), w, h)) { pool.pop_back(); return nullptr; }
        }
        best->busy = true;
        return &best->surface;
    }
    void Pass(ID3D11PixelShader *shader, const Constants &c, const Surface &target, int w, int h,
              ID3D11ShaderResourceView *t0, ID3D11ShaderResourceView *t1, ID3D11ShaderResourceView *t2) {
        context->UpdateSubresource(constants.Get(), 0, nullptr, &c, 0, 0);
        const D3D11_VIEWPORT vp{0, 0, (float)w, (float)h, 0, 1};
        Draw(shader, constants.Get(), target, vp, t0, t1, t2);
    }
    void Draw(ID3D11PixelShader *shader, ID3D11Buffer *cb, const Surface &target, const D3D11_VIEWPORT &vp,
              ID3D11ShaderResourceView *t0, ID3D11ShaderResourceView *t1, ID3D11ShaderResourceView *t2) {
        context->RSSetViewports(1, &vp);
        context->RSSetState(raster.Get());
        ID3D11RenderTargetView *rtv = target.target.Get();
        context->OMSetRenderTargets(1, &rtv, nullptr);
        const float factor[4]{};
        context->OMSetBlendState(opaque.Get(), factor, 0xFFFFFFFF);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vertex.Get(), nullptr, 0);
        context->PSSetShader(shader, nullptr, 0);
        context->PSSetConstantBuffers(0, 1, &cb);
        ID3D11SamplerState *s = sampler.Get();
        context->PSSetSamplers(0, 1, &s);
        ID3D11ShaderResourceView *views[3]{t0, t1, t2};
        context->PSSetShaderResources(0, 3, views);
        context->Draw(3, 0);
        ID3D11ShaderResourceView *none[3]{};
        context->PSSetShaderResources(0, 3, none);
        ID3D11RenderTargetView *noTarget = nullptr;
        context->OMSetRenderTargets(1, &noTarget, nullptr);
    }
    static void Low(Constants *c, const Surface &level, int w, int h) {
        Put(c->lowSize, {(float)w, (float)h, 1.f / level.width, 1.f / level.height});
    }

    // Blurs, glows and rays: the picture brought down to `soft` (or lower still for a wide blur,
    // where it looks the same) and worked on there. Leaves the result in *out, `factor` picture
    // pixels to each of its own.
    bool Reduce(Constants c, int kind, int w, int h, const Surface &in, const MaskView *mask, int soft, double sigma,
                Surface **out, int *factor, int *ow, int *oh, std::wstring *why) {
        int lw = (w + soft - 1) / soft, lh = (h + soft - 1) / soft, total = soft;
        Surface *level = Take(lw, lh);
        if (!level) return Fail(why, L"cannot allocate an effect picture");
        c.level[0] = (float)soft;
        Pass(prefilter.Get(), c, *level, lw, lh, in.view.Get(), mask ? mask->view : nullptr, nullptr);
        double s = sigma / soft;
        if (kind == kRays) {
            Surface *t = Take(lw, lh);
            if (!t) return Fail(why, L"cannot allocate an effect picture");
            Low(&c, *level, lw, lh);
            Pass(rays.Get(), c, *t, lw, lh, nullptr, nullptr, level->view.Get());
            level = t;
        } else {
            while (s > 4 && lw > 8 && lh > 8) {
                const int nw = (lw + 1) / 2, nh = (lh + 1) / 2;
                Surface *next = Take(nw, nh);
                if (!next) return Fail(why, L"cannot allocate an effect picture");
                Low(&c, *level, lw, lh);
                Pass(down.Get(), c, *next, nw, nh, nullptr, nullptr, level->view.Get());
                level = next; lw = nw; lh = nh; s /= 2; total *= 2;
            }
            // Averaging down and stretching back up already blur by about total²/4 square pixels;
            // the Gaussian adds only the rest, so every quality spreads about as far.
            s = std::sqrt(std::max(0.0, sigma * sigma - (total * total - 1) / 4.0)) / total;
            if (s > .3) {
                Surface *t = Take(lw, lh);
                if (!t) return Fail(why, L"cannot allocate an effect picture");
                Low(&c, *level, lw, lh);
                Put(c.level, {(float)total, (float)s, 1, 0});
                Pass(blur.Get(), c, *t, lw, lh, nullptr, nullptr, level->view.Get());
                Low(&c, *t, lw, lh);
                Put(c.level, {(float)total, (float)s, 0, 1});
                Pass(blur.Get(), c, *level, lw, lh, nullptr, nullptr, t->view.Get());
            }
        }
        *out = level; *factor = total; *ow = lw; *oh = lh;
        return true;
    }

    bool BuiltIn(const sg::PlacedEffect &pe, const EffectBox &box, const Surface &in, const Surface &out, const MaskView *mask,
                 const sg::Inputs &data, int soft, std::wstring *why) {
        const int kind = KindOf(pe.effect->type);
        if (kind < 0) return Fail(why, L"unknown effect " + pe.effect->type);
        const int w = box.Used(true), h = box.Used(false);
        const float W = (float)box.width, H = (float)box.height;
        Constants c;
        Put(c.size, {(float)w, (float)h, 1.f / in.width, 1.f / in.height});
        Put(c.box, {(float)box.margin, (float)box.margin, W, H});
        if (mask) Put(c.maskRect, {mask->u, mask->v, 1, 0});
        c.time = (float)data.time; c.kind = (float)kind; c.extend = box.extend ? 1.f : 0.f;
        c.seed = (float)((pe.effect->line * 7919u) % 65536u);
        const auto v = [&](const wchar_t *name) { return Get(pe, name)[0]; };
        const auto pair = [&](const wchar_t *name) { return Get(pe, name); };
        // Periods and lengths are kept off zero, where a motion could take them.
        const auto period = [&](const wchar_t *name) { return std::max(.01f, v(name)); };
        double sigma = 0;
        switch (kind) {
        case kBend: Put(c.p0, {v(L"strength"), period(L"period"), v(L"root"), v(L"waves")}); break;
        case kBreathe: Put(c.p0, {v(L"amount"), period(L"period")}); Put(c.p1, {pair(L"pivot")[0], pair(L"pivot")[1]}); break;
        case kWave: Put(c.p0, {v(L"amplitude"), std::max(.001f, v(L"length")), period(L"period"), v(L"direction")}); break;
        case kRings: Put(c.p0, {pair(L"center")[0], pair(L"center")[1], v(L"amplitude"), std::max(.001f, v(L"length"))}); Put(c.p1, {v(L"speed")}); break;
        case kFlow: Put(c.p0, {v(L"direction"), v(L"speed"), v(L"amount")}); break;
        case kPan: Put(c.p0, {v(L"speed-x"), v(L"speed-y")}); break;
        case kJitter: Put(c.p0, {v(L"amount"), v(L"rate")}); break;
        case kGlow: Put(c.p0, {v(L"strength"), v(L"radius"), v(L"threshold")}); sigma = v(L"radius") * H / 2; break;
        case kRays: Put(c.p0, {pair(L"center")[0], pair(L"center")[1], v(L"strength"), v(L"length")}); break;
        case kSheen: Put(c.p0, {v(L"angle"), std::max(.001f, v(L"width")), period(L"period"), v(L"strength")}); break;
        case kFlicker: Put(c.p0, {v(L"amount"), v(L"rate")}); break;
        case kGrade: Put(c.p0, {v(L"brightness"), v(L"contrast"), v(L"saturation"), v(L"temperature")}); break;
        case kTint: Put(c.p0, {v(L"amount")}); break;
        case kBlur: Put(c.p0, {v(L"radius")}); sigma = v(L"radius") * H / 2; break;
        case kHaze: Put(c.p0, {v(L"amount"), std::max(.001f, v(L"scale")), v(L"speed")}); break;
        case kSplit: Put(c.p0, {v(L"amount"), v(L"angle")}); break;
        case kGrain: Put(c.p0, {v(L"amount"), v(L"size")}); break;
        }
        if (kind == kGlow || kind == kRays || kind == kSheen || kind == kTint) {
            const auto color = Get(pe, L"color");
            std::copy(color.begin(), color.end(), c.tint);
        }
        ID3D11ShaderResourceView *lowView = nullptr;
        if (kind == kGlow || kind == kRays || kind == kBlur) {
            Surface *level = nullptr;
            int factor = 1, lw = 0, lh = 0;
            if (!Reduce(c, kind, w, h, in, mask, soft, std::max(0.0, sigma), &level, &factor, &lw, &lh, why)) return false;
            c.level[0] = (float)factor;
            Low(&c, *level, lw, lh);
            lowView = level->view.Get();
        }
        Pass(effect.Get(), c, out, w, h, in.view.Get(), mask ? mask->view : nullptr, lowView);
        return true;
    }

    // An author's effect shader: it reads the box alone at uv 0..1 and draws over the box; the margin
    // keeps what the earlier effects left there.
    bool Custom(const sg::PlacedEffect &pe, const EffectBox &box, const Surface &in, const Surface &out, const MaskView *mask,
                const std::map<std::wstring, ComPtr<ID3D11PixelShader>> &shaders, const sg::Inputs &data, std::wstring *why) {
        const auto it = shaders.find(pe.effect->source);
        if (it == shaders.end()) return Fail(why, L"missing effect shader " + pe.effect->source);
        const int W = box.width, H = box.height, M = box.margin;
        if (boxPictures.size() > 8) boxPictures.clear();
        if (boxMasks.size() > 8) boxMasks.clear();
        Surface &only = boxPictures[{W, H}];
        if (!only.Fit(device.Get(), W, H, false)) return Fail(why, L"cannot allocate an effect picture");
        const D3D11_BOX region{(UINT)M, (UINT)M, 0, (UINT)(M + W), (UINT)(M + H), 1};
        context->CopySubresourceRegion(only.texture.Get(), 0, 0, 0, 0, in.texture.Get(), 0, &region);
        const D3D11_BOX all{0, 0, 0, (UINT)box.Used(true), (UINT)box.Used(false), 1};
        context->CopySubresourceRegion(out.texture.Get(), 0, 0, 0, 0, in.texture.Get(), 0, &all);
        ID3D11ShaderResourceView *maskView = white.Get();
        if (mask) {
            Surface &m = boxMasks[{W, H}];
            if (!m.Fit(device.Get(), W, H, false)) return Fail(why, L"cannot allocate an effect picture");
            Constants c;
            Put(c.size, {(float)W, (float)H, 1.f / W, 1.f / H});
            Put(c.maskRect, {mask->u, mask->v, 1, 0});
            Pass(maskCopy.Get(), c, m, W, H, nullptr, mask->view, nullptr);
            maskView = m.view.Get();
        }
        const ShaderConstants sc = ShaderInputs(data, data.time, W, H, pe.values, L"");
        context->UpdateSubresource(scene.Get(), 0, nullptr, &sc, 0, 0);
        const D3D11_VIEWPORT vp{(float)M, (float)M, (float)W, (float)H, 0, 1};
        Draw(it->second.Get(), scene.Get(), out, vp, only.view.Get(), maskView, nullptr);
        return true;
    }
};

EffectChain::EffectChain() = default;
EffectChain::~EffectChain() = default;

bool EffectChain::Open(ID3D11Device *device, std::wstring *why) {
    auto s = std::make_unique<State>();
    s->device = device;
    device->GetImmediateContext(&s->context);
    ComPtr<ID3DBlob> blob;
    if (!Compile(kVertex, "main", "vs_5_0", &blob, why) ||
        FAILED(device->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &s->vertex)))
        return Fail(why, why && !why->empty() ? *why : L"cannot create the scene effects");
    const std::pair<const char *, ComPtr<ID3D11PixelShader> *> entries[]{
        {"Effect", &s->effect}, {"Prefilter", &s->prefilter}, {"Down", &s->down}, {"Blur", &s->blur}, {"Rays", &s->rays}, {"MaskCopy", &s->maskCopy}};
    for (const auto &[entry, shader] : entries)
        if (!Compile(kPixel, entry, "ps_5_0", &blob, why) ||
            FAILED(device->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, shader->GetAddressOf())))
            return Fail(why, why && !why->empty() ? *why : L"cannot create the scene effects");
    D3D11_BUFFER_DESC cb{};
    cb.Usage = D3D11_USAGE_DEFAULT; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.ByteWidth = sizeof(Constants);
    if (FAILED(device->CreateBuffer(&cb, nullptr, &s->constants))) return Fail(why, L"cannot create the effect constants");
    cb.ByteWidth = sizeof(ShaderConstants);
    if (FAILED(device->CreateBuffer(&cb, nullptr, &s->scene))) return Fail(why, L"cannot create the effect constants");
    D3D11_SAMPLER_DESC samp{};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&samp, &s->sampler))) return Fail(why, L"cannot create the effect sampler");
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&bd, &s->opaque))) return Fail(why, L"cannot create the effect blend state");
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rd, &s->raster))) return Fail(why, L"cannot create the effect rasterizer state");
    // White everywhere: the mask an author's effect reads when it has none.
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const uint32_t white = 0xFFFFFFFF;
    const D3D11_SUBRESOURCE_DATA init{&white, 4, 4};
    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(device->CreateTexture2D(&td, &init, &texture)) || FAILED(device->CreateShaderResourceView(texture.Get(), nullptr, &s->white)))
        return Fail(why, L"cannot create the effect mask");
    s_ = std::move(s);
    return true;
}

int EffectChain::Margin(const std::vector<sg::PlacedEffect> &effects, int width, int height) {
    double m = 0;
    for (const auto &pe : effects) {
        if (!pe.effect || pe.effect->type.empty()) continue;
        const auto v = [&](const wchar_t *name) { return std::abs((double)Get(pe, name)[0]); };
        const std::wstring &t = pe.effect->type;
        if (t == L"bend") m += v(L"strength") * .12 * (Get(pe, L"root")[0] < 1.5f ? height : width);
        else if (t == L"breathe") m += v(L"amount") * std::max(width, height);
        else if (t == L"wave" || t == L"rings") m += v(L"amplitude") * height;
        else if (t == L"flow" || t == L"jitter" || t == L"haze" || t == L"split") m += v(L"amount") * height;
        else if (t == L"glow" || t == L"blur") m += v(L"radius") * height * 1.5;
        else if (t == L"rays") m += v(L"length") * .25 * std::max(width, height);
    }
    if (m <= 0) return 0;
    // At most twice the box's own size on each side, and the whole picture within 8192 pixels.
    const int longest = std::max(width, height);
    return (int)std::max(0.0, std::min({std::ceil(m) + 2, 2.0 * longest, (kMaxEffectPicture - longest) / 2.0}));
}

bool EffectChain::Run(const std::vector<sg::PlacedEffect> &effects, const EffectBox &box, Surface *a, Surface *b,
                      const std::map<std::wstring, MaskView> &masks, const std::map<std::wstring, ComPtr<ID3D11PixelShader>> &shaders,
                      const sg::Inputs &data, int soft, Surface **result, std::wstring *why) {
    auto &s = *s_;
    if (!b->Fit(s.device.Get(), box.Used(true), box.Used(false))) return Fail(why, L"cannot allocate an effect picture");
    Surface *in = a, *out = b;
    for (const auto &pe : effects) {
        for (auto &p : s.pool) p.busy = false;
        const MaskView *mask = nullptr;
        if (const auto it = pe.mask.empty() ? masks.end() : masks.find(pe.mask); it != masks.end()) mask = &it->second;
        const bool done = pe.effect->source.empty() ? s.BuiltIn(pe, box, *in, *out, mask, data, std::clamp(soft, 1, 4), why)
                                                    : s.Custom(pe, box, *in, *out, mask, shaders, data, why);
        if (!done) return false;
        std::swap(in, out);
    }
    *result = in;
    return true;
}

void EffectChain::Trim() {
    if (!s_) return;
    s_->pool.clear();
    s_->boxPictures.clear();
    s_->boxMasks.clear();
}

}  // namespace animelogon::scene
