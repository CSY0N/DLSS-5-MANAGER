// Restored desktop DLSS FG from the project's original frame_generation.inl.
// Native runtime only: FLOW MFG is a separate, mutually exclusive backend.
static void LiveFlowCachePath(wchar_t (&path)[MAX_PATH])
{
    const DWORD n = GetEnvironmentVariableW(L"DLSS_MANAGER_SCREEN_DATA", path, MAX_PATH);
    if (n && n < MAX_PATH && GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return;
    const DWORD size = GetTempPathW(MAX_PATH, path);
    if (size && size < MAX_PATH - 24) {
        wcscat_s(path, L"DLSS5ManagerLiveFlow");
        if (CreateDirectoryW(path, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS) return;
    }
    GetCurrentDirectoryW(MAX_PATH, path);
}
#ifdef LF_RADEON
static bool FgRequested() { return false; }
static bool FgGateTakes(double, unsigned, double, bool) { return true; }
static void FgGateForget() {}
static void FgGateRepeat(double) {}
static void FgGateFresh(double) {}
static void FgQueueFromResize(uint32_t) {}
static bool FgKeepMotion(Pipeline &) { return true; }
static unsigned FgExtraFrames() { return 0; }
static void CloseFgResources() {}
static void FgStopIfFailed() {}
static void ShutdownFg() {}
static void ConfigureFgFrame(uint32_t) {}
static bool FgPresent(Pipeline &, ID3D12Resource *, D3D12_RESOURCE_STATES) { return false; }
static UINT64 g_fg_present_fence = 0;
#else
#include "lf_dlssg_compat.inl"
#include "lf_dlssg_ada.h"
#include "lf_fg_scale.inl"
#include "lf_fg_pace.inl"
static HANDLE g_fg_waitable = nullptr;
static UINT64 g_fg_present_fence = 0;
// Experimental desktop DLSS-G. Capture has no engine depth: use a flat plane
// and the estimated motion field. Keep this opt-in; UI/occlusions can distort.
static NrEvaluateFn g_fg_evaluate = nullptr;
static NVSDK_NGX_Result FgEvaluateBridge(ID3D12GraphicsCommandList *list,
    NVSDK_NGX_Handle *handle, NVSDK_NGX_Parameter *params, PFN_NVSDK_NGX_ProgressCallback cb)
{
    __try { return g_fg_evaluate(list, handle, params, cb); }
    __except (EXCEPTION_EXECUTE_HANDLER) { Trace("[dlssg] unavailable: evaluate exception 0x%08X", GetExceptionCode()); return NVSDK_NGX_Result_Fail; }
}
#define NVSDK_NGX_D3D12_EvaluateFeature_C FgEvaluateBridge
#include "include/nvsdk_ngx_helpers_dlssg.h"
#undef NVSDK_NGX_D3D12_EvaluateFeature_C

struct FgSlot {
    winrt::com_ptr<ID3D12Resource> real, interpolated[5];
    unsigned count = 1;
    int state = 0; // free, writing, ready, presenting; protected by mutex
    UINT64 sequence = 0;
    bool interpolate = false;
    double interval = 0.016;
    double source_time = 0;
    // The moment of the game this real frame stands for and the one before
    // it (QPC seconds; lf_fg_pace.inl), and when the pair was handed over.
    double moment = 0, prev_moment = 0;
    double ready_at = 0;
    // Where nothing moved near (lf_fg_still.inl): the slot's own tile map, and
    // whether any tile of it is at rest.
    winrt::com_ptr<ID3D12Resource> still;
    bool protect = false;
};
static struct FgState {
    HMODULE module = nullptr;
    bool initialized = false;
    NrCreateFn create = nullptr;
    NrReleaseFn release = nullptr;
    NVSDK_NGX_Parameter *params = nullptr;
    NVSDK_NGX_Handle *feature = nullptr;
    StagedTexture depth;
    winrt::com_ptr<ID3D12Resource> motion;
    winrt::com_ptr<ID3D12Resource> scaled_color;
    winrt::com_ptr<ID3D12Resource> output[5];
    winrt::com_ptr<ID3D12Resource> disable, disable_readback;
    UINT w = 0, height = 0, mw = 0, mh = 0;
    UINT full_w = 0, full_h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    FgSlot slots[4];
    std::mutex mutex;
    std::condition_variable wake;
    std::thread thread;
    std::atomic<bool> stop{false}, failed{false};
    UINT64 sequence = 0;
    std::chrono::steady_clock::time_point last;
    bool history = false;
    double source_time = 0, extra_budget = 0;
    std::atomic<unsigned> captured{0};
    ~FgState() { stop = true; wake.notify_all(); if (thread.joinable()) thread.join(); }
} g_fg;
#include "lf_fg_still.inl"

// Never free textures still referenced by submitted GPU work on a timeout.
static bool FgAwait(ID3D12Fence *fence, UINT64 value, DWORD timeout)
{
    if (AwaitMark(fence, value, timeout)) return true;
    if (lfCore.device && SUCCEEDED(lfCore.device->GetDeviceRemovedReason())) {
        Trace("[dlssg] unavailable: GPU work timed out; stopping this worker safely");
        TerminateProcess(GetCurrentProcess(), ERROR_TIMEOUT);
    }
    return false;
}

static int g_fg_ui_enabled = -1;
static unsigned g_fg_count = 1, g_fg_requested_count = 1, g_fg_runtime_max = 5;
static unsigned FgExtraFrames() { return g_fg_count; }

static bool FgRequested()
{
    static const bool requested = [] {
        char value[8] = {};
        return GetEnvironmentVariableA("LF_FRAMEGEN", value, sizeof(value)) == 1 && value[0] == '1';
    }();
    return (g_fg_ui_enabled < 0 ? requested : g_fg_ui_enabled != 0) && !g_fg.failed;
}

static void StopFgPresentation()
{
    g_fg.stop = true;
    g_fg.wake.notify_all();
    if (g_fg.thread.joinable()) g_fg.thread.join();
    // Hand the swapchain back to the ordinary present path: default latency,
    // the waitable handle dies with the swapchain, not with us.
    if (lfShow.chain != nullptr)
    {
        IDXGISwapChain2 *sc2 = nullptr;
        if (SUCCEEDED(lfShow.chain->QueryInterface(
                __uuidof(IDXGISwapChain2), reinterpret_cast<void **>(&sc2)))
            && sc2 != nullptr)
        {
            sc2->SetMaximumFrameLatency(LowLatencyWanted() ? 1 : 3);
            sc2->Release();
        }
    }
    if (g_fg_waitable && g_fg_waitable != lfShow.quick_waitable) CloseHandle(g_fg_waitable);
    g_fg_waitable = nullptr;
    for (auto &slot : g_fg.slots)
    {
        slot.real = nullptr; slot.still = nullptr; slot.protect = false;
        for (auto &image : slot.interpolated) image = nullptr;
        slot.state = 0;
    }
    FgStillClose();
    g_fg.history = false;
    g_fg.source_time = g_fg.extra_budget = 0;
}

static void CloseFgResources()
{
    StopFgPresentation();
    if (g_fg.feature) { g_fg.release(g_fg.feature); g_fg.feature = nullptr; }
    if (g_fg.depth.gpu_tex) { g_fg.depth.gpu_tex->Release(); g_fg.depth.gpu_tex = nullptr; }
    if (g_fg.depth.staging) { g_fg.depth.staging->Release(); g_fg.depth.staging = nullptr; }
    for (auto &image : g_fg.output) image = nullptr;
    g_fg.scaled_color = nullptr;
    g_fg_scale.display = nullptr;
    g_fg.disable = nullptr; g_fg.disable_readback = nullptr;
    g_fg.w = g_fg.height = 0;
}

static void ShutdownFg()
{
    CloseFgResources();
    if (g_fg.params) { NVSDK_NGX_D3D12_DestroyParameters(g_fg.params); g_fg.params = nullptr; }
    g_fg.initialized = false;
    g_fg.motion = nullptr;
}

// Preserve dense guides for DLSS-G before NR's still-pixel suppression. The
// latter intentionally zeros vectors in ambiguous regions and is not a dense
// game motion field. This copy does not invoke the FLOW MFG interpolator.
static bool FgKeepMotion(Pipeline &v)
{
    if (!g_fg.motion || g_fg.motion->GetDesc().Width != v.work_w || g_fg.motion->GetDesc().Height != v.work_h) {
        if (!FgAwait(lfCore.timeline, lfCore.timeline_value, 30000)) return false;
        g_fg.motion.attach(NewTexture(v.work_w, v.work_h, DXGI_FORMAT_R16G16_FLOAT, false));
        if (!g_fg.motion || !CmdOpen()) return false;
        auto b = Barrier(g_fg.motion.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        lfCore.cmd->ResourceBarrier(1, &b);
    } else if (!CmdOpen()) return false;
    D3D12_RESOURCE_BARRIER pre[] = {
        Barrier(v.motion.gpu_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Barrier(g_fg.motion.get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
    lfCore.cmd->ResourceBarrier(2, pre);
    lfCore.cmd->CopyResource(g_fg.motion.get(), v.motion.gpu_tex);
    D3D12_RESOURCE_BARRIER post[] = {
        Barrier(v.motion.gpu_tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(g_fg.motion.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    lfCore.cmd->ResourceBarrier(2, post);
    return CmdSubmit() != 0;
}

static void FgStopIfFailed()
{
    // Join a failed presenter before the ordinary path owns the swap chain.
    if (g_fg.failed && (g_fg.feature || g_fg.thread.joinable())) CloseFgResources();
}

// The presenter has its own command list/allocator/fence. The producer never
// waits for frame pacing. Slots being displayed cannot be recycled by it.
static void FgPresenter()
{
    winrt::com_ptr<ID3D12CommandAllocator> alloc;
    winrt::com_ptr<ID3D12GraphicsCommandList> list;
    winrt::com_ptr<ID3D12Fence> fence;
    if (FAILED(lfCore.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(alloc.put()))) ||
        FAILED(lfCore.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.get(), nullptr, IID_PPV_ARGS(list.put()))) ||
        FAILED(lfCore.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(fence.put()))))
    { Trace("[dlssg] unavailable: presenter allocation failed"); g_fg.failed = true; return; }
    list->Close();
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) { Trace("[dlssg] unavailable: presenter event allocation failed"); g_fg.failed = true; return; }
    UINT64 value = 0, shown = 0, generated = 0;
    // LF_FG_TRACE=<file>: one line per Present - when, which pair, the moment
    // of the game the picture stands for - for tests/screen_fg_pacing.py.
    FILE *trace = nullptr;
    {
        char trace_path[MAX_PATH] = {};
        const DWORD len = GetEnvironmentVariableA("LF_FG_TRACE", trace_path, sizeof(trace_path));
        if (len > 0 && len < sizeof(trace_path)) fopen_s(&trace, trace_path, "w");
        if (trace) fputs("t,generated,content,sequence,ready,source\n", trace);
    }
    double trace_content = 0.0, trace_ready = 0.0;
    UINT64 trace_sequence = 0;
    FgPace pace;
    unsigned reports = 0;
    FgSlot *showing = nullptr;       // the pair on its way to the screen
    unsigned still_frames = 0;       // frames in between that took still parts from their real frame
    auto report =std::chrono::steady_clock::now();
    auto reportCadence = [&] {
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - report).count();
        if (elapsed < 1.0) return;
        const unsigned captured = g_fg.captured.exchange(0);
        Trace("[dlssg] displayed %.1f FPS (%llu real + %llu generated in %.2fs); experimental flat-depth guides",
              shown / elapsed, shown - generated, generated, elapsed);
        Trace("[dlssg] cadence: source=%u real=%llu generated=%llu seconds=%.4f multiplier=%u",
              captured, shown - generated, generated, elapsed, g_fg_count + 1);
        shown = generated = 0; report = now;
        if (++reports % 5 == 0 && pace.pairs)
        {
            Trace("[dlssg] pacing: every frame %.1f ms after its moment (Queue target %u), %u of %u pairs came late, "
                  "%u frames in between passed over, %u pairs dropped; drawing takes %.2f ms (real) and %.2f ms "
                  "(in between), the Present %.2f ms after %.2f ms waiting for the screen",
                  pace.delay * 1000.0, FgQueueTarget(), pace.late, pace.pairs, pace.skipped, pace.dropped,
                  pace.lead[0] * 1000.0, pace.lead[1] * 1000.0, pace.call * 1000.0, pace.waited * 1000.0);
            if (still_frames) Trace("[dlssg] still pixels: %u frames in between kept their still parts from the real frame", still_frames);
            pace.pairs = pace.late = pace.skipped = pace.dropped = 0;
            still_frames = 0;
        }
    };
    // Until a moment (QPC seconds): the timer for most of it, the last half
    // millisecond in turns. False when the presenter is being stopped.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
    auto waitUntil = [&](double when) {
        for (;;)
        {
            if (g_fg.stop) return false;
            const double left = when - LatencyNow();
            if (left <= 0.0) return true;
            if (left > 0.0008)
            {
                LARGE_INTEGER due;
                due.QuadPart = -static_cast<LONGLONG>((std::min(left, 0.05) - 0.0005) * 1e7);
                if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 60);
                else Sleep(left > 0.003 ? 1 : 0);
            }
            else SwitchToThread();
        }
    };
    // Exactly one consumer and one wait per Present, including the first.
    // An extra startup wait consumes a permit without submitting a frame.
    // Bound waits so a hidden/stalled output cannot freeze capture or shutdown.
    auto waitForPresent = [&] {
        if (!g_fg_waitable) return;
        const DWORD most = static_cast<DWORD>(std::clamp(1500.0 / std::max(OutputRefresh(), 24.0), 4.0, 63.0));
        if (WaitForSingleObject(g_fg_waitable, most) != WAIT_OBJECT_0)
            Trace("[dlssg] waitable timeout after %lu ms; attempting present", most);
    };
    // A frame goes to the screen in two steps (round 28): drawn into the back
    // buffer as soon as the one before it has been presented, and presented
    // when its time comes. The copy waits for the card - under a game that
    // fills it, 1 to 5 ms, never the same twice - and done at the last moment
    // that wait was each frame's own lateness.
    auto draw = [&](ID3D12Resource *source, bool synthetic) {
        winrt::com_ptr<ID3D12Resource> bb;
        if (FAILED(lfShow.chain->GetBuffer(lfShow.chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(bb.put()))) ||
            FAILED(alloc->Reset()) || FAILED(list->Reset(alloc.get(), nullptr))) return false;
        auto pre = Barrier(bb.get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1, &pre);
        // A frame in between with still parts takes those from its real frame
        // (lf_fg_still.inl); the same target as the scaled frames', or the
        // one made for frames at the screen's size.
        // Both end in ShowBlit (round 31): a copy when the back buffer is the
        // picture's size, scaled when it is the whole screen under a window
        // of its own size (flow_upscaler.inl) - a plain copy between two
        // sizes is not a thing the card does.
        ID3D12Resource *still_target = g_fg_scale.display ? g_fg_scale.display.get() : g_fg_still.display.get();
        if (synthetic && showing != nullptr && showing->protect && showing->still && g_fg_still.ready && still_target != nullptr &&
            showing->real && still_target->GetDesc().Width == showing->real->GetDesc().Width &&
            still_target->GetDesc().Height == showing->real->GetDesc().Height) {
            FgStillShow(list.get(), source, showing->real.get(), showing->still.get(), still_target);
            auto ready = Barrier(still_target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &ready);
            ShowBlit(list.get(), bb.get(), still_target);
            auto rest = Barrier(still_target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1, &rest);
            ++still_frames;
        } else if (synthetic && g_fg_scale.display) {
            auto target = g_fg_scale.display.get();
            FgScaleRecord(list.get(), source, D3D12_RESOURCE_STATE_COPY_SOURCE, target, true);
            auto ready = Barrier(target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &ready);
            ShowBlit(list.get(), bb.get(), target);
            auto rest = Barrier(target, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->ResourceBarrier(1, &rest);
        } else ShowBlit(list.get(), bb.get(), source);
        D3D12_RESOURCE_STATES bb_state = D3D12_RESOURCE_STATE_COPY_DEST;
        if (ShareActive()) {
            auto to_source = Barrier(bb.get(), bb_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
            list->ResourceBarrier(1, &to_source);
            bb_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
            auto exported = HdrExportFrame(list.get(), bb.get(), bb_state);
            if (!exported) return false;
            ShareCopy(list.get(), exported, g_fg.full_w, g_fg.full_h);
        }
        // The FPS counter goes on last: what is recorded or shared has none.
        if (bb_state != D3D12_RESOURCE_STATE_COPY_DEST) {
            auto to_dest = Barrier(bb.get(), bb_state, D3D12_RESOURCE_STATE_COPY_DEST);
            list->ResourceBarrier(1, &to_dest);
            bb_state = D3D12_RESOURCE_STATE_COPY_DEST;
        }
        ShowProof(list.get(), bb.get(), !synthetic);
        CounterStamp(list.get(), bb.get());
        auto post = Barrier(bb.get(), bb_state, D3D12_RESOURCE_STATE_PRESENT);
        list->ResourceBarrier(1, &post);
        if (FAILED(list->Close())) return false;
        ID3D12CommandList *commands[] = {list.get()};
        lfCore.direct_queue->ExecuteCommandLists(1, commands);
        if (FAILED(lfCore.direct_queue->Signal(fence.get(), ++value)) ||
            FAILED(fence->SetEventOnCompletion(value, event)) ||
            WaitForSingleObject(event, 2000) != WAIT_OBJECT_0 ||
            fence->GetCompletedValue() < value) return false;
        return true;
    };
    auto flip = [&](bool synthetic, double source_time) {
        const HRESULT pr = ShowPresentNow(synthetic ? 0.0 : source_time);
        if (pr == DXGI_STATUS_MODE_CHANGED || pr == DXGI_STATUS_MODE_CHANGE_IN_PROGRESS)
        {
            // The mode changed under us. The present did not happen; treat
            // it as a benign skip - the pipeline's resize path rebuilds the
            // swapchain when the size follows, and the next frame presents
            // normally. Presenting into the old surface until then is what
            // froze the overlay black (v1.10-review H2).
            Trace("[dlssg] present reports a mode change - skipping a frame");
            lfShow.stale = true;
            return true;
        }
        if (pr == DXGI_ERROR_WAS_STILL_DRAWING) return true;
        if (pr == DXGI_STATUS_OCCLUDED)
        {
            // The window is hidden (minimised target): benign.
            return true;
        }
        if (FAILED(pr)) return false;
        ShareSend();
        ShowRevealOnce();
        ++shown;
        if (synthetic) ++generated;
        if (trace)
            fprintf(trace, "%.6f,%d,%.6f,%llu,%.6f,%.6f\n", LatencyNow(), synthetic ? 1 : 0, trace_content,
                    trace_sequence, trace_ready, source_time);
        return true;
    };
    while (!g_fg.stop && !g_fg.failed)
    {
        FgSlot *chosen = nullptr;
        {
            std::unique_lock<std::mutex> lock(g_fg.mutex);
            const bool ready = g_fg.wake.wait_for(lock, std::chrono::milliseconds(250), [] {
                if (g_fg.stop) return true;
                for (auto &s : g_fg.slots) if (s.state == 2) return true;
                return false;
            });
            if (g_fg.stop) break;
            if (!ready) { lock.unlock(); reportCadence(); continue; }
            // In order (lf_fg_pace.inl): the oldest pair first. Only a pair
            // whose real frame's time has already gone by while a newer one
            // waits is passed over - the presenter fell a whole pair behind.
            unsigned waiting = 0;
            for (auto &s : g_fg.slots)
                if (s.state == 2) { ++waiting; if (!chosen || s.sequence < chosen->sequence) chosen = &s; }
            while (waiting > 1 && pace.delay > 0.0 && chosen->interpolate &&
                   LatencyNow() > chosen->moment + pace.delay)
            {
                chosen->state = 0;
                ++pace.dropped;
                --waiting;
                chosen = nullptr;
                for (auto &s : g_fg.slots)
                    if (s.state == 2 && (!chosen || s.sequence < chosen->sequence)) chosen = &s;
            }
            chosen->state = 3;
        }
        trace_sequence = chosen->sequence;
        trace_ready = chosen->ready_at;
        // Every picture at its moment + the delay. NGX already produced this
        // complete pair; a queued next pair never cancels its remaining
        // phases (x4 otherwise often became x3). Sequence gaps in
        // presentation do not invalidate NGX's input history.
        const double span = chosen->moment - chosen->prev_moment;
        const double step = span / (chosen->count + 1);
        showing = chosen;
        const bool irregular = chosen->interpolate && span > 0.0 && pace.Irregular(span);
        bool scheduled = chosen->interpolate && span > 0.0 && !irregular;
        if (!chosen->interpolate) pace.Forget();
        else if (scheduled)
        {
            const double need = chosen->ready_at - chosen->prev_moment - step + pace.lead[chosen->count ? 1 : 0] +
                                pace.call + 0.0003;
            // A stall (the game loading, the card taken away for a moment):
            // shown as it comes, and not taken for the delay to keep.
            if (pace.delay > 0.0 && need > pace.delay + std::max(0.05, 2.0 * span)) { scheduled = false; ++pace.late; ++pace.pairs; }
            else pace.Took(need, step);
        }
        auto show = [&](ID3D12Resource *picture, bool synthetic, double moment, double source_time) {
            const double began = LatencyNow();
            if (!draw(picture, synthetic)) { g_fg.failed = true; return; }
            const double drawn = LatencyNow();
            if (scheduled && !waitUntil(moment + pace.delay - pace.call)) return;
            const double asked = LatencyNow();
            waitForPresent();
            if (g_fg.stop) return;
            trace_content = moment;
            const double called = LatencyNow();
            if (!flip(synthetic, source_time)) g_fg.failed = true;
            pace.Took(drawn - began, called - asked, LatencyNow() - called, synthetic);
        };
        if (irregular) pace.skipped += chosen->count;
        if (chosen->interpolate && !irregular)
        {
            for (unsigned index = 0; index < chosen->count && !g_fg.stop && !g_fg.failed; ++index)
            {
                const double moment = chosen->prev_moment + step * (index + 1);
                // Too late to be told from the next one: shown now it would
                // only push that one back (the two-at-once of before).
                if (LatencyNow() > moment + pace.delay + 0.75 * step) { ++pace.skipped; continue; }
                show(chosen->interpolated[index].get(), true, moment, 0.0);
            }
        }
        if (!g_fg.stop && !g_fg.failed) show(chosen->real.get(), false, chosen->moment, chosen->source_time);
        showing = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_fg.mutex);
            chosen->state = 0;
        }
        reportCadence();
    }
    if (value && fence->GetCompletedValue() < value) FgAwait(fence.get(), value, 30000);
    CloseHandle(event);
    if (timer) CloseHandle(timer);
    if (trace) fclose(trace);
    if (g_fg.failed) Trace("[dlssg] unavailable: presenter failed; ordinary output retained");
}

static NVSDK_NGX_Result FgInitSafe(NrInitFn init, const wchar_t *cache, NVSDK_NGX_Parameter *params)
{
    __try { return init(0x1000000ULL, cache, lfCore.device, NVSDK_NGX_Version_API, params); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NVSDK_NGX_Result_Fail; }
}
static NVSDK_NGX_Result FgCreateSafe(ID3D12GraphicsCommandList *list, NVSDK_NGX_Parameter *params, NVSDK_NGX_Handle **out)
{
    __try { return g_fg.create(list, NVSDK_NGX_Feature_FrameGeneration, params, out); }
    __except (EXCEPTION_EXECUTE_HANDLER) { return NVSDK_NGX_Result_Fail; }
}
static bool EnsureFg(Pipeline &v, DXGI_FORMAT format)
{
    if (!lfCore.ngx_ready) { Trace("[dlssg] unavailable: NGX is not initialized on this adapter"); return false; }
    const UINT full_w = v.stretched ? v.screen_w : v.work_w, full_h = v.stretched ? v.screen_h : v.work_h;
    char setting[16] = {};
    GetEnvironmentVariableA("LF_FG_MAX_HEIGHT", setting, sizeof(setting));
    const UINT max_height = static_cast<UINT>(std::max(0, atoi(setting)));
    const double scale = max_height >= 360 && max_height < full_h ? double(max_height) / full_h : 1.0;
    const UINT w = scale < 1 ? UINT(std::round(full_w * scale)) & ~1u : full_w;
    const UINT height = scale < 1 ? UINT(std::round(full_h * scale)) & ~1u : full_h;
    if (g_fg.feature && g_fg.w == w && g_fg.height == height && g_fg.mw == v.work_w &&
        g_fg.mh == v.work_h && g_fg.full_w == full_w && g_fg.full_h == full_h &&
        g_fg.format == format && g_fg.thread.joinable()) return true;
    CloseFgResources();
    if (!g_fg.module)
    {
        // The BYO library folder next to the worker, then the worker's own
        // directory: users drop nvngx_dlssg.dll into native\libraries\ to
        // pick the build they want, and native\ stays the bundled fallback.
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (auto slash = wcsrchr(path, L'\\')) *(slash + 1) = 0;
        wchar_t directory[MAX_PATH]; wcscpy_s(directory, path);
        if (!FgPrepareCompatibility(directory)) return false;
        wchar_t libraries[MAX_PATH]; wcscpy_s(libraries, directory);
        wcscat_s(libraries, L"libraries\\");
        wchar_t lib_path[MAX_PATH]; wcscpy_s(lib_path, libraries);
        wcscat_s(lib_path, L"nvngx_dlssg.dll");
        if (GetFileAttributesW(lib_path) != INVALID_FILE_ATTRIBUTES)
        {
            // The BYO file is verified (NVIDIA signature, machine-root
            // chain, product name) before it is mapped - a writable folder
            // next to the executable is otherwise the easiest DLL plant.
            if (TrustUserDll(lib_path, "nvngx_dlssg.dll"))
            {
                g_fg.module = LoadLibraryW(lib_path);
                if (g_fg.module)
                    Trace("[dlssg] FG runtime from native\\libraries\\ (BYO, "
                        "verified NVIDIA signature)");
            }
            else
                Trace("[dlssg] BYO refused: nvngx_dlssg.dll is not a "
                    "NVIDIA-signed runtime - falling back to the bundled "
                    "copy (%ls)", lib_path);
        }
        if (!g_fg.module)
        {
            wcscpy_s(path, directory);
            wcscat_s(path, L"nvngx_dlssg.dll");
            g_fg.module = LoadLibraryW(path);
        }
        if (!g_fg.module) { Trace("[dlssg] unavailable: nvngx_dlssg.dll load failed (Windows %lu)", GetLastError()); return false; }
        if (FgComputeCapability() == 89) {
            auto base = reinterpret_cast<BYTE *>(g_fg.module);
            auto dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
            auto nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
            const auto patched = lf_ada::Apply(base, nt->OptionalHeader.SizeOfImage, 89);
            g_fg_runtime_max = patched.applied ? 5u : 1u;
            g_fg_count = std::min(g_fg_count, g_fg_runtime_max);
            Trace("[dlssg] Ada temporal kernels: %u containers; %s", patched.containers,
                  patched.applied ? "MFG up to 6x enabled" : "unrecognized runtime, limited to 2x");
        }
    }
    if (!g_fg.initialized)
    {
        auto init = reinterpret_cast<NrInitFn>(GetProcAddress(g_fg.module, "NVSDK_NGX_D3D12_Init_Ext"));
        g_fg.create = reinterpret_cast<NrCreateFn>(GetProcAddress(g_fg.module, "NVSDK_NGX_D3D12_CreateFeature"));
        g_fg.release = reinterpret_cast<NrReleaseFn>(GetProcAddress(g_fg.module, "NVSDK_NGX_D3D12_ReleaseFeature"));
        g_fg_evaluate = reinterpret_cast<NrEvaluateFn>(GetProcAddress(g_fg.module, "NVSDK_NGX_D3D12_EvaluateFeature"));
        if (!init || !g_fg.create || !g_fg.release || !g_fg_evaluate) return false;
        if (!g_fg.params && NVSDK_NGX_FAILED(NVSDK_NGX_D3D12_AllocateParameters(&g_fg.params))) return false;
        wchar_t cache[MAX_PATH] = {};
        LiveFlowCachePath(cache);
        const auto result = FgInitSafe(init, cache, g_fg.params);
        Trace("[dlssg] Init_Ext -> 0x%08X", result);
        if (NVSDK_NGX_FAILED(result)) { Trace("[dlssg] unavailable: runtime initialization 0x%08X", result); return false; }
        g_fg.initialized = true;
    }
    auto p = g_fg.params;
    p->Reset();
    p->Set("CreationNodeMask", 1u); p->Set("VisibilityNodeMask", 1u);
    p->Set("Width", w); p->Set("Height", height);
    p->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFormat, (unsigned)format);
    p->Set(NVSDK_NGX_DLSSG_Parameter_InternalWidth, v.work_w);
    p->Set(NVSDK_NGX_DLSSG_Parameter_InternalHeight, v.work_h);
    p->Set(NVSDK_NGX_DLSSG_Parameter_DynamicResolution, 0u);
    if (!CmdOpen()) return false;
    const auto result = FgCreateSafe(lfCore.cmd, p, &g_fg.feature);
    if (!FgAwait(lfCore.timeline, CmdSubmit(), 30000) || NVSDK_NGX_FAILED(result) || !g_fg.feature)
    { Trace("[dlssg] unavailable: CreateFeature 0x%08X (adapter/driver/runtime support)", result); return false; }
    if (!StagedCreate(g_fg.depth, v.work_w, v.work_h, DXGI_FORMAT_R32_FLOAT, v.work_w * 4)) return false;
    std::vector<float> depth(static_cast<size_t>(v.work_w) * v.work_h, 0.5f);
    if (!StagedFill(g_fg.depth, reinterpret_cast<BYTE *>(depth.data()), v.work_w * 4, v.work_h) || !CmdOpen()) return false;
    StagedCopy(lfCore.cmd, g_fg.depth);
    auto b = Barrier(g_fg.depth.gpu_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    lfCore.cmd->ResourceBarrier(1, &b);
    if (!FgAwait(lfCore.timeline, CmdSubmit(), 30000)) return false;
    for (unsigned i = 0; i < g_fg_count; ++i)
    {
        g_fg.output[i].attach(NewTexture(w, height, format, true));
        if (!g_fg.output[i]) return false;
    }
    for (auto &slot : g_fg.slots)
    {
        slot.real.attach(NewTexture(full_w, full_h, format, false));
        if (!slot.real) return false;
        for (unsigned i = 0; i < g_fg_count; ++i)
        {
            slot.interpolated[i].attach(NewTexture(w, height, format, true));
            if (!slot.interpolated[i]) return false;
        }
    }
    if (!CmdOpen()) return false;
    for (unsigned i = 0; i < g_fg_count; ++i)
    {
        auto to_copy = Barrier(g_fg.output[i].get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        lfCore.cmd->ResourceBarrier(1, &to_copy);
    }
    for (auto &slot : g_fg.slots) for (unsigned i = 0; i <= g_fg_count; ++i)
    {
        auto texture = i ? slot.interpolated[i - 1].get() : slot.real.get();
        auto to_copy = Barrier(texture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        lfCore.cmd->ResourceBarrier(1, &to_copy);
    }
    if (!FgAwait(lfCore.timeline, CmdSubmit(), 30000)) return false;
    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = 20; buffer.Height = 1; buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1; buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    D3D12_HEAP_PROPERTIES heap = {}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(g_fg.disable.put())))) return false;
    heap.Type = D3D12_HEAP_TYPE_READBACK; buffer.Flags = D3D12_RESOURCE_FLAG_NONE;
    if (FAILED(lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &buffer,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(g_fg.disable_readback.put())))) return false;
    if (scale < 1) {
        g_fg.scaled_color.attach(NewTexture(w, height, format, true));
        if (!g_fg.scaled_color || !FgScaleReady(full_w, full_h, format)) return false;
        if (!CmdOpen()) return false;
        auto to_write = Barrier(g_fg.scaled_color.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        lfCore.cmd->ResourceBarrier(1, &to_write);
        auto display_write = Barrier(g_fg_scale.display.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        lfCore.cmd->ResourceBarrier(1, &display_write);
        if (!FgAwait(lfCore.timeline, CmdSubmit(), 30000)) return false;
    }
    // What does not move stays put (lf_fg_still.inl). Not having it is not a
    // failure of the generator.
    FgStillReady(full_w, full_h, v.work_w, v.work_h, format, !g_fg_scale.display);
    g_fg.w = w; g_fg.height = height; g_fg.full_w = full_w; g_fg.full_h = full_h;
    g_fg.mw = v.work_w; g_fg.mh = v.work_h; g_fg.format = format;
    g_fg.stop = false;
    // R11 refined: latency 1 belongs to the FG presenter while it owns the
    // present loop. The ordinary NR path presents Present(0,0) per frame and
    // never consumes the waitable - with latency 1 the swapchain would hold
    // a single queued present and every ordinary present would block or drop
    // against the compositor (the fullscreen flicker). The waitable handle
    // is taken here, from the same thread that will wait on it.
    if (lfShow.chain != nullptr)
    {
        IDXGISwapChain2 *sc2 = nullptr;
        if (SUCCEEDED(lfShow.chain->QueryInterface(
                __uuidof(IDXGISwapChain2), reinterpret_cast<void **>(&sc2)))
            && sc2 != nullptr)
        {
            sc2->SetMaximumFrameLatency(1);
            g_fg_waitable = lfShow.quick_waitable ? lfShow.quick_waitable : sc2->GetFrameLatencyWaitableObject();
            sc2->Release();
            Trace("[dlssg] the presenter is paced by the compositor (latency 1)");
        }
    }
    g_fg.captured = 0;
    g_fg.thread = std::thread(FgPresenter);
    Trace("[dlssg] %ux enabled at %ux%u, format=%u; flat depth and estimated motion (experimental)", g_fg_count + 1, w, height, format);
    if (scale < 1) Trace("[dlssg] generated at %ux%u, scaled to %ux%u; real frames retain source resolution", w, height, full_w, full_h);
    return true;
}

// Opt-in pixel regression capture, never enabled in ordinary runs.
static void FgDump(ID3D12Resource *source, D3D12_RESOURCE_STATES state, unsigned index)
{
    char dir[MAX_PATH] = {};
    if (!GetEnvironmentVariableA("LF_FG_DUMP", dir, sizeof(dir)) ||
        (g_fg.sequence != 30 && g_fg.sequence != 150 && g_fg.sequence != 300 &&
         !(g_fg.sequence == 29 && index == 0))) return;
    auto desc = source->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp = {};
    UINT rows; UINT64 rowbytes, bytes;
    lfCore.device->GetCopyableFootprints(&desc, 0, 1, 0, &fp, &rows, &rowbytes, &bytes);
    D3D12_HEAP_PROPERTIES heap = {}; heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd = {}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes; bd.Height = 1; bd.DepthOrArraySize = bd.MipLevels = 1;
    bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    winrt::com_ptr<ID3D12Resource> rb;
    if (FAILED(lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &bd,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(rb.put()))) || !CmdOpen()) return;
    auto pre = Barrier(source, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) lfCore.cmd->ResourceBarrier(1, &pre);
    D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
    from.pResource = source; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource = rb.get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = fp;
    lfCore.cmd->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    auto post = Barrier(source, D3D12_RESOURCE_STATE_COPY_SOURCE, state);
    if (state != D3D12_RESOURCE_STATE_COPY_SOURCE) lfCore.cmd->ResourceBarrier(1, &post);
    if (!FgAwait(lfCore.timeline, CmdSubmit(), 30000)) return;
    BYTE *data = nullptr; D3D12_RANGE read = {0, static_cast<SIZE_T>(bytes)}, written = {0, 0};
    if (FAILED(rb->Map(0, &read, reinterpret_cast<void **>(&data)))) return;
    char path[MAX_PATH]; sprintf_s(path, "%s/fg-%llu-%u.raw", dir, g_fg.sequence, index);
    FILE *file = nullptr;
    if (!fopen_s(&file, path, "wb") && file) {
        for (UINT y = 0; y < rows; ++y) fwrite(data + size_t(y) * fp.Footprint.RowPitch, 1, size_t(rowbytes), file);
        fclose(file);
    }
    rb->Unmap(0, &written);
}

static bool FgPresent(Pipeline &v, ID3D12Resource *color, D3D12_RESOURCE_STATES state)
{
    if (!FgRequested()) { StopFgPresentation(); return false; }
    if (!EnsureFg(v, color->GetDesc().Format))
    { Trace("[dlssg] unavailable: the runtime could not create frame generation; ordinary output retained"); g_fg.failed = true; CloseFgResources(); return false; }
    const auto now = std::chrono::steady_clock::now();
    const double taken = LatencyNow();
    const double source = (lfCap.window_live || lfCap.live) ? lfCap.stamp : 0.0;
    if (g_fg.history && source > 0 && source == g_fg.source_time && !lfShow.history_restart) return true;
    const double interval = g_fg.history && source > g_fg.source_time && g_fg.source_time > 0
        ? source - g_fg.source_time : (g_fg.history ? std::chrono::duration<double>(now - g_fg.last).count() : 0.016);
    const bool interpolate = g_fg.history && !lfShow.history_restart && interval < 0.2;
    g_fg.last = now;
    const double source_before = g_fg.source_time;
    g_fg.source_time = source;
    ++g_fg.captured;
    // An uncapped x4 requests three generated phases. The old refresh-based
    // budget used the capture rate, including pairs later discarded by the
    // presenter, and could turn a displayed 20-FPS x4 into only 60 FPS.
    // An explicit output cap can still reduce phases to preserve input latency.
    double target = OutputRefresh();
    const int cap = ShownCap();
    if (cap > 0) target = target > 0 ? std::min(target, double(cap)) : double(cap);
    unsigned count = FgGateCount(g_fg_count);
    if (cap > 0 && source > 0 && target > 0) {
        const double wanted = std::clamp(target * interval - 1.0, 0.0, double(g_fg_count));
        if (!interpolate) g_fg.extra_budget = 0;
        g_fg.extra_budget += wanted;
        count = std::min(g_fg_count, static_cast<unsigned>(g_fg.extra_budget + 0.0001));
        g_fg.extra_budget = std::max(0.0, g_fg.extra_budget - count);
    }
    // NGX history is advanced even in a real-only group - but not while the
    // gate gives no frames in between at all (the game alone fills the screen):
    // then NGX rests, and starts from a reset when frames are wanted again.
    const bool resting = g_fg_gate.count == 0 && cap <= 0;
    const unsigned evaluated = resting ? 0u : std::max(1u, count);
    static bool ngx_rested = false;
    auto p = g_fg.params;
    p->Reset();
    NVSDK_NGX_DLSSG_Opt_Eval_Params opt = {};
    for (int i = 0; i < 4; ++i)
        opt.cameraViewToClip[i][i] = opt.clipToCameraView[i][i] = opt.clipToLensClip[i][i] =
        opt.clipToPrevClip[i][i] = opt.prevClipToClip[i][i] = 1.0f;
    opt.mvecScale[0] = 1.f / v.work_w; opt.mvecScale[1] = 1.f / v.work_h;
    opt.cameraUp[1] = opt.cameraRight[0] = opt.cameraFwd[2] = 1;
    opt.cameraNear = 0.1f; opt.cameraFar = 1000; opt.cameraFOV = 1;
    opt.cameraAspectRatio = float(g_fg.w) / g_fg.height;
    opt.cameraMotionIncluded = opt.orthoProjection = opt.motionVectorsDilated = true;
    opt.colorBuffersHDR = g_fg.format == DXGI_FORMAT_R10G10B10A2_UNORM;
    opt.reset = !interpolate || ngx_rested;
    ngx_rested = resting;
    opt.mvecsSubrectSize = opt.depthSubrectSize = {v.work_w, v.work_h};
    opt.backbufferSubrectSize = opt.outputInterpSubrectSize = {g_fg.w, g_fg.height};
    NVSDK_NGX_D3D12_DLSSG_Eval_Params ep = {};
    ep.pBackbuffer = color;
    ep.pMVecs = !lfOut.sent_passthrough && g_fg.motion ? g_fg.motion.get() : v.motion.gpu_tex;
    ep.pDepth = g_fg.depth.gpu_tex;
    if (!CmdOpen()) { g_fg.failed = true; CloseFgResources(); return false; }
    // The motion at rest where the captured picture did not change since the
    // real frame before (lf_fg_still.inl). After a cut or a pause there is no
    // "before" to compare with.
    if (interpolate) ep.pMVecs = FgStillCalm(lfCore.cmd, v.picture.gpu_tex, ep.pMVecs);
    else FgStillForget();
    ID3D12Resource *model_color = color;
    D3D12_RESOURCE_STATES model_state = state;
    if (g_fg.scaled_color) {
        FgScaleRecord(lfCore.cmd, color, state, g_fg.scaled_color.get(), false);
        auto ready = Barrier(g_fg.scaled_color.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        lfCore.cmd->ResourceBarrier(1, &ready);
        model_color = g_fg.scaled_color.get();
        model_state = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        ep.pBackbuffer = model_color;
    }
    bool allow_interpolation = true;
    p->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, static_cast<unsigned long long>(g_fg.sequence + 1));
    opt.multiFrameCount = evaluated;
    for (unsigned index = 0; index < evaluated; ++index)
    {
        opt.multiFrameIndex = index + 1;
        ep.pOutputInterpFrame = g_fg.output[index].get();
        ep.pOutputDisableInterpolation = g_fg.disable.get();
        auto pre = Barrier(model_color, model_state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if (model_state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) lfCore.cmd->ResourceBarrier(1, &pre);
        auto out_pre = Barrier(g_fg.output[index].get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        lfCore.cmd->ResourceBarrier(1, &out_pre);
        const auto result = NGX_D3D12_EVALUATE_DLSSG(lfCore.cmd, g_fg.feature, p, &ep, &opt);
        auto disable_pre = Barrier(g_fg.disable.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        lfCore.cmd->ResourceBarrier(1, &disable_pre);
        lfCore.cmd->CopyBufferRegion(g_fg.disable_readback.get(), index * 4, g_fg.disable.get(), 0, 4);
        auto disable_post = Barrier(g_fg.disable.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        lfCore.cmd->ResourceBarrier(1, &disable_post);
        auto out_post = Barrier(g_fg.output[index].get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        lfCore.cmd->ResourceBarrier(1, &out_post);
        auto post = Barrier(model_color, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, model_state);
        if (model_state != D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) lfCore.cmd->ResourceBarrier(1, &post);
        if (NVSDK_NGX_FAILED(result))
        {
            FgAwait(lfCore.timeline, CmdSubmit(), 30000);
            if (g_fg_count > 1) {
                const unsigned next = g_fg_count > 3 ? 3 : 1;
                Trace("[dlssg] multiplier x%u refused 0x%08X; retrying x%u", g_fg_count + 1, result, next + 1);
                CloseFgResources(); g_fg_count = next; return false;
            }
            Trace("[dlssg] unavailable: Evaluate 0x%08X; ordinary output retained", result);
            g_fg.failed = true; CloseFgResources(); return false;
        }
    }
    if (g_fg.scaled_color) {
        auto rest = Barrier(model_color, model_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        lfCore.cmd->ResourceBarrier(1, &rest);
    }
    FgSlot *slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_fg.mutex);
        for (auto &s : g_fg.slots) if (s.state == 0) { slot = &s; break; }
        // None free: the oldest pair still waiting gives way (the presenter
        // shows them in order).
        if (!slot) for (auto &s : g_fg.slots) if (s.state == 2 && (!slot || s.sequence < slot->sequence)) slot = &s;
        if (!slot)
        {
            // Starvation: every slot is mid-flight. The submitted work still
            // ends here - record its token, or PresentFrame hands the defer
            // contract a STALE g_fg_present_fence from an earlier frame and
            // the token-order guard kills the worker (the blink-out class).
            const UINT64 fence = CmdSubmit();
            FgAwait(lfCore.timeline, fence, 30000);
            g_fg_present_fence = fence;
            return true;
        }
        slot->state = 1;
    }
    // The reserved slot is invisible to the presenter until the fence completes.
    // Hand it the generated textures and reuse its old buffers next frame.
    for (unsigned i = 0; i < evaluated; ++i)
        std::swap(g_fg.output[i], slot->interpolated[i]);
    {
        auto source = color;
        auto destination = slot->real.get();
        const auto rest = state;
        D3D12_RESOURCE_BARRIER before[] = {
            Barrier(source, rest, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Barrier(destination, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST)};
        lfCore.cmd->ResourceBarrier(2, before);
        lfCore.cmd->CopyResource(destination, source);
        D3D12_RESOURCE_BARRIER after[] = {
            Barrier(source, D3D12_RESOURCE_STATE_COPY_SOURCE, rest),
            Barrier(destination, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE)};
        lfCore.cmd->ResourceBarrier(2, after);
    }
    // Which tiles are at rest, into the slot's own map; this picture is "the
    // one before" from here on.
    FgStillTiles(lfCore.cmd, v.picture.gpu_tex, slot->still.get());
    const UINT64 fg_fence = CmdSubmit();
    if (!FgAwait(lfCore.timeline, fg_fence, 30000))
    { g_fg.failed = true; CloseFgResources(); return false; }
    g_fg_present_fence = fg_fence;
    const unsigned still_tiles = FgStillCounted();
    BYTE *disabled = nullptr;
    D3D12_RANGE read = {0, evaluated * 4}, written = {0, 0};
    if (SUCCEEDED(g_fg.disable_readback->Map(0, &read, reinterpret_cast<void **>(&disabled))))
    {
        for (unsigned i = 0; i < evaluated; ++i)
            allow_interpolation = allow_interpolation && disabled[i * 4] == 0;
        g_fg.disable_readback->Unmap(0, &written);
    }
    else allow_interpolation = false;
    FgDump(color, state, 0);
    for (unsigned i = 0; i < count; ++i) FgDump(slot->interpolated[i].get(), D3D12_RESOURCE_STATE_COPY_SOURCE, i + 1);
    {
        std::lock_guard<std::mutex> lock(g_fg.mutex);
        slot->sequence = ++g_fg.sequence;
        slot->count = count;
        slot->source_time = (lfCap.window_live || lfCap.live) ? lfCap.stamp : 0.0;
        // Without a time stamp of the capture's the moment is when it was taken.
        slot->ready_at = LatencyNow();
        const double raw = source > 0 ? source : taken;
        const bool chained = interpolate && source > 0 && source_before > 0 && source > source_before;
        slot->moment = g_fg_clock.Moment(raw, chained ? source - source_before : 0.0, OutputRefresh());
        slot->prev_moment = chained && g_fg_clock.before > 0.0 ? g_fg_clock.before : slot->moment - interval;
        slot->interpolate = interpolate && allow_interpolation;
        slot->protect = slot->interpolate && still_tiles > 0;
        // Keep all requested phases evenly spaced at the display's attainable
        // rate. A fast incoming pair must not compress x4 into tiny deadlines
        // which cancel phases while the compositor is still busy.
        const double paced = target > 0 ? std::max(interval, (count + 1) / target) : interval;
        slot->interval = std::clamp(paced, 0.004, 0.2);
        slot->state = 2;
    }
    g_fg.history = true;
    g_fg.wake.notify_one();
    return true;
}

// Explicit settings ride in each frame header; factor changes recreate FG only.
// The controller restarts the worker for enable/disable to isolate compatibility hooks.
static void ConfigureFgFrame(uint32_t flags)
{
    if (!(flags & 0x800)) return; // older clients may still use LF_FRAMEGEN
    const int enabled = (flags & 0x100) != 0;
    const unsigned count = std::min(5u, (((flags >> 9) & 3u) | ((flags >> 12) & 4u)) + 1u);
    if (g_fg_ui_enabled == enabled && g_fg_requested_count == count) return;
    CloseFgResources();
    g_fg_ui_enabled = enabled;
    g_fg_requested_count = count;
    g_fg_count = std::min(count, g_fg_runtime_max);
    g_fg.failed = false;
    lfOut.force_next = true;
    lfShow.stale = true;
    Trace("[dlssg] UI: %s, %ux", enabled ? "on" : "off", count + 1);
}

#endif
