#include "renderer.h"
#include "effects.h"
#include "lottie.h"
#include "particles.h"
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <dxgi.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include "animelogon/skin.h"
#include "animelogon/text.h"
#include "animelogon/texture.h"
using Microsoft::WRL::ComPtr;
namespace animelogon::scene {
namespace sg = scenegraph;
namespace {
constexpr double kPi = 3.14159265358979323846;

// A full-screen triangle for authors' layer shaders; TEXCOORD0 is in the register the contract says.
constexpr char kFullScreen[] = R"(
struct V { float2 uv : TEXCOORD0; float4 position : SV_Position; };
V main(uint id : SV_VertexID) {
    V v; v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv * float2(2,-2) + float2(-1,1),0,1); return v;
})";

// Every node is one quad: a unit square carried onto the canvas by an affine transform. The pixel
// shader takes a texture over the quad, a colour, a gradient, or an offscreen layer pixel for pixel,
// and a mask over the node's box. Colours leave it premultiplied; the blend state does the rest.
constexpr char kDrawCommon[] = R"(
cbuffer Draw : register(b0) {
    float4 toCanvas0, toCanvas1;      // canvas pixel = (dot(toCanvas0.xyz, (u, v, 1)), dot(toCanvas1.xyz, ...))
    float4 fromCanvas0, fromCanvas1;  // the box's unit square from a canvas pixel, for a whole-canvas quad
    float4 canvas;                    // xy: the target's size in pixels
    float4 sourceRect, maskRect;      // xy: the used part of the texture, as a fraction of it
    float4 maskMap;                   // the box's unit square from the quad's: xy scale, zw offset
    float4 color;
    float4 stops[8];
    float4 stopAt0, stopAt1;
    float opacity, mode, useMask, stopCount;
    float gradient, premultiplied, whole, unused;
};
)";
constexpr char kDrawVertex[] = R"(
struct V { float2 box : TEXCOORD0; float4 position : SV_Position; };
V main(uint id : SV_VertexID) {
    float2 q = float2(id & 1, id >> 1);
    V v; v.box = q;
    float2 p = whole > .5 ? q * canvas.xy : float2(dot(toCanvas0.xyz, float3(q, 1)), dot(toCanvas1.xyz, float3(q, 1)));
    v.position = float4(p / canvas.xy * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
})";
constexpr char kDrawPixel[] = R"(
Texture2D<float4> picture : register(t0);
Texture2D<float4> maskPicture : register(t1);
SamplerState linearClamp : register(s0);
float4 main(float2 box : TEXCOORD0, float4 position : SV_Position) : SV_Target {
    float2 uv = whole > .5 ? float2(dot(fromCanvas0.xyz, float3(position.xy, 1)), dot(fromCanvas1.xyz, float3(position.xy, 1))) : box * maskMap.xy + maskMap.zw;
    float4 c;
    if (mode < .5) c = picture.Sample(linearClamp, box * sourceRect.xy);
    else if (mode < 1.5) c = color;
    else if (mode < 2.5) {
        float at[8] = {stopAt0.x, stopAt0.y, stopAt0.z, stopAt0.w, stopAt1.x, stopAt1.y, stopAt1.z, stopAt1.w};
        float t = gradient < .5 ? box.y : box.x;
        c = stops[0];
        for (int i = 1; i < 8; ++i)
            if (i < stopCount && t > at[i - 1]) c = lerp(stops[i - 1], stops[i], saturate((t - at[i - 1]) / max(1e-5, at[i] - at[i - 1])));
    } else c = picture.Sample(linearClamp, position.xy / canvas.xy * sourceRect.xy);
    if (premultiplied < .5) c.rgb *= c.a;
    if (useMask > .5) c *= any(uv < 0) || any(uv > 1) ? 0 : maskPicture.Sample(linearClamp, uv * maskRect.xy).r;
    return c * opacity;
})";
struct DrawConstants {
    float toCanvas0[4]{}, toCanvas1[4]{}, fromCanvas0[4]{}, fromCanvas1[4]{};
    float canvas[4]{};
    float sourceRect[4]{1, 1, 0, 0}, maskRect[4]{1, 1, 0, 0};
    float maskMap[4]{1, 1, 0, 0};
    float color[4]{};
    float stops[8][4]{};
    float stopAt[8]{};
    float opacity = 1, mode = 0, useMask = 0, stopCount = 0;
    float gradient = 0, premultiplied = 1, whole = 0, unused = 0;
};
enum Mode { kTexture = 0, kColor = 1, kGradient = 2, kLayer = 3 };

// x' = a x + c y + e, y' = b x + d y + f.
struct Affine {
    double a = 1, b = 0, c = 0, d = 1, e = 0, f = 0;
};
Affine operator*(const Affine &m, const Affine &n) {
    return {m.a * n.a + m.c * n.b, m.b * n.a + m.d * n.b, m.a * n.c + m.c * n.d, m.b * n.c + m.d * n.d,
            m.a * n.e + m.c * n.f + m.e, m.b * n.e + m.d * n.f + m.f};
}
Affine Move(double x, double y) { return {1, 0, 0, 1, x, y}; }
Affine Scale(double x, double y) { return {x, 0, 0, y, 0, 0}; }
Affine Turn(double degrees) {
    const double r = degrees * kPi / 180, c = std::cos(r), s = std::sin(r);
    return {c, s, -s, c, 0, 0};
}
Affine Inverse(const Affine &m) {
    const double det = m.a * m.d - m.b * m.c;
    if (std::abs(det) < 1e-12) return {0, 0, 0, 0, -1, -1};
    const double a = m.d / det, b = -m.b / det, c = -m.c / det, d = m.a / det;
    return {a, b, c, d, -(a * m.e + c * m.f), -(b * m.e + d * m.f)};
}

struct Size {
    double w = 0, h = 0;
};

bool Fail(std::wstring *why, const std::wstring &s) { if (why) *why = s; return false; }
bool Compile(std::string_view source, const char *profile, ComPtr<ID3DBlob> *blob, std::wstring *why) {
    ComPtr<ID3DBlob> error;
    const HRESULT hr = D3DCompile(source.data(), source.size(), "scene", nullptr, nullptr, "main", profile,
                                  D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob->ReleaseAndGetAddressOf(), &error);
    if (FAILED(hr)) return Fail(why, error ? FromUtf8(std::string_view((const char *)error->GetBufferPointer(), std::min<size_t>(error->GetBufferSize(), 1024))) : L"shader compilation failed");
    return true;
}
std::array<float, 4> At(const std::map<std::wstring, std::array<float, 4>> &values, const wchar_t *key) {
    const auto it = values.find(key);
    return it == values.end() ? std::array<float, 4>{} : it->second;
}
double Number(const std::wstring &text) {
    const auto v = sg::Components(text);
    return v.empty() ? 0 : v[0];
}
template <size_t N> int Index(const wchar_t *const (&words)[N], const std::wstring &word) {
    for (size_t i = 0; i < N; ++i) if (word == words[i]) return (int)i;
    return 0;
}
const wchar_t *const kAnchorWords[] = {L"top-left", L"top", L"top-right", L"left", L"center", L"right",
                                       L"bottom-left", L"bottom", L"bottom-right"};
}  // namespace

struct Renderer::State {
    struct Texture {
        ComPtr<ID3D11Texture2D> texture;
        ComPtr<ID3D11ShaderResourceView> view;
        ComPtr<ID3D11RenderTargetView> target;
        int width = 0, height = 0;
        float u = 1, v = 1;          // the used part, as a fraction of the texture
        double usedW = 0, usedH = 0;  // the picture's own size in points
        bool premultiplied = true;
    };
    struct Source {
        const File *file = nullptr;
        texture::Info info;
        std::vector<texture::Level> levels;
        uint32_t skip = 0;
        Texture *into = nullptr;
    };
    sg::Document d;
    Files files;
    int width = 0, height = 0;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> drawVs, fullScreenVs;
    ComPtr<ID3D11PixelShader> drawPs;
    ComPtr<ID3D11Buffer> drawConstants, shaderConstants;
    ComPtr<ID3D11SamplerState> sampler;
    ComPtr<ID3D11BlendState> blends[4];
    ComPtr<ID3D11BlendState> opaque;
    // Mirrored nodes (a negative scale) turn their quad over, so nothing is culled.
    ComPtr<ID3D11RasterizerState> raster;
    std::map<std::wstring, Texture> pictures;               // images, masks and video first frames by asset id
    std::map<std::wstring, std::vector<Texture>> sequences;  // frame sequences
    std::map<std::wstring, std::unique_ptr<Lottie>> lotties;
    std::map<std::wstring, ComPtr<ID3D11PixelShader>> shaders;
    std::map<const sg::Node *, Texture> drawn;  // Lottie and shader layers, drawn for this frame
    std::vector<Surface> layers;                 // offscreen targets for isolated groups, one per depth
    // A node with effects is drawn into its own picture, its box with a margin around it for what
    // the effects move outward, and the effects run over that. The picture is kept while nothing
    // it depends on changes, so a still node with effects costs one composite a frame.
    struct Effected {
        Surface picture;
        std::string key;
        int width = 0, height = 0, margin = 0;
    };
    std::map<const sg::Node *, Effected> effected;
    Surface scratch;
    EffectChain chain;
    std::map<std::wstring, MaskView> masks;
    RenderQuality quality = RenderQuality::Auto;
    int soft = 2;  // the coarsest resolution divisor blurs, glows and rays work at this frame
    ParticleDrawer particles;
    int particlePercent = 100;  // the user's particle density setting
    double density = 1;         // the share of particles born this frame, the low quality halving it
    Texture videoPicture;
    Texture canvas;
    ComPtr<ID3D11Texture2D> videoTexture;
    ComPtr<ID3D11ShaderResourceView> videoView;
    std::wstring videoId;
    uint64_t textureBytes = 0;
    uint32_t reduced = 0;
    std::string lastKey;
    ComPtr<ID3D11Query> disjoint, begin, end;
    const Inputs *in = nullptr;

    bool Make(Texture *t, int w, int h) {
        if (t->width == w && t->height == h && t->texture) return true;
        *t = Texture{};
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = w; desc.Height = h; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &t->texture)) ||
            FAILED(device->CreateShaderResourceView(t->texture.Get(), nullptr, &t->view)) ||
            FAILED(device->CreateRenderTargetView(t->texture.Get(), nullptr, &t->target))) return false;
        t->width = w; t->height = h; t->usedW = w; t->usedH = h;
        return true;
    }
    // The top-left w x h of a surface, as a target to draw into and a picture to read.
    static Texture View(const Surface &s, int w, int h) {
        Texture t;
        t.texture = s.texture; t.view = s.view; t.target = s.target;
        t.width = w; t.height = h; t.u = (float)w / s.width; t.v = (float)h / s.height;
        t.usedW = w; t.usedH = h;
        return t;
    }
    static uint64_t BytesFrom(const Source &s) {
        uint64_t n = 0;
        for (size_t i = s.skip; i < s.levels.size(); ++i) n += s.levels[i].bytes;
        return n;
    }
    bool Create(const Source &s, std::wstring *why) {
        const auto &top = s.levels[s.skip];
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = top.width; desc.Height = top.height;
        desc.MipLevels = (UINT)(s.levels.size() - s.skip); desc.ArraySize = 1;
        desc.Format = s.info.format == texture::Format::Bc7 ? DXGI_FORMAT_BC7_UNORM :
                      s.info.format == texture::Format::Bc4 ? DXGI_FORMAT_BC4_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1; desc.Usage = D3D11_USAGE_IMMUTABLE; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        std::vector<D3D11_SUBRESOURCE_DATA> data;
        for (size_t i = s.skip; i < s.levels.size(); ++i)
            data.push_back({s.file->bytes.data() + s.levels[i].offset, s.levels[i].rowPitch, (UINT)s.levels[i].bytes});
        Texture &t = *s.into;
        if (FAILED(device->CreateTexture2D(&desc, data.data(), &t.texture)) ||
            FAILED(device->CreateShaderResourceView(t.texture.Get(), nullptr, &t.view)))
            return Fail(why, L"cannot create a scene texture");
        t.width = top.width; t.height = top.height;
        t.u = (float)s.info.usedWidth / s.info.width; t.v = (float)s.info.usedHeight / s.info.height;
        t.usedW = s.info.usedWidth; t.usedH = s.info.usedHeight;
        t.premultiplied = s.info.premultiplied || s.info.format == texture::Format::Bc4;
        return true;
    }
    // Every texture, with the finest levels of the largest ones left out until they fit the limit.
    bool Textures(int videoMemory, std::wstring *why) {
        std::vector<Source> sources;
        auto add = [&](const std::wstring &name, Texture *into) {
            Source s;
            s.file = sg::Find(files, name);
            s.into = into;
            if (!s.file || !texture::Read(s.file->bytes.data(), s.file->bytes.size(), &s.info, &s.levels, why))
                return Fail(why, L"missing or invalid texture " + name);
            sources.push_back(std::move(s));
            return true;
        };
        for (const auto &a : d.assets) {
            if (a.kind == sg::AssetKind::Image || a.kind == sg::AssetKind::Mask) {
                if (!add(L"textures/" + a.id + L".tex", &pictures[a.id])) return false;
            } else if (a.kind == sg::AssetKind::Video) {
                if (!add(sg::PosterName(a), &pictures[a.id])) return false;
                videoId = a.id;
            } else if (a.kind == sg::AssetKind::Frames) {
                auto &list = sequences[a.id];
                list.resize(a.files.size());
                for (size_t i = 0; i < a.files.size(); ++i)
                    if (!add(L"textures/" + a.id + L"-" + std::to_wstring(i) + L".tex", &list[i])) return false;
            }
        }
        uint64_t limit = (uint64_t)videoMemory << 20;
        if (!videoMemory) {
            ComPtr<IDXGIDevice> dxgi;
            ComPtr<IDXGIAdapter> adapter;
            DXGI_ADAPTER_DESC desc{};
            const bool modest = SUCCEEDED(device.As(&dxgi)) && SUCCEEDED(dxgi->GetAdapter(&adapter)) &&
                SUCCEEDED(adapter->GetDesc(&desc)) && desc.DedicatedVideoMemory < (2ull << 30);
            limit = (modest ? 256ull : 512ull) << 20;
        }
        uint64_t total = 0;
        for (const auto &s : sources) total += BytesFrom(s);
        while (total > limit) {
            Source *largest = nullptr;
            for (auto &s : sources) {
                // A block-compressed texture's finest level must stay a whole number of blocks.
                if (s.skip + 1 >= s.levels.size() || s.levels[s.skip + 1].width % 4 || s.levels[s.skip + 1].height % 4) continue;
                if (!largest || BytesFrom(s) > BytesFrom(*largest)) largest = &s;
            }
            if (!largest) break;
            total -= largest->levels[largest->skip].bytes;
            ++largest->skip;
            ++reduced;
        }
        textureBytes = total;
        for (const auto &s : sources) if (!Create(s, why)) return false;
        return true;
    }

    // The natural size of a node's content: its picture's, a Lottie's, or for a group the extent of
    // its children as written; a fill or a layer shader takes the box it is in.
    Size Natural(const sg::Placed &p, Size parent) const {
        if (p.node->kind == sg::NodeKind::Group) return Extent(p, parent);
        if (p.node->kind != sg::NodeKind::Layer) return parent;
        if (const auto it = pictures.find(p.source); it != pictures.end()) return {it->second.usedW, it->second.usedH};
        if (const auto it = sequences.find(p.source); it != sequences.end() && !it->second.empty())
            return {it->second[0].usedW, it->second[0].usedH};
        if (const auto it = lotties.find(p.source); it != lotties.end()) return {it->second->Width(), it->second->Height()};
        return parent;
    }
    // A node's box in its parent's units, from its written width and height and its natural size.
    Size BoxOf(const sg::Placed &p, double w, double h, Size parent) const {
        const Size n = Natural(p, parent);
        if (p.full) {
            if (n.w <= 0 || n.h <= 0 || p.fit == sg::Fit::Stretch) return parent;
            const double s = p.fit == sg::Fit::Cover ? std::max(parent.w / n.w, parent.h / n.h) : std::min(parent.w / n.w, parent.h / n.h);
            return {n.w * s, n.h * s};
        }
        if (w > 0 && h > 0) return {w, h};
        if (w > 0) return {w, n.w > 0 ? w * n.h / n.w : w};
        if (h > 0) return {n.h > 0 ? h * n.w / n.h : h, h};
        return n;
    }
    // A group's natural size: from its origin to the farthest corner of its children as written,
    // before motions, so that the group does not breathe as its children move.
    Size Extent(const sg::Placed &group, Size parent) const {
        Size e;
        for (const auto &c : group.children) {
            if (c.full) continue;
            auto text = [&](const wchar_t *k) { const auto it = c.text.find(k); return it == c.text.end() ? 0.0 : Number(it->second); };
            const Size box = BoxOf(c, text(L"width"), text(L"height"), parent);
            const auto at = c.text.find(L"anchor");
            const int anchor = at == c.text.end() ? 0 : Index(kAnchorWords, at->second);
            e.w = std::max(e.w, text(L"x") - (anchor % 3) * .5 * box.w + box.w);
            e.h = std::max(e.h, text(L"y") - (anchor / 3) * .5 * box.h + box.h);
        }
        return e.w > 0 && e.h > 0 ? e : parent;
    }
    // Where a node's box goes in its parent: placed by its anchor (or centred when it fills the
    // parent), then scaled and turned about its pivot.
    Affine Place(const sg::Placed &p, Size box, Size parent, double unit) const {
        double left, top;
        if (p.full) { left = (parent.w - box.w) / 2; top = (parent.h - box.h) / 2; }
        else { left = p.x * unit - (p.anchor % 3) * .5 * box.w; top = p.y * unit - (p.anchor / 3) * .5 * box.h; }
        const double px = p.pivotX * box.w, py = p.pivotY * box.h;
        return Move(left + px, top + py) * Turn(p.rotation) * Scale(p.scaleX, p.scaleY) * Move(-px, -py);
    }

    void Bind(const Texture &target) {
        D3D11_VIEWPORT vp{0, 0, (float)target.width, (float)target.height, 0, 1};
        context->RSSetViewports(1, &vp);
        ID3D11RenderTargetView *rtv = target.target.Get();
        context->OMSetRenderTargets(1, &rtv, nullptr);
        context->RSSetState(raster.Get());
    }
    void Quad(const Texture &target, const DrawConstants &c, sg::Blend blend, ID3D11ShaderResourceView *picture, ID3D11ShaderResourceView *mask) {
        context->UpdateSubresource(drawConstants.Get(), 0, nullptr, &c, 0, 0);
        Bind(target);
        context->IASetInputLayout(nullptr);
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        context->VSSetShader(drawVs.Get(), nullptr, 0);
        context->PSSetShader(drawPs.Get(), nullptr, 0);
        ID3D11Buffer *cb = drawConstants.Get();
        context->VSSetConstantBuffers(0, 1, &cb);
        context->PSSetConstantBuffers(0, 1, &cb);
        ID3D11SamplerState *s = sampler.Get();
        context->PSSetSamplers(0, 1, &s);
        ID3D11ShaderResourceView *views[2]{picture, mask};
        context->PSSetShaderResources(0, 2, views);
        const float factor[4]{};
        context->OMSetBlendState(blends[(int)blend].Get(), factor, 0xFFFFFFFF);
        context->Draw(4, 0);
        ID3D11ShaderResourceView *none[2]{};
        context->PSSetShaderResources(0, 2, none);
    }
    static void Corners(DrawConstants *c, const Affine &q, const Texture &target) {
        const float t0[4]{(float)q.a, (float)q.c, (float)q.e, 0}, t1[4]{(float)q.b, (float)q.d, (float)q.f, 0};
        std::copy(t0, t0 + 4, c->toCanvas0); std::copy(t1, t1 + 4, c->toCanvas1);
        c->canvas[0] = (float)target.width; c->canvas[1] = (float)target.height;
    }
    void Mask(DrawConstants *c, const std::wstring &mask, ID3D11ShaderResourceView **view) {
        const auto it = mask.empty() ? pictures.end() : pictures.find(mask);
        if (it == pictures.end()) return;
        c->useMask = 1; c->maskRect[0] = it->second.u; c->maskRect[1] = it->second.v;
        *view = it->second.view.Get();
    }
    // Whether a quad lands on the target at all.
    static bool Visible(const Affine &q, const Texture &target) {
        double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
        const double corners[4][2]{{0, 0}, {1, 0}, {0, 1}, {1, 1}};
        for (const auto &corner : corners) {
            const double u = corner[0], v = corner[1];
            const double x = q.a * u + q.c * v + q.e, y = q.b * u + q.d * v + q.f;
            x0 = std::min(x0, x); y0 = std::min(y0, y); x1 = std::max(x1, x); y1 = std::max(y1, y);
        }
        return x1 > 0 && y1 > 0 && x0 < target.width && y0 < target.height;
    }
    // The pixel size a box covers on the canvas, for what is drawn on the processor or by a shader.
    static std::pair<int, int> Pixels(const Affine &q, const Texture &target) {
        const double w = std::hypot(q.a, q.b), h = std::hypot(q.c, q.d);
        const double cap = 2.0 * std::max(target.width, target.height);
        return {(int)std::clamp(std::ceil(w), 1.0, std::min(cap, 4096.0)), (int)std::clamp(std::ceil(h), 1.0, std::min(cap, 4096.0))};
    }

    // What a layer shows: a texture view and its used part, drawing a Lottie or shader layer first.
    bool LayerPicture(const sg::Placed &p, const Affine &q, const Texture &target, const Texture **out, std::wstring *why) {
        if (p.source == videoId && videoView) {
            videoPicture.view = videoView; videoPicture.u = videoPicture.v = 1; videoPicture.premultiplied = true;
            *out = &videoPicture;
            return true;
        }
        if (const auto it = pictures.find(p.source); it != pictures.end()) { *out = &it->second; return true; }
        if (const auto it = sequences.find(p.source); it != sequences.end()) {
            *out = &it->second[std::clamp<size_t>((size_t)std::max(0, p.frame), 0, it->second.size() - 1)];
            return true;
        }
        const auto [w, h] = Pixels(q, target);
        if (const auto it = lotties.find(p.source); it != lotties.end()) {
            std::map<std::wstring, std::array<float, 4>> slots;
            for (const auto &[k, v] : p.values) if (k.rfind(L"slot.", 0) == 0) slots[k.substr(5)] = v;
            bool changed = false;
            Texture &t = drawn[p.node];
            const bool fresh = t.width != w || t.height != h;
            if (!it->second->Draw(slots, p.localTime, w, h, &changed, why)) return false;
            if (fresh) {
                t = Texture{};
                D3D11_TEXTURE2D_DESC desc{};
                desc.Width = w; desc.Height = h; desc.MipLevels = desc.ArraySize = 1;
                desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1; desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
                if (FAILED(device->CreateTexture2D(&desc, nullptr, &t.texture)) ||
                    FAILED(device->CreateShaderResourceView(t.texture.Get(), nullptr, &t.view))) return Fail(why, L"cannot allocate a Lottie texture");
                t.width = w; t.height = h; t.premultiplied = true;
            }
            if (changed || fresh) context->UpdateSubresource(t.texture.Get(), 0, nullptr, it->second->Pixels(), w * 4, 0);
            *out = &t;
            return true;
        }
        if (const auto it = shaders.find(p.source); it != shaders.end()) {
            Texture &t = drawn[p.node];
            if (!Make(&t, w, h)) return Fail(why, L"cannot allocate a shader layer");
            t.premultiplied = false;
            const ShaderConstants c = ShaderInputs(in->data, p.localTime, w, h, p.values, L"param.");
            context->UpdateSubresource(shaderConstants.Get(), 0, nullptr, &c, 0, 0);
            Bind(t);
            const float factor[4]{};
            context->OMSetBlendState(opaque.Get(), factor, 0xFFFFFFFF);
            context->IASetInputLayout(nullptr);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(fullScreenVs.Get(), nullptr, 0);
            context->PSSetShader(it->second.Get(), nullptr, 0);
            ID3D11Buffer *cb = shaderConstants.Get();
            context->PSSetConstantBuffers(0, 1, &cb);
            ID3D11SamplerState *s = sampler.Get();
            context->PSSetSamplers(0, 1, &s);
            context->Draw(3, 0);
            *out = &t;
            return true;
        }
        *out = nullptr;  // a picture the user chose from the library, not yet handed to the renderer
        return true;
    }

    // Draws `p` onto `target`; `parent` maps the node's parent units onto the canvas.
    bool DrawNode(const sg::Placed &p, const Affine &parent, Size parentBox, double unit, size_t depth, const Texture &target, std::wstring *why) {
        if (p.opacity <= 0) return true;
        const Size box = BoxOf(p, p.width * unit, p.height * unit, parentBox);
        if (box.w <= 0 || box.h <= 0) return true;
        const Affine m = parent * Place(p, box, parentBox, unit);
        if (!p.effects.empty()) return DrawEffected(p, m, box, parentBox, depth, target, why);
        return DrawContent(p, m, box, parentBox, depth, target, {p.opacity, p.blend, &p.mask}, why);
    }

    // How a node's content lands on what is below it.
    struct Look {
        float opacity = 1;
        sg::Blend blend = sg::Blend::Normal;
        const std::wstring *mask = nullptr;
        bool Plain() const { return opacity >= 1 && blend == sg::Blend::Normal && (!mask || mask->empty()); }
    };

    // Draws what `p` shows, its box (box.w x box.h in its own units) carried onto `target` by `m`.
    bool DrawContent(const sg::Placed &p, const Affine &m, Size box, Size parentBox, size_t depth, const Texture &target, const Look &look,
                     std::wstring *why) {
        const Affine q = m * Scale(box.w, box.h);
        const std::wstring none;
        const std::wstring &maskId = look.mask ? *look.mask : none;
        if (p.node->kind == sg::NodeKind::Group) {
            const Size content = Natural(p, parentBox);
            const Affine inner = m * Scale(box.w / content.w, box.h / content.h);
            if (look.Plain()) {
                for (const auto &c : p.children) if (!DrawNode(c, inner, content, 1, depth, target, why)) return false;
                return true;
            }
            if (depth >= layers.size()) return Fail(why, L"groups are nested too deeply");
            if (!layers[depth].Fit(device.Get(), target.width, target.height)) return Fail(why, L"cannot allocate a group layer");
            const Texture layer = View(layers[depth], target.width, target.height);
            const float clear[4]{};
            context->ClearRenderTargetView(layer.target.Get(), clear);
            for (const auto &c : p.children) if (!DrawNode(c, inner, content, 1, depth + 1, layer, why)) return false;
            DrawConstants c;
            Corners(&c, q, target);
            const Affine from = Inverse(q);
            const float f0[4]{(float)from.a, (float)from.c, (float)from.e, 0}, f1[4]{(float)from.b, (float)from.d, (float)from.f, 0};
            std::copy(f0, f0 + 4, c.fromCanvas0); std::copy(f1, f1 + 4, c.fromCanvas1);
            c.whole = 1; c.mode = kLayer; c.opacity = look.opacity;
            c.sourceRect[0] = layer.u; c.sourceRect[1] = layer.v;
            ID3D11ShaderResourceView *mask = nullptr;
            Mask(&c, maskId, &mask);
            Quad(target, c, look.blend, layer.view.Get(), mask);
            return true;
        }
        if (p.node->kind == sg::NodeKind::Particles) {
            // Particles fly out of their box, so they are not culled by it; each is blended on its own.
            std::vector<ParticlePicture> frames;
            if (const auto it = pictures.find(p.source); it != pictures.end())
                frames.push_back({it->second.view.Get(), it->second.u, it->second.v, it->second.premultiplied});
            else if (const auto seq = sequences.find(p.source); seq != sequences.end())
                for (const auto &t : seq->second) frames.push_back({t.view.Get(), t.u, t.v, t.premultiplied});
            ParticleTarget pt;
            pt.target = target.target.Get(); pt.width = target.width; pt.height = target.height;
            pt.a = m.a; pt.b = m.b; pt.c = m.c; pt.d = m.d; pt.e = m.e; pt.f = m.f;
            pt.boxWidth = box.w; pt.boxHeight = box.h;
            pt.blend = blends[(int)look.blend].Get(); pt.opacity = look.opacity;
            if (const auto it = maskId.empty() ? pictures.end() : pictures.find(maskId); it != pictures.end()) {
                pt.mask = it->second.view.Get(); pt.maskU = it->second.u; pt.maskV = it->second.v;
            }
            particles.Draw(p, frames, pt, in->data.time, density);
            return true;
        }
        if (!Visible(q, target)) return true;
        DrawConstants c;
        Corners(&c, q, target);
        c.opacity = look.opacity;
        ID3D11ShaderResourceView *picture = nullptr, *mask = nullptr;
        Mask(&c, maskId, &mask);
        if (p.node->kind == sg::NodeKind::Fill) {
            const auto colors = p.text.find(L"colors");
            std::vector<skin::Stop> stops;
            if (colors != p.text.end() && !colors->second.empty() && skin::ParseStops(colors->second, &stops) && !stops.empty()) {
                c.mode = kGradient;
                c.stopCount = (float)std::min<size_t>(8, stops.size());
                for (size_t i = 0; i < stops.size() && i < 8; ++i) {
                    const uint32_t v = stops[i].color;
                    const float a = (v >> 24) / 255.f;
                    c.stops[i][0] = ((v >> 16) & 255) / 255.f * a; c.stops[i][1] = ((v >> 8) & 255) / 255.f * a;
                    c.stops[i][2] = (v & 255) / 255.f * a; c.stops[i][3] = a;
                    c.stopAt[i] = stops[i].at;
                }
                const auto g = p.text.find(L"gradient");
                c.gradient = g != p.text.end() && g->second == L"horizontal" ? 1.f : 0.f;
            } else {
                const auto v = At(p.values, L"color");
                c.mode = kColor;
                c.color[0] = v[0] * v[3]; c.color[1] = v[1] * v[3]; c.color[2] = v[2] * v[3]; c.color[3] = v[3];
            }
        } else {
            const Texture *t = nullptr;
            if (!LayerPicture(p, q, target, &t, why)) return false;
            if (!t || !t->view) return true;
            c.mode = kTexture;
            c.sourceRect[0] = t->u; c.sourceRect[1] = t->v;
            c.premultiplied = t->premultiplied ? 1.f : 0.f;
            picture = t->view.Get();
        }
        Quad(target, c, look.blend, picture, mask);
        return true;
    }

    // A node with effects: its content drawn into its own picture at the size its box covers on the
    // canvas, the effects run over that, and the result composited by the node's transform, opacity,
    // blend and mask. Effects work in the node's own frame, so a turned node's bend still hangs from
    // its own top.
    bool DrawEffected(const sg::Placed &p, const Affine &m, Size box, Size parentBox, size_t depth, const Texture &target, std::wstring *why) {
        const auto [W, H] = Pixels(m * Scale(box.w, box.h), target);
        Effected &e = effected[p.node];
        // A node that fills the screen repeats its edges instead (EffectBox::extend). The margin only
        // grows while the box keeps its size, so a parameter moving with time does not resize the
        // picture every frame.
        if (e.width != W || e.height != H) e.margin = 0;
        e.margin = std::max(e.margin, p.full ? 0 : EffectChain::Margin(p.effects, W, H));
        const int M = e.margin, w = W + 2 * M, h = H + 2 * M;
        const double mx = (double)M / W, my = (double)M / H;
        const Affine padded = m * Move(-mx * box.w, -my * box.h) * Scale((1 + 2 * mx) * box.w, (1 + 2 * my) * box.h);
        if (!Visible(padded, target)) return true;
        std::string key;
        LocalKey(p, W, H, M, &key);
        if (key != e.key || !e.picture.texture) {
            e.key.clear();
            if (!e.picture.Fit(device.Get(), w, h)) return Fail(why, L"cannot allocate an effect picture");
            const Texture picture = View(e.picture, w, h);
            const float clear[4]{};
            context->ClearRenderTargetView(picture.target.Get(), clear);
            if (!DrawContent(p, Move(M, M) * Scale(W / box.w, H / box.h), box, parentBox, depth, picture, {}, why)) return false;
            Surface *result = nullptr;
            if (!chain.Run(p.effects, {W, H, M, p.full}, &e.picture, &scratch, masks, shaders, in->data, soft, &result, why)) return false;
            if (result != &e.picture) {
                const D3D11_BOX all{0, 0, 0, (UINT)w, (UINT)h, 1};
                context->CopySubresourceRegion(e.picture.texture.Get(), 0, 0, 0, 0, result->texture.Get(), 0, &all);
            }
            e.key = std::move(key);
            e.width = W; e.height = H;
        }
        DrawConstants c;
        Corners(&c, padded, target);
        c.mode = kTexture; c.opacity = p.opacity;
        c.sourceRect[0] = (float)w / e.picture.width; c.sourceRect[1] = (float)h / e.picture.height;
        const float map[4]{(float)(1 + 2 * mx), (float)(1 + 2 * my), (float)-mx, (float)-my};
        std::copy(map, map + 4, c.maskMap);
        ID3D11ShaderResourceView *mask = nullptr;
        Mask(&c, p.mask, &mask);
        Quad(target, c, p.blend, e.picture.view.Get(), mask);
        return true;
    }

    // What a node's effected picture depends on: everything Key() covers except where the node is
    // placed and how it is composited, which only change the final quad.
    void LocalKey(const sg::Placed &p, int w, int h, int margin, std::string *key) const {
        auto put = [&](const void *bytes, size_t n) { key->append((const char *)bytes, n); };
        const int size[]{w, h, margin};
        put(size, sizeof(size));
        put(&p.node, sizeof(p.node));
        put(&p.frame, sizeof(p.frame));
        key->append(ToUtf8(p.source)); key->push_back(0);
        for (const auto &[k, v] : p.values) {
            if (k == L"x" || k == L"y" || k == L"width" || k == L"height" || k == L"rotation" || k == L"scale" || k == L"scale-x" ||
                k == L"scale-y" || k == L"opacity")
                continue;
            key->append(ToUtf8(k)); put(v.data(), sizeof(float) * 4);
        }
        for (const auto &[k, v] : p.text) { key->append(ToUtf8(k)); key->push_back('='); key->append(ToUtf8(v)); key->push_back(0); }
        if (lotties.count(p.source) || shaders.count(p.source)) put(&p.localTime, sizeof(p.localTime));
        if (p.source == videoId) put(&in->videoSequence, sizeof(in->videoSequence));
        ParticlesKey(p, key);
        EffectsKey(p, key);
        for (const auto &c : p.children) Key(c, key);
    }
    // Particles move all the time, and how many there are follows the density setting.
    void ParticlesKey(const sg::Placed &p, std::string *key) const {
        if (p.node->kind != sg::NodeKind::Particles) return;
        key->append((const char *)&in->data.time, sizeof(in->data.time));
        key->append((const char *)&density, sizeof(density));
    }
    // A node's effects: their parameters and masks, the time when they move by themselves, and the
    // inputs when an author's shader can read them.
    void EffectsKey(const sg::Placed &p, std::string *key) const {
        auto put = [&](const void *bytes, size_t n) { key->append((const char *)bytes, n); };
        bool moves = false, reads = false;
        for (const auto &e : p.effects) {
            put(&e.effect, sizeof(e.effect));
            key->append(ToUtf8(e.mask)); key->push_back(0);
            for (const auto &[k, v] : e.values) { key->append(ToUtf8(k)); put(v.data(), sizeof(float) * 4); }
            moves = moves || sg::EffectMoves(*e.effect);
            reads = reads || !e.effect->source.empty();
        }
        if (moves) put(&in->data.time, sizeof(in->data.time));
        if (reads) {
            const auto &data = in->data;
            const double numbers[]{data.day, data.wake, data.battery, data.progress, (double)data.month, data.audio[0], data.audio[1], data.audio[2], data.audio[3]};
            put(numbers, sizeof(numbers));
            key->append(ToUtf8(data.mediaState)); key->push_back(0);
        }
        if (!p.effects.empty()) put(&soft, sizeof(soft));
    }

    // A short summary of everything a frame depends on: when it is the same as the last frame's,
    // the canvas already holds the picture.
    void Key(const sg::Placed &p, std::string *key) const {
        auto put = [&](const void *bytes, size_t n) { key->append((const char *)bytes, n); };
        const float numbers[]{p.x, p.y, p.width, p.height, p.pivotX, p.pivotY, p.rotation, p.scaleX, p.scaleY, p.opacity};
        put(numbers, sizeof(numbers));
        put(&p.frame, sizeof(p.frame));
        put(&p.node, sizeof(p.node));
        key->append(ToUtf8(p.source)); key->push_back(0);
        key->append(ToUtf8(p.mask)); key->push_back(0);
        for (const auto &[k, v] : p.values) { key->append(ToUtf8(k)); put(v.data(), sizeof(float) * 4); }
        // Words as well as numbers: a blend, a fit or colour stops chosen by a setting.
        for (const auto &[k, v] : p.text) { key->append(ToUtf8(k)); key->push_back('='); key->append(ToUtf8(v)); key->push_back(0); }
        if (lotties.count(p.source) || shaders.count(p.source) || p.source == videoId) put(&p.localTime, sizeof(p.localTime));
        ParticlesKey(p, key);
        EffectsKey(p, key);
        for (const auto &c : p.children) Key(c, key);
        key->push_back(1);
    }
};

Renderer::Renderer() = default;
Renderer::~Renderer() = default;
uint64_t Renderer::TextureBytes() const { return s_ ? s_->textureBytes : 0; }
uint32_t Renderer::ReducedLevels() const { return s_ ? s_->reduced : 0; }
void Renderer::SetQuality(RenderQuality quality) { if (s_) s_->quality = quality; }
void Renderer::SetParticles(int percent) { if (s_) s_->particlePercent = std::clamp(percent, 0, 100); }
float Renderer::TrialSeconds() const {
    float seconds = 4;
    for (const auto &[id, animation] : s_->lotties) seconds = std::max(seconds, animation->Duration());
    return std::min(120.f, seconds);
}

bool Renderer::Open(ID3D11Device *device, const Files &files, int width, int height, int videoMemory, std::wstring *why) {
    auto s = std::make_unique<State>();
    s->device = device;
    device->GetImmediateContext(&s->context);
    s->width = width; s->height = height;
    s->files = files;
    const auto *xml = sg::Find(s->files, L"scene.xml");
    if (!xml || !sg::Parse(std::string_view((const char *)xml->bytes.data(), xml->bytes.size()), sg::Mode::Stored, &s->d, why)) return false;
    if (!s->Textures(videoMemory, why)) return false;
    for (const auto &a : s->d.assets) {
        if (a.kind == sg::AssetKind::Lottie) {
            const auto *f = sg::Find(s->files, L"lottie/" + a.id + L".json");
            auto animation = std::make_unique<Lottie>();
            if (!f || !animation->Open(f->bytes, why)) return Fail(why, why && !why->empty() ? *why : L"missing Lottie " + a.id);
            s->lotties[a.id] = std::move(animation);
        } else if (a.kind == sg::AssetKind::LayerShader || a.kind == sg::AssetKind::EffectShader) {
            const auto *f = sg::Find(s->files, sg::CompiledName(a));
            if (!f || !CheckShader(f->bytes.data(), f->bytes.size(), why) ||
                FAILED(device->CreatePixelShader(f->bytes.data(), f->bytes.size(), nullptr, &s->shaders[a.id])))
                return Fail(why, L"cannot create the scene shader " + a.id);
        }
    }
    // A Lottie layer's <slot>s must be slots the file has.
    std::function<bool(const sg::Node &)> slots = [&](const sg::Node &n) {
        if (const auto *src = n.Find(L"source"); src && s->lotties.count(*src))
            for (const auto &[id, value] : n.params)
                if (!s->lotties.at(*src)->HasSlot(id)) return Fail(why, L"the Lottie animation has no slot " + id);
        for (const auto &c : n.children) if (!slots(c)) return false;
        return true;
    };
    for (const auto &n : s->d.nodes) if (!slots(n)) return false;
    if (!s->chain.Open(device, why) || !s->particles.Open(device, why)) return false;
    for (const auto &a : s->d.assets)
        if (a.kind == sg::AssetKind::Mask) {
            const auto &t = s->pictures[a.id];
            s->masks[a.id] = {t.view.Get(), t.u, t.v};
        }
    ComPtr<ID3DBlob> v, p, full;
    if (!Compile(std::string(kDrawCommon) + kDrawVertex, "vs_5_0", &v, why) || !Compile(std::string(kDrawCommon) + kDrawPixel, "ps_5_0", &p, why) ||
        !Compile(kFullScreen, "vs_5_0", &full, why) ||
        FAILED(device->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &s->drawVs)) ||
        FAILED(device->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &s->drawPs)) ||
        FAILED(device->CreateVertexShader(full->GetBufferPointer(), full->GetBufferSize(), nullptr, &s->fullScreenVs)))
        return Fail(why, L"cannot create the scene renderer's shaders");
    D3D11_BUFFER_DESC cb{};
    cb.Usage = D3D11_USAGE_DEFAULT; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.ByteWidth = sizeof(DrawConstants);
    if (FAILED(device->CreateBuffer(&cb, nullptr, &s->drawConstants))) return Fail(why, L"cannot create scene constants");
    cb.ByteWidth = sizeof(ShaderConstants);
    if (FAILED(device->CreateBuffer(&cb, nullptr, &s->shaderConstants))) return Fail(why, L"cannot create scene constants");
    D3D11_SAMPLER_DESC samp{};
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&samp, &s->sampler))) return Fail(why, L"cannot create the scene sampler");
    // Premultiplied colours: normal is "over"; add keeps what is below and adds light; screen is
    // 1 - (1 - a)(1 - b); multiply darkens what is below by the picture, through its coverage.
    struct BlendSpec { D3D11_BLEND src, dst, srcAlpha, dstAlpha; };
    const BlendSpec specs[4]{
        {D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA},
        {D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_ZERO, D3D11_BLEND_ONE},
        {D3D11_BLEND_INV_DEST_COLOR, D3D11_BLEND_ONE, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA},
        {D3D11_BLEND_DEST_COLOR, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA},
    };
    for (int i = 0; i < 4; ++i) {
        D3D11_BLEND_DESC bd{};
        auto &rt = bd.RenderTarget[0];
        rt.BlendEnable = TRUE;
        rt.SrcBlend = specs[i].src; rt.DestBlend = specs[i].dst; rt.BlendOp = D3D11_BLEND_OP_ADD;
        rt.SrcBlendAlpha = specs[i].srcAlpha; rt.DestBlendAlpha = specs[i].dstAlpha; rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
        if (FAILED(device->CreateBlendState(&bd, &s->blends[i]))) return Fail(why, L"cannot create the scene blend states");
    }
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&bd, &s->opaque))) return Fail(why, L"cannot create the scene blend states");
    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    if (FAILED(device->CreateRasterizerState(&rd, &s->raster))) return Fail(why, L"cannot create the scene rasterizer state");
    s->layers.resize(sg::kMaxNesting + 2);
    D3D11_QUERY_DESC query{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    if (FAILED(device->CreateQuery(&query, &s->disjoint))) return Fail(why, L"GPU timing is unavailable");
    query.Query = D3D11_QUERY_TIMESTAMP;
    if (FAILED(device->CreateQuery(&query, &s->begin)) || FAILED(device->CreateQuery(&query, &s->end))) return Fail(why, L"GPU timing is unavailable");
    s_ = std::move(s);
    return true;
}

bool Renderer::Draw(const Inputs &in, ID3D11Texture2D *video, ID3D11Texture2D *output, float scale, float *ms, std::wstring *why) {
    if (!s_) return Fail(why, L"the scene is not open");
    auto &s = *s_;
    const auto start = std::chrono::steady_clock::now();
    s.in = &in;
    if (video != s.videoTexture.Get()) {
        s.videoTexture = video;
        s.videoView.Reset();
        if (video && FAILED(s.device->CreateShaderResourceView(video, nullptr, &s.videoView))) return Fail(why, L"cannot read the video input");
    }
    const int cw = std::max(1, (int)std::lround(s.width * std::clamp(scale, .25f, 1.f)));
    const int ch = std::max(1, (int)std::lround(s.height * std::clamp(scale, .25f, 1.f)));
    if (s.canvas.width != cw || s.canvas.height != ch) {
        // Effect pictures were sized for the old canvas.
        s.effected.clear();
        s.scratch = Surface{};
        s.chain.Trim();
    }
    if (!s.Make(&s.canvas, cw, ch)) return Fail(why, L"cannot allocate the scene canvas");
    // Blurs, glows and rays are soft enough to work at a lower resolution and look the same: half
    // of it at the high quality, a quarter at medium and low, and full at the highest. The automatic
    // quality works at half until it lowers the canvas, then at a quarter.
    switch (s.quality) {
    case RenderQuality::Highest: s.soft = 1; break;
    case RenderQuality::High: s.soft = 2; break;
    case RenderQuality::Medium: case RenderQuality::Low: s.soft = 4; break;
    default: s.soft = scale >= 1 ? 2 : 4; break;
    }
    s.density = s.particlePercent / 100.0 * (s.quality == RenderQuality::Low ? .5 : 1);
    s.context->Begin(s.disjoint.Get());
    s.context->End(s.begin.Get());
    const sg::Frame frame = sg::Evaluate(s.d, in.values, in.data);
    std::string key;
    key.append((const char *)&cw, sizeof(cw)); key.append((const char *)&ch, sizeof(ch));
    key.append((const char *)&in.videoSequence, sizeof(in.videoSequence));
    const float camera[]{frame.cameraX, frame.cameraY, frame.cameraScale, frame.cameraRotation};
    key.append((const char *)camera, sizeof(camera));
    for (const auto &p : frame.nodes) s.Key(p, &key);
    if (key != s.lastKey) {
        s.lastKey = key;
        const float black[4]{0, 0, 0, 1};
        s.context->ClearRenderTargetView(s.canvas.target.Get(), black);
        // The stage covers the canvas, cut evenly at the sides or top and bottom.
        const double W = s.d.width, H = s.d.height;
        const double unit = std::max(cw / W, ch / H);
        const Affine stage = Move((cw - W * unit) / 2, (ch - H * unit) / 2) * Scale(unit, unit);
        for (const auto &p : frame.nodes) {
            if (p.space == sg::Space::Screen) {
                // Pinned to the display: the anchor's corner of the screen, moved by x and y in percent of
                // its height, at the stage's scale.
                sg::Placed q = p;
                q.x = (float)((p.anchor % 3) * .5 * cw + p.x * ch / 100.0) / (float)unit;
                q.y = (float)((p.anchor / 3) * .5 * ch + p.y * ch / 100.0) / (float)unit;
                if (!s.DrawNode(q, Scale(unit, unit), {cw / unit, ch / unit}, 1, 0, s.canvas, why)) return false;
                continue;
            }
            // The camera moves each node by its depth: far ones barely, near ones fully.
            const double d = p.depth;
            const Affine cameraMove = Move(W / 2, H / 2) * Turn(frame.cameraRotation * d) *
                Scale(1 + (frame.cameraScale - 1) * d, 1 + (frame.cameraScale - 1) * d) * Move(-W / 2, -H / 2) *
                Move(-frame.cameraX * d, -frame.cameraY * d);
            if (!s.DrawNode(p, stage * cameraMove, {W, H}, 1, 0, s.canvas, why)) return false;
        }
    }
    D3D11_TEXTURE2D_DESC od{};
    output->GetDesc(&od);
    if ((int)od.Width == cw && (int)od.Height == ch) s.context->CopyResource(output, s.canvas.texture.Get());
    else {
        // The canvas drawn at a lower resolution, stretched over the output.
        ComPtr<ID3D11RenderTargetView> rtv;
        if (FAILED(s.device->CreateRenderTargetView(output, nullptr, &rtv))) return Fail(why, L"cannot draw the scene output");
        State::Texture target;
        target.target = rtv; target.width = (int)od.Width; target.height = (int)od.Height;
        DrawConstants c;
        c.canvas[0] = (float)od.Width; c.canvas[1] = (float)od.Height;
        c.whole = 1; c.mode = kLayer; c.opacity = 1;
        s.Quad(target, c, sg::Blend::Normal, s.canvas.view.Get(), nullptr);
    }
    ID3D11RenderTargetView *none = nullptr;
    s.context->OMSetRenderTargets(1, &none, nullptr);
    s.context->End(s.end.Get());
    s.context->End(s.disjoint.Get());
    s.context->Flush();
    const float cpu = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - start).count();
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{};
    UINT64 begin = 0, end = 0;
    const ULONGLONG deadline = GetTickCount64() + 1500;
    while (s.context->GetData(s.disjoint.Get(), &disjoint, sizeof(disjoint), 0) == S_FALSE && GetTickCount64() < deadline) Sleep(1);
    if (s.context->GetData(s.disjoint.Get(), &disjoint, sizeof(disjoint), 0) != S_OK || disjoint.Disjoint || !disjoint.Frequency ||
        s.context->GetData(s.begin.Get(), &begin, sizeof(begin), 0) != S_OK || s.context->GetData(s.end.Get(), &end, sizeof(end), 0) != S_OK)
        return Fail(why, L"GPU frame timing failed");
    *ms = std::max(cpu, (float)((end - begin) * 1000.0 / disjoint.Frequency));
    return true;
}
}  // namespace animelogon::scene
