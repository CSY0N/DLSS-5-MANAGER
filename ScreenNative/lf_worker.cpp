// LIVE FLOW worker - the process the screen engine (mod files/native/screen)
// starts with --video / --live. It takes the screen or one window straight on
// the graphics card, runs the neural network on it, and shows the result in
// its own click-through window; the engine only steers it through a binary
// pipe (lf_wire.h) and a few shared-memory sections.
//
// The file is short on purpose: everything it needs is declared up front
// (lf_declare.h), so the topic files below can come in any order:
//   lf_trace      log lines and failure reports
//   lf_gpu_core   the D3D12 device, the command ring, fences
//   lf_settings   switches from the environment and the extras word
//   lf_link       the pipe and shared memory with the engine
//   lf_desktop    the source: desktop duplication, window and screen capture
//   lf_luma       small luma copies for motion estimation
//   lf_pipeline   working textures, resizing, the network pass, compare wipe
//   lf_network    the NGX runtime and the network itself
//   lf_show       the output window and its swap chain
//   lf_timing     optional timings (LF_PHASE=1)
//   lf_loop       the live loop that ties them together
// Built as nvngx.dll (build.cmd) and, with LF_RADEON, as the Radeon worker.

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dwmapi.h>
#include <winrt/base.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include "lf_share.h"
#include <cstdio>
#include <cstdarg>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <fcntl.h>
#include <io.h>
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <atomic>
#include <chrono>
#include <memory>
#include <map>
#include <set>
#include "lf_hdr_display.h"
#include "lf_hdr_shaders.h"
#include "lf_signature.h"
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif
#include "lf_wire.h"
#include "lf_msm_link.h"
#include "lf_types.h"
#include "lf_declare.h"
#include "lf_state.h"
#include "lf_shader_cache.inl"

#include "capture_shortcuts.inl"
#include "lf_resize_shaders.h"
#include "halo_reduction.inl"
#include "denoise.inl"
#include "post_finish.inl"
#include "flow_upscaler.inl"
#include "lf_vrr.inl"
#include "lf_latency.inl"
#include "lf_counter.inl"
#include "lf_hdr_show.inl"
#include "lf_ofa.inl"
#include "multipass.inl"
#ifdef LF_RADEON
#include "radeon/radeon_pass.inl"
#endif
#include "lf_dlssg.inl"
#include "flow_mfg.inl"
#include "lf_own_motion.inl"

#include "lf_trace.inl"
#include "lf_gpu_core.inl"
#include "lf_msm_link.inl"
#include "lf_settings.inl"
#include "lf_link.inl"
#include "lf_desktop.inl"
#include "lf_luma.inl"
#include "lf_pipeline.inl"
#include "lf_network.inl"
#include "lf_show.inl"
#include "lf_timing.inl"
#include "lf_shader_warm.inl"
#include "lf_loop.inl"

int main(int argc, char **argv)
{
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll"))
    {
        typedef BOOL(WINAPI * DpiContextFn)(HANDLE);
        auto set_context = reinterpret_cast<DpiContextFn>(GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (set_context == nullptr || !set_context(reinterpret_cast<HANDLE>(-4)))
            SetProcessDPIAware();
    }
    bool live = false;
    for (int i = 1; i < argc; ++i)
    {
        if (strcmp(argv[i], "--video") == 0) live = true;
        else if (strcmp(argv[i], "--live") == 0) { live = true; lfRun.unbounded = true; }
    }
    Trace("LIVE FLOW worker, built %s %s", __DATE__, __TIME__);
    if (!live) { Trace("run me with --video or --live: I only serve LIVE FLOW"); return 1; }
    // The shaders compile while the card and the network come up
    // (lf_shader_warm.inl).
    const bool fg_startup = FgRequested();
    if (!fg_startup) ShaderWarmStart();
    if (!GpuCoreStart()) return 1;
#ifndef LF_RADEON
    if (fg_startup) {
        wchar_t directory[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, directory, MAX_PATH);
        if (auto slash = wcsrchr(directory, L'\\')) *(slash + 1) = 0;
        if (!FgPrepareCompatibility(directory, true)) g_fg.failed = true;
    }
#endif
    if (fg_startup) ShaderWarmStart();
#ifdef LF_RADEON
    RadeonInit();
    ShareStart(lfCore.device);
    const int code = LiveLoop();
    RadeonExit(code);
    return code;
#else
    if (!NgxStart()) Trace("[card] NR unavailable - capture and FLOW MFG remain available");
    ShareStart(lfCore.device);
    return LiveLoop();
#endif
}
