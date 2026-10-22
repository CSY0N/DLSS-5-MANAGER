// LIVE FLOW on a Radeon (ALPHA) - the small passes around the network.
//
// The rest of LIVE FLOW works in 8-bit display colour; the Radeon runtime
// works in linear light, half-float, on the surface the FidelityFX dispatch
// writes. These passes carry the frame across and back, so everything after
// the network - the residual composite, halo reduction, sharpness, edge
// smoothing, the colours, the wipe, FLOW MFG - sees exactly what it sees on
// an NVIDIA card. All four share one root signature:
//   t0, t1  two textures      u0  one texture (or buffer)
//   b0      eight constants   s0  linear clamp sampler

#pragma once

// Display colour (sRGB-encoded RGBA8) -> linear half-float, same size.
static const char kRadeonToLinear[] = R"hlsl(
Texture2D<float4> gSrc : register(t0);
RWTexture2D<float4> gDst : register(u0);
cbuffer K : register(b0) { uint gW, gH, gSrgb, gK3; float4 gK4; };
float3 Linear(float3 c) { return c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gW || id.y >= gH) return;
    float3 c = saturate(gSrc[id.xy].rgb);
    gDst[id.xy] = float4(gSrgb != 0 ? Linear(c) : c, 1.0);
}
)hlsl";

// LIVE FLOW's motion (current -> previous, in pixels of its own grid) fitted
// to the network's extent and put in THAT grid's pixels, so the dispatch can
// carry a scale of 1. Bilinear: a motion field is smooth, and the two sizes
// are close.
static const char kRadeonMotionFit[] = R"hlsl(
Texture2D<float2> gSrc : register(t0);
RWTexture2D<float2> gDst : register(u0);
SamplerState gLinear : register(s0);
cbuffer K : register(b0) { uint gW, gH, gSrcW, gSrcH; float4 gK4; };
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gW || id.y >= gH) return;
    float2 uv = (float2(id.xy) + 0.5) / float2(gW, gH);
    float2 m = gSrc.SampleLevel(gLinear, uv, 0) * (float2(gW, gH) / float2(gSrcW, gSrcH));
    gDst[id.xy] = all(isfinite(m)) ? m : float2(0, 0);
}
)hlsl";

// Linear half-float -> display colour, same size. t0 is what the network
// left, t1 the frame it was given: a pixel the network turned into NaN/inf
// falls back to the input instead of going black. Highlights above the
// shoulder roll off toward white rather than clipping flat (the network lifts
// local highlights past 1.0 in linear light); gShoulder >= 1 is a plain clip.
static const char kRadeonToDisplay[] = R"hlsl(
Texture2D<float4> gNet : register(t0);
Texture2D<float4> gIn : register(t1);
RWTexture2D<float4> gDst : register(u0);
cbuffer K : register(b0) { uint gW, gH, gSrgb, gK3; float gShoulder, gK5, gK6, gK7; };
float3 Encode(float3 c) { return c <= 0.0031308 ? c * 12.92 : 1.055 * pow(c, 1.0 / 2.4) - 0.055; }
float Roll(float x, float a) { return x <= a ? x : a + (1.0 - a) * (1.0 - exp(-(x - a) / (1.0 - a))); }
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID) {
    if (id.x >= gW || id.y >= gH) return;
    float3 c = gNet[id.xy].rgb;
    if (any(!isfinite(c))) c = gIn[id.xy].rgb;
    c = max(c, 0.0);
    if (gShoulder < 1.0) {
        float a = clamp(gShoulder, 0.05, 0.99);
        c = float3(Roll(c.r, a), Roll(c.g, a), Roll(c.b, a));
    }
    c = saturate(c);
    gDst[id.xy] = float4(gSrgb != 0 ? Encode(c) : c, 1.0);
}
)hlsl";

// The check that tells "the network did nothing" from "the network made it
// black" from "all is well" in one log line: 64 x 36 samples of the frame we
// handed over (t1) and of what came back (t0). Writes four numbers: the mean
// luminance of each, the share of samples the network changed visibly, and
// the share that came back as NaN/inf.
static const char kRadeonProbe[] = R"hlsl(
Texture2D<float4> gNet : register(t0);
Texture2D<float4> gIn : register(t1);
RWBuffer<float> gOut : register(u0);
cbuffer K : register(b0) { uint gW, gH, gK2, gK3; float4 gK4; };
groupshared float4 gSum[64];
[numthreads(64, 1, 1)]
void CSMain(uint3 t : SV_GroupThreadID) {
    const float3 luma = float3(0.2126, 0.7152, 0.0722);
    float4 s = 0;
    for (uint j = 0; j < 36; ++j) {
        uint2 p = uint2(uint((t.x + 0.5) * gW / 64.0), uint((j + 0.5) * gH / 36.0));
        float3 a = gIn[p].rgb;
        float3 b = gNet[p].rgb;
        bool bad = any(!isfinite(b));
        s.x += dot(a, luma);
        s.y += bad ? 0.0 : dot(b, luma);
        s.z += (!bad && any(abs(b - a) > 0.004)) ? 1.0 : 0.0;
        s.w += bad ? 1.0 : 0.0;
    }
    gSum[t.x] = s;
    GroupMemoryBarrierWithGroupSync();
    if (t.x == 0) {
        float4 total = 0;
        for (uint i = 0; i < 64; ++i) total += gSum[i];
        total /= 64.0 * 36.0;
        gOut[0] = total.x; gOut[1] = total.y; gOut[2] = total.z; gOut[3] = total.w;
    }
}
)hlsl";
