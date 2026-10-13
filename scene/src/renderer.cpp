#include "renderer.h"
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include "animelogon/bitmap.h"
#include "animelogon/image.h"
#include "animelogon/text.h"
using Microsoft::WRL::ComPtr;
namespace animelogon::scene {
namespace {
constexpr char header[] = R"(
cbuffer Scene : register(b0) {
    float time; float2 resolution; float dayfraction;
    float battery; float playback; float playbackState; float reserved;
    float4 param[16];
};
Texture2D<float4> image0 : register(t0);
Texture2D<float4> image1 : register(t1);
Texture2D<float4> image2 : register(t2);
Texture2D<float4> image3 : register(t3);
Texture2D<float4> below : register(t4);
SamplerState linearClamp : register(s0);
)";
constexpr char vertex[] = R"(
// TEXCOORD must occupy the same signature register as the scene pixel shader.
struct V { float2 uv : TEXCOORD0; float4 position : SV_Position; };
V main(uint id : SV_VertexID) {
    V v; v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv * float2(2,-2) + float2(-1,1),0,1); return v;
})";
constexpr char compose[] = R"(
cbuffer Blit : register(b0) {
    float4 rect; float4 sampleRect; float opacity; float blend; float premultiplied; float base;
};
Texture2D<float4> bottom : register(t0);
Texture2D<float4> layer : register(t1);
SamplerState linearClamp : register(s0);
float4 main(float2 uv : TEXCOORD0) : SV_Target {
    float4 dst = base > .5 ? float4(0,0,0,1) : bottom.Sample(linearClamp,uv);
    float2 q = (uv - rect.xy) / rect.zw;
    if (any(q < 0) || any(q > 1)) return dst;
    q = q * sampleRect.zw + sampleRect.xy;
    if (any(q < 0) || any(q > 1)) return dst;
    float4 src = saturate(layer.Sample(linearClamp,q));
    src.rgb *= premultiplied > .5 ? 1 : src.a;
    src *= opacity;
    float3 rgb = src.rgb + dst.rgb * (1-src.a);
    if (blend > .5 && blend < 1.5) rgb = dst.rgb + src.rgb;
    else if (blend > 1.5 && blend < 2.5) rgb = dst.rgb + src.rgb - dst.rgb * src.rgb;
    else if (blend > 2.5) rgb = dst.rgb * (1-src.a+src.rgb);
    return float4(rgb,1);
})";
struct Constants {
    float time = 0, resolution[2]{}, day = 0;
    float battery = -1, playback = 0, playing = 0, reserved = 0;
    float param[16][4]{};
};
struct Blit {
    float rect[4]{0,0,1,1}, sampleRect[4]{0,0,1,1};
    float opacity = 1, blend = 0, premul = 0, base = 0;
};
struct Texture {
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    ComPtr<ID3D11RenderTargetView> target;
    int width = 0, height = 0;
};
bool Fail(std::wstring *why, const std::wstring &s) { if (why) *why = s; return false; }
bool Compile(std::string_view source, const char *profile, ComPtr<ID3DBlob> *blob, std::wstring *why) {
    ComPtr<ID3DBlob> error;
    const HRESULT hr = D3DCompile(source.data(), source.size(), "scene", nullptr, nullptr, "main", profile,
                                  D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob->ReleaseAndGetAddressOf(), &error);
    if (FAILED(hr)) return Fail(why, error ? FromUtf8(std::string_view((const char *)error->GetBufferPointer(), std::min<size_t>(error->GetBufferSize(), 1024))) : L"shader compilation failed");
    return true;
}
bool ShaderContract(const void *bytes, size_t size, std::wstring *why) {
    ComPtr<ID3D11ShaderReflection> r;
    D3D11_SHADER_DESC d{};
    if (FAILED(D3DReflect(bytes, size, IID_PPV_ARGS(&r))) || FAILED(r->GetDesc(&d)) ||
        D3D11_SHVER_GET_TYPE(d.Version) != D3D11_SHVER_PIXEL_SHADER || D3D11_SHVER_GET_MAJOR(d.Version) != 5 ||
        D3D11_SHVER_GET_MINOR(d.Version) != 0 || d.OutputParameters != 1 || d.InputParameters > 1)
        return Fail(why, L"shader must use the scene ps_5_0 contract");
    D3D11_SIGNATURE_PARAMETER_DESC p{};
    r->GetOutputParameterDesc(0, &p);
    if (p.SystemValueType != D3D_NAME_TARGET || p.SemanticIndex != 0 || p.Mask != 15 || p.ComponentType != D3D_REGISTER_COMPONENT_FLOAT32)
        return Fail(why, L"main must return float4 SV_Target");
    if (d.InputParameters) {
        r->GetInputParameterDesc(0, &p);
        if (strcmp(p.SemanticName, "TEXCOORD") || p.SemanticIndex || p.Mask != 3 || p.ComponentType != D3D_REGISTER_COMPONENT_FLOAT32)
            return Fail(why, L"main must accept float2 TEXCOORD0");
    }
    for (UINT i = 0; i < d.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC b{}; r->GetResourceBindingDesc(i, &b);
        bool good = b.BindCount == 1;
        if (b.Type == D3D_SIT_CBUFFER) {
            D3D11_SHADER_BUFFER_DESC c{};
            good = good && b.BindPoint == 0 && !strcmp(b.Name, "Scene") &&
                SUCCEEDED(r->GetConstantBufferByName("Scene")->GetDesc(&c)) && c.Size == sizeof(Constants);
        } else if (b.Type == D3D_SIT_TEXTURE) {
            const char *names[] = {"image0", "image1", "image2", "image3", "below"};
            good = good && b.BindPoint < 5 && b.Dimension == D3D_SRV_DIMENSION_TEXTURE2D && !strcmp(b.Name, names[b.BindPoint]);
        } else if (b.Type == D3D_SIT_SAMPLER) good = good && b.BindPoint == 0 && !strcmp(b.Name, "linearClamp");
        else good = false;
        if (!good) return Fail(why, L"shader uses resources outside the scene contract");
    }
    return true;
}
}
struct Renderer::State {
    Document d; Files normalized;
    int width = 0, height = 0;
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> blit;
    ComPtr<ID3D11Buffer> constants, blitConstants;
    ComPtr<ID3D11SamplerState> sampler;
    std::array<Texture, 4> images;
    Texture canvas[2], video;
    std::map<const Layer *, Texture> effects;
    std::map<std::wstring, ComPtr<ID3D11PixelShader>> shaders;
    ComPtr<ID3D11Query> disjoint, begin, end;
    bool Make(Texture *t, int w, int h, const uint8_t *pixels = nullptr) {
        if (t->width == w && t->height == h && t->texture && !pixels) return true;
        *t = Texture{};
        D3D11_TEXTURE2D_DESC desc{}; desc.Width = w; desc.Height = h; desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
        desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        D3D11_SUBRESOURCE_DATA data{pixels, (UINT)w * 4, 0};
        if (FAILED(device->CreateTexture2D(&desc, pixels ? &data : nullptr, &t->texture)) ||
            FAILED(device->CreateShaderResourceView(t->texture.Get(), nullptr, &t->view)) ||
            FAILED(device->CreateRenderTargetView(t->texture.Get(), nullptr, &t->target))) return false;
        t->width = w; t->height = h; return true;
    }
    bool Bitmap(Texture *t, const File &f, std::wstring *why) {
        int w = 0, h = 0;
        return (bitmap::ReadSize(f.bytes.data(), f.bytes.size(), f.bytes.size(), &w, &h) &&
            Make(t, w, h, f.bytes.data() + bitmap::kHeaderBytes)) || Fail(why, L"invalid canonical scene image");
    }
    void DrawTo(const Texture &target, ID3D11PixelShader *shader, ID3D11Buffer *cb, const std::vector<ID3D11ShaderResourceView *> &views) {
        D3D11_VIEWPORT vp{0,0,(float)target.width,(float)target.height,0,1};
        context->RSSetViewports(1, &vp);
        ID3D11RenderTargetView *rtv = target.target.Get(); context->OMSetRenderTargets(1, &rtv, nullptr);
        context->IASetInputLayout(nullptr); context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0); context->PSSetShader(shader, nullptr, 0);
        context->PSSetConstantBuffers(0, 1, &cb);
        ID3D11SamplerState *s = sampler.Get(); context->PSSetSamplers(0, 1, &s);
        context->PSSetShaderResources(0, (UINT)views.size(), views.data());
        context->Draw(3, 0);
        ID3D11ShaderResourceView *none[5]{}; context->PSSetShaderResources(0, 5, none);
        context->OMSetRenderTargets(0, nullptr, nullptr);
    }
    void Composite(const Texture &dst, const Texture *bottom, const Texture &layer, const Blit &b) {
        context->UpdateSubresource(blitConstants.Get(), 0, nullptr, &b, 0, 0);
        DrawTo(dst, blit.Get(), blitConstants.Get(), {bottom ? bottom->view.Get() : nullptr, layer.view.Get()});
    }
    bool LayerFrame(const Layer &l, const Inputs &in, const Texture *below, Texture **out, float scale, std::wstring *why) {
        if (l.kind == Kind::Image) {
            for (size_t i = 0; i < d.images.size(); ++i) if (d.images[i].id == l.image) { *out = &images[i]; return true; }
        }
        if (l.kind == Kind::Video) { *out = &video; return true; }
        if (l.kind == Kind::Lottie) return Fail(why, L"this renderer does not yet support Lottie");
        const int h = std::max(1, (int)(height * scale * (l.full ? 1 : l.height / 100)));
        const int w = std::max(1, (int)(h * (double)width / height));
        auto &effect = effects[&l];
        if (!Make(&effect, w, h)) return Fail(why, L"cannot allocate the effect texture");
        Constants c; c.time = in.time; c.resolution[0] = (float)w; c.resolution[1] = (float)h;
        c.day = in.day; c.battery = in.battery; c.playback = in.progress; c.playing = in.playing;
        for (const auto &p : l.params) {
            const auto v = Value(d, in.values, p.value); const size_t n = wcstoul(p.id.c_str(), nullptr, 10);
            std::copy(v.begin(), v.end(), c.param[n]);
        }
        context->UpdateSubresource(constants.Get(), 0, nullptr, &c, 0, 0);
        DrawTo(effect, shaders.at(l.file).Get(), constants.Get(),
            {images[0].view.Get(), images[1].view.Get(), images[2].view.Get(), images[3].view.Get(), l.readsBelow && below ? below->view.Get() : nullptr});
        *out = &effect; return true;
    }
};
Renderer::Renderer() = default;
Renderer::~Renderer() = default;
const Files &Renderer::Normalized() const { return s_->normalized; }
bool Renderer::Open(ID3D11Device *device, Files files, bool importing, int width, int height, std::wstring *why) {
    auto s = std::make_unique<State>(); s->device = device; device->GetImmediateContext(&s->context);
    s->width = width; s->height = height;
    const auto *xml = Find(files, L"scene.xml");
    if (!xml || !Parse(std::string_view((const char *)xml->bytes.data(), xml->bytes.size()), &s->d, why)) return false;
    auto &d = s->d;
    if (importing) {
        if (d.base.kind == Kind::Video) {
            const auto *poster = Find(files, L"images/video.bmp");
            if (!poster) return Fail(why, L"video first frame is missing");
            s->normalized.push_back(*poster);
        }
        for (size_t i = 0; i < d.images.size(); ++i) {
            const auto *f = Find(files, d.images[i].file);
            image::Picture picture;
            if (!f || image::NormalizeBytes(f->bytes.data(), f->bytes.size(), &picture, why) != image::Status::Ok) return Fail(why, L"cannot normalize a scene image");
            const std::wstring name = L"images/i" + std::to_wstring(i) + L".bmp";
            s->normalized.push_back({name, bitmap::Build(picture.width, picture.height, picture.bgra.data(), picture.bgra.size())});
            for (auto &e : d.xml.children) if (e.name == L"image" && e.Find(L"id") && *e.Find(L"id") == d.images[i].id)
                for (auto &a : e.attributes) if (a.first == L"src") a.second = name;
            d.images[i].file = name;
        }
        std::map<std::wstring, std::wstring> renamed;
        auto normalizeLayer = [&](Layer &l) {
            if (l.kind != Kind::Shader && l.kind != Kind::Lottie) return true;
            if (auto it = renamed.find(l.file); it != renamed.end()) { l.file = it->second; return true; }
            const auto *f = Find(files, l.file);
            if (!f) return Fail(why, L"missing scene asset " + l.file);
            const std::wstring name = (l.kind == Kind::Shader ? L"shaders/s" : L"lottie/l") + std::to_wstring(renamed.size()) + (l.kind == Kind::Shader ? L".hlsl" : L".json");
            if (f->bytes.size() > (l.kind == Kind::Shader ? kMaxShaderBytes : kMaxLottieBytes)) return Fail(why, L"scene asset is too large");
            s->normalized.push_back({name, f->bytes});
            if (l.kind == Kind::Shader) {
                const std::string source = std::string(header) + std::string((const char *)f->bytes.data(), f->bytes.size());
                ComPtr<ID3DBlob> blob;
                if (!Compile(source, "ps_5_0", &blob, why) || !ShaderContract(blob->GetBufferPointer(), blob->GetBufferSize(), why)) return false;
                const auto *p = (const uint8_t *)blob->GetBufferPointer();
                s->normalized.push_back({BytecodeName(name), {p, p + blob->GetBufferSize()}});
            }
            renamed[l.file] = name; l.file = name; return true;
        };
        if (!normalizeLayer(d.base)) return false;
        for (auto &l : d.layers) if (!normalizeLayer(l)) return false;
        for (auto &e : d.xml.children) if (e.name == L"base" || e.name == L"layer")
            for (auto &a : e.attributes) {
                if ((a.first == L"shader" || a.first == L"lottie") && renamed.count(a.second)) a.second = renamed.at(a.second);
                else if (a.first == L"video") a.second = L"video.mp4";
            }
        if (d.base.kind == Kind::Video) d.base.file = L"video.mp4";
        const std::string normalized = Normalize(d);
        s->normalized.push_back({L"scene.xml", {(const uint8_t *)normalized.data(), (const uint8_t *)normalized.data() + normalized.size()}});
        files = s->normalized;
    } else s->normalized = files;
    for (size_t i = 0; i < d.images.size(); ++i) {
        const auto *f = Find(files, d.images[i].file);
        if (!f || !s->Bitmap(&s->images[i], *f, why)) return false;
    }
    auto shader = [&](const Layer &l) {
        if (l.kind != Kind::Shader || s->shaders.count(l.file)) return true;
        const auto *f = Find(files, BytecodeName(l.file));
        if (!f || !ShaderContract(f->bytes.data(), f->bytes.size(), why) ||
            FAILED(device->CreatePixelShader(f->bytes.data(), f->bytes.size(), nullptr, &s->shaders[l.file]))) return Fail(why, L"cannot create the scene shader");
        return true;
    };
    if (!shader(d.base)) return false;
    for (const auto &l : d.layers) if (!shader(l)) return false;
    if (d.base.kind == Kind::Video) {
        const auto *f = Find(files, L"images/video.bmp");
        if (!f || !s->Bitmap(&s->video, *f, why)) return false;
    }
    ComPtr<ID3DBlob> v, p;
    if (!Compile(vertex, "vs_5_0", &v, why) || !Compile(compose, "ps_5_0", &p, why) ||
        FAILED(device->CreateVertexShader(v->GetBufferPointer(), v->GetBufferSize(), nullptr, &s->vs)) ||
        FAILED(device->CreatePixelShader(p->GetBufferPointer(), p->GetBufferSize(), nullptr, &s->blit))) return false;
    D3D11_BUFFER_DESC cb{}; cb.Usage = D3D11_USAGE_DEFAULT; cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.ByteWidth = sizeof(Constants); if (FAILED(device->CreateBuffer(&cb, nullptr, &s->constants))) return false;
    cb.ByteWidth = sizeof(Blit); if (FAILED(device->CreateBuffer(&cb, nullptr, &s->blitConstants))) return false;
    D3D11_SAMPLER_DESC samp{}; samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP; samp.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device->CreateSamplerState(&samp, &s->sampler))) return false;
    D3D11_QUERY_DESC query{D3D11_QUERY_TIMESTAMP_DISJOINT, 0};
    if (FAILED(device->CreateQuery(&query, &s->disjoint))) return Fail(why, L"GPU timing is unavailable");
    query.Query = D3D11_QUERY_TIMESTAMP;
    if (FAILED(device->CreateQuery(&query, &s->begin)) || FAILED(device->CreateQuery(&query, &s->end))) return false;
    s_ = std::move(s); return true;
}
bool Renderer::Draw(const Inputs &in, ID3D11Texture2D *video, ID3D11Texture2D *output, float scale, float *ms, std::wstring *why) {
    auto &s = *s_;
    const auto start = std::chrono::steady_clock::now();
    if (!s.Make(&s.canvas[0], s.width, s.height) || !s.Make(&s.canvas[1], s.width, s.height)) return Fail(why, L"cannot allocate scene canvases");
    if (video) {
        if (!s.Make(&s.video, s.width, s.height)) return false;
        s.context->CopyResource(s.video.texture.Get(), video);
    }
    s.context->Begin(s.disjoint.Get()); s.context->End(s.begin.Get());
    Texture *layer = nullptr;
    if (!s.LayerFrame(s.d.base, in, nullptr, &layer, scale, why)) return false;
    Blit b; b.base = 1;
    if (s.d.base.kind == Kind::Image || s.d.base.kind == Kind::Video) {
        const float imageAspect = (float)layer->width / layer->height, canvasAspect = (float)s.width / s.height;
        if (s.d.base.fit == Scaling::Fill) {
            if (imageAspect > canvasAspect) { b.sampleRect[2] = canvasAspect / imageAspect; b.sampleRect[0] = (1-b.sampleRect[2]) / 2; }
            else { b.sampleRect[3] = imageAspect / canvasAspect; b.sampleRect[1] = (1-b.sampleRect[3]) / 2; }
        } else if (s.d.base.fit == Scaling::Fit) {
            if (imageAspect > canvasAspect) { b.rect[3] = canvasAspect / imageAspect; b.rect[1] = (1-b.rect[3]) / 2; }
            else { b.rect[2] = imageAspect / canvasAspect; b.rect[0] = (1-b.rect[2]) / 2; }
        }
    }
    s.Composite(s.canvas[0], nullptr, *layer, b);
    int current = 0;
    for (const auto &l : s.d.layers) {
        if (Value(s.d, in.values, l.when)[0] == 0) continue;
        if (!s.LayerFrame(l, in, &s.canvas[current], &layer, scale, why)) return false;
        b = Blit{}; b.opacity = std::clamp(Value(s.d, in.values, l.opacity)[0], 0.f, 1.f); b.blend = (float)l.blend;
        if (!l.full) {
            b.rect[3] = l.height / 100; b.rect[2] = b.rect[3] * ((float)layer->width / layer->height) / ((float)s.width / s.height);
            b.rect[0] = (l.anchor % 3) * .5f * (1-b.rect[2]) + l.x / 100;
            b.rect[1] = (l.anchor / 3) * .5f * (1-b.rect[3]) + l.y / 100;
        }
        s.Composite(s.canvas[1-current], &s.canvas[current], *layer, b); current = 1-current;
    }
    s.context->CopyResource(output, s.canvas[current].texture.Get());
    s.context->End(s.end.Get()); s.context->End(s.disjoint.Get()); s.context->Flush();
    const float cpu = std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now()-start).count();
    D3D11_QUERY_DATA_TIMESTAMP_DISJOINT disjoint{}; UINT64 begin = 0, end = 0;
    const ULONGLONG deadline = GetTickCount64() + 1500;
    while (s.context->GetData(s.disjoint.Get(), &disjoint, sizeof(disjoint), 0) == S_FALSE && GetTickCount64() < deadline) Sleep(1);
    if (s.context->GetData(s.disjoint.Get(), &disjoint, sizeof(disjoint), 0) != S_OK || disjoint.Disjoint || !disjoint.Frequency ||
        s.context->GetData(s.begin.Get(), &begin, sizeof(begin), 0) != S_OK || s.context->GetData(s.end.Get(), &end, sizeof(end), 0) != S_OK)
        return Fail(why, L"GPU frame timing failed");
    *ms = std::max(cpu, (float)((end-begin) * 1000.0 / disjoint.Frequency));
    return true;
}
}  // namespace animelogon::scene
