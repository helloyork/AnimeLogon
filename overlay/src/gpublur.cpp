#include "gpublur.h"

#include "animelogon/blur.h"
#include "blur_box.h"
#include "blur_gauss.h"
#include "blur_up.h"
#include "blur_vs.h"

using Microsoft::WRL::ComPtr;

namespace {

// shaders/blur.hlsl's cbuffer Blur.
struct Constants {
    int size[2];
    int factor;
    float sigma;
    int direction[2];
    float scale[2];
};
static_assert(sizeof(Constants) % 16 == 0, "constant buffer size");

}  // namespace

bool GpuBlur::Create(ID3D11Device *device) {
    device_ = device;
    if (FAILED(device->CreateVertexShader(g_blur_vs, sizeof(g_blur_vs), nullptr, &vs_)) ||
        FAILED(device->CreatePixelShader(g_blur_box, sizeof(g_blur_box), nullptr, &box_)) ||
        FAILED(device->CreatePixelShader(g_blur_gauss, sizeof(g_blur_gauss), nullptr, &gauss_)) ||
        FAILED(device->CreatePixelShader(g_blur_up, sizeof(g_blur_up), nullptr, &up_)))
        return false;
    D3D11_BUFFER_DESC b{};
    b.ByteWidth = sizeof(Constants);
    b.Usage = D3D11_USAGE_DEFAULT;
    b.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    if (FAILED(device->CreateBuffer(&b, nullptr, &constants_))) return false;
    D3D11_SAMPLER_DESC s{};
    s.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    s.MaxLOD = D3D11_FLOAT32_MAX;
    return SUCCEEDED(device->CreateSamplerState(&s, &sampler_));
}

bool GpuBlur::Make(Surface *s, UINT width, UINT height, DXGI_FORMAT format) {
    D3D11_TEXTURE2D_DESC d{};
    d.Width = width;
    d.Height = height;
    d.MipLevels = 1;
    d.ArraySize = 1;
    d.Format = format;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    return SUCCEEDED(device_->CreateTexture2D(&d, nullptr, &s->texture)) &&
           SUCCEEDED(device_->CreateRenderTargetView(s->texture.Get(), nullptr, &s->rtv)) &&
           SUCCEEDED(device_->CreateShaderResourceView(s->texture.Get(), nullptr, &s->srv));
}

ID3D11RenderTargetView *GpuBlur::Begin(UINT width, UINT height, float sigma) {
    now_ = nullptr;
    if (!device_ || !width || !height || !(sigma > 0.0f)) return nullptr;
    const int factor = animelogon::blur::Factor(sigma);
    const auto key = std::make_tuple(width, height, factor);
    auto it = surfaces_.find(key);
    if (it == surfaces_.end()) {
        Surfaces s;
        s.cellsW = (width + factor - 1) / factor;
        s.cellsH = (height + factor - 1) / factor;
        // The picture as the window would show it; the cells in half floats, so that a smooth dark
        // gradient spread over few cells does not come back up in bands.
        if (!Make(&s.picture, width, height, DXGI_FORMAT_B8G8R8A8_UNORM) ||
            !Make(&s.cells, s.cellsW, s.cellsH, DXGI_FORMAT_R16G16B16A16_FLOAT) ||
            !Make(&s.across, s.cellsW, s.cellsH, DXGI_FORMAT_R16G16B16A16_FLOAT))
            return nullptr;
        it = surfaces_.emplace(key, std::move(s)).first;
    }
    now_ = &it->second;
    width_ = width;
    height_ = height;
    factor_ = factor;
    sigma_ = sigma;
    return now_->picture.rtv.Get();
}

void GpuBlur::Pass(ID3D11DeviceContext *context, ID3D11PixelShader *shader, ID3D11ShaderResourceView *from,
                   ID3D11RenderTargetView *to, UINT width, UINT height) {
    D3D11_VIEWPORT vp{0.0f, 0.0f, (float)width, (float)height, 0.0f, 1.0f};
    context->RSSetViewports(1, &vp);
    context->OMSetRenderTargets(1, &to, nullptr);
    context->PSSetShader(shader, nullptr, 0);
    context->PSSetShaderResources(0, 1, &from);
    context->Draw(3, 0);
    ID3D11ShaderResourceView *none = nullptr;
    context->PSSetShaderResources(0, 1, &none);
    context->OMSetRenderTargets(0, nullptr, nullptr);
}

bool GpuBlur::End(ID3D11DeviceContext *context, ID3D11RenderTargetView *target) {
    if (!now_) return false;
    Surfaces &s = *now_;
    now_ = nullptr;
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(nullptr);
    context->VSSetShader(vs_.Get(), nullptr, 0);
    ID3D11Buffer *cb[] = {constants_.Get()};
    context->PSSetConstantBuffers(0, 1, cb);
    ID3D11SamplerState *samplers[] = {sampler_.Get()};
    context->PSSetSamplers(0, 1, samplers);

    Constants c{};
    c.size[0] = (int)width_;
    c.size[1] = (int)height_;
    c.factor = factor_;
    context->UpdateSubresource(constants_.Get(), 0, nullptr, &c, 0, 0);
    Pass(context, box_.Get(), s.picture.srv.Get(), s.cells.rtv.Get(), s.cellsW, s.cellsH);

    c.size[0] = (int)s.cellsW;
    c.size[1] = (int)s.cellsH;
    c.sigma = sigma_ / (float)factor_;
    c.direction[0] = 1;
    c.direction[1] = 0;
    context->UpdateSubresource(constants_.Get(), 0, nullptr, &c, 0, 0);
    Pass(context, gauss_.Get(), s.cells.srv.Get(), s.across.rtv.Get(), s.cellsW, s.cellsH);
    c.direction[0] = 0;
    c.direction[1] = 1;
    context->UpdateSubresource(constants_.Get(), 0, nullptr, &c, 0, 0);
    Pass(context, gauss_.Get(), s.across.srv.Get(), s.cells.rtv.Get(), s.cellsW, s.cellsH);

    c.scale[0] = 1.0f / ((float)factor_ * (float)s.cellsW);
    c.scale[1] = 1.0f / ((float)factor_ * (float)s.cellsH);
    context->UpdateSubresource(constants_.Get(), 0, nullptr, &c, 0, 0);
    Pass(context, up_.Get(), s.cells.srv.Get(), target, width_, height_);
    return true;
}
