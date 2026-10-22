// LIVE FLOW on a Radeon (ALPHA) - see radeon_fsr.h.

#include "radeon_fsr.h"
#include "radeon_ffx.h"

#include <windows.h>

namespace lfr {

bool FsrFollow::Load(const std::wstring &dir, std::string &why)
{
    if (create_ != nullptr) return true;
    const wchar_t *name = L"amd_fidelityfx_upscaler_dx12.dll";
    HMODULE module = LoadLibraryExW((dir + L"\\" + name).c_str(), nullptr,
                                    LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (module == nullptr) module = LoadLibraryW(name);
    if (module == nullptr)
    {
        why = "amd_fidelityfx_upscaler_dx12.dll was not found beside the runtime";
        return false;
    }
    auto create = reinterpret_cast<CreateFn>(GetProcAddress(module, "ffxCreateContext"));
    auto dispatch = reinterpret_cast<DispatchFn>(GetProcAddress(module, "ffxDispatch"));
    auto destroy = reinterpret_cast<DestroyFn>(GetProcAddress(module, "ffxDestroyContext"));
    if (create == nullptr || dispatch == nullptr || destroy == nullptr)
    {
        why = "amd_fidelityfx_upscaler_dx12.dll has no ffxCreateContext/ffxDispatch/ffxDestroyContext";
        return false;
    }
    create_ = create;
    dispatch_ = dispatch;
    destroy_ = destroy;
    return true;
}

bool FsrFollow::Prepare(ID3D12Device *device, UINT w, UINT h, std::string &why,
                        void (*step)(const char *))
{
    if (create_ == nullptr) { why = "the upscaler is not loaded"; return false; }
    if (context_ != nullptr && w == w_ && h == h_) return true;
    Release();

    lfr_ffx::BackendDx12 backend{};
    backend.head.kind = lfr_ffx::kBackendDx12;
    backend.device = device;
    lfr_ffx::UpscaleCreate desc{};
    desc.head.kind = lfr_ffx::kUpscaleCreate;
    desc.head.next = &backend.head;
    // Linear light in, may exceed 1.0. No auto exposure: the exposure is ours
    // (a 1x1 surface handed over with every dispatch), because an adapting
    // one pumps the brightness on desktop content.
    desc.flags = lfr_ffx::kHdrInput;
    desc.max_render = { w, h };
    desc.max_output = { w, h };
    if (step) step("ffxCreateContext: calling");
    const uint32_t rc = create_(&context_, &desc.head, nullptr);
    if (step) step("ffxCreateContext: returned");
    if (rc != lfr_ffx::kOk || context_ == nullptr)
    {
        context_ = nullptr;
        why = "ffxCreateContext returned " + std::to_string(rc);
        return false;
    }
    w_ = w;
    h_ = h;
    return true;
}

bool FsrFollow::Dispatch(ID3D12GraphicsCommandList *list, ID3D12Resource *colour,
                         ID3D12Resource *depth, ID3D12Resource *motion,
                         ID3D12Resource *exposure, ID3D12Resource *out,
                         float frame_ms, bool reset, std::string &why)
{
    if (context_ == nullptr) { why = "no upscaler context"; return false; }
    lfr_ffx::UpscaleDispatch d{};
    d.head.kind = lfr_ffx::kUpscaleDispatch;
    d.list = list;
    d.colour = lfr_ffx::Describe(colour, lfr_ffx::kReadable);
    d.depth = lfr_ffx::Describe(depth, lfr_ffx::kReadable);
    // The motion is what marks THIS dispatch as the one to run the network
    // on - the runtime ignores upscaler dispatches without it.
    d.motion = lfr_ffx::Describe(motion, lfr_ffx::kReadable);
    d.exposure = lfr_ffx::Describe(exposure, lfr_ffx::kReadable);
    d.output = lfr_ffx::Describe(out, lfr_ffx::kWritable);
    // Our motion is already in render pixels (the fit pass scaled it), so
    // the scale is 1 - the same number whichever way a reader applies it.
    d.motion_scale = { 1.0f, 1.0f };
    d.render = { w_, h_ };
    d.upscale = { w_, h_ };
    d.frame_ms = frame_ms > 0.0f ? frame_ms : 16.7f;
    d.pre_exposure = 1.0f;
    d.reset = reset;
    // A desktop has no camera; FidelityFX still wants plausible numbers.
    d.near_plane = 0.1f;
    d.far_plane = 1000.0f;
    d.fov_y = 1.0f;
    d.metres_per_unit = 1.0f;
    const uint32_t rc = dispatch_(&context_, &d.head);
    if (rc != lfr_ffx::kOk)
    {
        why = "ffxDispatch returned " + std::to_string(rc);
        return false;
    }
    return true;
}

void FsrFollow::Release()
{
    if (context_ != nullptr && destroy_ != nullptr) destroy_(&context_, nullptr);
    context_ = nullptr;
    w_ = h_ = 0;
}

}  // namespace lfr
