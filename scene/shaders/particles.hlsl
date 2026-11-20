// One instance per candidate particle. Candidates are numbered and born evenly, as many a second
// as can be alive at once over a lifetime (max / lifetime); a share of them, rate / that, is kept,
// so the rate and the density setting thin the same stream instead of renumbering it. A candidate
// that is not kept, not born yet or already gone collapses to nothing off the target.
//
// Each particle drifts with the wind, carries its own velocity (slowed by drag towards the speed
// gravity alone would give it, or under plain gravity without drag), sways, spins, and changes
// size, opacity and colour over its life.
cbuffer Emitter : register(b0) {
    float4 toTarget0, toTarget1;  // target pixel = (dot(toTarget0.xyz, (x, y, 1)), dot(toTarget1.xyz, (x, y, 1)))
    float4 target;                // xy: the target's size in pixels; zw: the box's size in its units
    float4 timing;                // x: age of the first candidate; y: candidates a second; z: share kept; w: lifetime
    float4 motion;                // x: speed; y: angle (radians); z: spread (radians); w: gravity
    float4 motion2;               // x: wind; y: drag; z: spin (degrees a second); w: sway
    float4 look;                  // x: size; y: size at the end; z: opacity at the end; w: fade
    float4 color, colorEnd;       // straight
    float4 extra;                 // x: twinkle; y: stretch; z: where they are born (0 top .. 6 area); w: frames
    float4 art;                   // xy: the used part of the texture; z: 1 with a texture; w: 1 when it is premultiplied
    float4 maskRect;              // xy: the used part of the mask; z: 1 with a mask; w: the node's opacity
    float4 frameNow;              // x: the frame this draw covers; y: frames a second
    uint4 numbers;                // x: the first candidate's number; y: seed
};
Texture2D<float4> picture : register(t0);
Texture2D<float4> maskPicture : register(t1);
SamplerState linearClamp : register(s0);
static const float kTau = 6.28318531;
struct V {
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
    float2 box : TEXCOORD1;
    float4 color : COLOR0;
};
float Rand(uint n, uint salt) {
    uint h = n * 747796405u + salt * 2891336453u + numbers.y * 1181783497u;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h * (1.0 / 4294967296.0);
}
V Vertex(uint corner : SV_VertexID, uint id : SV_InstanceID) {
    V o;
    o.position = float4(-10, -10, 0, 1); o.uv = 0; o.box = 0; o.color = 0;
    uint n = numbers.x + id;
    if (Rand(n, 1) >= timing.z) return o;
    float life = timing.w * (.8 + .4 * Rand(n, 2));
    float age = timing.x - (id + Rand(n, 3)) / timing.y;
    if (age < 0 || age >= life) return o;
    float f = age / life;
    if (extra.w > 1 && fmod(floor(age * frameNow.y), extra.w) != frameNow.x) return o;
    float W = target.z, H = target.w;
    float size = lerp(look.x, look.y, f) * (.7 + .6 * Rand(n, 4));
    float e = extra.z, r5 = Rand(n, 5), r6 = Rand(n, 6);
    float2 at = e < .5 ? float2(r5 * W, -size) : e < 1.5 ? float2(r5 * W, H + size) : e < 2.5 ? float2(-size, r5 * H) :
                e < 3.5 ? float2(W + size, r5 * H) : e < 4.5 ? float2(W, H) * .5 : e < 5.5 ? float2(r5 * W, H * .5) : float2(r5 * W, r6 * H);
    float theta = motion.y + (Rand(n, 7) - .5) * motion.z;
    float2 u0 = motion.x * (.8 + .4 * Rand(n, 8)) * float2(cos(theta), sin(theta));
    float drag = motion2.y, g = motion.w;
    float2 pos, vel;
    if (drag > 1e-4) {
        float2 terminal = float2(0, g / drag);
        float d = exp(-drag * age);
        pos = terminal * age + (u0 - terminal) * (1 - d) / drag;
        vel = terminal + (u0 - terminal) * d;
    } else {
        pos = u0 * age + float2(0, .5 * g * age * age);
        vel = u0 + float2(0, g * age);
    }
    pos.x += motion2.x * age;
    vel.x += motion2.x;
    float period = 2 + 3 * Rand(n, 9), phase = Rand(n, 10) * kTau;
    pos += motion2.w * float2(sin(kTau * age / period + phase), .5 * sin(kTau * age / (period * 1.3) + phase * 2));
    pos += at;
    float alpha = lerp(1, look.z, f);
    if (look.w > 0) alpha *= saturate(f / look.w) * saturate((1 - f) / look.w);
    alpha *= 1 - extra.x * (.5 + .5 * sin(kTau * age * (.5 + 1.5 * Rand(n, 11)) + Rand(n, 12) * kTau));
    // Streaks lie along the motion; anything else turns by its spin.
    float speedNow = length(vel), len = size;
    float2 down;
    if (extra.y > 0 && speedNow > 1e-3) {
        down = vel / speedNow;
        len = max(size, speedNow * extra.y);
    } else {
        float turn = radians(Rand(n, 13) * 360 + motion2.z * (.5 + Rand(n, 14)) * (Rand(n, 15) < .5 ? -1 : 1) * age);
        down = float2(-sin(turn), cos(turn));
    }
    float2 right = float2(down.y, -down.x);
    float2 q = float2(corner & 1, corner >> 1) - .5;
    float2 p = pos + right * q.x * size + down * q.y * len;
    float2 px = float2(dot(toTarget0.xyz, float3(p, 1)), dot(toTarget1.xyz, float3(p, 1)));
    o.position = float4(px / target.xy * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = q + .5;
    o.box = p / float2(W, H);
    float4 c = lerp(color, colorEnd, f);
    c.a *= alpha * maskRect.w;
    o.color = float4(c.rgb * c.a, c.a);
    return o;
}
float4 Pixel(V v) : SV_Target {
    float4 c;
    if (art.z > .5) {
        c = picture.Sample(linearClamp, v.uv * art.xy);
        if (art.w < .5) c.rgb *= c.a;
    } else if (extra.y > 0) {
        // A streak: soft across, tapering at both ends.
        float2 d = abs(v.uv - .5) * 2;
        c = saturate(1 - d.x) * saturate((1 - d.y) * 3);
    } else {
        // A soft dot.
        c = saturate((1 - length(v.uv - .5) * 2) * 1.5);
    }
    c *= v.color;
    if (maskRect.z > .5) c *= any(v.box < 0) || any(v.box > 1) ? 0 : maskPicture.Sample(linearClamp, v.box * maskRect.xy).r;
    return c;
}
