#include "draw.hlsli"
struct V { float2 box : TEXCOORD0; float4 position : SV_Position; };
V main(uint id : SV_VertexID) {
    float2 q = float2(id & 1, id >> 1);
    V v; v.box = q;
    float2 p = whole > .5 ? q * canvas.xy : float2(dot(toCanvas0.xyz, float3(q, 1)), dot(toCanvas1.xyz, float3(q, 1)));
    v.position = float4(p / canvas.xy * float2(2, -2) + float2(-1, 1), 0, 1);
    return v;
}
