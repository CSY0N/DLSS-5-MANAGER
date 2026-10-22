// lf_own_motion.inl - the first two pieces of LIVE FLOW's frame loop moved
// from the engine (Python) into the worker (round 23; the person asked for
// it a step at a time, each step checked before the next):
//
//   1. The scene cut. The engine used to compare each frame's small luma with
//      the one before (motion_guides.py: mean difference over 0.24 of full
//      scale) and send "reset" with the frame, so the network and FLOW MFG
//      would drop their history across a cut. The worker already makes that
//      luma itself right after each capture (lf_luma.inl); it now does the
//      comparison too - the same measure and the same threshold.
//
//   2. The network's motion. Where NVIDIA Optical Flow is not there or fails
//      (AMD, older cards), the engine computed the motion on the CPU with
//      OpenCV (DIS at 320x180) and sent the field with every frame through
//      the pipe. The worker now finds it on the GPU with the estimate FLOW
//      MFG has used since round 17 (flow_estimate.h: coarse to fine on a
//      colour pyramid, both directions checked against each other) - the
//      same compiled kernels, an instance of its own. Measured on the
//      football truth test that estimate was better than the CPU field (the
//      ball 2.75 px off instead of 9.6) and costs about 2 ms a frame on an
//      RTX 3060 instead of 7 ms of CPU.
//
// With both, a frame marked kFrameOwnMotion carries no motion and no reset:
// the engine only says "the next frame" and waits for its result.
//
//   And the still check that went with the engine's motion (3). The CPU field
//   was cleaned before it was sent: vectors under half a pixel set to none,
//   and a vector kept only where it explained the change between the two
//   frames better than "nothing moved" did (motion_guides.py _did_move).
//   NVIDIA Optical Flow's field never had that - on a still picture it holds
//   small random vectors, and the network, which follows its motion, shivered
//   with them: measured on a still photo, twice the frame-to-frame change of
//   the CPU path (0.21 against 0.09 levels), the "air over a hot road" people
//   saw. MotionSettle does the same cleaning on the GPU, after either source.

// ---- 1. The scene cut --------------------------------------------------------

// How far this capture is from the one before: the mean luma difference, 0..1
// of full scale. Uses the luma the capture just wrote (lfLuma.view, w x h
// bytes). -1 when it cannot be told (no luma, the first frame, a new size).
// Exactly 0 is the picture before over again (SourceSeen).
static double LumaChangeNow()
{
    static std::vector<BYTE> last;
    if (!lfLuma.mapped || !lfCap.luma_ok || lfLuma.view == nullptr || lfLuma.w == 0 || lfLuma.h == 0) return -1.0;
    const size_t n = static_cast<size_t>(lfLuma.w) * lfLuma.h;
    if (last.size() != n)
    {
        last.assign(lfLuma.view, lfLuma.view + n);
        return -1.0;           // the first frame is a reset anyway
    }
    uint64_t sum = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const int d = int(lfLuma.view[i]) - int(last[i]);
        sum += static_cast<uint64_t>(d < 0 ? -d : d);
    }
    memcpy(last.data(), lfLuma.view, n);
    return double(sum) / (255.0 * double(n));
}

// Is this capture a different scene from the one before?
static bool SceneCutNow(double change)
{
    if (change <= 0.24) return false;
    Trace("[history] scene cut (%.2f of full scale) - the network and FLOW MFG start fresh", change);
    return true;
}

// The capture handed over the picture before once more (round 31). Desktop
// Duplication reports a new frame of the desktop whenever the compositor draws
// it again - also when what it drew was LIVE FLOW's own output on top of the
// game, which the capture leaves out: the same picture of the game, with a new
// time stamp. Where the output is put on the screen without the compositor
// that never happens, and it had not shown; where it is not (another overlay
// on top, a window that is not the whole screen) every frame shown came back
// as a "frame of the game": a 72 FPS game read as 165, frames in between made
// of two identical pictures, the generator switched off for "no room", a wave
// in the pace at 20 FPS. Such a frame is nothing new: not shown, not paced by,
// not generated from. (A game standing still hands over the same picture too,
// and is passed over the same way.) Said when repeats come in among new
// frames - a still picture is no news - once, and again after a long rest.
static void SourceSeen(bool repeated)
{
    static double since = -1e9, said = -1e9;
    static unsigned repeats = 0, fresh = 0;
    const double now = LatencyNow();
    if (now - since > 2.0) { since = now; repeats = fresh = 0; }
    if (!repeated) { ++fresh; return; }
    ++repeats;
    FgGateRepeat(now);
    if (repeats >= 16 && fresh >= 16 && now - said > 60.0)
    {
        said = now;
        Trace("[source] the desktop is handed over again each time LIVE FLOW shows a frame (the compositor draws the "
              "output) - those repeats are passed over; only the game's own frames count");
    }
}

// ---- 2. The network's motion, found on the GPU -------------------------------

static struct NetMotionState
{
    ID3D12DescriptorHeap *heap = nullptr;   // t0 prev, t1 cur, t2/t3 no hint, t4 pyramid; u0 pyramid, u1 fields, u2/u3 result
    ID3D12Resource *prev = nullptr;         // the picture before, NPSR at rest
    ID3D12Resource *pyramid = nullptr, *fields = nullptr;     // UNORDERED_ACCESS at rest
    ID3D12Resource *mv = nullptr, *reverse = nullptr;         // NPSR at rest
    ID3D12Resource *zero = nullptr;         // no motion, for the first frame and a cut
    ID3D12Resource *bound_cur = nullptr;
    UINT pw = 0, ph = 0, mw = 0, mh = 0, ew = 0, eh = 0, levels = 0, ratio = 1;
    bool have_prev = false, failed = false, told = false, flow_valid = false;
    double flow_stamp = -1.0;
    LARGE_INTEGER last = {};
} g_nm;

static ID3D12Resource *NetMotionTexture(UINT w, UINT hgt, DXGI_FORMAT format, bool uav, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES def = {};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = hgt; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = format; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource *r = nullptr;
    if (FAILED(lfCore.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td, state, nullptr,
                                                      __uuidof(ID3D12Resource), reinterpret_cast<void **>(&r))))
        return nullptr;
    return r;
}

static void NetMotionClose()
{
    for (ID3D12Resource **r : { &g_nm.prev, &g_nm.pyramid, &g_nm.fields, &g_nm.mv, &g_nm.reverse, &g_nm.zero })
        if (*r) { (*r)->Release(); *r = nullptr; }
    g_nm.bound_cur = nullptr;
    g_nm.pw = g_nm.ph = g_nm.mw = g_nm.mh = 0;
    g_nm.have_prev = false;
    g_nm.flow_valid = false;
}

// Preserve both directions for MFG. The NR guide is a separate copy which
// MotionSettle may zero; the reverse field must not be silently discarded
// when the presenter's additional refinement yields to GPU load.
static bool NetMotionForFlow(ID3D12Resource *&backward, ID3D12Resource *&forward)
{
    if (!g_nm.flow_valid || !g_nm.mv || !g_nm.reverse ||
        !(lfCap.live || lfCap.window_live) || g_nm.flow_stamp != lfCap.stamp) return false;
    backward = g_nm.mv; forward = g_nm.reverse;
    return true;
}

// Its textures for this picture and motion size; sizes as FLOW MFG picks them
// (level 0 no wider than 800, no taller than 450).
static bool NetMotionReady(UINT pw, UINT ph, UINT mw, UINT mh)
{
    if (g_nm.failed) return false;
    if (g_nm.prev && g_nm.pw == pw && g_nm.ph == ph && g_nm.mw == mw && g_nm.mh == mh) return true;
    if (!EnsureFlowEstimate()) { g_nm.failed = true; Trace("[motion] no GPU estimate here - motion stays still"); return false; }
    NetMotionClose();
    if (g_nm.heap == nullptr)
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 9;
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(lfCore.device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void **>(&g_nm.heap))))
        { g_nm.failed = true; return false; }
    }
    UINT ratio = 1;
    while ((mw + ratio - 1) / ratio > 800 || (mh + ratio - 1) / ratio > 450) ratio *= 2;
    const UINT ew = (mw + ratio - 1) / ratio, eh = (mh + ratio - 1) / ratio;
    UINT levels = 0;
    auto side = [](UINT n, UINT l) { return (n + (1u << l) - 1u) >> l; };
    while (side(ew, levels + 1) >= 16 && side(eh, levels + 1) >= 16) ++levels;
    const UINT aw = ew + (ew + 1) / 2, band = eh + 16;
    g_nm.prev = NetMotionTexture(pw, ph, DXGI_FORMAT_R8G8B8A8_UNORM, false, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_nm.pyramid = NetMotionTexture(aw, 2 * band, DXGI_FORMAT_R32_UINT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_nm.fields = NetMotionTexture(aw, 4 * band, DXGI_FORMAT_R32_UINT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_nm.mv = NetMotionTexture(mw, mh, DXGI_FORMAT_R16G16_FLOAT, true, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_nm.reverse = NetMotionTexture(mw, mh, DXGI_FORMAT_R16G16_FLOAT, true, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    g_nm.zero = NetMotionTexture(mw, mh, DXGI_FORMAT_R16G16_FLOAT, false, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (!g_nm.prev || !g_nm.pyramid || !g_nm.fields || !g_nm.mv || !g_nm.reverse || !g_nm.zero)
    {
        Trace("[motion] the GPU estimate's textures failed at %ux%u", mw, mh);
        NetMotionClose();
        g_nm.failed = true;
        return false;
    }
    g_nm.pw = pw; g_nm.ph = ph; g_nm.mw = mw; g_nm.mh = mh;
    g_nm.ew = ew; g_nm.eh = eh; g_nm.levels = levels; g_nm.ratio = ratio;
    return true;
}

static void NetMotionBind(ID3D12Resource *cur)
{
    if (g_nm.bound_cur == cur) return;
    const UINT stride = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_nm.heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    lfCore.device->CreateShaderResourceView(g_nm.prev, &sd, cpu); cpu.ptr += stride;
    lfCore.device->CreateShaderResourceView(cur, &sd, cpu); cpu.ptr += stride;
    sd.Format = DXGI_FORMAT_R16G16_FLOAT;            // no outside estimate (gFlags 0): null views
    lfCore.device->CreateShaderResourceView(nullptr, &sd, cpu); cpu.ptr += stride;
    lfCore.device->CreateShaderResourceView(nullptr, &sd, cpu); cpu.ptr += stride;
    sd.Format = DXGI_FORMAT_R32_UINT;
    lfCore.device->CreateShaderResourceView(g_nm.pyramid, &sd, cpu); cpu.ptr += stride;
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    ud.Format = DXGI_FORMAT_R32_UINT;
    lfCore.device->CreateUnorderedAccessView(g_nm.pyramid, nullptr, &ud, cpu); cpu.ptr += stride;
    lfCore.device->CreateUnorderedAccessView(g_nm.fields, nullptr, &ud, cpu); cpu.ptr += stride;
    ud.Format = DXGI_FORMAT_R16G16_FLOAT;
    lfCore.device->CreateUnorderedAccessView(g_nm.mv, nullptr, &ud, cpu); cpu.ptr += stride;
    lfCore.device->CreateUnorderedAccessView(g_nm.reverse, nullptr, &ud, cpu);
    g_nm.bound_cur = cur;
}

// The estimate, recorded into the open list: prev and cur are shader
// resources; afterwards g_nm.mv is (current -> previous, motion texels).
static void NetMotionRecord(ID3D12GraphicsCommandList *list, double interval)
{
    ID3D12DescriptorHeap *heaps[] = { g_nm.heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_flow.est_rs);
    list->SetComputeRootDescriptorTable(1, g_nm.heap->GetGPUDescriptorHandleForHeapStart());
    const UINT ew = g_nm.ew, eh = g_nm.eh, levels = g_nm.levels;
    UINT32 k[14] = { g_nm.pw, g_nm.ph, ew, eh, 0, levels, 0u, EstimateRadius(interval),
                     0, 0, 0, g_nm.mw, g_nm.mh, g_nm.ratio };
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
        Barrier(g_nm.mv, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(g_nm.reverse, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    list->ResourceBarrier(_countof(open), open);
    pass(0, 0, 0, 0, 0, ew, eh, g_nm.pyramid);
    for (UINT l = 1; l <= levels; ++l) pass(1, l, 0, 0, 0, side(ew, l), side(eh, l), g_nm.pyramid);
    D3D12_RESOURCE_BARRIER to_read = Barrier(g_nm.pyramid, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    list->ResourceBarrier(1, &to_read);
    UINT set = 0;
    auto spreads = [&](UINT l) {
        for (UINT step : { 4u, 2u, 1u })
        {
            if (l == 0 && step != 1) continue;
            pass(4, l, set, 1 - set, step, side(ew, l), side(eh, l), g_nm.fields);
            set = 1 - set;
        }
    };
    pass(2, levels, 0, 0, 0, side(ew, levels), side(eh, levels), g_nm.fields);
    spreads(levels);
    for (int l = int(levels) - 1; l >= 0; --l)
    {
        pass(3, UINT(l), set, 1 - set, 0, side(ew, UINT(l)), side(eh, UINT(l)), g_nm.fields);
        set = 1 - set;
        spreads(UINT(l));
    }
    pass(5, 0, set, 1 - set, 0, ew, eh, g_nm.fields);
    set = 1 - set;
    pass(6, 0, set, set, 0, ew, eh, g_nm.mv);
    D3D12_RESOURCE_BARRIER close[] = {
        Barrier(g_nm.pyramid, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(g_nm.mv, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(g_nm.reverse, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    list->ResourceBarrier(_countof(close), close);
}

// The network's motion for this frame, in place of the engine's field:
// estimated between the picture before and this one, or none on the first
// frame and across a cut. Same contract as FeedMotion (sent_mark: submit
// and hand back the fence instead of waiting).
static bool NetMotionRun(Pipeline &pipe, bool reset, UINT64 *sent_mark)
{
    g_nm.flow_valid = false;
    g_nm.flow_stamp = lfCap.stamp;
    if (sent_mark) *sent_mark = 0;
    const bool flow_only = FlowActive() && lfOut.sent_passthrough && g_flow.est_on;
    // FLOW estimates the exact pair again on its own queue. NR is off: keep
    // a zero external hint and avoid a second pyramid/search for every frame.
    // Invalidate NR history so re-enabling NR starts from the current image.
    ID3D12Resource *cur = pipe.picture.gpu_tex;
    const D3D12_RESOURCE_DESC pd = cur->GetDesc();
    const UINT pw = static_cast<UINT>(pd.Width), ph = pd.Height;
    const bool ready = NetMotionReady(pw, ph, pipe.work_w, pipe.work_h);
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const double interval = g_nm.last.QuadPart ? double(now.QuadPart - g_nm.last.QuadPart) / double(freq.QuadPart) : 1.0 / 60.0;
    g_nm.last = now;
    if (!CmdOpen()) return false;
    GpuStageOpen(GS_MOVE);
    const bool estimate = ready && g_nm.have_prev && !reset && !flow_only;
    if (estimate)
    {
        NetMotionBind(cur);
        NetMotionRecord(lfCore.cmd, interval);
    }
    ID3D12Resource *from = estimate ? g_nm.mv : (ready ? g_nm.zero : nullptr);
    if (from != nullptr)
    {
        const D3D12_RESOURCE_STATES from_rest = estimate ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                                         : D3D12_RESOURCE_STATE_COPY_SOURCE;
        // The motion texture rests as a shader resource once it was fed;
        // before that it is still in its creation state, COPY_DEST.
        const bool from_moves = from_rest != D3D12_RESOURCE_STATE_COPY_SOURCE;
        D3D12_RESOURCE_BARRIER in[2];
        UINT count = 0;
        if (pipe.fed)
            in[count++] = Barrier(pipe.motion.gpu_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                  D3D12_RESOURCE_STATE_COPY_DEST);
        if (from_moves) in[count++] = Barrier(from, from_rest, D3D12_RESOURCE_STATE_COPY_SOURCE);
        if (count) lfCore.cmd->ResourceBarrier(count, in);
        lfCore.cmd->CopyResource(pipe.motion.gpu_tex, from);
        D3D12_RESOURCE_BARRIER out[] = {
            Barrier(pipe.motion.gpu_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Barrier(from, D3D12_RESOURCE_STATE_COPY_SOURCE, from_rest),
        };
        lfCore.cmd->ResourceBarrier(from_moves ? 2 : 1, out);
        // NR does not need colour history while FLOW owns the estimate.
        if (!flow_only) {
        // This picture is the next frame's "before".
        D3D12_RESOURCE_BARRIER keep_in[] = {
            Barrier(cur, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Barrier(g_nm.prev, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        lfCore.cmd->ResourceBarrier(_countof(keep_in), keep_in);
        lfCore.cmd->CopyResource(g_nm.prev, cur);
        D3D12_RESOURCE_BARRIER keep_out[] = {
            Barrier(cur, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Barrier(g_nm.prev, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        lfCore.cmd->ResourceBarrier(_countof(keep_out), keep_out);
        }
        g_nm.have_prev = !flow_only;
    }
    GpuStageClose(GS_MOVE);
    const UINT64 fence = CmdSubmit();
    if (fence == 0) return false;
    if (from != nullptr) pipe.fed = true;
    if (!g_nm.told && ready)
    {
        g_nm.told = true;
        Trace("[motion] the worker finds the network's motion itself: GPU estimate %ux%u (level 0 %ux%u, %u levels)",
            g_nm.mw, g_nm.mh, g_nm.ew, g_nm.eh, g_nm.levels);
    }
    if (sent_mark) { g_nm.flow_valid = estimate; *sent_mark = fence; return true; }
    const bool complete = GpuStageWait(GS_MOVE, fence, 30000);
    g_nm.flow_valid = complete && estimate;
    return complete;
}

// ---- 3. The still check --------------------------------------------------------

static const char kSettleHlsl[] = R"hlsl(
Texture2D<float4>   gPic   : register(t0);   // this frame's picture
Texture2D<float>    gPrevL : register(t1);   // last frame's luma, motion size
RWTexture2D<float2> gMv    : register(u0);   // the motion (current -> previous, motion texels), settled in place
RWTexture2D<float>  gCurL  : register(u1);   // this frame's luma, motion size
SamplerState        gLin   : register(s0);
cbuffer SC : register(b0) { uint gMW; uint gMH; uint gHavePrev; float gFloor; };
static const float3 kLuma = float3(0.299f, 0.587f, 0.114f);

[numthreads(8, 8, 1)]
void LumaMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gMW || id.y >= gMH) return;
    float2 uv = (float2(id.xy) + 0.5f) / float2(gMW, gMH);
    gCurL[id.xy] = dot(gPic.SampleLevel(gLin, uv, 0).rgb, kLuma);
}

[numthreads(8, 8, 1)]
void SettleMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gMW || id.y >= gMH) return;
    int2 p = int2(id.xy);
    float2 mv = gMv[p];
    if (!all(isfinite(mv)) || length(mv) < gFloor) { gMv[p] = float2(0.0f, 0.0f); return; }
    if (gHavePrev == 0u) return;
    // Does this motion explain the change better than standing still? Over a
    // 9x9 texel neighbourhood (every other texel), the difference to the
    // previous frame moved by it against the difference to it unmoved.
    int2 last = int2(int(gMW) - 1, int(gMH) - 1);
    float2 size = float2(gMW, gMH);
    float missMoved = 0.0f, missStill = 0.0f;
    [unroll] for (int y = -4; y <= 4; y += 2)
    [unroll] for (int x = -4; x <= 4; x += 2)
    {
        int2 q = clamp(p + int2(x, y), int2(0, 0), last);
        float c = gCurL[q];
        missStill += abs(c - gPrevL.Load(int3(q, 0)));
        missMoved += abs(c - gPrevL.SampleLevel(gLin, (float2(q) + 0.5f + mv) / size, 0));
    }
    // Half a level (of 255) per sample, as the CPU check had it.
    if (missMoved >= missStill - 25.0f * (0.5f / 255.0f)) gMv[p] = float2(0.0f, 0.0f);
}
)hlsl";

static struct SettleState
{
    ID3D12RootSignature *rs = nullptr;
    ID3D12PipelineState *luma_pso = nullptr, *settle_pso = nullptr;
    ID3D12DescriptorHeap *heap = nullptr;          // two sets: t0 picture, t1 prev luma, u0 motion, u1 cur luma
    ID3D12Resource *luma[2] = {};                  // [cur] UNORDERED_ACCESS, [1 - cur] NPSR at rest
    ID3D12Resource *bound[2][2] = {};              // picture and motion each set was written for
    UINT mw = 0, mh = 0, cur = 0;
    bool have_prev = false, failed = false, told = false;
} g_settle;

static bool SettleReady(UINT mw, UINT mh)
{
    if (g_settle.failed) return false;
    if (g_settle.settle_pso == nullptr)
    {
        g_settle.failed = true;
        HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
        auto compile = compiler ? static_cast<CompileFn>(&CachedCompile) : nullptr;
        HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
        auto serialize = d3d12 ? reinterpret_cast<SerializeRootFn>(
                                     GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
        if (compile == nullptr || serialize == nullptr) return false;
        ID3DBlob *codes[2] = {};
        const char *entries[2] = { "LumaMain", "SettleMain" };
        for (int i = 0; i < 2; ++i)
        {
            ID3DBlob *errors = nullptr;
            const HRESULT hr = compile(kSettleHlsl, sizeof(kSettleHlsl) - 1, "settle.hlsl", nullptr, nullptr,
                                       entries[i], "cs_5_0", 0, 0, &codes[i], &errors);
            if (FAILED(hr) || codes[i] == nullptr)
            {
                Trace("[motion] still check %s failed 0x%08X: %s", entries[i], hr,
                    errors ? static_cast<const char *>(errors->GetBufferPointer()) : "(no log)");
                if (errors) errors->Release();
                for (auto *c : codes) if (c) c->Release();
                return false;
            }
            if (errors) errors->Release();
        }
        D3D12_DESCRIPTOR_RANGE ranges[2] = {};
        ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        ranges[0].NumDescriptors = 2;
        ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        ranges[1].NumDescriptors = 2;
        ranges[1].OffsetInDescriptorsFromTableStart = 2;
        D3D12_ROOT_PARAMETER params[2] = {};
        params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        params[0].Constants.Num32BitValues = 4;
        params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[1].DescriptorTable.NumDescriptorRanges = 2;
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
        ID3DBlob *blob = nullptr, *errors = nullptr;
        HRESULT hr = serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
        if (errors) errors->Release();
        if (SUCCEEDED(hr) && blob)
        {
            hr = lfCore.device->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(),
                                                    __uuidof(ID3D12RootSignature), reinterpret_cast<void **>(&g_settle.rs));
            blob->Release();
        }
        ID3D12PipelineState **psos[2] = { &g_settle.luma_pso, &g_settle.settle_pso };
        for (int i = 0; i < 2 && SUCCEEDED(hr); ++i)
        {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
            pd.pRootSignature = g_settle.rs;
            pd.CS.pShaderBytecode = codes[i]->GetBufferPointer();
            pd.CS.BytecodeLength = codes[i]->GetBufferSize();
            hr = lfCore.device->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void **>(psos[i]));
        }
        for (auto *c : codes) c->Release();
        if (SUCCEEDED(hr))
        {
            D3D12_DESCRIPTOR_HEAP_DESC hd = {};
            hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
            hd.NumDescriptors = 8;
            hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            hr = lfCore.device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void **>(&g_settle.heap));
        }
        if (FAILED(hr)) { Trace("[motion] still check pipeline failed 0x%08X", hr); return false; }
        g_settle.failed = false;
    }
    if (g_settle.luma[0] && g_settle.mw == mw && g_settle.mh == mh) return true;
    for (auto &l : g_settle.luma) if (l) { l->Release(); l = nullptr; }
    g_settle.luma[0] = NetMotionTexture(mw, mh, DXGI_FORMAT_R32_FLOAT, true, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    g_settle.luma[1] = NetMotionTexture(mw, mh, DXGI_FORMAT_R32_FLOAT, true, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (!g_settle.luma[0] || !g_settle.luma[1]) { g_settle.failed = true; return false; }
    g_settle.mw = mw; g_settle.mh = mh; g_settle.cur = 0; g_settle.have_prev = false;
    memset(g_settle.bound, 0, sizeof(g_settle.bound));
    return true;
}

// Settle the network's motion (pipe.motion, a shader resource on entry and
// exit) against the picture: recorded and sent on its own list, after the
// motion and before the network, on the same queue.
static void MotionSettle(Pipeline &pipe, bool reset)
{
    const UINT mw = pipe.work_w, mh = pipe.work_h;
    if (!SettleReady(mw, mh)) return;
    if (reset) g_settle.have_prev = false;
    const UINT set = g_settle.cur;
    ID3D12Resource *picture = pipe.picture.gpu_tex, *motion = pipe.motion.gpu_tex;
    const UINT stride = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    if (g_settle.bound[set][0] != picture || g_settle.bound[set][1] != motion)
    {
        D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_settle.heap->GetCPUDescriptorHandleForHeapStart();
        cpu.ptr += static_cast<SIZE_T>(set) * 4 * stride;
        D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
        sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        sd.Texture2D.MipLevels = 1;
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        lfCore.device->CreateShaderResourceView(picture, &sd, cpu); cpu.ptr += stride;
        sd.Format = DXGI_FORMAT_R32_FLOAT;
        lfCore.device->CreateShaderResourceView(g_settle.luma[1 - set], &sd, cpu); cpu.ptr += stride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        ud.Format = DXGI_FORMAT_R16G16_FLOAT;
        lfCore.device->CreateUnorderedAccessView(motion, nullptr, &ud, cpu); cpu.ptr += stride;
        ud.Format = DXGI_FORMAT_R32_FLOAT;
        lfCore.device->CreateUnorderedAccessView(g_settle.luma[set], nullptr, &ud, cpu);
        g_settle.bound[set][0] = picture;
        g_settle.bound[set][1] = motion;
    }
    if (!CmdOpen()) return;
    ID3D12GraphicsCommandList *list = lfCore.cmd;
    D3D12_RESOURCE_BARRIER in = Barrier(motion, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->ResourceBarrier(1, &in);
    ID3D12DescriptorHeap *heaps[] = { g_settle.heap };
    list->SetDescriptorHeaps(1, heaps);
    list->SetComputeRootSignature(g_settle.rs);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_settle.heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(set) * 4 * stride;
    list->SetComputeRootDescriptorTable(1, gpu);
    const float floor_px = 0.5f;
    UINT32 c[4] = { mw, mh, g_settle.have_prev ? 1u : 0u, 0 };
    memcpy(&c[3], &floor_px, sizeof(float));
    list->SetComputeRoot32BitConstants(0, 4, c, 0);
    list->SetPipelineState(g_settle.luma_pso);
    list->Dispatch((mw + 7) / 8, (mh + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER luma_done = {};
    luma_done.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    luma_done.UAV.pResource = g_settle.luma[set];
    list->ResourceBarrier(1, &luma_done);
    list->SetPipelineState(g_settle.settle_pso);
    list->Dispatch((mw + 7) / 8, (mh + 7) / 8, 1);
    // This frame's luma is the next frame's "before"; the other becomes the
    // one written next time.
    D3D12_RESOURCE_BARRIER out[] = {
        Barrier(motion, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(g_settle.luma[set], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(g_settle.luma[1 - set], D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    list->ResourceBarrier(_countof(out), out);
    if (CmdSubmit() == 0) return;
    g_settle.cur = 1 - set;
    g_settle.have_prev = true;
    if (!g_settle.told)
    {
        g_settle.told = true;
        Trace("[motion] still places settle to no motion (under %.1f px, or where the motion does not explain the change)",
            floor_px);
    }
}
