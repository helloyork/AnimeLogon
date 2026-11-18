// Curtains of light across the sky. A layer shader: it draws the layer's box from nothing but the
// Scene constants. param[0] and param[2] are the colours at the foot and the top of a curtain,
// param[1].x the speed, param[3].x the brightness. The layer is blended with `add`, so the colour
// returned is light added to the sky.
float4 main(float2 uv : TEXCOORD0) : SV_Target {
    float t = time * param[1].x;
    float3 light = 0;
    for (int i = 0; i < 3; ++i) {
        float fi = i;
        // Each curtain's foot waves slowly across the sky.
        float foot = 0.42 + 0.06 * fi + 0.06 * sin(uv.x * (2.2 + fi) + t * (0.25 + 0.08 * fi)) + 0.025 * sin(uv.x * 7.0 - t * 0.6 + fi);
        float above = foot - uv.y;
        // Brightest at the foot, fading upwards, gone just below it.
        float curtain = above > 0 ? exp(-above / (0.12 + 0.04 * fi)) : exp(-pow(above / 0.015, 2));
        // Brighter and dimmer stretches along it, with fine upright rays.
        float along = 0.5 + 0.5 * sin(uv.x * (5.0 + 2.0 * fi) + t * 0.4 + fi * 2.0);
        float rays = 0.75 + 0.25 * sin(uv.x * 140.0 + 3.0 * sin(uv.x * 11.0 + t));
        float3 colour = lerp(param[0].rgb, param[2].rgb, saturate(above / 0.25));
        light += curtain * along * rays * colour * (0.9 - 0.25 * fi);
    }
    return float4(light * param[3].x, 1);
}
