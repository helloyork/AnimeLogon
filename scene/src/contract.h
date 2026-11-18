// What an author's shader is given and must keep to (D51). A layer shader draws from nothing but
// the Scene constants; an effect shader also reads `source`, the node's picture so far within its
// box (premultiplied), and `mask`, the effect's mask over the box (white everywhere when the effect
// has none). Shaders are ps_5_0, return one float4 SV_Target and take at most a float2 TEXCOORD0
// (the box's 0..1). Compiling prepends kShaderHeader to the author's file.
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace animelogon::scene {

constexpr char kShaderHeader[] = R"(
cbuffer Scene : register(b0) {
    float time; float2 resolution; float day;
    float battery; float progress; float playing; float wake;
    float4 audio;
    float month; float3 reserved;
    float4 param[16];
};
Texture2D<float4> source : register(t0);
Texture2D<float4> mask : register(t1);
SamplerState linearClamp : register(s0);
)";
struct ShaderConstants {
    float time = 0, resolution[2]{}, day = 0;
    float battery = -1, progress = -1, playing = 0, wake = 0;
    float audio[4]{};
    float month = 1, reserved[3]{};
    float param[16][4]{};
};

// Compiles an author's shader source to DXBC and checks it keeps to the contract.
bool CompileShader(std::string_view source, std::vector<uint8_t> *bytecode, std::wstring *why);
// Whether compiled DXBC keeps to the contract: what the library's compiled form is checked against.
bool CheckShader(const uint8_t *bytes, size_t size, std::wstring *why);

}  // namespace animelogon::scene
