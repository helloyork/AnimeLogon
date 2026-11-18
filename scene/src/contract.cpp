#include "contract.h"
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include <algorithm>
#include <cstring>
#include "animelogon/scenegraph.h"
#include "animelogon/text.h"
using Microsoft::WRL::ComPtr;
namespace animelogon::scene {
namespace {
bool Fail(std::wstring *why, const std::wstring &s) { if (why) *why = s; return false; }
}  // namespace

bool CompileShader(std::string_view source, std::vector<uint8_t> *bytecode, std::wstring *why) {
    const std::string text = std::string(kShaderHeader) + std::string(source);
    ComPtr<ID3DBlob> blob, error;
    if (FAILED(D3DCompile(text.data(), text.size(), "scene", nullptr, nullptr, "main", "ps_5_0",
                          D3DCOMPILE_ENABLE_STRICTNESS | D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &blob, &error)))
        return Fail(why, error ? FromUtf8(std::string_view((const char *)error->GetBufferPointer(), std::min<size_t>(error->GetBufferSize(), 1024)))
                               : L"shader compilation failed");
    const auto *p = (const uint8_t *)blob->GetBufferPointer();
    bytecode->assign(p, p + blob->GetBufferSize());
    return CheckShader(bytecode->data(), bytecode->size(), why);
}

bool CheckShader(const uint8_t *bytes, size_t size, std::wstring *why) {
    ComPtr<ID3D11ShaderReflection> r;
    D3D11_SHADER_DESC d{};
    if (size < 4 || size > scenegraph::kMaxCompiledBytes || memcmp(bytes, "DXBC", 4) ||
        FAILED(D3DReflect(bytes, size, IID_PPV_ARGS(&r))) || FAILED(r->GetDesc(&d)) ||
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
        D3D11_SHADER_INPUT_BIND_DESC b{};
        r->GetResourceBindingDesc(i, &b);
        bool good = b.BindCount == 1;
        if (b.Type == D3D_SIT_CBUFFER) {
            D3D11_SHADER_BUFFER_DESC c{};
            good = good && b.BindPoint == 0 && !strcmp(b.Name, "Scene") &&
                SUCCEEDED(r->GetConstantBufferByName("Scene")->GetDesc(&c)) && c.Size == sizeof(ShaderConstants);
        } else if (b.Type == D3D_SIT_TEXTURE) {
            const char *names[] = {"source", "mask"};
            good = good && b.BindPoint < 2 && b.Dimension == D3D_SRV_DIMENSION_TEXTURE2D && !strcmp(b.Name, names[b.BindPoint]);
        } else if (b.Type == D3D_SIT_SAMPLER) good = good && b.BindPoint == 0 && !strcmp(b.Name, "linearClamp");
        else good = false;
        if (!good) return Fail(why, L"shader uses resources outside the scene contract");
    }
    return true;
}

}  // namespace animelogon::scene
