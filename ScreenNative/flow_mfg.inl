// FLOW MFG - LIVE FLOW's own frame multiplication, x2 to x6.
//
// LIVE FLOW's only frame generator (DLSS 4.5 FG was taken out in round 24):
// it needs no vendor runtime - the frames in between are made here, from the
// two real frames either side and the motion field LIVE FLOW already computes
// for the network - the same idea as Lossless Scaling, on any GPU.
//
// flow_shader.h uses current->previous and (when NVOFA is available)
// previous->current motion. Endpoint colour and forward/backward consistency
// reject unreliable paths; validated neighbouring vectors fill boundaries.
// Colour is warped at full output resolution; flow estimation is capped at
// 720p. No neural inference is run on generated frames.
//
// Small fast objects. The estimate loses an object that moves further between
// two frames than it is wide - a ball at 15 FPS it had right in one pair of
// eight - and a frame in between cannot place what has no motion: the ball
// jumped from real frame to real frame while the pitch moved smoothly. So
// once per pair, first, four passes find it (flow_shader.h): where the
// estimate does not carry the pixels, one seed per 8x8-texel tile is looked
// up in the previous frame by a block search, and each motion found is kept
// wherever it carries the pixels around it better than the estimate did.
//
// The HUD. Then, before the pair's first frame in between,
// two passes run: HudMain keeps a per-pixel verdict on the screen-locked layer
// (a pixel that stays put while the motion around it says it should have
// moved, and would not have matched if it had), and FixMain rebuilds the
// motion under and beside that layer from the scene's own motion around it.
// Frames in between then draw the HUD where it stands, follow the scene
// behind it from whichever side it can be seen, and keep a HUD's soft edge
// still while the scene under it moves. The verdict lives across pairs and
// resets only where the history does.
//
// Why it presents on a queue of its own. The network takes most of every
// frame on the GPU. On the queue the network uses, a frame in between
// would wait behind the next network pass and then arrive together with the
// real frame - the stutter frame generation is supposed to remove. So while
// FLOW MFG is on, the output window's swap chain is created on a second,
// high-priority queue, and everything that touches the window - the frames
// in between and the real ones - is drawn and presented from there by the
// presenter thread below. The main path only hands frames over.
//
// Turning it on or off therefore rebuilds the output window once (a swap
// chain cannot move between queues). HDR output keeps the ordinary path.
//
// The motion. FLOW MFG finds it itself, first thing every pair, in compute
// shaders (flow_estimate.h) - both directions, on any GPU - and what arrives
// from outside (NVIDIA Optical Flow on RTX cards, the CPU field elsewhere) is
// one more candidate in it. On the football truth test the CPU field had left
// FLOW MFG on a Radeon with the ball 9.6 px off and lost in 15% of frames
// (NVOFA: 2.75 px, 1.5%); the estimate has 2.5 px and 0.7%, and the pitch - the
// part that stands still or pans - 17-39% closer than NVOFA's. LF_FLOW_ESTIMATE=0
// hands the outside motion on as it was.

#include "flow_shader.h"
#include "flow_estimate.h"

struct FlowSlot
{
    ID3D12Resource *prev = nullptr, *cur = nullptr, *mv = nullptr, *reverse = nullptr;   // NON_PIXEL_SHADER_RESOURCE at rest
    int state = 0;            // 0 free, 1 writing, 2 ready, 3 presenting - under the mutex
    UINT64 sequence = 0;
    UINT64 ready_fence = 0;   // direct queue's copy; the presenter waits on GPU
    unsigned count = 0;       // frames in between, before the real one
    bool interpolate = false, both = false;
    double interval = 0.016;
    double source_time = 0, ready_time = 0;
};

static struct FlowState
{
    ID3D12CommandQueue   *queue = nullptr;
    ID3D12RootSignature  *rs = nullptr;
    ID3D12PipelineState  *pso = nullptr;     // CSMain: a frame in between
    ID3D12PipelineState  *pso_hud = nullptr; // HudMain: once per pair
    ID3D12PipelineState  *pso_fix = nullptr; // FixMain: once per pair
    ID3D12PipelineState  *pso_resid = nullptr, *pso_seed = nullptr,   // the small-object tracker,
                         *pso_search = nullptr, *pso_track = nullptr; // once per pair
    ID3D12PipelineState  *pso_block = nullptr, *pso_texel = nullptr;  // the block search, once per pair
    ID3D12PipelineState  *pso_despeckle = nullptr;                    // after every frame in between
    // Round 26: where the crossing search can find anything (RangeMain,
    // ReachMain) and where a HUD is (HudTileMain), once per pair; FastMain
    // draws a frame in between in performance mode.
    ID3D12PipelineState  *pso_range = nullptr, *pso_reach = nullptr, *pso_hudtile = nullptr, *pso_fast = nullptr;
    bool                  fast = false;      // performance mode, as the resources were built for it
    bool                  async_compute = false; // requested queue mode, part of the resource reuse key
    int                   motion_source = -1;
    ID3D12DescriptorHeap *heap = nullptr;   // one descriptor set per slot
    ID3D12Resource       *last = nullptr;   // the previous real frame, COPY_SOURCE at rest
    ID3D12Resource       *scratch = nullptr; // the frame being drawn, UNORDERED_ACCESS at rest
    ID3D12Resource       *shown = nullptr;   // a frame in between once DespeckleMain has been over it, UNORDERED_ACCESS at rest
    ID3D12Resource       *still = nullptr;   // per pixel: pairs stood still, and the HUD verdict
    ID3D12Resource       *fix_mv = nullptr, *fix_reverse = nullptr; // FixMain's motion, NON_PIXEL_SHADER_RESOURCE at rest
    ID3D12Resource       *trk_mv = nullptr, *trk_reverse = nullptr; // TrackMain's motion, NON_PIXEL_SHADER_RESOURCE at rest
    ID3D12Resource       *resid = nullptr;   // how badly the estimate carries each texel, UNORDERED_ACCESS
    ID3D12Resource       *tiles = nullptr;   // seed and search result per tile, then the block search's
                                             // fields, UNORDERED_ACCESS
    UINT                  tiles_x = 0, tiles_y = 0, blocks_x = 0, blocks_y = 0;
    ID3D12Resource       *readback = nullptr; // only while a recording is running
    UINT                  rb_pitch = 0;
    // The motion estimate (flow_estimate.h): its pipeline and a descriptor set
    // per slot; its pyramid and working motion (UNORDERED_ACCESS at rest); and
    // its result (NON_PIXEL_SHADER_RESOURCE at rest), which the kernels after it
    // read as the estimate in place of the slot's own motion.
    ID3D12RootSignature  *est_rs = nullptr;
    ID3D12PipelineState  *est_pso[7] = {};   // EstBase EstHalf FlowCoarse FlowRefine FlowSpread FlowFinal FlowCheck
    ID3D12DescriptorHeap *est_heap = nullptr;
    ID3D12Resource       *est_pyramid = nullptr, *est_fields = nullptr, *est_mv = nullptr, *est_reverse = nullptr;
    UINT                  est_w = 0, est_h = 0, est_levels = 0, est_ratio = 1;
    bool                  est_on = false, est_failed = false;
    FlowSlot slots[3];
    UINT w = 0, hgt = 0, mw = 0, mh = 0;
    DXGI_FORMAT format = DXGI_FORMAT_R8G8B8A8_UNORM;
    std::atomic<unsigned> captured{0};
    // What a recording reads back: the display's size with Flow Super
    // Resolution (w x hgt is the internal one then), w x hgt otherwise.
    UINT out_w = 0, out_h = 0;
    std::mutex mutex;
    std::condition_variable wake;
    std::thread thread;
    std::atomic<bool> stop{false}, failed{false};
    UINT64 sequence = 0;
    std::chrono::steady_clock::time_point last_time;
    double last_source_time = 0;
    bool history = false;
    bool pipeline_failed = false;
    ~FlowState() { stop = true; wake.notify_all(); if (thread.joinable()) thread.join(); }
} g_flow;

// Per slot, in the shader's register order: previous, current, the repaired
// backward/forward motion, the slot's estimated backward/forward motion, the
// tracked backward/forward motion; then the output, the stillness/HUD state,
// the repaired and the tracked motion again (written), the residual and the
// tile records.
// Then the frame drawn (read) and the frame shown (written), for DespeckleMain.
static const UINT kFlowDescriptors = 18;

// Motion texels per tile side, per block side, and how many rounds the block
// search runs - kTile, kBlock and kBlockRounds in flow_shader.h.
static const UINT kFlowTile = 8;
static const UINT kFlowBlock = 4;
static const UINT kFlowBlockRounds = 4;

// gTiles' records (float4 each), as flow_shader.h lays them out: the tracker's
// two per tile, the block search's four per block, RangeMain's and
// ReachMain's two per tile, and the HUD map's one per 8x8 pixels of the
// picture (w x hgt - EnsureFlow sets g_flow.w only once everything is made).
static UINT64 FlowTileRecords(UINT w, UINT hgt)
{
    return UINT64(g_flow.tiles_x) * g_flow.tiles_y * 4 + UINT64(g_flow.blocks_x) * g_flow.blocks_y * 4 +
           UINT64((w + 7) / 8) * ((hgt + 7) / 8);
}

// Performance mode (round 26): FLOW MFG costs the card a fraction of the
// full drawing - FastMain for the frames in between (the two motions only, no
// neighbour candidates, crossing search or third-person rule; the HUD, still
// places and HUD edges as ever), the estimate at a quarter of its size at most
// 400x225, and no tracker or block search. At 1440p on an RTX 3060, x4 at 60
// real FPS: about a quarter of the card instead of more than half
// (tests/flow_cost.py). LF_FLOW_FAST or the reshape's kReshapeFlowFast.
static int g_flow_fast = -1;

static bool FlowFastWanted()
{
    if (g_flow_fast < 0)
    {
        char text[4] = {};
        g_flow_fast = GetEnvironmentVariableA("LF_FLOW_FAST", text, sizeof(text)) > 0 && text[0] == '1' ? 1 : 0;
    }
    return g_flow_fast == 1;
}

static void FlowFastFromResize(uint32_t flags)
{
    if ((flags & kReshapeDisplay) == 0) return;
    const int want = (flags & kReshapeFlowFast) != 0 ? 1 : 0;
    if (want != (FlowFastWanted() ? 1 : 0))
        Trace("[flow] performance mode %s", want ? "on" : "off - the full drawing");
    g_flow_fast = want;
}

static bool FlowWanted()
{
    return FlowExtraFrames() > 0 && !FgRequested() && !g_flow.failed;
}

static bool FlowActive()
{
    return lfShow.on_flow_queue && FlowWanted();
}

static bool FlowAsyncWanted()
{
    // Keep ordinary scheduling and NR on their existing queue. In the
    // explicit Low Latency mode, MFG alone can overlap the game's graphics
    // on COMPUTE; the swap chain always stays on DIRECT. At normal process
    // priority a loaded-GPU A/B showed the extra queue could cost throughput.
    // An explicit override is useful for driver compatibility and A/B tests.
    char option[8] = {};
    if (GetEnvironmentVariableA("LF_FLOW_ASYNC_COMPUTE", option, sizeof(option)))
        return option[0] == '1';
    // NrLive() means the feature is allocated, even while the user bypasses
    // NR. The just-produced frame tells us whether NR actually ran.
    return lfOut.sent_passthrough && LowLatencyWanted();
}

// A failed submission must not release an allocator or texture still used
// by either presenter queue. Use a private event: the capture thread also
// waits on lfCore.timeline_event while this thread is alive.
static bool FlowDrainQueue(ID3D12CommandQueue *queue)
{
    winrt::com_ptr<ID3D12Fence> fence;
    if (FAILED(lfCore.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put())))) return false;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return false;
    const bool done = SUCCEEDED(queue->Signal(fence.get(), 1)) &&
        SUCCEEDED(fence->SetEventOnCompletion(1, event)) &&
        WaitForSingleObject(event, 2000) == WAIT_OBJECT_0 && fence->GetCompletedValue() == 1;
    CloseHandle(event);
    return done;
}

static void FlowDrainFailed()
{
    if (FAILED(lfCore.device->GetDeviceRemovedReason())) return; // the device has already stopped executing
    Trace("[flow] GPU work did not retire - restarting the worker instead of reusing in-flight resources");
    // This is the isolated worker, not the manager. Its host already handles
    // worker loss; continuing here would allow use-after-free on the GPU.
    TerminateProcess(GetCurrentProcess(), ERROR_TIMEOUT);
}

static ID3D12CommandQueue *FlowSwapQueue()
{
    if (!FlowWanted()) return lfCore.direct_queue;
    if (g_flow.queue == nullptr)
    {
        D3D12_COMMAND_QUEUE_DESC qd = {};
        qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        qd.Priority = D3D12_COMMAND_QUEUE_PRIORITY_HIGH;
        if (FAILED(lfCore.device->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue),
                                             reinterpret_cast<void **>(&g_flow.queue))))
        {
            Trace("[flow] could not create the presenting queue - FLOW MFG stays off");
            g_flow.failed = true;
            return lfCore.direct_queue;
        }
    }
    return g_flow.queue;
}

static void CloseFlow()
{
    g_flow.stop = true;
    g_flow.wake.notify_all();
    if (g_flow.thread.joinable()) g_flow.thread.join();
    // Ready slots can still be in the direct queue after the presenter stops.
    UINT64 copied = 0;
    for (const auto &slot : g_flow.slots) copied = std::max(copied, slot.ready_fence);
    if (copied && lfCore.timeline && !AwaitMark(lfCore.timeline, copied, 30000)) FlowDrainFailed();
    if (lfShow.chain != nullptr && lfShow.flow_waitable != nullptr)
    {
        IDXGISwapChain2 *sc2 = nullptr;
        if (SUCCEEDED(lfShow.chain->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void **>(&sc2))) && sc2)
        {
            sc2->SetMaximumFrameLatency(3);
            sc2->Release();
        }
    }
    for (auto &slot : g_flow.slots)
    {
        for (ID3D12Resource **r : { &slot.prev, &slot.cur, &slot.mv, &slot.reverse })
            if (*r != nullptr) { (*r)->Release(); *r = nullptr; }
        slot.state = 0;
        slot.ready_fence = 0;
    }
    if (g_flow.last != nullptr) { g_flow.last->Release(); g_flow.last = nullptr; }
    if (g_flow.scratch != nullptr) { g_flow.scratch->Release(); g_flow.scratch = nullptr; }
    if (g_flow.shown != nullptr) { g_flow.shown->Release(); g_flow.shown = nullptr; }
    if (g_flow.still != nullptr) { g_flow.still->Release(); g_flow.still = nullptr; }
    if (g_flow.fix_mv != nullptr) { g_flow.fix_mv->Release(); g_flow.fix_mv = nullptr; }
    if (g_flow.fix_reverse != nullptr) { g_flow.fix_reverse->Release(); g_flow.fix_reverse = nullptr; }
    for (ID3D12Resource **r : { &g_flow.trk_mv, &g_flow.trk_reverse, &g_flow.resid, &g_flow.tiles })
        if (*r != nullptr) { (*r)->Release(); *r = nullptr; }
    g_flow.tiles_x = g_flow.tiles_y = g_flow.blocks_x = g_flow.blocks_y = 0;
    if (g_flow.readback != nullptr) { g_flow.readback->Release(); g_flow.readback = nullptr; }
    g_flow.rb_pitch = 0;
    SuperResExportRelease(true);
    for (ID3D12Resource **r : { &g_flow.est_pyramid, &g_flow.est_fields, &g_flow.est_mv, &g_flow.est_reverse })
        if (*r != nullptr) { (*r)->Release(); *r = nullptr; }
    g_flow.est_on = false;
    g_flow.w = g_flow.hgt = g_flow.mw = g_flow.mh = 0;
    g_flow.out_w = g_flow.out_h = 0;
    g_flow.history = false;
    g_flow.last_source_time = 0;
    g_flow.motion_source = -1;
}

static bool EnsureFlowPipeline()
{
    if (g_flow.pso != nullptr && g_flow.heap != nullptr) return true;
    if (g_flow.pipeline_failed) return false;
    g_flow.pipeline_failed = true;
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler ? static_cast<CompileFn>(&CachedCompile) : nullptr;
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    auto serialize = d3d12 ? reinterpret_cast<SerializeRootFn>(
                                 GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
    if (compile == nullptr || serialize == nullptr) { Trace("[flow] compiler unavailable"); return false; }
    // One source, fourteen kernels: the frame in between (in full, or
    // FastMain's in performance mode), and the passes run once per pair - the
    // small-object tracker, the block search, the HUD and its map, and where
    // the crossing search can find anything.
    const char *entries[14] = { "CSMain", "HudMain", "FixMain", "ResidMain", "SeedMain", "SearchMain", "TrackMain",
                                "BlockMain", "TexelMain", "DespeckleMain", "RangeMain", "ReachMain", "HudTileMain",
                                "FastMain" };
    ID3DBlob *codes[14] = {};
    ID3DBlob *logs[14] = {};
    HRESULT results[14] = {};
    // Side by side: one after another the kernels took three seconds, all of
    // them inside the frame that turned FLOW MFG on - close to the host's
    // five-second watchdog on a slower processor. D3DCompile may run on
    // several threads at once. A thread that cannot be started leaves its
    // kernel to this one.
    //
    // And once only: the code is kept in %TEMP%\DLSS5Manager-flow, named by a
    // hash of this source, so every later start - and every FLOW MFG switched
    // on - loads it instead of stalling on the compiler again (~1.2 s each
    // time). A changed shader has another name; a bad or partial file fails
    // D3D12 and is compiled afresh.
    typedef HRESULT(WINAPI *PFN_D3DCreateBlob_)(SIZE_T, ID3DBlob **);
    auto make_blob = reinterpret_cast<PFN_D3DCreateBlob_>(GetProcAddress(compiler, "D3DCreateBlob"));
    UINT64 source_hash = 1469598103934665603ull;
    for (size_t k = 0; k + 1 < sizeof(kFlowHlsl); ++k) { source_hash ^= UINT8(kFlowHlsl[k]); source_hash *= 1099511628211ull; }
    wchar_t cache_dir[MAX_PATH] = {};
    const DWORD temp_len = GetTempPathW(MAX_PATH, cache_dir);
    const bool cache = make_blob != nullptr && temp_len > 0 && temp_len < MAX_PATH - 64;
    if (cache) { wcscat_s(cache_dir, L"DLSS5Manager-flow"); CreateDirectoryW(cache_dir, nullptr); }
    auto cache_path = [&](int i) {
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\%016llX-%S.cso", cache_dir, source_hash, entries[i]);
        return std::wstring(path);
    };
    bool loaded[_countof(entries)] = {};
    auto build = [&](int i) {
        if (cache)
        {
            HANDLE f = CreateFileW(cache_path(i).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE)
            {
                LARGE_INTEGER size = {};
                DWORD got = 0;
                if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (16 << 20) &&
                    SUCCEEDED(make_blob(SIZE_T(size.QuadPart), &codes[i])) &&
                    ReadFile(f, codes[i]->GetBufferPointer(), DWORD(size.QuadPart), &got, nullptr) && got == DWORD(size.QuadPart))
                { CloseHandle(f); results[i] = S_OK; loaded[i] = true; return; }
                CloseHandle(f);
                if (codes[i]) { codes[i]->Release(); codes[i] = nullptr; }
            }
        }
        results[i] = compile(kFlowHlsl, sizeof(kFlowHlsl) - 1, "flow.hlsl", nullptr, nullptr,
                             entries[i], "cs_5_0", 0, 0, &codes[i], &logs[i]);
        if (cache && SUCCEEDED(results[i]) && codes[i])
        {
            // Written under a temporary name and moved in whole: another
            // worker starting at the same moment never reads half a file.
            const std::wstring final_path = cache_path(i), part = final_path + L".part";
            HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE)
            {
                DWORD put = 0;
                const BOOL ok = WriteFile(f, codes[i]->GetBufferPointer(), DWORD(codes[i]->GetBufferSize()), &put, nullptr);
                CloseHandle(f);
                if (!ok || !MoveFileExW(part.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(part.c_str());
            }
        }
    };
    const auto compile_start = std::chrono::steady_clock::now();
    {
        std::vector<std::thread> builders;
        bool started[_countof(entries)] = {};
        for (int i = 1; i < int(_countof(entries)); ++i)
        {
            try { builders.emplace_back(build, i); started[i] = true; }
            catch (...) {}
        }
        build(0);
        for (auto &b : builders) b.join();
        for (int i = 1; i < int(_countof(entries)); ++i) if (!started[i]) build(i);
    }
    HRESULT hr = S_OK;
    for (int i = 0; i < int(_countof(entries)); ++i)
    {
        if (FAILED(results[i]) || codes[i] == nullptr)
        {
            Trace("[flow] shader %s failed 0x%08X: %s", entries[i], results[i],
                logs[i] ? static_cast<const char *>(logs[i]->GetBufferPointer()) : "(no log)");
            for (ID3DBlob *l : logs) if (l) l->Release();
            for (ID3DBlob *c : codes) if (c) c->Release();
            return false;
        }
    }
    for (ID3DBlob *&l : logs) if (l) { l->Release(); l = nullptr; }
    int from_cache = 0;
    for (bool l : loaded) from_cache += l ? 1 : 0;
    Trace("[flow] %d kernels ready in %.0f ms (%d from the cache)", int(_countof(entries)),
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - compile_start).count(), from_cache);
    ID3DBlob *errors = nullptr;
    D3D12_DESCRIPTOR_RANGE ranges[4] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 8;               // colour x2, repaired, estimated and tracked motion x2
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 8;               // frame, HUD state, repaired x2, tracked x2, residual, tiles
    ranges[1].OffsetInDescriptorsFromTableStart = 8;
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].NumDescriptors = 1;               // t8: the frame drawn
    ranges[2].BaseShaderRegister = 8;
    ranges[2].OffsetInDescriptorsFromTableStart = 16;
    ranges[3].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[3].NumDescriptors = 1;               // u8: the frame shown
    ranges[3].BaseShaderRegister = 8;
    ranges[3].OffsetInDescriptorsFromTableStart = 17;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 7;     // cbuffer FC: size, t, accumulate, both, rings, block round
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 4;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = _countof(params);
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &samp;
    ID3DBlob *rs_blob = nullptr;
    hr = serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &errors);
    if (errors) errors->Release();
    auto release_codes = [&]() { for (ID3DBlob *&c : codes) if (c) { c->Release(); c = nullptr; } };
    if (FAILED(hr) || rs_blob == nullptr) { Trace("[flow] root signature failed 0x%08X", hr); release_codes(); return false; }
    if (g_flow.rs == nullptr)
        hr = lfCore.device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                        __uuidof(ID3D12RootSignature), reinterpret_cast<void **>(&g_flow.rs));
    rs_blob->Release();
    if (FAILED(hr)) { release_codes(); return false; }
    ID3D12PipelineState *made[_countof(entries)] = {};
    for (int i = 0; i < int(_countof(entries)) && SUCCEEDED(hr); ++i)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = g_flow.rs;
        pd.CS.pShaderBytecode = codes[i]->GetBufferPointer();
        pd.CS.BytecodeLength = codes[i]->GetBufferSize();
        hr = lfCore.device->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void **>(&made[i]));
    }
    release_codes();
    if (FAILED(hr))
    {
        Trace("[flow] pipeline failed 0x%08X", hr);
        for (ID3D12PipelineState *p : made) if (p) p->Release();
        return false;
    }
    g_flow.pso = made[0]; g_flow.pso_hud = made[1]; g_flow.pso_fix = made[2];
    g_flow.pso_resid = made[3]; g_flow.pso_seed = made[4]; g_flow.pso_search = made[5]; g_flow.pso_track = made[6];
    g_flow.pso_block = made[7]; g_flow.pso_texel = made[8]; g_flow.pso_despeckle = made[9];
    g_flow.pso_range = made[10]; g_flow.pso_reach = made[11]; g_flow.pso_hudtile = made[12]; g_flow.pso_fast = made[13];
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = kFlowDescriptors * _countof(g_flow.slots);
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(lfCore.device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void **>(&g_flow.heap))))
        return false;
    g_flow.pipeline_failed = false;
    return true;
}

// The motion estimate's kernels (flow_estimate.h), compiled once and kept in
// the same cache as the others. Failing here costs nothing but the estimate:
// FLOW MFG then reads the outside motion as it did before.
static bool EnsureFlowEstimate()
{
    if (g_flow.est_heap != nullptr) return true;
    if (g_flow.est_failed) return false;
    g_flow.est_failed = true;
    {
        char v[8] = {};
        const DWORD got = GetEnvironmentVariableA("LF_FLOW_ESTIMATE", v, sizeof(v));
        if (got > 0 && got < sizeof(v) && v[0] == '0')
        { Trace("[flow] own motion estimate off (LF_FLOW_ESTIMATE=0) - the outside motion as it comes"); return false; }
    }
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler ? static_cast<CompileFn>(&CachedCompile) : nullptr;
    typedef HRESULT(WINAPI *PFN_D3DCreateBlob_)(SIZE_T, ID3DBlob **);
    auto make_blob = compiler ? reinterpret_cast<PFN_D3DCreateBlob_>(GetProcAddress(compiler, "D3DCreateBlob")) : nullptr;
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    auto serialize = d3d12 ? reinterpret_cast<SerializeRootFn>(
                                 GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
    if (compile == nullptr || serialize == nullptr) return false;
    const char *entries[7] = { "EstBase", "EstHalf", "FlowCoarse", "FlowRefine", "FlowSpread", "FlowFinal", "FlowCheck" };
    UINT64 hash = 1469598103934665603ull;
    for (size_t k = 0; k + 1 < sizeof(kFlowEstimateHlsl); ++k) { hash ^= UINT8(kFlowEstimateHlsl[k]); hash *= 1099511628211ull; }
    wchar_t dir[MAX_PATH] = {};
    const DWORD temp_len = GetTempPathW(MAX_PATH, dir);
    const bool cache = make_blob != nullptr && temp_len > 0 && temp_len < MAX_PATH - 64;
    if (cache) { wcscat_s(dir, L"DLSS5Manager-flow"); CreateDirectoryW(dir, nullptr); }
    ID3DBlob *codes[7] = {};
    HRESULT results[7] = {};
    ID3DBlob *logs[7] = {};
    auto build = [&](int i) {
        wchar_t path[MAX_PATH] = {};
        if (cache)
        {
            swprintf_s(path, L"%s\\%016llX-%S.cso", dir, hash, entries[i]);
            HANDLE f = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE)
            {
                LARGE_INTEGER size = {};
                DWORD got = 0;
                if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (16 << 20) &&
                    SUCCEEDED(make_blob(SIZE_T(size.QuadPart), &codes[i])) &&
                    ReadFile(f, codes[i]->GetBufferPointer(), DWORD(size.QuadPart), &got, nullptr) && got == DWORD(size.QuadPart))
                { CloseHandle(f); results[i] = S_OK; return; }
                CloseHandle(f);
                if (codes[i]) { codes[i]->Release(); codes[i] = nullptr; }
            }
        }
        results[i] = compile(kFlowEstimateHlsl, sizeof(kFlowEstimateHlsl) - 1, "flow_estimate.hlsl", nullptr, nullptr,
                             entries[i], "cs_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &codes[i], &logs[i]);
        if (cache && SUCCEEDED(results[i]) && codes[i])
        {
            const std::wstring final_path = path, part = final_path + L".part";
            HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE)
            {
                DWORD put = 0;
                const BOOL ok = WriteFile(f, codes[i]->GetBufferPointer(), DWORD(codes[i]->GetBufferSize()), &put, nullptr);
                CloseHandle(f);
                if (!ok || !MoveFileExW(part.c_str(), final_path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(part.c_str());
            }
        }
    };
    {
        std::vector<std::thread> builders;
        bool started[7] = {};
        for (int i = 1; i < 7; ++i)
        {
            try { builders.emplace_back(build, i); started[i] = true; }
            catch (...) {}
        }
        build(0);
        for (auto &b : builders) b.join();
        for (int i = 1; i < 7; ++i) if (!started[i]) build(i);
    }
    bool compiled = true;
    for (int i = 0; i < 7; ++i)
        if (FAILED(results[i]) || codes[i] == nullptr)
        {
            Trace("[flow] motion estimate kernel %s failed 0x%08X: %s", entries[i], results[i],
                logs[i] ? static_cast<const char *>(logs[i]->GetBufferPointer()) : "(no log)");
            compiled = false;
        }
    for (ID3DBlob *l : logs) if (l) l->Release();
    auto release_codes = [&]() { for (ID3DBlob *&c : codes) if (c) { c->Release(); c = nullptr; } };
    if (!compiled) { release_codes(); return false; }

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 5;               // the two frames, the outside motion x2, the pyramid
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 4;               // the pyramid, the working motion, the result x2
    ranges[1].OffsetInDescriptorsFromTableStart = 5;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 14;    // cbuffer EC
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 2;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &samp;
    ID3DBlob *rs_blob = nullptr, *errors = nullptr;
    HRESULT hr = serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &errors);
    if (errors) errors->Release();
    if (FAILED(hr) || rs_blob == nullptr) { release_codes(); return false; }
    hr = lfCore.device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                    __uuidof(ID3D12RootSignature), reinterpret_cast<void **>(&g_flow.est_rs));
    rs_blob->Release();
    for (int i = 0; i < 7 && SUCCEEDED(hr); ++i)
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = g_flow.est_rs;
        pd.CS.pShaderBytecode = codes[i]->GetBufferPointer();
        pd.CS.BytecodeLength = codes[i]->GetBufferSize();
        hr = lfCore.device->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void **>(&g_flow.est_pso[i]));
    }
    release_codes();
    if (SUCCEEDED(hr))
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 9 * _countof(g_flow.slots);
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = lfCore.device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void **>(&g_flow.est_heap));
    }
    if (FAILED(hr))
    {
        Trace("[flow] motion estimate pipeline failed 0x%08X - the outside motion as it comes", hr);
        for (ID3D12PipelineState *&p : g_flow.est_pso) if (p) { p->Release(); p = nullptr; }
        if (g_flow.est_rs) { g_flow.est_rs->Release(); g_flow.est_rs = nullptr; }
        return false;
    }
    g_flow.est_failed = false;
    return true;
}

// Its textures at this motion size, and a descriptor set per slot. Level 0 is
// the motion texture halved until it is no wider than 800 (and no taller than
// 450): the estimate costs by its pixels, and past that more of them buys
// nothing the refinement after it does not do anyway. Performance mode stops
// at 400x225: a quarter of the estimate's time.
static bool EnsureFlowEstimateTargets(UINT mw, UINT mh)
{
    const UINT cap_w = g_flow.fast ? 400u : 800u, cap_h = g_flow.fast ? 225u : 450u;
    UINT ratio = 1;
    while ((mw + ratio - 1) / ratio > cap_w || (mh + ratio - 1) / ratio > cap_h) ratio *= 2;
    const UINT ew = (mw + ratio - 1) / ratio, eh = (mh + ratio - 1) / ratio;
    UINT levels = 0;
    auto side = [](UINT n, UINT l) { return (n + (1u << l) - 1u) >> l; };
    while (side(ew, levels + 1) >= 16 && side(eh, levels + 1) >= 16) ++levels;
    const UINT aw = ew + (ew + 1) / 2, band = eh + 16;
    g_flow.est_pyramid = NewTexture(aw, 2 * band, DXGI_FORMAT_R32_UINT, true);
    g_flow.est_fields = NewTexture(aw, 4 * band, DXGI_FORMAT_R32_UINT, true);
    g_flow.est_mv = NewTexture(mw, mh, DXGI_FORMAT_R16G16_FLOAT, true);
    g_flow.est_reverse = NewTexture(mw, mh, DXGI_FORMAT_R16G16_FLOAT, true);
    if (!g_flow.est_pyramid || !g_flow.est_fields || !g_flow.est_mv || !g_flow.est_reverse) return false;
    g_flow.est_w = ew; g_flow.est_h = eh; g_flow.est_levels = levels; g_flow.est_ratio = ratio;
    const UINT stride = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_flow.est_heap->GetCPUDescriptorHandleForHeapStart();
    for (auto &slot : g_flow.slots)
    {
        for (ID3D12Resource *r : { slot.prev, slot.cur, slot.mv, slot.reverse, g_flow.est_pyramid })
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format = r->GetDesc().Format;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            lfCore.device->CreateShaderResourceView(r, &sd, cpu);
            cpu.ptr += stride;
        }
        for (ID3D12Resource *r : { g_flow.est_pyramid, g_flow.est_fields, g_flow.est_mv, g_flow.est_reverse })
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
            ud.Format = r->GetDesc().Format;
            ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            lfCore.device->CreateUnorderedAccessView(r, nullptr, &ud, cpu);
            cpu.ptr += stride;
        }
    }
    return true;
}

// How far the smallest level looks, by how long the pair is: a slow source
// moves further between two frames, and at a fast one the wide searches would
// only cost (at a 60 FPS source they would come out of the game's time).
static UINT EstimateRadius(double interval)
{
    return interval >= 1.0 / 20.0 ? 6u : (interval >= 1.0 / 40.0 ? 4u : 3u);
}

// The estimate for one pair, recorded into the presenter's list. Leaves the
// result in est_mv / est_reverse (readable) and the list's heap and root
// signature its own - the caller sets FLOW's again.
static void RecordFlowEstimate(ID3D12GraphicsCommandList *list, unsigned slot_index, bool outside_both, double interval)
{
    ID3D12DescriptorHeap *heaps[] = { g_flow.est_heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_flow.est_rs);
    const UINT stride = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_flow.est_heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(slot_index) * 9 * stride;
    list->SetComputeRootDescriptorTable(1, gpu);
    const UINT ew = g_flow.est_w, eh = g_flow.est_h, levels = g_flow.est_levels;
    // The slot's own motion is always there (NVOFA's or the CPU's); its other
    // half only with NVOFA's two directions.
    UINT32 k[14] = { g_flow.w, g_flow.hgt, ew, eh, 0, levels, 1u | (outside_both ? 2u : 0u), EstimateRadius(interval),
                     0, 0, 0, g_flow.mw, g_flow.mh, g_flow.est_ratio };
    auto uav = [](ID3D12Resource *r) {
        D3D12_RESOURCE_BARRIER b = {};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = r;
        return b;
    };
    auto side = [](UINT n, UINT l) { return (n + (1u << l) - 1u) >> l; };
    auto pass = [&](int pso, UINT level, UINT from, UINT to, UINT step, UINT x, UINT y, ID3D12Resource *written) {
        k[4] = level; k[8] = step; k[9] = from; k[10] = to;
        list->SetComputeRoot32BitConstants(0, 14, k, 0);
        list->SetPipelineState(g_flow.est_pso[pso]);
        list->Dispatch((x + 7) / 8, (y + 7) / 8, 2);
        D3D12_RESOURCE_BARRIER b = uav(written);
        list->ResourceBarrier(1, &b);
    };
    D3D12_RESOURCE_BARRIER open[] = {
        Barrier(g_flow.est_mv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(g_flow.est_reverse, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    list->ResourceBarrier(_countof(open), open);
    // The pyramid, built through its UAV; then read through the texture cache.
    pass(0, 0, 0, 0, 0, ew, eh, g_flow.est_pyramid);
    for (UINT l = 1; l <= levels; ++l) pass(1, l, 0, 0, 0, side(ew, l), side(eh, l), g_flow.est_pyramid);
    D3D12_RESOURCE_BARRIER to_read = Barrier(g_flow.est_pyramid, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &to_read);
    // Coarse to fine; a pass reads one set of motion and writes the other.
    UINT set = 0;
    auto spreads = [&](UINT l) {
        for (UINT step : { 4u, 2u, 1u })
        {
            if (l == 0 && step != 1) continue;
            pass(4, l, set, 1 - set, step, side(ew, l), side(eh, l), g_flow.est_fields);
            set = 1 - set;
        }
    };
    pass(2, levels, 0, 0, 0, side(ew, levels), side(eh, levels), g_flow.est_fields);
    spreads(levels);
    for (int l = int(levels) - 1; l >= 0; --l)
    {
        pass(3, UINT(l), set, 1 - set, 0, side(ew, UINT(l)), side(eh, UINT(l)), g_flow.est_fields);
        set = 1 - set;
        spreads(UINT(l));
    }
    pass(5, 0, set, 1 - set, 0, ew, eh, g_flow.est_fields);
    set = 1 - set;
    pass(6, 0, set, set, 0, ew, eh, g_flow.est_mv);
    D3D12_RESOURCE_BARRIER close[] = {
        Barrier(g_flow.est_pyramid, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(g_flow.est_mv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(g_flow.est_reverse, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    list->ResourceBarrier(_countof(close), close);
}

// Not estimated this pair (the card is full): the outside motion stands in
// for the estimate, as it used to be read directly.
static void RecordFlowOutsideMotion(ID3D12GraphicsCommandList *list, unsigned slot_index)
{
    const FlowSlot &slot = g_flow.slots[slot_index];
    D3D12_RESOURCE_BARRIER pre[] = {
        Barrier(g_flow.est_mv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        Barrier(g_flow.est_reverse, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        Barrier(slot.mv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Barrier(slot.reverse, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
    };
    list->ResourceBarrier(_countof(pre), pre);
    list->CopyResource(g_flow.est_mv, slot.mv);
    list->CopyResource(g_flow.est_reverse, slot.reverse);
    D3D12_RESOURCE_BARRIER post[] = {
        Barrier(g_flow.est_mv, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(g_flow.est_reverse, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(slot.mv, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(slot.reverse, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    list->ResourceBarrier(_countof(post), post);
}

// The buffer a recording is read back through. Built the first time a
// recording asks for pixels and kept until FLOW MFG stops, so nothing is
// spent on it in the ordinary case.
static bool EnsureFlowReadback()
{
    if (g_flow.readback != nullptr) return true;
    if (g_flow.out_w == 0 || g_flow.out_h == 0) return false;
    const UINT pitch = (g_flow.out_w * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                       ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd = {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = static_cast<UINT64>(pitch) * g_flow.out_h;
    rd.Height = 1;
    rd.DepthOrArraySize = 1;
    rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &rd,
                                              D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              __uuidof(ID3D12Resource),
                                              reinterpret_cast<void **>(&g_flow.readback))))
    { Trace("[flow] read back buffer failed - the recording keeps the real frames"); return false; }
    g_flow.rb_pitch = pitch;
    Trace("[flow] recording reads back %ux%u from the presenter", g_flow.out_w, g_flow.out_h);
    return true;
}

// The presenter owns the window while FLOW MFG is on: it draws every frame -
// in between and real - into its scratch texture, copies it to the back
// buffer and presents, all on its own queue, paced by the compositor.
static void FlowPresenter()
{
    winrt::com_ptr<ID3D12CommandQueue> compute_queue;
    ID3D12CommandQueue *work_queue = g_flow.queue;
    D3D12_COMMAND_LIST_TYPE work_type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (g_flow.async_compute) {
        D3D12_COMMAND_QUEUE_DESC desc = {};
        desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        if (SUCCEEDED(lfCore.device->CreateCommandQueue(&desc, IID_PPV_ARGS(compute_queue.put())))) {
            work_queue = compute_queue.get();
            work_type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
        }
        else Trace("[flow] async compute unavailable - using DIRECT interpolation");
    }
    winrt::com_ptr<ID3D12CommandAllocator> present_alloc;
    winrt::com_ptr<ID3D12GraphicsCommandList> present_list;
    if (compute_queue) {
        if (FAILED(lfCore.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(present_alloc.put()))) ||
            FAILED(lfCore.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, present_alloc.get(), nullptr, IID_PPV_ARGS(present_list.put()))) ||
            FAILED(present_list->Close())) {
            present_list = nullptr;
            present_alloc = nullptr;
            compute_queue = nullptr;
            work_queue = g_flow.queue;
            work_type = D3D12_COMMAND_LIST_TYPE_DIRECT;
            Trace("[flow] async composition setup failed - using DIRECT interpolation");
        }
    }
    winrt::com_ptr<ID3D12CommandAllocator> alloc;
    winrt::com_ptr<ID3D12GraphicsCommandList> list;
    winrt::com_ptr<ID3D12Fence> fence;
    auto create_work = [&]() {
        return SUCCEEDED(lfCore.device->CreateCommandAllocator(work_type, IID_PPV_ARGS(alloc.put()))) &&
            SUCCEEDED(lfCore.device->CreateCommandList(0, work_type, alloc.get(), nullptr, IID_PPV_ARGS(list.put()))) &&
            SUCCEEDED(list->Close());
    };
    bool work_ready = create_work();
    if (!work_ready && compute_queue) {
        list = nullptr;
        alloc = nullptr;
        present_list = nullptr;
        present_alloc = nullptr;
        compute_queue = nullptr;
        work_queue = g_flow.queue;
        work_type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        Trace("[flow] async command setup failed - using DIRECT interpolation");
        work_ready = create_work();
    }
    if (!work_ready || FAILED(lfCore.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put()))))
    { g_flow.failed = true; return; }
    Trace("[flow] %s interpolation with DIRECT composition/presentation", compute_queue ? "async compute" : "DIRECT");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) { g_flow.failed = true; return; }
    const UINT stride = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    UINT64 value = 0, previous = 0, shown = 0, generated = 0, dropped = 0;
    auto report = std::chrono::steady_clock::now();
    bool buffer_ready = false;
    auto wait_buffer = [&]() {
        if (buffer_ready || lfShow.flow_waitable == nullptr) return true;
        const DWORD most = DWORD(std::clamp(1500.0 / std::max(OutputRefresh(), 24.0), 4.0, 63.0));
        const DWORD result = WaitForSingleObject(lfShow.flow_waitable, most);
        if (result == WAIT_FAILED) return false;
        // A transient overlay/compositor delay must not tear down the worker.
        if (result == WAIT_TIMEOUT) Trace("[flow] waitable timeout after %lu ms; attempting present", most);
        buffer_ready = true;
        return true;
    };
    // The refresh of the screen the output window is on, asked again every
    // second: in window mode the window can be dragged onto another screen,
    // and pacing kept at one screen's rate on the other (60 Hz on a 144 Hz
    // screen, or the other way round) shows a different number of frames on
    // each - the two screens must behave the same.
    auto display_refresh = []() {
        MONITORINFOEXW monitor = {}; monitor.cbSize = sizeof(monitor);
        DEVMODEW mode = {}; mode.dmSize = sizeof(mode);
        if (GetMonitorInfoW(MonitorFromWindow(lfShow.hwnd, MONITOR_DEFAULTTONEAREST), &monitor) &&
            EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
            return double(mode.dmDisplayFrequency);
        return 60.0;
    };
    double refresh = display_refresh();
    auto refresh_asked = std::chrono::steady_clock::now();
    // Present(0) lets DWM pick the newest frame instead of keeping a late
    // frame for another whole refresh. Keep one frame in flight.
    Trace("[flow] display pacing %.1f Hz, clocked mailbox, latency 1", refresh);

    auto seconds = []() {
        LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
        return double(q.QuadPart) / double(f.QuadPart);
    };
    // The searches (tracker, block search) only while there is room for them.
    // Where the source is slow because the card is full - the game and the
    // network sharing it - their time came out of the network, which slowed,
    // which made the pairs longer and the searches more likely: 68 FPS down to
    // 13 in ten seconds on an RTX 3060, and a settings change then stalled the
    // worker past the host's watchdog. A pair whose per-pair work took more
    // than a fifth of its interval turns them off; they are tried again 3 s on.
    bool search_ok = true, searched = false;
    double search_retry = 0;
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);
    auto wait_until = [&](double deadline) {
        while (!g_flow.stop) {
            const double remaining = deadline - seconds();
            if (remaining <= 0) break;
            // A high-resolution timer avoids the 15.6ms condition-variable
            // timeout quantum. Spin only the final 150us, never a whole frame.
            if (timer && remaining > .0003) {
                LARGE_INTEGER due; due.QuadPart = -LONGLONG(std::min(.005, remaining - .00015) * 1e7);
                if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE))
                    WaitForSingleObject(timer, 10);
                else SwitchToThread();
            } else SwitchToThread();
        }
    };
    double last_present = 0, delay = 0, render_budget = .0015, cadence_interval = 0;
    double draw_sum = 0, draw_peak = 0;   // each shown frame's drawing and copy, until the GPU is done
    unsigned draw_count = 0;
    // Round 25, said in the cadence line: what a frame in between really costs
    // with a game on the card (its drawing waits behind the game's work and
    // the network's) and the share of them that came too late - the "works,
    // then stops" pulse a person saw at x4 in a game (1.7 made of 3). Measured
    // with a stand-in game (tests/screen_latency_load.py --coupled): 8-11 ms
    // and 15-40% late at the normal scheduling class, and planning fewer
    // frames a pair did not make them less late (the game's work comes in
    // bursts); 2.2 ms and 4-8% with Low Latency (lf_latency.inl).
    double frame_cost = .002;
    // The real frame rate, smoothed over pairs: the low-FPS edge step follows
    // it, so a game hovering around 20 FPS no longer switches it on and off
    // from one pair to the next (it did, several times a second).
    double real_fps_smooth = 0;
    double missed_smooth = 0;   // the share of a pair's planned frames that came too late
    bool low_fps_said = false;
    double intervals[5] = {}; unsigned interval_count = 0, interval_index = 0;
    double ages[32] = {}; unsigned age_count = 0, age_index = 0;
    char trace_path[MAX_PATH] = {};
    FILE *trace = nullptr;
    if (GetEnvironmentVariableA("LF_FLOW_TRACE", trace_path, sizeof(trace_path)))
        fopen_s(&trace, trace_path, "w");
    if (trace) fprintf(trace, "present,sequence,alpha,source,ready,interval,draw_ms\n");

    auto record_frame = [&](unsigned slot_index, float t, float accumulate) -> ID3D12Resource * {
        ID3D12DescriptorHeap *heaps[] = { g_flow.heap };
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(g_flow.rs);
        const auto &pair = g_flow.slots[slot_index];
        // The search for a surface crossing a pixel reaches as far as the
        // pair is long: at 7.5 FPS a surface travels eight 60-Hz ticks.
        const UINT32 rings = std::clamp(UINT32(std::ceil(pair.interval * 60.0)), 2u, 8u);
        // The last constant is FLOW VIEW here: CSMain draws how FLOW MFG sees
        // the picture when it is set (BlockMain/TrackMain read it as their round).
        UINT32 c[7] = { g_flow.w, g_flow.hgt, 0, 0, UINT32(pair.both), rings, FlowViewShown() ? 1u : 0u };
        memcpy(&c[2], &t, sizeof(float));
        memcpy(&c[3], &accumulate, sizeof(float));
        list->SetComputeRoot32BitConstants(0, 7, c, 0);
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_flow.heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += static_cast<UINT64>(slot_index) * kFlowDescriptors * stride;
        list->SetComputeRootDescriptorTable(1, gpu);
        // Performance mode draws the frame lightly (FastMain); FLOW VIEW always
        // shows the full drawing's view of the picture.
        const bool light = g_flow.fast && t > 0.0f && t < 1.0f && !FlowViewShown();
        list->SetPipelineState(light ? g_flow.pso_fast : g_flow.pso);
        list->Dispatch((g_flow.w + 7) / 8, (g_flow.hgt + 7) / 8, 1);
        // A frame in between goes through DespeckleMain (the frame drawn carries
        // how sure it was of each pixel; the frame shown is opaque again) - not
        // a real frame, and not FLOW VIEW's picture, which shows what was drawn.
        ID3D12Resource *out = g_flow.scratch;
        if ((t > 0.0f && t < 1.0f && !FlowViewShown()) || g_flow.format == DXGI_FORMAT_R10G10B10A2_UNORM)
        {
            D3D12_RESOURCE_BARRIER to_read = Barrier(g_flow.scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            list->ResourceBarrier(1, &to_read);
            // Below about 20 real FPS DespeckleMain also steadies the unsure
            // edges (flow_shader.h); the strength rides in gAccum, which the
            // frame drawn above has already read. From the smoothed rate, and
            // said only when it really changes (on under 19, off over 21).
            const double real_fps = real_fps_smooth > 0 ? real_fps_smooth : 60.0;
            const float low_fps = std::clamp(float((20.0 - real_fps) / 8.0), 0.0f, 1.0f);
            if (!low_fps_said && real_fps < 19.0)
            {
                low_fps_said = true;
                Trace("[flow] %.0f real FPS - below 20 the unsure edges of the frames in between are steadied", real_fps);
            }
            else if (low_fps_said && real_fps > 21.0)
            {
                low_fps_said = false;
                Trace("[flow] %.0f real FPS - the edges are drawn as they are again", real_fps);
            }
            UINT32 low_bits = 0;
            memcpy(&low_bits, &low_fps, sizeof(low_bits));
            list->SetComputeRoot32BitConstant(0, low_bits, 3);
            list->SetPipelineState(g_flow.pso_despeckle);
            list->Dispatch((g_flow.w + 7) / 8, (g_flow.hgt + 7) / 8, 1);
            D3D12_RESOURCE_BARRIER back = Barrier(g_flow.scratch, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                     D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1, &back);
            out = g_flow.shown;
        }
        return out;
    };
    auto show = [&](unsigned slot_index, float t, float accumulate, double deadline) {
        const double draw_start = seconds();
        winrt::com_ptr<ID3D12Resource> bb;
        if (FAILED(lfShow.chain->GetBuffer(lfShow.chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(bb.put()))) ||
            FAILED(alloc->Reset()) || FAILED(list->Reset(alloc.get(), nullptr))) return false;
        ID3D12Resource *out = record_frame(slot_index, t, accumulate);
        ID3D12GraphicsCommandList *composition = list.get();
        if (compute_queue) {
            if (FAILED(list->Close())) return false;
            ID3D12CommandList *work[] = { list.get() };
            work_queue->ExecuteCommandLists(1, work);
            if (FAILED(work_queue->Signal(fence.get(), ++value)) || FAILED(g_flow.queue->Wait(fence.get(), value)) ||
                FAILED(present_alloc->Reset()) || FAILED(present_list->Reset(present_alloc.get(), nullptr))) return false;
            composition = present_list.get();
        }
        D3D12_RESOURCE_BARRIER pre[] = {
            Barrier(out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Barrier(bb.get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        composition->ResourceBarrier(_countof(pre), pre);
        // Scaled onto the whole screen while FLOW Upscaler is on.
        ShowBlit(composition, bb.get(), out);
        // Out to whoever is recording, from here rather than from the
        // handover: this is the only place every frame passes through, so a
        // Spout recorder (OBS) gets the frames in between as well - the
        // recording runs at the rate FLOW MFG shows, not the network's.
        ID3D12Resource *export_out = out;
        if (ShareActive() || ExportWanted()) {
            export_out = HdrExportFrame(composition, out, D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (!export_out) return false;
            ShareCopy(composition, export_out, g_flow.w, g_flow.hgt);
        }
        // And into the buffer the client's own recorder reads, while there is
        // one. Same picture, same moment - the recording then holds the
        // frames in between instead of only the network's.
        bool exporting = ExportWanted() && EnsureFlowReadback();
        if (exporting)
        {
            // Flow Super Resolution: at the display's size, like the engine's
            // own read-backs (flow_upscaler.inl).
            const bool scaled = g_flow.out_w != g_flow.w || g_flow.out_h != g_flow.hgt;
            ID3D12Resource *export_from = scaled ? SuperResExport(composition, export_out, g_flow.out_w, g_flow.out_h, true) : export_out;
            if (export_from == nullptr) exporting = false;
            else
            {
                D3D12_TEXTURE_COPY_LOCATION dst = {};
                dst.pResource = g_flow.readback;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                dst.PlacedFootprint.Footprint.Width = g_flow.out_w;
                dst.PlacedFootprint.Footprint.Height = g_flow.out_h;
                dst.PlacedFootprint.Footprint.Depth = 1;
                dst.PlacedFootprint.Footprint.RowPitch = g_flow.rb_pitch;
                D3D12_TEXTURE_COPY_LOCATION src = {};
                src.pResource = export_from;
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                composition->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                if (scaled) SuperResExportDone(composition, true);
            }
        }
        ShowProof(composition, bb.get(), t >= 1.0f);
        CounterStamp(composition, bb.get());
        D3D12_RESOURCE_BARRIER post[] = {
            Barrier(out, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Barrier(bb.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT),
        };
        composition->ResourceBarrier(_countof(post), post);
        if (FAILED(composition->Close())) return false;
        ID3D12CommandList *lists[] = { composition };
        g_flow.queue->ExecuteCommandLists(1, lists);
        if (FAILED(g_flow.queue->Signal(fence.get(), ++value)) ||
            FAILED(fence->SetEventOnCompletion(value, event)) ||
            WaitForSingleObject(event, 2000) != WAIT_OBJECT_0 ||
            fence->GetCompletedValue() < value) return false;
        const double draw_seconds = seconds() - draw_start;
        // Endpoints are cheap copies. Including them, and capping the lead
        // time at 6 ms, systematically started generated work too late when
        // a game shared the GPU (measured 7-11 ms per generated frame).
        if (t > 0.0f && t < 1.0f)
            render_budget += .2 * (std::clamp(draw_seconds + .0005, .0003, .03) - render_budget);
        // A frame in between: a real one is only copied and would make the
        // cost look smaller than what the planning is about.
        if (t > 0.0f && t < 1.0f) frame_cost += .15 * (std::min(draw_seconds, .1) - frame_cost);
        draw_sum += draw_seconds;
        draw_peak = std::max(draw_peak, draw_seconds);
        ++draw_count;
        // Finish GPU work BEFORE the presentation deadline. Waiting before
        // dispatch added a variable GPU delay to every interpolation sample.
        wait_until(deadline);
        // A real frame's age is measured (lf_latency.inl); a frame in between
        // has no source of its own.
        const HRESULT pr = ShowPresentNow(t >= 1.0f ? g_flow.slots[slot_index].source_time : 0.0);
        last_present = seconds();
        if (trace) {
            const auto &s = g_flow.slots[slot_index];
            const double stamp = seconds();
            fprintf(trace, "%.7f,%llu,%.5f,%.7f,%.7f,%.7f,%.4f\n",
                stamp, s.sequence, t, s.source_time, s.ready_time, s.interval, (stamp-draw_start)*1000);
        }
        buffer_ready = false;
        if (pr == DXGI_STATUS_MODE_CHANGED || pr == DXGI_STATUS_MODE_CHANGE_IN_PROGRESS)
        {
            Trace("[flow] present reports a mode change - skipping a frame");
            lfShow.stale = true;
            return true;
        }
        if (pr == DXGI_STATUS_OCCLUDED || pr == DXGI_ERROR_WAS_STILL_DRAWING) return true;
        if (FAILED(pr)) return false;
        ShareSend();
        if (exporting)
        {
            // After the present, not before it: the pixels are already on
            // their way to the screen and the copy out costs the recording,
            // not the picture.
            void *mapped = nullptr;
            D3D12_RANGE whole = { 0, static_cast<SIZE_T>(g_flow.rb_pitch) * g_flow.out_h };
            if (SUCCEEDED(g_flow.readback->Map(0, &whole, &mapped)) && mapped != nullptr)
            {
                ExportPicture(static_cast<const BYTE *>(mapped), g_flow.rb_pitch,
                                 g_flow.out_w, g_flow.out_h);
                D3D12_RANGE none = { 0, 0 };
                g_flow.readback->Unmap(0, &none);
            }
        }
        ShowRevealOnce();
        ++shown;
        if (t < 1.0f) ++generated;
        return true;
    };

    // Once per pair, as soon as it is picked and before any of its frames is
    // timed: the motion refined (the small-object tracker, then the block
    // search), the HUD verdict (HudMain), then the motion rebuilt under and
    // beside the HUD (FixMain), which every frame of the pair reuses. Done
    // inside the first frame's draw it made that frame late under load - the
    // network shares the GPU - and a late frame costs the next slot.
    // accumulate: 1 carries the history on, -1 starts it again.
    auto prepare = [&](unsigned slot_index, float accumulate) {
        if (FAILED(work_queue->Wait(lfCore.timeline, g_flow.slots[slot_index].ready_fence))) return false;
        if (FAILED(alloc->Reset()) || FAILED(list->Reset(alloc.get(), nullptr))) return false;
        // The motion first: keep the bidirectional estimate on every pair.
        // The load watch may skip the expensive small-object/block searches,
        // not switch the basic motion algorithm for three seconds. That
        // switch exposed raw, coarse flow right when a game loaded the GPU
        // and made surfaces alternate between coherent and torn patches.
        // Not for a pair that starts the history: nothing is drawn between its
        // frames, and its first-use cost (11 ms once) tripped the watch below
        // at the start of every session.
        if (g_flow.est_on)
        {
            if (accumulate >= 0.0f)
            {
                RecordFlowEstimate(list.get(), slot_index, g_flow.slots[slot_index].both, g_flow.slots[slot_index].interval);
                g_flow.slots[slot_index].both = true;
            }
            else
                RecordFlowOutsideMotion(list.get(), slot_index);
        }
        ID3D12DescriptorHeap *heaps[] = { g_flow.heap };
        list->SetDescriptorHeaps(1, heaps);
        list->SetComputeRootSignature(g_flow.rs);
        const float t = 0.0f;
        // The block search reaches as far as the pair is long.
        const UINT32 rings = std::clamp(UINT32(std::ceil(g_flow.slots[slot_index].interval * 60.0)), 2u, 8u);
        UINT32 c[7] = { g_flow.w, g_flow.hgt, 0, 0, UINT32(g_flow.slots[slot_index].both), rings, 0 };
        memcpy(&c[2], &t, sizeof(float));
        memcpy(&c[3], &accumulate, sizeof(float));
        list->SetComputeRoot32BitConstants(0, 7, c, 0);
        D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_flow.heap->GetGPUDescriptorHandleForHeapStart();
        gpu.ptr += static_cast<UINT64>(slot_index) * kFlowDescriptors * stride;
        list->SetComputeRootDescriptorTable(1, gpu);
        auto uav_barrier = [](ID3D12Resource *r) {
            D3D12_RESOURCE_BARRIER b = {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            b.UAV.pResource = r;
            return b;
        };
        D3D12_RESOURCE_BARRIER still_barrier = uav_barrier(g_flow.still);
        D3D12_RESOURCE_BARRIER open[] = {
            still_barrier,
            Barrier(g_flow.fix_mv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Barrier(g_flow.fix_reverse, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Barrier(g_flow.trk_mv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Barrier(g_flow.trk_reverse, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        };
        list->ResourceBarrier(_countof(open), open);
        // The small-object tracker and the block search are for a slow source:
        // there a ball moves further between two frames than it is wide and
        // the estimate loses it. From 40 FPS up the estimate keeps up, and the
        // searches (some 7 ms a pair at 1440p on an RTX 3060) would come out of
        // the game's own frame time - TrackMain then hands the estimate on.
        // Performance mode runs neither, at any frame rate (round 26).
        const bool search = g_flow.slots[slot_index].interval >= 1.0 / 40.0 && search_ok && !g_flow.fast;
        // Watch optional searches only. The essential estimate has a fixed
        // resolution cap (smaller in performance mode) and remains stable.
        searched = search;
        if (!search)
        {
            list->SetComputeRoot32BitConstant(0, 0xFFFFu, 6);   // kSkipSearch
            list->SetPipelineState(g_flow.pso_track);
            list->Dispatch((g_flow.mw + 7) / 8, (g_flow.mh + 7) / 8, 1);
            list->SetComputeRoot32BitConstant(0, 0, 6);
        }
        else
        {
        // The small-object tracker: residual, a seed per tile, the search,
        // then every texel takes what carries it best.
        list->SetPipelineState(g_flow.pso_resid);
        list->Dispatch((g_flow.mw + 7) / 8, (g_flow.mh + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER resid_barrier = uav_barrier(g_flow.resid);
        list->ResourceBarrier(1, &resid_barrier);
        list->SetPipelineState(g_flow.pso_seed);
        list->Dispatch(g_flow.tiles_x, g_flow.tiles_y, 1);
        D3D12_RESOURCE_BARRIER tiles_barrier = uav_barrier(g_flow.tiles);
        list->ResourceBarrier(1, &tiles_barrier);
        list->SetPipelineState(g_flow.pso_search);
        list->Dispatch(g_flow.tiles_x, g_flow.tiles_y, 1);
        list->ResourceBarrier(1, &tiles_barrier);
        list->SetPipelineState(g_flow.pso_track);
        list->Dispatch((g_flow.mw + 7) / 8, (g_flow.mh + 7) / 8, 1);
        // The block search starts from the tracked motion (read), a round at
        // a time - the round is the last constant - both directions at once;
        // TexelMain then writes its result over the tracked motion.
        D3D12_RESOURCE_BARRIER to_read[] = {
            tiles_barrier,
            Barrier(g_flow.trk_mv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Barrier(g_flow.trk_reverse, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        list->ResourceBarrier(_countof(to_read), to_read);
        list->SetPipelineState(g_flow.pso_block);
        for (UINT32 round = 0; round < kFlowBlockRounds; ++round)
        {
            list->SetComputeRoot32BitConstant(0, round, 6);
            list->Dispatch(g_flow.blocks_x, g_flow.blocks_y, 2);
            list->ResourceBarrier(1, &tiles_barrier);
        }
        list->SetComputeRoot32BitConstant(0, 0, 6);
        D3D12_RESOURCE_BARRIER to_write[] = {
            Barrier(g_flow.trk_mv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Barrier(g_flow.trk_reverse, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        };
        list->ResourceBarrier(_countof(to_write), to_write);
        list->SetPipelineState(g_flow.pso_texel);
        list->Dispatch((g_flow.mw + 7) / 8, (g_flow.mh + 7) / 8, 2);
        }
        // The HUD verdict reads what the tracker found (a texel it pinned
        // still), and FixMain starts from the tracked motion.
        D3D12_RESOURCE_BARRIER tracked[] = {
            Barrier(g_flow.trk_mv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Barrier(g_flow.trk_reverse, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        list->ResourceBarrier(_countof(tracked), tracked);
        list->SetPipelineState(g_flow.pso_hud);
        list->Dispatch((g_flow.w + 7) / 8, (g_flow.hgt + 7) / 8, 1);
        list->ResourceBarrier(1, &still_barrier);
        list->SetPipelineState(g_flow.pso_fix);
        list->Dispatch((g_flow.mw + 7) / 8, (g_flow.mh + 7) / 8, 1);
        // FixMain also marks the pixels near a HUD verdict in the state.
        D3D12_RESOURCE_BARRIER close[] = {
            still_barrier,
            Barrier(g_flow.fix_mv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Barrier(g_flow.fix_reverse, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        list->ResourceBarrier(_countof(close), close);
        // Round 26, from the repaired motion and the HUD state as they now
        // stand: where the crossing search can find anything (the full drawing
        // only - performance mode has no search), then where a HUD is.
        D3D12_RESOURCE_BARRIER records = uav_barrier(g_flow.tiles);
        if (!g_flow.fast)
        {
            list->SetPipelineState(g_flow.pso_range);
            list->Dispatch(g_flow.tiles_x, g_flow.tiles_y, 1);
            list->ResourceBarrier(1, &records);
            list->SetPipelineState(g_flow.pso_reach);
            list->Dispatch((g_flow.tiles_x + 7) / 8, (g_flow.tiles_y + 7) / 8, 1);
            list->ResourceBarrier(1, &records);
        }
        list->SetPipelineState(g_flow.pso_hudtile);
        list->Dispatch(((g_flow.w + 7) / 8 + 7) / 8, ((g_flow.hgt + 7) / 8 + 7) / 8, 1);   // a thread per 8x8 pixels
        list->ResourceBarrier(1, &records);
        if (FAILED(list->Close())) return false;
        ID3D12CommandList *lists[] = { list.get() };
        work_queue->ExecuteCommandLists(1, lists);
        return SUCCEEDED(work_queue->Signal(fence.get(), ++value)) &&
               SUCCEEDED(fence->SetEventOnCompletion(value, event)) &&
               WaitForSingleObject(event, 2000) == WAIT_OBJECT_0 && fence->GetCompletedValue() >= value;
    };

    double source_interval_ms = 0;
    auto reportCadence = [&]() {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - report).count();
        if (elapsed >= 1.0)
        {
            Trace("[flow] displayed %.1f FPS (real + generated), x%u", shown / elapsed, FlowExtraFrames() + 1);
            Trace("[flow] cadence: real=%llu generated=%llu dropped-pairs=%llu over=%.3fs source-interval=%.3fms "
                  "draw=%.2f/%.2fms at %ux%u, a frame in between costs %.1f ms, %.0f%% late source=%u multiplier=%u",
                shown - generated, generated, dropped, elapsed, source_interval_ms,
                draw_count ? draw_sum * 1000.0 / draw_count : 0.0, draw_peak * 1000.0, g_flow.w, g_flow.hgt,
                frame_cost * 1000.0, missed_smooth * 100.0, g_flow.captured.exchange(0), FlowExtraFrames() + 1);
            shown = 0;
            generated = dropped = 0;
            draw_sum = draw_peak = 0;
            draw_count = 0;
            report = now;
        }
    };

    while (!g_flow.stop && !g_flow.failed)
    {
        FlowSlot *chosen = nullptr;
        unsigned chosen_index = 0;
        {
            std::unique_lock<std::mutex> lock(g_flow.mutex);
            const bool ready = g_flow.wake.wait_for(lock, std::chrono::milliseconds(250), [] {
                if (g_flow.stop) return true;
                for (auto &s : g_flow.slots) if (s.state == 2) return true;
                return false;
            });
            if (g_flow.stop) break;
            if (!ready) { lock.unlock(); reportCadence(); continue; }
            for (unsigned i = 0; i < _countof(g_flow.slots); ++i)
            {
                FlowSlot &s = g_flow.slots[i];
                if (s.state == 2 && (!chosen || s.sequence > chosen->sequence)) { chosen = &s; chosen_index = i; }
            }
            for (auto &s : g_flow.slots) if (s.state == 2 && &s != chosen) s.state = 0;
            chosen->state = 3;
        }
        if (previous && chosen->sequence > previous + 1)
            dropped += chosen->sequence - previous - 1; // includes overwritten ready slots
        // A pair the presenter skipped keeps the HUD verdict and the
        // stillness count: its two frames are still consecutive captures.
        // Only a real break in the history (the first frame, a reset, a
        // stall, a long gap) clears them.
        const double prepare_start = seconds();
        if (!prepare(chosen_index, chosen->interpolate ? 1.0f : -1.0f)) { g_flow.failed = true; break; }
        // Counted into the pair's age below, so the timing plans for it.
        const double prepared = seconds() - prepare_start;
        if (searched && prepared > 0.2 * chosen->interval)
        {
            if (search_ok)
                Trace("[flow] the card is full (%.1f ms for a %.1f ms pair) - the searches wait",
                    prepared * 1000.0, chosen->interval * 1000.0);
            search_ok = false;
            search_retry = seconds() + 3.0;
        }
        else if (!search_ok && seconds() >= search_retry)
            search_ok = true;
        const auto start = std::chrono::steady_clock::now();
        const unsigned count = chosen->count;
        if (std::chrono::steady_clock::now() - refresh_asked > std::chrono::seconds(1))
        {
            refresh_asked = std::chrono::steady_clock::now();
            const double now_refresh = display_refresh();
            if (now_refresh != refresh)
                Trace("[flow] the output screen now runs at %.1f Hz (was %.1f) - the pacing follows", now_refresh, refresh);
            refresh = now_refresh;
        }
        // The frame limit (the LIVE FLOW slider) caps what is shown.
        // With VRR the screen refreshes on each frame, so what is shown stays
        // inside its range (lf_vrr.inl); the real frames are not held back.
        const int limit = ShownCap();
        const double top = lfShow.vrr ? VrrCeiling(refresh) : refresh;
        const double shown_rate = limit > 0 ? std::min(top, double(limit)) : top;
        const unsigned steps = std::min(count + 1, std::max(1u,
            static_cast<unsigned>(std::round(chosen->interval * shown_rate))));
        if (chosen->interval > 0)
        {
            const double pair_fps = 1.0 / chosen->interval;
            real_fps_smooth = real_fps_smooth > 0 ? real_fps_smooth + .15 * (pair_fps - real_fps_smooth) : pair_fps;
        }
        // Every slot owns its exact consecutive input pair. A dropped output
        // slot does not invalidate that pair's motion; deadlines below skip
        // obsolete samples. Resetting on an output drop suppressed all the
        // intermediate frames precisely while the GPU was busy.
        const bool continuous = chosen->interpolate && previous != 0;
        const double step = chosen->interval / steps;
        const double source = chosen->source_time > 0 ? chosen->source_time : chosen->ready_time;
        // Adapt to capture/service latency, not time spent behind our own
        // presentation hold. Feeding that residence back into the cushion
        // grows delay on every pair whenever prepare + draw exceeds a slot.
        // A late pair instead loses obsolete intermediate slots below.
        const double age = std::clamp(std::max(0.0, chosen->ready_time - source) + prepared, 0.0, .12);
        if (!continuous) {
            age_count = age_index = interval_count = interval_index = 0;
            delay = cadence_interval = 0;
        }
        ages[age_index++ % 32] = age;
        age_count = std::min(32u, age_count + 1);
        std::vector<double> sorted(ages, ages + age_count);
        std::sort(sorted.begin(), sorted.end());
        // A bounded jitter cushion, not an accumulating queue of pairs.
        // Low latency: the first generated frame is ready immediately and
        // the endpoint follows after N-1 slots, not a whole extra interval.
        // The Low Latency switch (lf_latency.inl) keeps the median of the
        // recent ages instead of their 75th percentile: a smaller cushion,
        // and the missed-slot rule below still keeps the pacing.
        // Round 25: only while frames are not being missed - with late frames
        // the smaller cushion cost more than it saved ("not stable").
        const unsigned cushion_at = LowLatencyWanted() && missed_smooth < .05 ? (age_count - 1) / 2
                                                                              : (age_count - 1) * 3 / 4;
        const double desired = sorted[cushion_at] +
            chosen->interval - step + render_budget + .0005;
        // A real cadence step (e.g. 30 -> 15 -> 30 FPS) changes the amount of
        // interpolation lookahead. Following it at only .1 slot per pair held
        // the low-FPS delay for many frames after the source recovered. The
        // missed-slot / no-burst rules below safely discard obsolete samples.
        intervals[interval_index++ % 5] = chosen->interval;
        interval_count = std::min(5u, interval_count + 1);
        std::vector<double> recent(intervals, intervals + interval_count);
        std::sort(recent.begin(), recent.end());
        const double cadence = recent[interval_count / 2];
        // Require a persistent change, not a single missed capture or the
        // alternating intervals produced when source/display rates differ.
        const bool cadence_change = continuous && interval_count >= 3 && cadence_interval > 0 &&
            (cadence > cadence_interval * 1.65 || cadence < cadence_interval / 1.65);
        if (cadence_interval == 0 || cadence_change) cadence_interval = cadence;
        else cadence_interval += .1 * (cadence - cadence_interval);
        if (delay == 0 || age_count < 4 || cadence_change) delay = desired;
        else delay += std::clamp(desired - delay, -step * .1, step * .1);
        const double end = source + delay;
        // How late a frame in between may be and still be shown (round 27).
        // The back buffer comes free once a refresh, so a frame is shown on a
        // refresh, not on its deadline: with a slot as long as one refresh
        // (100 real FPS x2 on a 200 Hz screen) a third of the frames in
        // between fell outside 35% of a slot and were thrown away - though
        // each would have been on screen at the very next refresh, in order.
        // Nearly one refresh is allowed, never a whole slot.
        const double late_by = std::max(step * .35, std::min(step * .9, .9 / std::max(refresh, 24.0)));
        unsigned missed = 0;
        for (unsigned k = continuous ? 1 : steps; k <= steps && !g_flow.stop && !g_flow.failed; ++k) {
            double deadline = continuous ? end - (steps - k) * step : seconds();
            // A missed slot is dropped; never catch up with a burst of old
            // samples. Do keep the real endpoint when the source stops.
            if (k < steps && (seconds() > deadline + late_by ||
                             deadline < last_present + step * .65)) { ++missed; continue; }
            deadline = std::max(deadline, last_present + step * .65);
            wait_until(deadline - render_budget);
            if (g_flow.stop) break;
            if (!wait_buffer()) { g_flow.failed = true; break; }
            if (k < steps && seconds() > deadline + late_by) { ++missed; continue; }
            if (!show(chosen_index, float(k) / steps, 0.0f, deadline)) g_flow.failed = true;
        }
        if (continuous && steps > 1) missed_smooth += .1 * (double(missed) / double(steps - 1) - missed_smooth);
        previous = chosen->sequence;
        source_interval_ms = chosen->interval * 1000.0;
        {
            std::lock_guard<std::mutex> lock(g_flow.mutex);
            chosen->state = 0;
        }
        reportCadence();
    }
    if (g_flow.failed) {
        if (compute_queue && !FlowDrainQueue(compute_queue.get())) FlowDrainFailed();
        if (!FlowDrainQueue(g_flow.queue)) FlowDrainFailed();
    }
    CloseHandle(event);
    if (timer) CloseHandle(timer);
    if (trace) fclose(trace);
    if (g_flow.failed)
    {
        Trace("[flow] presenter failed - back to the ordinary output");
        lfShow.stale = true;
    }
}

static bool EnsureFlow(UINT w, UINT hgt, UINT mw, UINT mh, UINT ow, UINT oh, DXGI_FORMAT format)
{
    // Performance mode sizes the estimate and the tile records differently:
    // switching it builds FLOW MFG again, like a new size.
    const bool fast = FlowFastWanted();
    const bool async_compute = FlowAsyncWanted();
    if (g_flow.thread.joinable() && g_flow.w == w && g_flow.hgt == hgt && g_flow.mw == mw && g_flow.mh == mh &&
        g_flow.out_w == ow && g_flow.out_h == oh && g_flow.format == format && g_flow.fast == fast && g_flow.async_compute == async_compute)
        return true;
    CloseFlow();
    g_flow.fast = fast;
    g_flow.format = format;
    g_flow.async_compute = async_compute;
    if (!EnsureFlowPipeline() || g_flow.queue == nullptr) return false;
    g_flow.last = NewTexture(w, hgt, format, false);
    g_flow.scratch = NewTexture(w, hgt, format == DXGI_FORMAT_R10G10B10A2_UNORM ? DXGI_FORMAT_R16G16B16A16_FLOAT : format, true);
    g_flow.shown = NewTexture(w, hgt, format, true);
    g_flow.still = NewTexture(w, hgt, DXGI_FORMAT_R32_FLOAT, true);
    g_flow.fix_mv = NewTexture(mw, mh, DXGI_FORMAT_R16G16_FLOAT, true);
    g_flow.fix_reverse = NewTexture(mw, mh, DXGI_FORMAT_R16G16_FLOAT, true);
    g_flow.trk_mv = NewTexture(mw, mh, DXGI_FORMAT_R16G16_FLOAT, true);
    g_flow.trk_reverse = NewTexture(mw, mh, DXGI_FORMAT_R16G16_FLOAT, true);
    g_flow.resid = NewTexture(mw, mh, DXGI_FORMAT_R32_UINT, true);
    g_flow.tiles_x = (mw + kFlowTile - 1) / kFlowTile;
    g_flow.tiles_y = (mh + kFlowTile - 1) / kFlowTile;
    g_flow.blocks_x = (mw + kFlowBlock - 1) / kFlowBlock;
    g_flow.blocks_y = (mh + kFlowBlock - 1) / kFlowBlock;
    {
        // Two float4 per tile: the seed and what the search found; then four
        // per block: the block search's two fields, each read one round and
        // written the next; then two per tile again: RangeMain's and
        // ReachMain's motion ranges; then one per 8x8 pixels: the HUD map.
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd = {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = FlowTileRecords(w, hgt) * 16;
        rd.Height = 1;
        rd.DepthOrArraySize = 1;
        rd.MipLevels = 1;
        rd.Format = DXGI_FORMAT_UNKNOWN;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        lfCore.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                       __uuidof(ID3D12Resource), reinterpret_cast<void **>(&g_flow.tiles));
    }
    bool ok = g_flow.last != nullptr && g_flow.scratch != nullptr && g_flow.shown != nullptr && g_flow.still != nullptr &&
              g_flow.fix_mv != nullptr && g_flow.fix_reverse != nullptr &&
              g_flow.trk_mv != nullptr && g_flow.trk_reverse != nullptr &&
              g_flow.resid != nullptr && g_flow.tiles != nullptr;
    for (auto &slot : g_flow.slots)
    {
        slot.prev = NewTexture(w, hgt, format, false);
        slot.cur = NewTexture(w, hgt, format, false);
        slot.mv = NewTexture(mw, mh, DXGI_FORMAT_R16G16_FLOAT, false);
        slot.reverse = NewTexture(mw, mh, DXGI_FORMAT_R16G16_FLOAT, false);
        slot.state = 0;
        ok = ok && slot.prev && slot.cur && slot.mv && slot.reverse;
    }
    if (!ok) { Trace("[flow] %ux%u textures failed", w, hgt); CloseFlow(); return false; }
    g_flow.est_on = EnsureFlowEstimate() && EnsureFlowEstimateTargets(mw, mh);
    if (!g_flow.est_on)
        for (ID3D12Resource **r : { &g_flow.est_pyramid, &g_flow.est_fields, &g_flow.est_mv, &g_flow.est_reverse })
            if (*r != nullptr) { (*r)->Release(); *r = nullptr; }
    if (!CmdOpen()) { CloseFlow(); return false; }
    std::vector<D3D12_RESOURCE_BARRIER> setup;
    setup.push_back(Barrier(g_flow.last, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE));
    setup.push_back(Barrier(g_flow.scratch, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    setup.push_back(Barrier(g_flow.shown, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    setup.push_back(Barrier(g_flow.still, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    setup.push_back(Barrier(g_flow.fix_mv, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    setup.push_back(Barrier(g_flow.fix_reverse, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    setup.push_back(Barrier(g_flow.trk_mv, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    setup.push_back(Barrier(g_flow.trk_reverse, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    setup.push_back(Barrier(g_flow.resid, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    setup.push_back(Barrier(g_flow.tiles, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
    for (auto &slot : g_flow.slots)
        for (ID3D12Resource *r : { slot.prev, slot.cur, slot.mv, slot.reverse })
            setup.push_back(Barrier(r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    if (g_flow.est_on)
    {
        setup.push_back(Barrier(g_flow.est_pyramid, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        setup.push_back(Barrier(g_flow.est_fields, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
        setup.push_back(Barrier(g_flow.est_mv, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        setup.push_back(Barrier(g_flow.est_reverse, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
    }
    lfCore.cmd->ResourceBarrier(static_cast<UINT>(setup.size()), setup.data());
    if (!AwaitMark(lfCore.timeline, CmdSubmit(), 30000)) { CloseFlow(); return false; }

    const UINT stride = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_flow.heap->GetCPUDescriptorHandleForHeapStart();
    for (auto &slot : g_flow.slots)
    {
        // The repaired motion is shared: the presenter draws one pair at a
        // time, and FixMain rewrites it before that pair's first frame.
        // "The estimate": FLOW MFG's own when it has one, the slot's otherwise.
        ID3D12Resource *srv[8] = { slot.prev, slot.cur, g_flow.fix_mv, g_flow.fix_reverse,
                                   g_flow.est_on ? g_flow.est_mv : slot.mv, g_flow.est_on ? g_flow.est_reverse : slot.reverse,
                                   g_flow.trk_mv, g_flow.trk_reverse };
        for (ID3D12Resource *r : srv)
        {
            D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
            sd.Format = r->GetDesc().Format;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            lfCore.device->CreateShaderResourceView(r, &sd, cpu);
            cpu.ptr += stride;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = g_flow.scratch->GetDesc().Format;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        lfCore.device->CreateUnorderedAccessView(g_flow.scratch, nullptr, &ud, cpu);
        cpu.ptr += stride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC sud = {};
        sud.Format = DXGI_FORMAT_R32_FLOAT;
        sud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        lfCore.device->CreateUnorderedAccessView(g_flow.still, nullptr, &sud, cpu);
        cpu.ptr += stride;
        for (ID3D12Resource *r : { g_flow.fix_mv, g_flow.fix_reverse, g_flow.trk_mv, g_flow.trk_reverse })
        {
            D3D12_UNORDERED_ACCESS_VIEW_DESC mud = {};
            mud.Format = DXGI_FORMAT_R16G16_FLOAT;
            mud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            lfCore.device->CreateUnorderedAccessView(r, nullptr, &mud, cpu);
            cpu.ptr += stride;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC rud = {};
        rud.Format = DXGI_FORMAT_R32_UINT;
        rud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        lfCore.device->CreateUnorderedAccessView(g_flow.resid, nullptr, &rud, cpu);
        cpu.ptr += stride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC tud = {};
        tud.Format = DXGI_FORMAT_UNKNOWN;
        tud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        tud.Buffer.NumElements = UINT(FlowTileRecords(w, hgt));
        tud.Buffer.StructureByteStride = 16;
        lfCore.device->CreateUnorderedAccessView(g_flow.tiles, nullptr, &tud, cpu);
        cpu.ptr += stride;
        // t8 / u8: the frame drawn, and the frame shown (DespeckleMain).
        D3D12_SHADER_RESOURCE_VIEW_DESC dsd = {};
        dsd.Format = g_flow.scratch->GetDesc().Format;
        dsd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        dsd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        dsd.Texture2D.MipLevels = 1;
        lfCore.device->CreateShaderResourceView(g_flow.scratch, &dsd, cpu);
        cpu.ptr += stride;
        ud.Format = format;
        lfCore.device->CreateUnorderedAccessView(g_flow.shown, nullptr, &ud, cpu);
        cpu.ptr += stride;
    }
    g_flow.w = w; g_flow.hgt = hgt; g_flow.mw = mw; g_flow.mh = mh;
    g_flow.out_w = ow; g_flow.out_h = oh;
    g_flow.stop = false;
    g_flow.history = false;
    // Latency 1 and the waitable: the compositor releases one back buffer per
    // vblank and the presenter waits for it instead of guessing the clock.
    if (lfShow.chain != nullptr)
    {
        IDXGISwapChain2 *sc2 = nullptr;
        if (SUCCEEDED(lfShow.chain->QueryInterface(__uuidof(IDXGISwapChain2), reinterpret_cast<void **>(&sc2))) && sc2)
        {
            sc2->SetMaximumFrameLatency(1);
            if (!lfShow.flow_waitable) lfShow.flow_waitable = sc2->GetFrameLatencyWaitableObject();
            sc2->Release();
        }
    }
    g_flow.captured = 0;
    g_flow.thread = std::thread(FlowPresenter);
    Trace("[flow] FLOW MFG x%u at %ux%u (motion %ux%u), %s, presented on its own queue",
        FlowExtraFrames() + 1, w, hgt, mw, mh, g_flow.fast ? "performance mode" : "the full drawing");
    if (g_flow.est_on)
        Trace("[flow] own motion estimate: %ux%u, %u levels, both directions - %s taken as a candidate",
            g_flow.est_w, g_flow.est_h, g_flow.est_levels + 1,
            OfaWanted() && !ofa.failed ? "NVIDIA Optical Flow" : "the CPU field");
    return true;
}

// Nothing was handed over (starved or failed), but the defer-tail contract
// still wants a fence after the evaluate: an empty submission gives one.
static bool FlowToken()
{
    if (!CmdOpen()) return false;
    const UINT64 fence = CmdSubmit();
    if (!AwaitMark(lfCore.timeline, fence, 30000)) return false;
    lfShow.flow_mark = fence;
    return true;
}

// Bounded, opt-in diagnostics: replay identical captured colours and estimated
// motion through the production shader without a live desktop or timing noise.
static void DumpFlowPair(FlowSlot &slot)
{
    char folder[MAX_PATH] = {};
    if (!GetEnvironmentVariableA("LF_FLOW_DUMP", folder, sizeof(folder))) return;
    const UINT64 sequence = g_flow.sequence + 1;
    if (sequence < 50 || sequence >= 58 || g_flow.w != 1280) return;
    char path[MAX_PATH]; FILE *file = nullptr;
    sprintf_s(path, "%s/pair-%04llu.json", folder, sequence);
    if (!fopen_s(&file, path, "w") && file) {
        fprintf(file, "{\"colour\":[%u,%u],\"motion\":[%u,%u]}",
            g_flow.w, g_flow.hgt, g_flow.mw, g_flow.mh); fclose(file);
    }
    for (auto pair : {std::make_pair(slot.prev, "prev"), std::make_pair(slot.cur, "cur"),
                      std::make_pair(slot.mv, "mv"), std::make_pair(slot.reverse, "reverse")}) {
        auto desc = pair.first->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT rows; UINT64 rowbytes, bytes;
        lfCore.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowbytes, &bytes);
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes; rd.Height = 1; rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        winrt::com_ptr<ID3D12Resource> rb;
        if (FAILED(lfCore.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(rb.put()))) || !CmdOpen()) return;
        auto pre = Barrier(pair.first, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        lfCore.cmd->ResourceBarrier(1, &pre);
        D3D12_TEXTURE_COPY_LOCATION src{}, dst{};
        src.pResource = pair.first; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.pResource = rb.get(); dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint = fp;
        lfCore.cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        auto post = Barrier(pair.first, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        lfCore.cmd->ResourceBarrier(1, &post);
        if (!AwaitMark(lfCore.timeline, CmdSubmit(), 30000)) return;
        BYTE *data = nullptr; D3D12_RANGE read{0, SIZE_T(bytes)}, written{0, 0};
        if (FAILED(rb->Map(0, &read, reinterpret_cast<void **>(&data)))) return;
        sprintf_s(path, "%s/%s-%04llu.bin", folder, pair.second, sequence);
        if (!fopen_s(&file, path, "wb") && file) {
            for (UINT y = 0; y < rows; ++y) fwrite(data + SIZE_T(y) * fp.Footprint.RowPitch, 1, SIZE_T(rowbytes), file);
            fclose(file);
        }
        rb->Unmap(0, &written);
    }
}

// The main path's half: copy the finished frame, the one before it and the
// motion between them into a free slot and hand it to the presenter.
static bool FlowPresent(Pipeline &v, ID3D12Resource *color, D3D12_RESOURCE_STATES state)
{
    const UINT w = v.stretched ? v.screen_w : v.work_w;
    const UINT hgt = v.stretched ? v.screen_h : v.work_h;
    // Flow Super Resolution: FLOW MFG draws at the internal size; only its
    // recording export goes out at the display's.
    if (!EnsureFlow(w, hgt, v.work_w, v.work_h, v.super_res ? v.display_w : w, v.super_res ? v.display_h : hgt, color->GetDesc().Format))
    {
        g_flow.failed = true;
        CloseFlow();
        lfShow.stale = true;
        Trace("[flow] could not start - back to the ordinary output");
        return FlowToken();
    }
    const auto now = std::chrono::steady_clock::now();
    const double source_time = (lfCap.window_live || lfCap.live) ? lfCap.stamp : 0.0;
    if (g_flow.history && source_time > 0 && source_time == g_flow.last_source_time && !lfShow.history_restart)
        return FlowToken();
    const double interval = g_flow.history && source_time > g_flow.last_source_time && g_flow.last_source_time > 0
        ? source_time - g_flow.last_source_time
        : (g_flow.history ? std::chrono::duration<double>(now - g_flow.last_time).count() : 0.016);
    // Down to 5 FPS. Below that a pair is a pause more than a motion.
    const bool interpolate = g_flow.history && !lfShow.history_restart && interval < 0.2;
    g_flow.last_time = now;
    g_flow.last_source_time = source_time;
    ++g_flow.captured;

    FlowSlot *slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_flow.mutex);
        for (auto &s : g_flow.slots) if (s.state == 0) { slot = &s; break; }
        if (!slot) for (auto &s : g_flow.slots) if (s.state == 2) { slot = &s; break; }
        if (slot) slot->state = 1;
    }
    if (!slot) return FlowToken();   // every slot is on screen right now: drop this one

    if (!CmdOpen())
    {
        std::lock_guard<std::mutex> lock(g_flow.mutex);
        slot->state = 0;
        return false;
    }
    ID3D12Resource *motion = v.motion.gpu_tex, *reverse = nullptr;
    bool both = ofa.bidirectional && ofa.valid && ofa.mfg_motion && ofa.reverse && !ofa.failed;
    const bool hardware_motion = both;
    if (both) { motion = ofa.mfg_motion.get(); reverse = ofa.reverse.get(); }
    else both = NetMotionForFlow(motion, reverse);
    if (both && (motion->GetDesc().Width != v.work_w || motion->GetDesc().Height != v.work_h ||
                 reverse->GetDesc().Width != v.work_w || reverse->GetDesc().Height != v.work_h)) {
        motion = v.motion.gpu_tex; reverse = nullptr; both = false;
    }
    const int source = both ? (hardware_motion ? 1 : 2) : 0;
    if (source != g_flow.motion_source) {
        Trace("[flow] motion pair: %s", source == 1 ? "NVOFA, both directions before NR settling" :
              source == 2 ? "GPU estimate, both directions before NR settling" : "single-direction guide (history reset or external input)");
        g_flow.motion_source = source;
    }
    std::vector<D3D12_RESOURCE_BARRIER> pre = {
        Barrier(slot->prev, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        Barrier(slot->cur, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        Barrier(slot->mv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        Barrier(motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
    };
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE)
        pre.push_back(Barrier(color, state, D3D12_RESOURCE_STATE_COPY_SOURCE));
    lfCore.cmd->ResourceBarrier(static_cast<UINT>(pre.size()), pre.data());
    lfCore.cmd->CopyResource(slot->prev, g_flow.history ? g_flow.last : color);
    lfCore.cmd->CopyResource(slot->cur, color);
    lfCore.cmd->CopyResource(slot->mv, motion);
    if (both) {
        D3D12_RESOURCE_BARRIER rev_pre[] = {
            Barrier(slot->reverse, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
            Barrier(reverse, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE)};
        lfCore.cmd->ResourceBarrier(2, rev_pre);
        lfCore.cmd->CopyResource(slot->reverse, reverse);
        D3D12_RESOURCE_BARRIER rev_post[] = {
            Barrier(slot->reverse, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Barrier(reverse, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
        lfCore.cmd->ResourceBarrier(2, rev_post);
    }
    D3D12_RESOURCE_BARRIER last_in = Barrier(g_flow.last, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    lfCore.cmd->ResourceBarrier(1, &last_in);
    lfCore.cmd->CopyResource(g_flow.last, color);
    // Nothing is exported from here while FLOW MFG is on. The presenter
    // exports what it shows - the frames in between as well as the real ones -
    // so an outside recorder sees the multiplied picture instead of the
    // network's rate. Exporting from both would mean two queues copying into
    // the one shared texture.
    std::vector<D3D12_RESOURCE_BARRIER> post = {
        Barrier(g_flow.last, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Barrier(slot->prev, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(slot->cur, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(slot->mv, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(motion, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE)
        post.push_back(Barrier(color, D3D12_RESOURCE_STATE_COPY_SOURCE, state));
    lfCore.cmd->ResourceBarrier(static_cast<UINT>(post.size()), post.data());
    const UINT64 fence = CmdSubmit();
    if (!fence)
    {
        std::lock_guard<std::mutex> lock(g_flow.mutex);
        slot->state = 0;
        return false;
    }
    lfShow.flow_mark = fence;
    DumpFlowPair(*slot);
    {
        std::lock_guard<std::mutex> lock(g_flow.mutex);
        slot->sequence = ++g_flow.sequence;
        slot->ready_fence = fence;
        slot->count = FlowExtraFrames();
        slot->interpolate = interpolate;
        slot->both = both;
        slot->interval = std::clamp(interval, 0.004, 0.2);
        slot->source_time = source_time;
        LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f);
        slot->ready_time = double(q.QuadPart) / double(f.QuadPart);
        slot->state = 2;
    }
    g_flow.history = true;
    g_flow.wake.notify_one();
    return true;
}
