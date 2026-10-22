// lf_types.h - the worker's own types and constants: the device core, the
// function pointer types of the runtimes it loads, the timing records, the
// pipeline state, and the three small shaders that live in C strings.
#pragma once

// The device and its one command list, recorded on a ring of three
// allocators; the timeline fence says when an allocator may be reused.
struct GpuCore
{
    ID3D12Device              *device;
    ID3D12CommandQueue        *direct_queue;
    ID3D12GraphicsCommandList *cmd;
    static const int           kRing = 3;
    ID3D12CommandAllocator    *allocators[kRing];
    UINT64                     allocator_mark[kRing];
    int                        ring_slot;
    ID3D12Fence               *timeline;
    HANDLE                     timeline_event;
    UINT64                     timeline_value;
    bool                       ngx_ready;
    NVSDK_NGX_Parameter       *ngx_params;
    NVSDK_NGX_Handle          *ngx_feature;
};
static GpuCore lfCore;

using NrInitFn = NVSDK_NGX_Result (NVSDK_CONV *)(unsigned long long, const wchar_t *, ID3D12Device *, NVSDK_NGX_Version, const NVSDK_NGX_Parameter *);

using NrCreateFn = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList *, NVSDK_NGX_Feature, const NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);

using NrEvaluateFn = NVSDK_NGX_Result (NVSDK_CONV *)(ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *, const NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback);

using NrReleaseFn = NVSDK_NGX_Result (NVSDK_CONV *)(NVSDK_NGX_Handle *);

static constexpr unsigned kNvapiInit = 0x0150E828u;

static constexpr unsigned kNvapiEnumGpus  = 0xE5AC921Fu;

static constexpr unsigned kNvapiArchInfo   = 0xD8265D24u;

static constexpr unsigned kArchTuring    = 0x160u;

static constexpr unsigned kArchAmpere    = 0x170u;

static constexpr unsigned kArchHopper    = 0x180u;

static constexpr unsigned kArchAda       = 0x190u;

static constexpr unsigned kArchBlackwell = 0x1B0u;

struct ArchRecord
{
    unsigned version, architecture, implementation, revision;
};

using NvQueryFn = void *(__cdecl *)(unsigned);

using NvArchFn = int (__cdecl *)(void *, ArchRecord *);

static const int         kArchSlots = 64;

struct FrameTiming {
    double acquire_start, acquired, present_call, source_time;
    UINT64 source_qpc, capture;
    const char *kind = "none";
    UINT64 fence;
    bool reported;
};

enum GpuStage { GS_CONVERT, GS_LUMA, GS_MOVE, GS_NETWORK, GS_SHOW, GS_COUNT, GS_NONE = -1 };

static const char *kStageLabel[GS_COUNT] = { "convert", "luma", "motion", "network", "show" };

typedef HRESULT (WINAPI *D3d12CreateFn)(IUnknown *, D3D_FEATURE_LEVEL, REFIID, void **);

typedef HRESULT (WINAPI *DxgiFactoryFn)(REFIID, void **);

#ifdef LF_RADEON
static bool g_radeon_adapter = false;
static char g_radeon_card[128] = "unknown";
#endif

struct StagedTexture
{
    ID3D12Resource *gpu_tex = nullptr;
    ID3D12Resource *staging = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT layout = {};
    UINT row_count = 0;
    UINT64 row_bytes = 0;
};

struct Pipeline
{
    UINT work_w = 0, work_h = 0;
    UINT screen_w = 0, screen_h = 0;
    bool stretched = false;
    // Flow Super Resolution (flow_upscaler.inl): the whole pipeline at the
    // working size, the picture scaled up to display_w x display_h only at the
    // output and for readbacks. screen_w/h are 0 and stretched is false then.
    bool super_res = false;
    UINT display_w = 0, display_h = 0;
    bool picture_new = true;     // still in the COPY_DEST it was made in
    StagedTexture feed_full;     // the engine's own pictures at the display's size, made on first use
    StagedTexture picture, depth_in, motion, mask_in;
    ID3D12Resource *result = nullptr;
    ID3D12Resource *result_cpu = nullptr;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT result_layout = {};
    UINT result_rows = 0;
    UINT64 result_row_bytes = 0;
    bool fed = false;

    bool net_small = false;
    UINT net_w = 0, net_h = 0;
    ID3D12Resource *net_in = nullptr;
    ID3D12Resource *net_out = nullptr;

    bool add_detail = false;
    float detail_gain = 1.0f;
};

static constexpr uint32_t kExtrasValid = 0x80000000u;

static const char kResizeHlsl[] = R"hlsl(
Texture2D<float2> fieldIn : register(t0);
RWTexture2D<float2> fieldOut : register(u0);
cbuffer Extent : register(b0) { uint outW; uint outH; uint inW; uint inH; };

float2 FieldAt(int2 at, int2 last)
{
    return fieldIn[clamp(at, int2(0, 0), last)];
}

[numthreads(8, 8, 1)]
void CSMain(uint3 cell : SV_DispatchThreadID)
{
    if (cell.x >= outW || cell.y >= outH) return;
    float2 pos = (float2(cell.xy) + 0.5f) * float2(inW, inH) / float2(outW, outH) - 0.5f;
    float2 along = frac(pos);
    int2 base = int2(floor(pos));
    int2 last = int2(inW - 1, inH - 1);
    float2 upper = lerp(FieldAt(base, last), FieldAt(base + int2(1, 0), last), along.x);
    float2 lower = lerp(FieldAt(base + int2(0, 1), last), FieldAt(base + int2(1, 1), last), along.x);
    fieldOut[cell.xy] = lerp(upper, lower, along.y);
}
)hlsl";

static const char kDetailHlsl[] = R"hlsl(
Texture2D<float4> screenPic : register(t0);
Texture2D<float4> netBefore : register(t1);
Texture2D<float4> netAfter : register(t2);
Texture2D<float4> netChange : register(t3);
RWTexture2D<float4> composed : register(u0);
cbuffer Detail : register(b0) { uint picW; uint picH; float gain; float edgeGuard; float keepColour; float lowLight; };
SamplerState bilinear : register(s0);
static const float3 kLumaWeights = float3(0.299f, 0.587f, 0.114f);

// The network's change here. Without the edge guard: the bilinear difference.
// With it, each of the four neighbours counts less the further its brightness
// is from this pixel's, so a change does not spill across an edge as a halo.
float3 ChangeAt(float2 uv, float3 here)
{
    if (edgeGuard <= 0.0f)
    {
        float3 before = netBefore.SampleLevel(bilinear, uv, 0).rgb;
        float3 after = netAfter.SampleLevel(bilinear, uv, 0).rgb;
        return after - before;
    }
    uint netW, netH;
    netBefore.GetDimensions(netW, netH);
    float2 pos = uv * float2(netW, netH) - 0.5f;
    float2 corner = floor(pos);
    float2 part = pos - corner;
    int2 base = int2(corner);
    int2 last = int2(int(netW) - 1, int(netH) - 1);
    float hereLuma = dot(here, kLumaWeights);
    float3 total = 0.0f;
    float weights = 0.0f;
    [unroll] for (int dy = 0; dy < 2; ++dy)
    [unroll] for (int dx = 0; dx < 2; ++dx)
    {
        int2 at = clamp(base + int2(dx, dy), int2(0, 0), last);
        float spatial = (dx == 0 ? 1.0f - part.x : part.x) * (dy == 0 ? 1.0f - part.y : part.y);
        float lumaGap = dot(netBefore[at].rgb, kLumaWeights) - hereLuma;
        float similar = exp(-lumaGap * lumaGap * 50.0f);
        float weight = spatial * (1.0f - edgeGuard + edgeGuard * similar) + 1e-5f;
        total += netChange[at].rgb * weight;
        weights += weight;
    }
    return total / weights;
}

// Low-light noise (round 34): the network's change averaged over the nine
// network pixels around this one, each counting less the further its
// brightness is from this pixel's - so the grain the network makes in the dark
// goes, and an edge keeps its side.
float3 SmoothChangeAt(float2 uv, float hereLuma)
{
    uint netW, netH;
    netBefore.GetDimensions(netW, netH);
    int2 centre = int2(uv * float2(netW, netH));
    int2 last = int2(int(netW) - 1, int(netH) - 1);
    float3 total = 0.0f;
    float weights = 0.0f;
    [unroll] for (int dy = -1; dy <= 1; ++dy)
    [unroll] for (int dx = -1; dx <= 1; ++dx)
    {
        int2 at = clamp(centre + int2(dx, dy), int2(0, 0), last);
        float lumaGap = dot(netBefore[at].rgb, kLumaWeights) - hereLuma;
        float weight = exp(-lumaGap * lumaGap * 200.0f) * (dx == 0 && dy == 0 ? 2.0f : 1.0f) + 1e-5f;
        total += netChange[at].rgb * weight;
        weights += weight;
    }
    return total / weights;
}

[numthreads(8, 8, 1)]
void CSMain(uint3 cell : SV_DispatchThreadID)
{
    if (cell.x >= picW || cell.y >= picH) return;
    float2 uv = (float2(cell.xy) + 0.5f) / float2(picW, picH);
    float3 here = screenPic[cell.xy].rgb;
    float3 change;
    // Low-light noise (round 34). Only where it is dark - fully below 4% of
    // white, not at all above 30% - the picture is smoothed over its eight
    // neighbours that are as bright as it is (grain differs a little, an edge a
    // lot), and the network's change over its own neighbours. Bright parts are
    // left exactly as they were.
    float dark = 0.0f;
    if (lowLight > 0.0f)
    {
        float lumaHere = dot(here, kLumaWeights);
        dark = (1.0f - smoothstep(0.04f, 0.30f, lumaHere)) * saturate(lowLight);
        if (dark > 0.0f)
        {
            float3 sum = here;
            float sumWeight = 1.0f;
            int2 last = int2(int(picW) - 1, int(picH) - 1);
            [unroll] for (int dy = -1; dy <= 1; ++dy)
            [unroll] for (int dx = -1; dx <= 1; ++dx)
            {
                if (dx == 0 && dy == 0) continue;
                float3 there = screenPic[clamp(int2(cell.xy) + int2(dx, dy), int2(0, 0), last)].rgb;
                float lumaGap = dot(there, kLumaWeights) - lumaHere;
                float weight = exp(-lumaGap * lumaGap * 200.0f);   // grain (a few hundredths) counts, an edge (tenths) not
                sum += there * weight;
                sumWeight += weight;
            }
            here = lerp(here, sum / sumWeight, dark);
        }
    }
    change = ChangeAt(uv, here);
    if (dark > 0.0f) change = lerp(change, SmoothChangeAt(uv, dot(here, kLumaWeights)), dark);
    float3 target = max(here + change * gain, 0.0);
    // Keep colours (round 29). The network is about light and structure; with
    // them it also takes colour out of the picture (measured: 7 to 14 points of
    // saturation in one pass). Here the picture keeps its own colour and takes
    // the network's brightness: every channel scaled alike, so hue and
    // saturation stay exactly as they were. In the dark, where a ratio means
    // nothing, the brightness is added instead; a channel pushed past white is
    // brought back towards grey just far enough to fit.
    if (keepColour > 0.0f)
    {
        float lumaHere = dot(here, kLumaWeights);
        float lumaTarget = dot(target, kLumaWeights);
        float3 scaled = here * (lumaTarget / max(lumaHere, 1e-4f));
        float3 added = here + (lumaTarget - lumaHere);
        float3 kept = lerp(added, scaled, saturate(lumaHere / 0.06f));
        float top = max(kept.r, max(kept.g, kept.b));
        if (top > 1.0f) kept = lumaTarget + (kept - lumaTarget) * saturate((1.0f - lumaTarget) / max(top - lumaTarget, 1e-4f));
        target = lerp(target, max(kept, 0.0f), saturate(keepColour));
    }
    composed[cell.xy] = float4(target, 1.0);
}
)hlsl";

typedef HRESULT(WINAPI *CompileFn)(LPCVOID, SIZE_T, LPCSTR, const void *, void *,
                                         LPCSTR, LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

typedef HRESULT(WINAPI *SerializeRootFn)(const D3D12_ROOT_SIGNATURE_DESC *,
                                                          D3D_ROOT_SIGNATURE_VERSION,
                                                          ID3DBlob **, ID3DBlob **);

static const char kLumaHlsl[] = R"hlsl(
Texture2D<float4> picture : register(t0);
RWTexture2D<float> lumaOut : register(u0);
cbuffer Extent : register(b0) { uint picW; uint picH; uint lumaW; uint lumaH; };

[numthreads(8, 8, 1)]
void CSMain(uint3 cell : SV_DispatchThreadID)
{
    if (cell.x >= lumaW || cell.y >= lumaH) return;
    float2 span = float2(picW, picH) / float2(lumaW, lumaH);
    float2 from = float2(cell.xy) * span;
    float2 to = min(from + span, float2(picW, picH));
    uint left = (uint)floor(from.x), top = (uint)floor(from.y);
    uint right = min((uint)ceil(to.x), picW), bottom = min((uint)ceil(to.y), picH);
    float acc = 0.0f;
    for (uint row = top; row < bottom; ++row)
        for (uint col = left; col < right; ++col)
        {
            float4 texel = picture.Load(int3(col, row, 0));
            float cover = max(0.0f, min(to.x, col + 1.0f) - max(from.x, (float)col))
                        * max(0.0f, min(to.y, row + 1.0f) - max(from.y, (float)row));
            acc += cover * dot(texel.rgb, float3(0.299f, 0.587f, 0.114f));
        }
    lumaOut[cell.xy] = acc / max((to.x - from.x) * (to.y - from.y), 1e-6f);
}
)hlsl";

enum { PHASE_TAKE, PHASE_SOURCE, PHASE_FEED, PHASE_RUN, PHASE_SHOW, PHASE_WHOLE, PHASE_COUNT };

enum class BridgeResult { Done, NewSize, NewFormat, Error };

namespace wgc_ns = winrt::Windows::Graphics::Capture;

namespace wgdx_ns = winrt::Windows::Graphics::DirectX;

struct __declspec(uuid("A9B3D012-3DF2-4EE3-B8D1-8695F457D3C1"))
IDxgiAccessBridge : ::IUnknown
{
    virtual HRESULT __stdcall GetInterface(GUID const &id, void **object) = 0;
};

struct FrameDrop
{
    std::mutex mutex;
    wgc_ns::Direct3D11CaptureFrame latest{nullptr};
    winrt::handle arrived{CreateEventW(nullptr, FALSE, FALSE, nullptr)};
    std::atomic<bool> latest_only{false};
    ~FrameDrop() { try { if (latest) latest.Close(); } catch (...) {} }
};

struct WgcLink
{
    wgc_ns::GraphicsCaptureItem item{nullptr};
    wgc_ns::Direct3D11CaptureFramePool pool{nullptr};
    wgc_ns::GraphicsCaptureSession session{nullptr};
    wgdx_ns::Direct3D11::IDirect3DDevice device{nullptr};
    bool hdr = false;
    std::shared_ptr<FrameDrop> mailbox;
    winrt::event_token arrival_token{};
};

static const char *kPhaseLabel[PHASE_COUNT] =
    { "take", "source", "feed", "network", "show", "whole" };

static const double kShowEdges[] = { 5.0, 12.0, 20.0, 28.0, 40.0 };
