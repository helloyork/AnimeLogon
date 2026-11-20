#include "draw.hlsli"
Texture2D<float4> picture : register(t0);
Texture2D<float4> maskPicture : register(t1);
SamplerState linearClamp : register(s0);
float4 main(float2 box : TEXCOORD0, float4 position : SV_Position) : SV_Target {
    float2 uv = whole > .5 ? float2(dot(fromCanvas0.xyz, float3(position.xy, 1)), dot(fromCanvas1.xyz, float3(position.xy, 1))) : box * maskMap.xy + maskMap.zw;
    float4 c;
    if (mode < .5) c = picture.Sample(linearClamp, box * sourceRect.xy + sourceRect.zw);
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
}
