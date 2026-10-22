// LIVE FLOW on a Radeon (ALPHA) - the FidelityFX dispatch the runtime follows.
//
// The neural runtime was written to live in a game, and its idea of "the
// frame" is the game's FidelityFX upscaler dispatch: it hooks the upscaler
// and runs the network on the dispatch that carries motion vectors. Without a
// real dispatch it never sees a frame of ours. So LIVE FLOW makes one per
// frame, 1:1 (render size = output size - nothing is upscaled here; our own
// passes do the scaling around it), with the motion bound.
//
// The upscaler DLL is AMD's and is loaded by its own name
// (amd_fidelityfx_upscaler_dx12.dll): that is the name the runtime looks for
// when it installs its hook.

#pragma once

#include <cstdint>
#include <string>

#include <d3d12.h>

namespace lfr {

class FsrFollow {
public:
    // Loads the upscaler from `dir` (then the normal search path).
    bool Load(const std::wstring &dir, std::string &why);
    bool Loaded() const { return create_ != nullptr; }

    // One context at w x h, render = output. Re-made when the size changes.
    // `step` is told before and after the create call, so a create that
    // never returns leaves its name in the log.
    bool Prepare(ID3D12Device *device, UINT w, UINT h, std::string &why,
                 void (*step)(const char *) = nullptr);
    bool Prepared() const { return context_ != nullptr; }
    UINT Width() const { return w_; }
    UINT Height() const { return h_; }

    // Records the dispatch into `list`. colour/depth/motion/exposure are read
    // (NON_PIXEL_SHADER_RESOURCE), `out` is written (UNORDERED_ACCESS).
    // Motion is in render pixels, current -> previous.
    bool Dispatch(ID3D12GraphicsCommandList *list, ID3D12Resource *colour,
                  ID3D12Resource *depth, ID3D12Resource *motion,
                  ID3D12Resource *exposure, ID3D12Resource *out,
                  float frame_ms, bool reset, std::string &why);

    // The context goes; the module stays loaded for the life of the process
    // (the runtime's hook lives inside it).
    void Release();

private:
    using CreateFn = uint32_t (*)(void **, void *, const void *);
    using DispatchFn = uint32_t (*)(void **, const void *);
    using DestroyFn = uint32_t (*)(void **, const void *);
    CreateFn create_ = nullptr;
    DispatchFn dispatch_ = nullptr;
    DestroyFn destroy_ = nullptr;
    void *context_ = nullptr;
    UINT w_ = 0, h_ = 0;
};

}  // namespace lfr
