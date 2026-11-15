// A theme's wallpaper blur on the graphics card, in the three steps common/include/animelogon/blur.h
// describes and does on the processor, so that both draw the same picture:
//   Box   - each cell the mean of the factor x factor pixels of the picture it covers;
//   Gauss - a Gaussian over the cells along one direction, edges held (run twice);
//   Up    - each window pixel bilinear between cell centres, held at the edges.
// Every pass draws a full-screen triangle into its own target, sized as that step's output.

cbuffer Blur : register(b0) {
    int2 size;       // the texels read: the picture (Box) or the cells (Gauss)
    int factor;      // Box: the cell size, in pixels
    float sigma;     // Gauss: the standard deviation, in cells
    int2 direction;  // Gauss: (1, 0) along the rows, (0, 1) along the columns
    float2 scale;    // Up: from window pixels to the cells' UV, 1 / (factor * cells)
};

Texture2D<float4> source : register(t0);
SamplerState linearClamp : register(s0);

float4 VS(uint id : SV_VertexID) : SV_Position {
    float2 p = float2((id << 1) & 2, id & 2);
    return float4(p * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 Box(float4 pos : SV_Position) : SV_Target {
    int2 origin = int2(pos.xy) * factor;
    float3 sum = 0;
    float n = 0;
    [loop] for (int y = 0; y < factor; ++y) {
        [loop] for (int x = 0; x < factor; ++x) {
            int2 p = origin + int2(x, y);
            if (p.x < size.x && p.y < size.y) {
                sum += source.Load(int3(p, 0)).rgb;
                n += 1;
            }
        }
    }
    return float4(sum / max(n, 1), 1);
}

float4 Gauss(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy);
    int radius = (int)ceil(3 * sigma);
    float3 sum = 0;
    float total = 0;
    [loop] for (int k = -radius; k <= radius; ++k) {
        float w = exp(-0.5 * k * k / (sigma * sigma));
        int2 q = clamp(p + direction * k, int2(0, 0), size - 1);
        sum += w * source.Load(int3(q, 0)).rgb;
        total += w;
    }
    return float4(sum / total, 1);
}

float4 Up(float4 pos : SV_Position) : SV_Target {
    return float4(source.Sample(linearClamp, pos.xy * scale).rgb, 1);
}
