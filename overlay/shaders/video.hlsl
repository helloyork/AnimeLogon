// NV12 (BT.709, limited range) to RGB, with the window's placement on the video. Before the
// first frame, the baked still -- the picture Windows already shows -- in its place.

cbuffer Params : register(b0) {
    float2 scale;     // window pixel to picture UV
    float2 offset;
    float2 uvMax;     // the visible part of the texture
    float dim;        // 0 shows the picture, 1 is black
    float source;     // 0 black, 1 the video, 2 the still
};

Texture2D<float> luma : register(t0);
Texture2D<float2> chroma : register(t1);
Texture2D<float4> still : register(t2);
SamplerState linearClamp : register(s0);

float4 VS(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 PS(float4 pos : SV_Position) : SV_Target {
    float2 uv = pos.xy * scale + offset;
    if (source < 0.5 || any(uv < 0) || any(uv > 1)) return float4(0, 0, 0, 1);
    if (source > 1.5) return float4(still.Sample(linearClamp, uv).rgb * (1.0 - dim), 1.0);
    uv *= uvMax;
    float y = (luma.Sample(linearClamp, uv) - 16.0 / 255.0) * (255.0 / 219.0);
    float2 c = (chroma.Sample(linearClamp, uv) - 128.0 / 255.0) * (255.0 / 224.0);
    float3 rgb = float3(y + 1.5748 * c.y, y - 0.1873 * c.x - 0.4681 * c.y, y + 1.8556 * c.x);
    return float4(saturate(rgb) * (1.0 - dim), 1.0);
}
