// LIVE FLOW on a Radeon (ALPHA) - the FidelityFX upscaler interface, as far as
// we use it.
//
// Only the structures one upscale context and one dispatch need, declared
// here in our own words. The layout is AMD's (FidelityFX API, the upscaler of
// amd_fidelityfx_upscaler_dx12.dll): every size and offset below was printed
// by a probe compiled against AMD's own headers, and the static_asserts pin
// them, so a mistake here fails the build instead of the frame. The neural
// runtime reads the SAME dispatch description when it follows our dispatch,
// so this layout is its contract too.

#pragma once

#include <cstddef>
#include <cstdint>

#include <d3d12.h>

namespace lfr_ffx {

struct Head {
    uint64_t kind;   // which description this is (the constants below)
    Head *next;      // chained descriptions; the backend hangs here
};

struct Extent { uint32_t w, h; };
struct Pair { float x, y; };

struct SurfaceInfo {
    uint32_t kind;     // 2 = 2D texture
    uint32_t format;   // the API's own format numbers (Format* below)
    uint32_t width;
    uint32_t height;
    uint32_t depth;
    uint32_t mips;
    uint32_t flags;
    uint32_t usage;    // 0 read-only, 2 = may be written (UAV)
};

struct Surface {
    void *resource;    // ID3D12Resource *
    SurfaceInfo info;
    uint32_t state;    // the state the resource is in when the dispatch runs
};

struct BackendDx12 {
    Head head;
    ID3D12Device *device;
};

struct UpscaleCreate {
    Head head;
    uint32_t flags;
    Extent max_render;
    Extent max_output;
    void *on_message;  // runtime messages; not used
};

struct UpscaleDispatch {
    Head head;
    void *list;                     // ID3D12GraphicsCommandList *
    Surface colour;
    Surface depth;
    Surface motion;
    Surface exposure;               // 1x1, optional
    Surface reactive;               // optional
    Surface composition;            // optional
    Surface output;
    Pair jitter;
    Pair motion_scale;              // motion * this = render pixels
    Extent render;
    Extent upscale;
    bool sharpen;
    float sharpness;
    float frame_ms;
    float pre_exposure;
    bool reset;
    float near_plane;
    float far_plane;
    float fov_y;
    float metres_per_unit;
    uint32_t flags;
};

inline constexpr uint64_t kBackendDx12 = 2;
inline constexpr uint64_t kUpscaleCreate = 0x10000;
inline constexpr uint64_t kUpscaleDispatch = 0x10001;

inline constexpr uint32_t kReadable = 4;    // compute read
inline constexpr uint32_t kWritable = 2;    // unordered access
inline constexpr uint32_t kTexture2D = 2;
inline constexpr uint32_t kUsageWrite = 2;
inline constexpr uint32_t kHdrInput = 1;    // linear, may exceed 1.0

inline constexpr uint32_t FormatRgba16f = 4;
inline constexpr uint32_t FormatRgba8 = 10;
inline constexpr uint32_t FormatRg16f = 18;
inline constexpr uint32_t FormatR32f = 28;

inline constexpr uint32_t kOk = 0;

static_assert(sizeof(Head) == 16, "header");
static_assert(sizeof(SurfaceInfo) == 32, "surface description");
static_assert(sizeof(Surface) == 48 && offsetof(Surface, info) == 8 &&
              offsetof(Surface, state) == 40, "surface");
static_assert(sizeof(BackendDx12) == 24, "backend");
static_assert(sizeof(UpscaleCreate) == 48 && offsetof(UpscaleCreate, flags) == 16 &&
              offsetof(UpscaleCreate, max_render) == 20 &&
              offsetof(UpscaleCreate, max_output) == 28 &&
              offsetof(UpscaleCreate, on_message) == 40, "create");
static_assert(sizeof(UpscaleDispatch) == 432, "dispatch size");
static_assert(offsetof(UpscaleDispatch, list) == 16 && offsetof(UpscaleDispatch, colour) == 24 &&
              offsetof(UpscaleDispatch, depth) == 72 && offsetof(UpscaleDispatch, motion) == 120 &&
              offsetof(UpscaleDispatch, exposure) == 168 && offsetof(UpscaleDispatch, reactive) == 216 &&
              offsetof(UpscaleDispatch, composition) == 264 && offsetof(UpscaleDispatch, output) == 312 &&
              offsetof(UpscaleDispatch, jitter) == 360 && offsetof(UpscaleDispatch, motion_scale) == 368 &&
              offsetof(UpscaleDispatch, render) == 376 && offsetof(UpscaleDispatch, upscale) == 384 &&
              offsetof(UpscaleDispatch, sharpen) == 392 && offsetof(UpscaleDispatch, sharpness) == 396 &&
              offsetof(UpscaleDispatch, frame_ms) == 400 && offsetof(UpscaleDispatch, pre_exposure) == 404 &&
              offsetof(UpscaleDispatch, reset) == 408 && offsetof(UpscaleDispatch, near_plane) == 412 &&
              offsetof(UpscaleDispatch, far_plane) == 416 && offsetof(UpscaleDispatch, fov_y) == 420 &&
              offsetof(UpscaleDispatch, metres_per_unit) == 424 && offsetof(UpscaleDispatch, flags) == 428,
              "dispatch layout");

// A texture described the way the API wants it. Only the four formats this
// path hands over are mapped; anything else is a programming error and says
// so with format 0 (unknown), which the upscaler refuses loudly.
inline Surface Describe(ID3D12Resource *r, uint32_t state)
{
    Surface s{};
    s.resource = r;
    s.state = state;
    if (r == nullptr) return s;
    const D3D12_RESOURCE_DESC d = r->GetDesc();
    s.info.kind = kTexture2D;
    switch (d.Format)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT: s.info.format = FormatRgba16f; break;
    case DXGI_FORMAT_R8G8B8A8_UNORM:     s.info.format = FormatRgba8; break;
    case DXGI_FORMAT_R16G16_FLOAT:       s.info.format = FormatRg16f; break;
    case DXGI_FORMAT_R32_FLOAT:          s.info.format = FormatR32f; break;
    default:                             s.info.format = 0; break;
    }
    s.info.width = static_cast<uint32_t>(d.Width);
    s.info.height = d.Height;
    s.info.depth = d.DepthOrArraySize;
    s.info.mips = d.MipLevels;
    s.info.usage = (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) ? kUsageWrite : 0u;
    return s;
}

}  // namespace lfr_ffx
