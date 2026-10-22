// lf_resize_shaders.h - the picture resize shader (area average down,
// bilinear up) and the bounded crisping pass.
#pragma once

static const char kResizeFineHlsl[] = R"hlsl(
Texture2D<float4> picIn : register(t0);
RWTexture2D<float4> picOut : register(u0);
cbuffer Extent : register(b0) { uint outW; uint outH; uint inW; uint inH; };

float4 PicAt(int2 at, int2 last) { return picIn[clamp(at, int2(0, 0), last)]; }

float4 AreaAverage(uint2 cell, int2 last)
{
    float2 from = float2(cell) * float2(inW, inH) / float2(outW, outH);
    float2 to = float2(cell + 1) * float2(inW, inH) / float2(outW, outH);
    float4 acc = 0;
    [loop] for (int row = int(floor(from.y)); row < int(ceil(to.y)); ++row) {
        float wy = max(0, min(to.y, float(row + 1)) - max(from.y, float(row)));
        [loop] for (int col = int(floor(from.x)); col < int(ceil(to.x)); ++col) {
            float wx = max(0, min(to.x, float(col + 1)) - max(from.x, float(col)));
            acc += PicAt(int2(col, row), last) * wx * wy;
        }
    }
    return acc / ((to.x - from.x) * (to.y - from.y));
}

float4 Bilinear(uint2 cell, int2 last)
{
    float2 pos = (float2(cell) + .5) * float2(inW, inH) / float2(outW, outH) - .5;
    int2 base = int2(floor(pos));
    float2 along = frac(pos);
    float4 upper = lerp(PicAt(base, last), PicAt(base + int2(1, 0), last), along.x);
    float4 lower = lerp(PicAt(base + int2(0, 1), last), PicAt(base + int2(1, 1), last), along.x);
    return lerp(upper, lower, along.y);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 cell : SV_DispatchThreadID)
{
    if (cell.x >= outW || cell.y >= outH) return;
    int2 last = int2(inW - 1, inH - 1);
    if (inW > outW || inH > outH) picOut[cell.xy] = AreaAverage(cell.xy, last);
    else picOut[cell.xy] = Bilinear(cell.xy, last);
}
)hlsl";

static const char kCrispHlsl[] = R"hlsl(
Texture2D<float4> picIn : register(t0);
RWTexture2D<float4> picOut : register(u0);
cbuffer Extent : register(b0) { uint picW; uint picH; uint spareW; uint spareH; };

float3 Neighbour(int2 at, int2 last) { return picIn[clamp(at, int2(0, 0), last)].rgb; }

[numthreads(8, 8, 1)]
void CSMain(uint3 cell : SV_DispatchThreadID)
{
    if (cell.x >= picW || cell.y >= picH) return;
    int2 at = int2(cell.xy), last = int2(picW - 1, picH - 1);
    float4 centre = picIn[at];
    float3 up = Neighbour(at + int2(0, -1), last);
    float3 down = Neighbour(at + int2(0, 1), last);
    float3 right = Neighbour(at + int2(1, 0), last);
    float3 left = Neighbour(at + int2(-1, 0), last);
    float3 lowest = min(centre.rgb, min(min(up, down), min(right, left)));
    float3 highest = max(centre.rgb, max(max(up, down), max(right, left)));
    float3 edge = centre.rgb - (up + down + right + left) * .25;
    picOut[at] = float4(clamp(centre.rgb + .3 * edge, lowest, highest), centre.a);
}
)hlsl";
