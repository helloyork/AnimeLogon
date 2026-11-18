// Ripples spreading from the centre of the node's box. An effect shader: `source` is the node's
// picture so far within its box, premultiplied, and the result must be premultiplied too. `mask`
// is the effect's mask over the box, white everywhere when the effect has none.
// param[0].x: how far the ripples push, as a fraction of the box. param[1].x: ripples across the
// box; param[1].y: ripples passing a point each second.
float4 main(float2 uv : TEXCOORD0) : SV_Target {
    float2 d = uv - 0.5;
    d.x *= resolution.x / resolution.y;  // round ripples on a box that is not square
    float r = length(d);
    float push = sin(r * param[1].x * 6.2831853 - time * param[1].y * 6.2831853) * param[0].x;
    push *= mask.Sample(linearClamp, uv).r * smoothstep(0.0, 0.05, r);
    float2 away = r > 0 ? d / r : 0;
    away.x /= resolution.x / resolution.y;
    return source.Sample(linearClamp, uv + away * push);
}
