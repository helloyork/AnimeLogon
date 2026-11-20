// The built-in effects, numbered as effects.cpp lists them, and the passes the blurs, glows and rays
// are made of. Distances an author writes are fractions of the box's height (bend's of the box's
// length from its root); the shader turns them into the picture's pixels. Pictures are premultiplied
// by alpha throughout.
cbuffer Effect : register(b0) {
    float4 size;      // xy: the picture's used size in pixels; zw: 1 / its texture's size
    float4 box;       // xy: the box's top-left in the picture's pixels; zw: the box's size
    float4 p0, p1;    // the effect's parameters
    float4 tint;      // its colour, straight
    float4 maskRect;  // xy: the used part of the mask's texture; z: 1 when the effect has a mask
    float4 lowSize;   // xy: the used size of the low-resolution picture being read; zw: 1 / its texture's size
    float4 level;     // x: picture pixels per low-resolution pixel; y: blur sigma; zw: blur direction
    float time, kind, extend, seed;
};
Texture2D<float4> picture : register(t0);
Texture2D<float4> maskPicture : register(t1);
Texture2D<float4> low : register(t2);
SamplerState linearClamp : register(s0);
static const float kTau = 6.28318531;
static const float3 kLuma = float3(.2126, .7152, .0722);

// The picture at a position in its pixels: transparent outside it, or for a picture that covers the
// screen, the box's nearest edge pixel.
float4 At(float2 px) {
    if (extend > .5) px = clamp(px, box.xy + .5, box.xy + box.zw - .5);
    else {
        if (any(px < 0) || any(px > size.xy)) return 0;
        px = clamp(px, .5, size.xy - .5);
    }
    return picture.SampleLevel(linearClamp, px * size.zw, 0);
}
// The effect's mask over the box; outside the box nothing is masked in.
float MaskAt(float2 uv) {
    if (maskRect.z < .5) return 1;
    if (any(uv < 0) || any(uv > 1)) return 0;
    return maskPicture.SampleLevel(linearClamp, uv * maskRect.xy, 0).r;
}
// The low-resolution picture under a position in the picture's pixels.
float4 Low(float2 px) {
    float2 q = clamp(px / level.x, .5, lowSize.xy - .5);
    return low.SampleLevel(linearClamp, q * lowSize.zw, 0);
}
float Hash(float2 p) {
    uint2 q = asuint(int2(floor(p)));
    uint h = q.x * 1597334677u ^ q.y * 3812015801u;
    h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16;
    return h * (1.0 / 4294967296.0);
}
float Noise(float2 p) {
    float2 i = floor(p), f = p - i, u = f * f * (3 - 2 * f);
    return lerp(lerp(Hash(i), Hash(i + float2(1, 0)), u.x), lerp(Hash(i + float2(0, 1)), Hash(i + 1), u.x), u.y);
}

float4 Effect(float2 unused : TEXCOORD0, float4 position : SV_Position) : SV_Target {
    float2 px = position.xy;
    float2 uv = (px - box.xy) / box.zw;
    float m = MaskAt(uv);
    float W = box.z, H = box.w;
    int k = (int)kind;
    [branch] if (k == 0) {  // bend: sways more the farther from its root, the tip lagging by `waves`
        float root = p0.z;
        float s = saturate(root < .5 ? uv.y : root < 1.5 ? 1 - uv.y : root < 2.5 ? uv.x : 1 - uv.x);
        float d = p0.x * .12 * (root < 1.5 ? H : W) * s * s * sin(kTau * (time / p0.y - p0.w * s)) * m;
        return At(px - (root < 1.5 ? float2(d, 0) : float2(0, d)));
    }
    [branch] if (k == 1) {  // breathe: grows about its pivot and back, more upward than sideways
        float g = p0.x * (.5 - .5 * cos(kTau * time / p0.y)) * m;
        float2 pivot = box.xy + p1.xy * box.zw;
        return At(pivot + (px - pivot) / (1 + g * float2(.5, 1)));
    }
    [branch] if (k == 2) {  // wave: a sine travelling across or down
        if (p0.w < .5) return At(px - float2(0, p0.x * H * sin(kTau * ((px.x - box.x) / H / p0.y - time / p0.z)) * m));
        return At(px - float2(p0.x * H * sin(kTau * ((px.y - box.y) / H / p0.y - time / p0.z)) * m, 0));
    }
    [branch] if (k == 3) {  // rings: ripples spreading from a centre
        float2 d = px - (box.xy + p0.xy * box.zw);
        float r = length(d) / H;
        float a = p0.z * H * sin(kTau * (r / p0.w - p1.x * time)) * smoothstep(0, p0.w * .5, r) * m;
        return At(px - (r > 0 ? d / (r * H) : 0) * a);
    }
    [branch] if (k == 4) {  // flow: streaks of disturbance running along a direction
        float a = radians(p0.x);
        float2 dir = float2(cos(a), sin(a)), across = float2(-dir.y, dir.x);
        float2 P = (px - box.xy) / H;
        float2 q = float2((dot(P, dir) - p0.y * time) * 4, dot(P, across) * 16) + seed;
        float2 off = dir * (Noise(q) * 2 - 1) + across * .3 * (Noise(q + 31.7) * 2 - 1);
        return At(px - off * p0.z * H * m);
    }
    [branch] if (k == 5) {  // pan: the picture scrolls within its box and wraps around
        if (any(uv < 0) || any(uv > 1)) return At(px);
        float2 moved = frac(uv - p0.xy * time * H / box.zw);
        return lerp(At(px), At(box.xy + moved * box.zw), m);
    }
    [branch] if (k == 6) {  // jitter: jumps to a new nearby place `rate` times a second
        float step = floor(time * p0.y);
        float2 j = float2(Hash(float2(step, seed)), Hash(float2(seed, step + 7))) * 2 - 1;
        return At(px - j * p0.x * H * m);
    }
    [branch] if (k == 7) {  // glow: the bright parts, blurred, added as light
        float4 c = At(px);
        return float4(c.rgb + Low(px).rgb * tint.rgb * tint.a * p0.x, c.a);
    }
    [branch] if (k == 8) {  // rays: the bright parts streaked away from a centre, added as light
        float4 c = At(px);
        return float4(c.rgb + Low(px).rgb * tint.rgb * tint.a * p0.z, c.a);
    }
    [branch] if (k == 9) {  // sheen: a band of light crossing in the first third of each period
        float a = radians(p0.x);
        float2 dir = float2(cos(a), sin(a));
        float s = dot((px - box.xy - box.zw * .5) / H, dir);
        float reach = .5 * (abs(dir.x) * W + abs(dir.y) * H) / H + p0.y;
        float f = frac(time / p0.z) * 3;
        float band = f < 1 ? 1 - smoothstep(0, p0.y * .5, abs(s - lerp(-reach, reach, f))) : 0;
        float4 c = At(px);
        return float4(c.rgb + tint.rgb * tint.a * p0.w * band * c.a * m, c.a);
    }
    [branch] if (k == 10) {  // flicker: dims and recovers unevenly
        float4 c = At(px);
        return float4(c.rgb * (1 - p0.x * Noise(float2(time * p0.y, seed)) * m), c.a);
    }
    [branch] if (k == 11) {  // grade: brightness, contrast, saturation and warmth
        float4 c = At(px);
        if (c.a <= 0) return c;
        float3 v = (c.rgb / c.a - .5) * p0.y + .5 + p0.x;
        v = lerp(dot(v, kLuma), v, p0.z) + p0.w * float3(.12, .02, -.12);
        return float4(lerp(c.rgb, saturate(v) * c.a, m), c.a);
    }
    [branch] if (k == 12) {  // tint: shades of one colour
        float4 c = At(px);
        return float4(lerp(c.rgb, dot(c.rgb, kLuma) * tint.rgb, p0.x * tint.a * m), c.a);
    }
    [branch] if (k == 13) return lerp(At(px), Low(px), m);  // blur
    [branch] if (k == 14) {  // haze: rising shimmer
        float2 P = (px - box.xy) / H / p0.y;
        P.y += p0.z * time / p0.y;
        float2 d = float2(Noise(P + seed), Noise(P + seed + 19.1)) * 2 - 1;
        return At(px - d * float2(1, .5) * p0.x * H * m);
    }
    [branch] if (k == 15) {  // split: red and blue pulled apart
        float a = radians(p0.y);
        float2 o = float2(cos(a), sin(a)) * p0.x * H * m;
        float4 r = At(px - o), g = At(px), b = At(px + o);
        return float4(r.r, g.g, b.b, max(max(r.a, g.a), b.a));
    }
    [branch] if (k == 16) {  // grain: fresh noise 24 times a second
        float frame = fmod(floor(time * 24), 4096);
        float n = Hash(floor(px / max(p0.y, .5)) + frame * float2(37, 91) + seed) - .5;
        float4 c = At(px);
        return float4(max(0, c.rgb + n * p0.x * c.a * m), c.a);
    }
    return At(px);
}

// The first low-resolution picture: the average of the picture's pixels under each one, keeping
// only the bright parts for a glow (above its threshold) and for rays, where the mask lets them.
float4 Bright(float2 px) {
    float4 c = At(px);
    int k = (int)kind;
    if (k == 13) return c;
    float l = dot(c.rgb, kLuma) / max(c.a, 1e-4);
    float w = k == 7 ? saturate((l - p0.z) / max(1 - p0.z, 1e-3)) : smoothstep(.3, 1, l);
    return c * w * MaskAt((px - box.xy) / box.zw);
}
float4 Prefilter(float2 unused : TEXCOORD0, float4 position : SV_Position) : SV_Target {
    float f = level.x;
    if (f < 1.5) return Bright(position.xy);
    float2 corner = (position.xy - .5) * f + 1;
    int n = (int)(f / 2);
    float4 sum = 0;
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) sum += Bright(corner + 2 * float2(x, y));
    return sum / (n * n);
}
// Half the size: each pixel the average of the 2 x 2 below it.
float4 Down(float2 unused : TEXCOORD0, float4 position : SV_Position) : SV_Target {
    float2 q = clamp((position.xy - .5) * 2 + 1, .5, lowSize.xy - .5);
    return low.SampleLevel(linearClamp, q * lowSize.zw, 0);
}
// One direction of a Gaussian blur.
float4 Blur(float2 unused : TEXCOORD0, float4 position : SV_Position) : SV_Target {
    float sigma = max(level.y, .1);
    int r = min(16, (int)ceil(sigma * 3));
    float4 sum = 0;
    float total = 0;
    [loop] for (int i = -r; i <= r; ++i) {
        float w = exp(-.5 * i * i / (sigma * sigma));
        float2 q = clamp(position.xy + level.zw * i, .5, lowSize.xy - .5);
        sum += low.SampleLevel(linearClamp, q * lowSize.zw, 0) * w;
        total += w;
    }
    return sum / total;
}
// Each pixel gathers the bright parts on the way to the centre, the nearer ones counting more.
float4 Rays(float2 unused : TEXCOORD0, float4 position : SV_Position) : SV_Target {
    float2 c = (box.xy + p0.xy * box.zw) / level.x;
    float2 d = (c - position.xy) * saturate(p0.w) / 32;
    float4 sum = 0;
    float weight = 1, total = 0;
    [loop] for (int i = 0; i < 32; ++i) {
        float2 q = clamp(position.xy + d * i, .5, lowSize.xy - .5);
        sum += low.SampleLevel(linearClamp, q * lowSize.zw, 0) * weight;
        total += weight;
        weight *= .97;
    }
    return sum / total;
}
// The mask over a box-sized picture, for an author's effect shader to read at its own uv.
float4 MaskCopy(float2 unused : TEXCOORD0, float4 position : SV_Position) : SV_Target {
    return MaskAt(position.xy / size.xy);
}
