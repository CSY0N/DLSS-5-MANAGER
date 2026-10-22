// LIVE FLOW on a Radeon (ALPHA) - the worker's side.
//
// Included by screen_worker.cpp in the Radeon build only (LF_RADEON), right
// before PipelineRun. It replaces exactly one thing: the NGX evaluate. The
// frame LIVE FLOW hands the network (8-bit display colour, the network's
// extent) goes in, the network's result comes out in the same place, and
// everything around it - the residual composite, halo reduction, sharpness,
// edge smoothing, the colours, the wipe, FLOW MFG, the recording, HDR - is
// the NVIDIA build's own code, untouched.
//
// Per frame, one command list:
//   colour (RGBA8)   -> to-linear   -> lin (RGBA16F)
//   LIVE FLOW motion -> motion fit  -> motion (RG16F, network pixels)
//   lin + motion + exposure -> FidelityFX dispatch -> net; the runtime follows
//                      that dispatch and runs the network on net in place
//   net (or lin, when the network is off) -> to-display -> the result (RGBA8)
//
// Nothing NVIDIA runs in this build: no NGX, no NVOFA, no DLSS 4.5 FG.

#include "radeon_engine.h"
#include "radeon_fsr.h"
#include "radeon_passes.h"

struct RadeonState
{
    lfr::NeuralRuntime runtime;
    lfr::FsrFollow fsr;
    std::wstring dir;
    bool live = false;          // the runtime and the upscaler are up: frames go through them
    bool failed = false;        // latched off - frames pass through unprocessed
    bool hosted = false;        // a runtime image was loaded into this process
    bool settled = false;       // the runtime's engine verdict is in
    bool test = false;          // LF_RADEON_TEST=fsr: every pass and the dispatch, no runtime
    bool srgb = true;           // the capture is sRGB-encoded (always, on the SDR path)
    float shoulder = 0.85f;     // highlight roll-off start; 1 = plain clip

    // The passes: one root signature, four programs, one descriptor heap with
    // a range per allocator slot (a slot is only reused once the GPU is done
    // with it, so its descriptors can be rewritten every frame).
    ID3D12RootSignature *rs = nullptr;
    ID3D12PipelineState *to_linear = nullptr, *motion_fit = nullptr, *to_display = nullptr, *probe = nullptr;
    ID3D12DescriptorHeap *heap = nullptr;

    // Surfaces at the network's extent.
    UINT w = 0, h = 0;
    ID3D12Resource *lin = nullptr;       // RGBA16F, rests readable
    ID3D12Resource *motion = nullptr;    // RG16F, rests readable
    ID3D12Resource *depth = nullptr;     // R32F zeros - a desktop has no depth
    ID3D12Resource *net = nullptr;       // RGBA16F, rests writable: the dispatch's output, edited by the network
    ID3D12Resource *exposure = nullptr;  // 1x1 R32F, rests readable
    ID3D12Resource *exposure_up = nullptr;
    BYTE *exposure_map = nullptr;
    float exposure_now = -1.0f;          // the value in the texture; -1 = never written
    float exposure_smooth = 1.0f;
    ULONGLONG exposure_tick = 0;
    ID3D12Resource *probe_buf = nullptr, *probe_read = nullptr;

    // The probe runs on every frame the network gets (a few thousand texel
    // reads) into this allocator slot's part of probe_read, and is read the
    // next time the slot comes round - by then the GPU is past it, because
    // CmdOpen waited for that slot. No stall, no extra fence.
    bool probe_valid[GpuCore::kRing] = {};
    float seen_in = -1.0f, seen_out = 0.0f, seen_changed = 0.0f, seen_broken = 0.0f;
    ULONGLONG probe_tick = 0;

    // Accounting.
    uint64_t frames = 0, frames_at_report = 0;
    ULONGLONG first_tick = 0, last_tick = 0, report_tick = 0;
    float frame_ms = 16.7f;
    uint32_t jobs_at_report = 0;
    bool gave_up_said = false, silent_said = false;
    float strength_said = -1.0f;
};

static RadeonState g_rx;

static void RadeonSay(const char *line) { Trace("[radeon] %s", line); }

static bool RadeonLive() { return g_rx.live && !g_rx.failed; }

// Where the runtime files are: beside this program, or LF_RADEON_DIR.
static std::wstring RadeonDir()
{
    wchar_t buf[MAX_PATH] = {};
    const DWORD got = GetEnvironmentVariableW(L"LF_RADEON_DIR", buf, MAX_PATH);
    if (got > 0 && got < MAX_PATH)
    {
        std::wstring d(buf);
        while (!d.empty() && (d.back() == L'\\' || d.back() == L'/')) d.pop_back();
        return d;
    }
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (wchar_t *s = wcsrchr(buf, L'\\')) *s = L'\0';
    return buf;
}

// A crash names itself before the process goes: where (module + offset), what,
// and how far the Radeon path had come. "[crash]" reaches the shared log.
static LONG WINAPI RadeonCrashFilter(EXCEPTION_POINTERS *info)
{
    const EXCEPTION_RECORD *rec = info ? info->ExceptionRecord : nullptr;
    const DWORD code = rec ? rec->ExceptionCode : 0;
    char where[160] = "unknown";
    HMODULE module = nullptr;
    if (rec && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                  static_cast<LPCSTR>(rec->ExceptionAddress), &module) && module)
    {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(module, path, MAX_PATH);
        const char *base = strrchr(path, '\\');
        sprintf_s(where, "%s + 0x%llX", base ? base + 1 : path,
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(rec->ExceptionAddress) -
                                                  reinterpret_cast<uintptr_t>(module)));
    }
    Trace("[crash] the Radeon worker crashed: 0x%08X at %s", static_cast<unsigned>(code), where);
    if (code == EXCEPTION_ACCESS_VIOLATION && rec && rec->NumberParameters >= 2)
        Trace("[crash] it tried to %s address 0x%llX", rec->ExceptionInformation[0] ? "write" : "read",
            static_cast<unsigned long long>(rec->ExceptionInformation[1]));
    Trace("[crash] state: runtime %s, live=%d failed=%d engine=%s, %llu frames, runtime jobs %u",
        g_rx.runtime.WhichName(), g_rx.live ? 1 : 0, g_rx.failed ? 1 : 0,
        g_rx.settled ? (g_rx.failed ? "down" : "up") : "pending",
        static_cast<unsigned long long>(g_rx.frames), g_rx.runtime.Jobs());
    fflush(nullptr);
    return EXCEPTION_EXECUTE_HANDLER;
}

// ---- the passes -------------------------------------------------------------

static bool RadeonPipeline()
{
    if (g_rx.probe != nullptr) return true;
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler ? static_cast<CompileFn>(&CachedCompile) : nullptr;
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    auto serialize = d3d12 ? reinterpret_cast<SerializeRootFn>(
                                 GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
    if (compile == nullptr || serialize == nullptr) { Trace("[radeon] no shader compiler"); return false; }

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 2;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 2;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 8;
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 2;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &sampler;
    ID3DBlob *blob = nullptr, *errors = nullptr;
    HRESULT hr = serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (errors) errors->Release();
    if (FAILED(hr) || blob == nullptr) { Trace("[radeon] root signature failed 0x%08X", hr); return false; }
    hr = lfCore.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                    IID_PPV_ARGS(&g_rx.rs));
    blob->Release();
    if (FAILED(hr)) { Trace("[radeon] CreateRootSignature failed 0x%08X", hr); return false; }

    auto build = [&](const char *source, size_t size, const char *name, ID3D12PipelineState **out) {
        ID3DBlob *code = nullptr, *log = nullptr;
        const HRESULT c = compile(source, size, name, nullptr, nullptr, "CSMain", "cs_5_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &log);
        if (FAILED(c) || code == nullptr)
        {
            Trace("[radeon] %s did not compile (0x%08X): %s", name, c,
                log ? static_cast<const char *>(log->GetBufferPointer()) : "no message");
            if (log) log->Release();
            return false;
        }
        if (log) log->Release();
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = g_rx.rs;
        pd.CS = { code->GetBufferPointer(), code->GetBufferSize() };
        const HRESULT p = lfCore.device->CreateComputePipelineState(&pd, IID_PPV_ARGS(out));
        code->Release();
        if (FAILED(p)) { Trace("[radeon] %s pipeline failed 0x%08X", name, p); return false; }
        return true;
    };
    if (!build(kRadeonToLinear, sizeof(kRadeonToLinear) - 1, "radeon_to_linear", &g_rx.to_linear) ||
        !build(kRadeonMotionFit, sizeof(kRadeonMotionFit) - 1, "radeon_motion_fit", &g_rx.motion_fit) ||
        !build(kRadeonToDisplay, sizeof(kRadeonToDisplay) - 1, "radeon_to_display", &g_rx.to_display) ||
        !build(kRadeonProbe, sizeof(kRadeonProbe) - 1, "radeon_probe", &g_rx.probe))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = GpuCore::kRing * 4 * 3;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = lfCore.device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_rx.heap));
    if (FAILED(hr)) { Trace("[radeon] descriptor heap failed 0x%08X", hr); return false; }
    return true;
}

// Slot 0..3 of the current allocator slot's range: [t0, t1, u0].
static D3D12_GPU_DESCRIPTOR_HANDLE RadeonBind(UINT slot, ID3D12Resource *t0, DXGI_FORMAT f0,
                                              ID3D12Resource *t1, DXGI_FORMAT f1,
                                              ID3D12Resource *u0, DXGI_FORMAT fu, UINT buffer_elements = 0)
{
    const UINT stride = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT index = (static_cast<UINT>(lfCore.ring_slot) * 4 + slot) * 3;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_rx.heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(index) * stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    sd.Format = f0;
    lfCore.device->CreateShaderResourceView(t0, &sd, cpu);
    cpu.ptr += stride;
    sd.Format = f1;
    lfCore.device->CreateShaderResourceView(t1, &sd, cpu);
    cpu.ptr += stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = fu;
    if (buffer_elements > 0)
    {
        ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        ud.Buffer.NumElements = buffer_elements;
    }
    else
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    lfCore.device->CreateUnorderedAccessView(u0, nullptr, &ud, cpu);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_rx.heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(index) * stride;
    return gpu;
}

// Everything is set again for every pass: the runtime records its own work
// into our list inside the dispatch, with its own heaps and signatures.
static void RadeonRun(D3D12_GPU_DESCRIPTOR_HANDLE table, ID3D12PipelineState *pso,
                      const UINT32 k[8], UINT groups_x, UINT groups_y)
{
    ID3D12DescriptorHeap *heaps[] = { g_rx.heap };
    lfCore.cmd->SetDescriptorHeaps(1, heaps);
    lfCore.cmd->SetComputeRootSignature(g_rx.rs);
    lfCore.cmd->SetPipelineState(pso);
    lfCore.cmd->SetComputeRootDescriptorTable(0, table);
    lfCore.cmd->SetComputeRoot32BitConstants(1, 8, k, 0);
    lfCore.cmd->Dispatch(groups_x, groups_y, 1);
}

static void RadeonBarrier(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    const D3D12_RESOURCE_BARRIER b = Barrier(r, from, to);
    lfCore.cmd->ResourceBarrier(1, &b);
}

// ---- surfaces -----------------------------------------------------------------

static void RadeonReleaseSurfaces()
{
    auto drop = [](ID3D12Resource *&r) { if (r) { r->Release(); r = nullptr; } };
    if (g_rx.exposure_up && g_rx.exposure_map) g_rx.exposure_up->Unmap(0, nullptr);
    g_rx.exposure_map = nullptr;
    drop(g_rx.lin); drop(g_rx.motion); drop(g_rx.depth); drop(g_rx.net);
    drop(g_rx.exposure); drop(g_rx.exposure_up);
    drop(g_rx.probe_buf); drop(g_rx.probe_read);
    g_rx.exposure_now = -1.0f;
    for (bool &valid : g_rx.probe_valid) valid = false;
    g_rx.seen_in = -1.0f;
    g_rx.fsr.Release();
    g_rx.w = g_rx.h = 0;
}

static ID3D12Resource *RadeonTexture(UINT w, UINT hgt, DXGI_FORMAT format, D3D12_RESOURCE_STATES state,
                                     D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w; rd.Height = hgt; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = format; rd.SampleDesc.Count = 1;
    rd.Flags = flags;
    ID3D12Resource *t = nullptr;
    return SUCCEEDED(lfCore.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
                                                    IID_PPV_ARGS(&t))) ? t : nullptr;
}

static ID3D12Resource *RadeonBuffer(D3D12_HEAP_TYPE type, UINT64 size, D3D12_RESOURCE_STATES state,
                                    D3D12_RESOURCE_FLAGS flags)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = type;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = size; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.Flags = flags;
    ID3D12Resource *b = nullptr;
    return SUCCEEDED(lfCore.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr,
                                                    IID_PPV_ARGS(&b))) ? b : nullptr;
}

static bool RadeonSurfaces(UINT w, UINT hgt)
{
    if (g_rx.lin != nullptr && g_rx.w == w && g_rx.h == hgt) return true;
    if (g_rx.lin != nullptr)
    {
        // A new size: the old surfaces may still be in flight.
        AwaitMark(lfCore.timeline, lfCore.timeline_value, 5000);
        if (g_rx.runtime.Which() == lfr::Release::V0217 && g_rx.runtime.Rebuilding())
            Trace("[radeon] resizing while the runtime says it is rebuilding its own staging");
        RadeonReleaseSurfaces();
    }
    const auto readable = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    const auto writable = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    const auto uav = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    g_rx.lin = RadeonTexture(w, hgt, DXGI_FORMAT_R16G16B16A16_FLOAT, readable, uav);
    g_rx.motion = RadeonTexture(w, hgt, DXGI_FORMAT_R16G16_FLOAT, readable, uav);
    g_rx.depth = RadeonTexture(w, hgt, DXGI_FORMAT_R32_FLOAT, readable, uav);
    // The runtime edits this one in place and may treat it as a render target.
    g_rx.net = RadeonTexture(w, hgt, DXGI_FORMAT_R16G16B16A16_FLOAT, writable,
                             uav | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    g_rx.exposure = RadeonTexture(1, 1, DXGI_FORMAT_R32_FLOAT, readable, uav);
    // One 512-byte placement per allocator slot: the CPU writes one while the
    // GPU may still copy from another.
    g_rx.exposure_up = RadeonBuffer(D3D12_HEAP_TYPE_UPLOAD, 512 * GpuCore::kRing,
                                    D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
    g_rx.probe_buf = RadeonBuffer(D3D12_HEAP_TYPE_DEFAULT, 256, writable, uav);
    g_rx.probe_read = RadeonBuffer(D3D12_HEAP_TYPE_READBACK, 256 * GpuCore::kRing, D3D12_RESOURCE_STATE_COPY_DEST,
                                   D3D12_RESOURCE_FLAG_NONE);
    if (!g_rx.lin || !g_rx.motion || !g_rx.depth || !g_rx.net || !g_rx.exposure || !g_rx.exposure_up ||
        !g_rx.probe_buf || !g_rx.probe_read ||
        FAILED(g_rx.exposure_up->Map(0, nullptr, reinterpret_cast<void **>(&g_rx.exposure_map))))
    {
        g_rx.exposure_map = nullptr;
        Trace("[radeon] the network's surfaces could not be created at %ux%u", w, hgt);
        RadeonReleaseSurfaces();
        return false;
    }
    g_rx.w = w;
    g_rx.h = hgt;
    Trace("[radeon] surfaces at %ux%u (linear RGBA16F in/out, RG16F motion, 1x1 exposure)", w, hgt);
    return true;
}

// ---- exposure -----------------------------------------------------------------

// What the network is told about the frame's exposure, from the frame's own
// brightness (the probe's input mean, a few frames old - smoothed anyway):
// dark content is lifted, bright content brought down, around a middle of
// 0.35 in display terms - an unchanging 1.0 washes bright desktops out to
// white on this runtime. Smoothed over half a second, so a scene change does
// not step. LF_RADEON_EXPOSURE=fixed hands it a constant 1.0 instead.
static float RadeonExposureTarget()
{
    static const bool fixed = [] {
        char v[16] = {};
        const DWORD got = GetEnvironmentVariableA("LF_RADEON_EXPOSURE", v, sizeof(v));
        return got > 0 && got < sizeof(v) && _stricmp(v, "fixed") == 0;
    }();
    if (fixed || g_rx.seen_in < 0.0f) return 1.0f;
    const float mean = powf(g_rx.seen_in, 1.0f / 2.2f);   // linear mean -> display terms
    float target = 1.0f + (0.35f - mean) * 2.0f;
    target = target < 0.5f ? 0.5f : (target > 2.0f ? 2.0f : target);
    const ULONGLONG now = GetTickCount64();
    if (g_rx.exposure_tick == 0) g_rx.exposure_smooth = target;
    else
    {
        const float dt = static_cast<float>(now - g_rx.exposure_tick) / 1000.0f;
        g_rx.exposure_smooth += (target - g_rx.exposure_smooth) * (1.0f - expf(-dt / 0.5f));
    }
    g_rx.exposure_tick = now;
    return g_rx.exposure_smooth;
}

// Into the 1x1 texture, only when it moved.
static void RadeonExposure()
{
    const float value = RadeonExposureTarget();
    if (g_rx.exposure_now >= 0.0f && fabsf(value - g_rx.exposure_now) < 1e-3f) return;
    const UINT64 offset = 512ull * static_cast<UINT64>(lfCore.ring_slot);
    memcpy(g_rx.exposure_map + offset, &value, sizeof(float));
    D3D12_TEXTURE_COPY_LOCATION src = {}, dst = {};
    src.pResource = g_rx.exposure_up;
    src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    src.PlacedFootprint.Offset = offset;
    src.PlacedFootprint.Footprint = { DXGI_FORMAT_R32_FLOAT, 1, 1, 1, 256 };
    dst.pResource = g_rx.exposure;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    RadeonBarrier(g_rx.exposure, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    lfCore.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    RadeonBarrier(g_rx.exposure, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_rx.exposure_now = value;
}

// ---- what the runtime says ------------------------------------------------------

static void RadeonQuote(const char *needle, const char *what)
{
    std::string line;
    if (g_rx.runtime.LastLogLine(needle, line)) Trace("[radeon] runtime log, %s: %s", what, line.c_str());
}

// Its engine starts only once our swapchain exists, so this is asked from the
// frame loop. Up: said once. Down: one retry with the HIP device we matched,
// then off - the picture passes through, and the log says why.
static void RadeonVerdict()
{
    if (!g_rx.live || g_rx.failed || g_rx.settled) return;
    // Loading 150 MB of weights and bringing HIP up takes its time on a first
    // run; a no is only concluded after 15 s of its log moving without the line.
    const lfr::Verdict verdict = g_rx.runtime.Poll(15000);
    if (verdict == lfr::Verdict::Up)
    {
        g_rx.settled = true;
        Trace("[radeon] engine up: the runtime started its network (its own 'engine init ok')");
        RadeonQuote("using HIP device", "device");
        return;
    }
    if (verdict == lfr::Verdict::Pending)
    {
        // Silence is not a verdict - but thirty seconds of it, with frames
        // going in, means the runtime never saw our swapchain.
        if (!g_rx.silent_said && g_rx.first_tick && GetTickCount64() - g_rx.first_tick > 30000)
        {
            g_rx.silent_said = true;
            g_rx.settled = true;
            g_rx.failed = true;
            g_rx.runtime.Disable();
            Trace("[radeon] neural pass off: the runtime wrote nothing about its engine in 30 s of frames "
                "- it never saw LIVE FLOW's swapchain (hooks %s). The picture passes through.",
                g_rx.runtime.HooksSeen() ? "were in place" : "were NOT seen at start");
        }
        return;
    }
    if (!g_rx.runtime.Retried())
    {
        Trace("[radeon] the runtime's engine did not start with its own device match - "
            "trying once more with HIP device #%d (the one matched to our adapter)", g_rx.runtime.HipIndex());
        if (g_rx.runtime.RetryWithOurHip()) return;
        Trace("[radeon] the second start could not be made");
    }
    g_rx.settled = true;
    g_rx.failed = true;
    g_rx.runtime.Disable();
    Trace("[radeon] neural pass off: the runtime's engine never started on this machine. The picture "
        "passes through (FLOW MFG, the colours and the rest still work). Its reason is in "
        "dlssnr_on_amd.log beside the runtime.");
    RadeonQuote("engine init", "its verdict");
    RadeonQuote("UNSUPPORTED GPU", "card");
    RadeonQuote("env: HIP", "HIP");
}

// The probe's four numbers from this allocator slot's last round (complete:
// CmdOpen waited for the slot), and every ten seconds a line about them.
static void RadeonProbeCollect()
{
    const int slot = lfCore.ring_slot;
    if (g_rx.probe_read == nullptr || !g_rx.probe_valid[slot]) return;
    g_rx.probe_valid[slot] = false;
    const SIZE_T at = 256u * static_cast<SIZE_T>(slot);
    float *p = nullptr;
    const D3D12_RANGE range = { at, at + 16 };
    if (FAILED(g_rx.probe_read->Map(0, &range, reinterpret_cast<void **>(&p))) || p == nullptr) return;
    const float *v = reinterpret_cast<const float *>(reinterpret_cast<const BYTE *>(p) + at);
    const float in = v[0], out = v[1], changed = v[2], broken = v[3];
    const D3D12_RANGE none = { 0, 0 };
    g_rx.probe_read->Unmap(0, &none);
    if (!(in >= 0.0f && in < 1000.0f)) return;
    g_rx.seen_in = in;
    g_rx.seen_out = out;
    g_rx.seen_changed = changed;
    g_rx.seen_broken = broken;
    const ULONGLONG now = GetTickCount64();
    if (g_rx.probe_tick != 0 && now - g_rx.probe_tick < 10000) return;
    g_rx.probe_tick = now;
    const char *reading =
        g_rx.test ? "FidelityFX alone (test mode - no network)" :
        broken > 0.01f ? "the network returns NaN/inf - those pixels fall back to the input" :
        (in > 0.02f && out < 0.002f) ? "the network returns BLACK - check dlssnr_on_amd.log" :
        changed < 0.001f ? "the network changes nothing yet (engine starting, or not following our dispatch)" :
        "the network is processing the frames";
    Trace("[radeon] probe: input luminance %.3f, output %.3f, changed %.1f%%, NaN/inf %.1f%% - %s",
        in, out, changed * 100.0f, broken * 100.0f, reading);
}

static void RadeonReport()
{
    const ULONGLONG now = GetTickCount64();
    if (g_rx.report_tick != 0 && now - g_rx.report_tick < 30000) return;
    const uint32_t jobs = g_rx.runtime.Jobs();
    const uint64_t frames = g_rx.frames - g_rx.frames_at_report;
    const double seconds = g_rx.report_tick ? (now - g_rx.report_tick) / 1000.0 : 0.0;
    char timeouts[24] = "n/a";
    if (g_rx.runtime.TimeoutsKnown()) sprintf_s(timeouts, "%u", g_rx.runtime.Timeouts());
    if (g_rx.report_tick == 0) { g_rx.report_tick = now; g_rx.jobs_at_report = jobs; return; }
    Trace("[radeon] %llu frames (%.1f fps), runtime jobs %u (+%u), runtime timeouts %s, strength %.4f, exposure %.2f",
        static_cast<unsigned long long>(g_rx.frames), seconds > 0 ? frames / seconds : 0.0, jobs,
        jobs - g_rx.jobs_at_report, timeouts, g_rx.runtime.StrengthNow(), g_rx.exposure_now);
    if (!g_rx.test && g_rx.report_tick != 0 && frames > 30 && jobs == g_rx.jobs_at_report && g_rx.settled)
        Trace("[radeon] the runtime recorded no job in the last %llu frames - it is not taking our "
            "frames (see dlssnr_on_amd.log)", static_cast<unsigned long long>(frames));
    RadeonQuote("dispatches", "feed");
    RadeonQuote("encoded mean", "exposure it saw");
    if (!g_rx.gave_up_said && g_rx.runtime.GaveUp() && g_rx.frames > 0)
    {
        g_rx.gave_up_said = true;
        Trace("[radeon] the runtime has latched its own failure flag");
        RadeonQuote("error", "last error");
    }
    g_rx.report_tick = now;
    g_rx.frames_at_report = g_rx.frames;
    g_rx.jobs_at_report = jobs;
}

// The FidelityFX context create is where one card was seen to stop for good;
// a watch thread names the step if it has not returned in four seconds.
static const char *volatile g_rx_step = nullptr;
static void RadeonStep(const char *step) { g_rx_step = step; Trace("[radeon] upscaler setup: %s", step); }
static DWORD WINAPI RadeonStallWatch(void *done)
{
    if (WaitForSingleObject(static_cast<HANDLE>(done), 4000) == WAIT_TIMEOUT)
        Trace("[radeon] the upscaler setup has not returned after 4 s - still in: %s (inside the "
            "FidelityFX upscaler or a driver component it loads)", g_rx_step ? g_rx_step : "?");
    return 0;
}

// ---- one frame ------------------------------------------------------------------

// colour: what the network is handed (RGBA8, readable). result: where its
// answer goes (RGBA8, writable), the same size. Recorded into the open list.
// False only when not even the pass-through could be recorded.
static bool RadeonEvaluate(ID3D12Resource *colour, ID3D12Resource *result, UINT w, UINT hgt,
                           const Pipeline &v, int reset)
{
    RadeonVerdict();
    RadeonProbeCollect();
    if (!RadeonPipeline() || !RadeonSurfaces(w, hgt))
    {
        g_rx.failed = true;
        Trace("[radeon] neural pass off: its passes could not be prepared");
        return false;
    }
    const ULONGLONG now = GetTickCount64();
    if (g_rx.first_tick == 0) g_rx.first_tick = now;
    if (g_rx.last_tick != 0)
    {
        const float ms = static_cast<float>(now - g_rx.last_tick);
        g_rx.frame_ms = ms < 1.0f ? 1.0f : (ms > 200.0f ? 200.0f : ms);
    }
    g_rx.last_tick = now;
    bool network = RadeonLive();

    // 1. Display colour -> linear.
    UINT32 k[8] = { w, hgt, g_rx.srgb ? 1u : 0u, 0, 0, 0, 0, 0 };
    RadeonBarrier(g_rx.lin, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    RadeonRun(RadeonBind(0, colour, DXGI_FORMAT_R8G8B8A8_UNORM, colour, DXGI_FORMAT_R8G8B8A8_UNORM,
                         g_rx.lin, DXGI_FORMAT_R16G16B16A16_FLOAT),
              g_rx.to_linear, k, (w + 7) / 8, (hgt + 7) / 8);
    RadeonBarrier(g_rx.lin, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if (network)
    {
        // 2. LIVE FLOW's motion, in the network's pixels.
        UINT32 km[8] = { w, hgt, v.work_w, v.work_h, 0, 0, 0, 0 };
        RadeonBarrier(g_rx.motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        RadeonRun(RadeonBind(1, v.motion.gpu_tex, DXGI_FORMAT_R16G16_FLOAT, v.motion.gpu_tex, DXGI_FORMAT_R16G16_FLOAT,
                             g_rx.motion, DXGI_FORMAT_R16G16_FLOAT),
                  g_rx.motion_fit, km, (w + 7) / 8, (hgt + 7) / 8);
        RadeonBarrier(g_rx.motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        RadeonExposure();

        // 3. The settings, then the dispatch the runtime follows.
        lfr::Knobs knobs;
        knobs.tone = lfLink.hello.local_tone;
        knobs.structure = lfLink.hello.local_structure;
        knobs.skin = lfLink.hello.skin_structure;
        knobs.character_mask = lfLink.hello.auto_mask != 0;
        knobs.tone_channels = lfLink.hello.style == 2 ? 1u : 0u;
        knobs.intensity = lfLink.hello.intensity;
        g_rx.runtime.Apply(knobs);
        if (g_rx.runtime.StrengthNow() != g_rx.strength_said)
        {
            g_rx.strength_said = g_rx.runtime.StrengthNow();
            Trace("[radeon] intensity %.2f -> network strength %.4f", lfLink.hello.intensity, g_rx.strength_said);
        }
        if (reset) g_rx.runtime.ForgetHistory();
        std::string why;
        if (!g_rx.fsr.Prepared())
        {
            HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            HANDLE watch = done ? CreateThread(nullptr, 0, RadeonStallWatch, done, 0, nullptr) : nullptr;
            const bool made = g_rx.fsr.Prepare(lfCore.device, w, hgt, why, RadeonStep);
            if (done) SetEvent(done);
            if (watch) { WaitForSingleObject(watch, INFINITE); CloseHandle(watch); }
            if (done) CloseHandle(done);
            if (!made)
            {
                Trace("[radeon] neural pass off: the FidelityFX context could not be made - %s", why.c_str());
                g_rx.failed = true;
                network = false;
            }
            else
                Trace("[radeon] FidelityFX context %ux%u (1:1 - the dispatch the runtime follows)", w, hgt);
        }
        if (network && !g_rx.fsr.Dispatch(lfCore.cmd, g_rx.lin, g_rx.depth, g_rx.motion, g_rx.exposure,
                                          g_rx.net, g_rx.frame_ms, reset != 0, why))
        {
            Trace("[radeon] neural pass off: %s", why.c_str());
            g_rx.failed = true;
            network = false;
        }
    }

    // 4. Back to display colour: the network's answer, or the frame itself.
    ID3D12Resource *answer = network ? g_rx.net : g_rx.lin;
    if (network)
        RadeonBarrier(g_rx.net, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    UINT32 kd[8] = { w, hgt, g_rx.srgb ? 1u : 0u, 0, 0, 0, 0, 0 };
    const float shoulder = network ? g_rx.shoulder : 1.0f;
    memcpy(&kd[4], &shoulder, sizeof(float));
    RadeonRun(RadeonBind(2, answer, DXGI_FORMAT_R16G16B16A16_FLOAT, g_rx.lin, DXGI_FORMAT_R16G16B16A16_FLOAT,
                         result, DXGI_FORMAT_R8G8B8A8_UNORM),
              g_rx.to_display, kd, (w + 7) / 8, (hgt + 7) / 8);

    // 5. The probe (see kRadeonProbe): the exposure's input, and the line
    //    that says what the network is doing.
    if (network)
    {
        UINT32 kp[8] = { w, hgt, 0, 0, 0, 0, 0, 0 };
        RadeonRun(RadeonBind(3, g_rx.net, DXGI_FORMAT_R16G16B16A16_FLOAT, g_rx.lin, DXGI_FORMAT_R16G16B16A16_FLOAT,
                             g_rx.probe_buf, DXGI_FORMAT_R32_FLOAT, 4),
                  g_rx.probe, kp, 1, 1);
        RadeonBarrier(g_rx.probe_buf, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        lfCore.cmd->CopyBufferRegion(g_rx.probe_read, 256ull * static_cast<UINT64>(lfCore.ring_slot), g_rx.probe_buf, 0, 16);
        RadeonBarrier(g_rx.probe_buf, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        g_rx.probe_valid[lfCore.ring_slot] = true;
    }
    if (network)
        RadeonBarrier(g_rx.net, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    ++g_rx.frames;
    if (network) RadeonReport();
    return true;
}

// ---- start and end --------------------------------------------------------------

// Instead of NGX. Never fatal: without the runtime the worker still runs - the
// picture passes through, and FLOW MFG, the colours and the rest work on it.
static bool RadeonInit()
{
    SetUnhandledExceptionFilter(RadeonCrashFilter);
    g_rx.dir = RadeonDir();
    Trace("[radeon] ===== LIVE FLOW for AMD Radeon - ALPHA =====");
    Trace("[radeon] runtime folder: %ls", g_rx.dir.c_str());
    {
        char v[16] = {};
        const DWORD got = GetEnvironmentVariableA("LF_RADEON_SHOULDER", v, sizeof(v));
        if (got > 0 && got < sizeof(v))
        {
            const float f = static_cast<float>(atof(v));
            if (f >= 0.05f && f <= 1.0f) g_rx.shoulder = f;
        }
    }
    // A check on any card: LF_RADEON_TEST=fsr runs every pass and the
    // FidelityFX dispatch - all of this path but the runtime, which needs a
    // Radeon - so the conversions, the motion, the exposure and the dispatch
    // layout are exercised where there is no AMD card to test on.
    {
        char t[8] = {};
        const DWORD got = GetEnvironmentVariableA("LF_RADEON_TEST", t, sizeof(t));
        g_rx.test = got > 0 && got < sizeof(t) && _stricmp(t, "fsr") == 0;
    }
    if (g_rx.test)
    {
        Trace("[radeon] TEST MODE (LF_RADEON_TEST=fsr): no runtime - the passes and the FidelityFX dispatch alone");
        std::string why;
        if (!g_rx.fsr.Load(g_rx.dir, why)) { Trace("[radeon] test: %s", why.c_str()); return false; }
        if (!RadeonPipeline()) return false;
        g_rx.live = true;
        g_rx.settled = true;
        Trace("[radeon] engine up (TEST: FidelityFX only, no network)");
        return true;
    }
    // LF_RADEON_ANY_GPU=1 goes on to the runtime on any card - for checking
    // the loader's own steps (hash, settings file, HIP) where there is no Radeon.
    char any[4] = {};
    const bool any_gpu = GetEnvironmentVariableA("LF_RADEON_ANY_GPU", any, sizeof(any)) > 0 && any[0] == '1';
    if (!g_radeon_adapter && !any_gpu)
    {
        Trace("[radeon] neural pass off: LIVE FLOW runs on '%s', which is not an AMD Radeon - the "
            "network needs an RX 7000 or RX 9000. The picture passes through.", g_radeon_card);
        return false;
    }
    // v0.3.1 first; LF_RADEON_BUILD=0217 puts the older build first.
    const wchar_t *files[2] = { L"lfr_runtime_0310.dll", L"lfr_runtime_0217.dll" };
    {
        char v[8] = {};
        if (GetEnvironmentVariableA("LF_RADEON_BUILD", v, sizeof(v)) > 0 && strcmp(v, "0217") == 0)
            std::swap(files[0], files[1]);
    }
    bool opened = false;
    for (const wchar_t *file : files)
    {
        if (GetFileAttributesW((g_rx.dir + L"\\" + file).c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            Trace("[radeon] %ls: not present", file);
            continue;
        }
        opened = g_rx.runtime.Open(g_rx.dir, file, lfCore.device, lfCore.direct_queue, RadeonSay);
        if (opened) break;
        Trace("[radeon] %ls: %s", file, g_rx.runtime.Why().c_str());
        if (!g_rx.runtime.Sha256().empty()) Trace("[radeon] its SHA-256: %s", g_rx.runtime.Sha256().c_str());
        // Once an image is loaded (its hooks installed, pinned), a second
        // one must not follow it into the process.
        if (g_rx.runtime.Loaded()) { g_rx.hosted = true; break; }
    }
    if (!opened)
    {
        Trace("[radeon] neural pass off: no usable runtime - the picture passes through");
        return false;
    }
    g_rx.hosted = true;
    Trace("[radeon] runtime %s loaded (SHA-256 %.16s...), HIP device #%d of %d, interop %s",
        g_rx.runtime.WhichName(), g_rx.runtime.Sha256().c_str(), g_rx.runtime.HipIndex(),
        g_rx.runtime.HipCount(), g_rx.runtime.Interop() == 0 ? "off (LF_RADEON_INTEROP=0)" : "on");
    if (g_rx.runtime.HooksSeen())
        Trace("[radeon] its D3D12/DXGI hooks are in place (%u ms)", g_rx.runtime.HooksMs());
    else
        Trace("[radeon] its hooks were NOT announced within %u ms - it may not see our frames", g_rx.runtime.HooksMs());

    // The upscaler AFTER the runtime: its hook goes into the upscaler when
    // that loads.
    std::string why;
    if (!g_rx.fsr.Load(g_rx.dir, why))
    {
        Trace("[radeon] neural pass off: %s", why.c_str());
        return false;
    }
    if (!RadeonPipeline())
    {
        Trace("[radeon] neural pass off: the conversion passes did not build");
        return false;
    }
    g_rx.live = true;
    Trace("[radeon] ===== ready: DLSS 5 runs on the Radeon (ALPHA); FLOW MFG uses CPU motion =====");
    return true;
}

// Before the device goes: the runtime's threads must stop while it exists.
static void RadeonShutdown()
{
    if (!g_rx.hosted) return;
    if (lfCore.timeline) AwaitMark(lfCore.timeline, lfCore.timeline_value, 5000);
    RadeonReleaseSurfaces();
    g_rx.runtime.Stop();
    g_rx.live = false;
    Trace("[radeon] the runtime was stopped before the device");
}

// The runtime's unload path is not safe to run; once it has been hosted the
// process ends without it (everything of ours is closed by then).
static void RadeonExit(int code)
{
    if (!g_rx.hosted) return;
    fflush(nullptr);
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
}
