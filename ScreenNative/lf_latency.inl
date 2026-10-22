// lf_latency.inl - Low Latency (round 24), the LIVE FLOW switch in DLSS 4.5
// FG's old place.
//
// What a LIVE FLOW frame waits on between the game's frame reaching the
// desktop and the processed one reaching the screen, and what the switch does
// about each - the ideas of NVIDIA Reflex and AMD Anti-Lag, applied to a
// picture that comes from the screen instead of from a game's render loop:
//   1. The output queue. The output window's swap chain lets up to three
//      presents wait for the screen. Whenever frames come faster than the
//      screen takes them - VRR's synchronised presents, a 60 Hz screen under a
//      120 FPS source - every shown frame is one or two refreshes older than
//      it had to be. On: two buffers and SetMaximumFrameLatency(1), so one
//      frame waits at most, never a queue.
//   2. When the picture is taken. The frame-latency waitable object says when
//      the screen has taken the last frame; the capture waits for that
//      (LowLatencyWait) instead of taking a picture that would then wait in
//      Present. The picture is taken as late as it can be - Reflex's "start
//      the frame just in time" - and it is always the newest one (the desktop
//      capture hands out the latest frame, not the next in a queue).
//   3. FLOW MFG. Its presenter keeps one frame queued already; with the switch
//      the cushion it keeps for jitter before each pair is the median of the
//      recent frame ages instead of their 75th percentile (flow_mfg.inl).
//   4. The loop's thread runs above normal priority while it is on, so a busy
//      CPU delays neither the capture nor the Present.
//   5. The card's scheduling class (round 25): realtime or normal, whichever
//      the measurement below says is quicker on this game (LowLatencyChoose).
// Nothing is dropped and nothing is re-timed that the pacing depends on: the
// frame limit, VRR and FLOW MFG's own clock work as before.
//
// Measured, not assumed: each shown frame's age - from the moment its source
// reached the desktop (the capture's own time stamp: DXGI's LastPresentTime,
// Windows Graphics Capture's SystemRelativeTime) to the refresh that showed
// it (DXGI frame statistics) - is summed up every two seconds as [latency].
// Only DXGI: the same on NVIDIA, AMD and Intel.

static int g_low_latency = -1;   // -1 until LF_LOW_LATENCY or a reshape has said

static bool LowLatencyWanted()
{
    if (g_low_latency < 0)
    {
        char text[4] = {};
        g_low_latency = GetEnvironmentVariableA("LF_LOW_LATENCY", text, sizeof(text)) > 0 && text[0] == '1' ? 1 : 0;
        if (g_low_latency) Trace("[latency] Low Latency on from LF_LOW_LATENCY");
    }
    return g_low_latency == 1;
}

static void LowLatencyFromResize(uint32_t flags)
{
    if ((flags & kReshapeDisplay) == 0) return;
    const int want = (flags & kReshapeLowLatency) != 0 ? 1 : 0;
    if (want != (LowLatencyWanted() ? 1 : 0)) Trace("[latency] Low Latency %s", want ? "on" : "off");
    g_low_latency = want;
}

static struct LatencyState
{
    std::mutex lock;
    // The waitable object is a count of frames the screen took: waited on at
    // most once per Present, or a later wait would sit out its whole timeout.
    bool owed = false;
    IDXGISwapChain3 *chain = nullptr;
    // Presents not yet seen on screen: their number and their source's time.
    static constexpr unsigned kRing = 16;
    UINT counts[kRing] = {};
    double sources[kRing] = {};
    double called[kRing] = {};
    unsigned next = 0;
    double last_source = 0.0;
    unsigned reals = 0;   // real frames presented since the last summary, each once
    // Two seconds of it.
    std::vector<double> shown_ms, queue_ms;
    double waited = 0.0;
    unsigned waits = 0, timeouts = 0;
    ULONGLONG since = 0;
    bool was_low = false;
} g_lat;

static double LatencyNow()
{
    static LARGE_INTEGER frequency = {};
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return double(now.QuadPart) / double(frequency.QuadPart);
}

// The card's time between LIVE FLOW and the game (round 25, measured with a
// stand-in game keeping 60% of the card busy, tests/screen_latency_load.py):
// at Windows' normal GPU scheduling class the two are time-sliced and the DLSS
// 5 network's 34 ms stretch to ~100 ms - 8 processed frames a second, 121 ms
// from the game's frame to the screen - and D3D12 queue priorities and the
// classes below realtime change nothing. At the realtime class LIVE FLOW's
// work goes first: 20 frames a second, 40 ms. What keeps the game fed is that
// Low Latency never processes a picture twice (the loop skips a frame with
// nothing new, lf_loop.inl) - after each new game frame the card is the
// game's again until the next one arrives.
//
// But realtime is not always quicker. With nothing else on the card (the
// football fixture, tests/screen_low_latency.py) it cost 19 ms on the plain
// output and 22 ms on FLOW MFG x2 - more frames processed, each one older when
// it reached the screen - where the normal class with the rest of Low Latency
// saved 0-3 ms. So the class is never assumed: LowLatencyChoose tries both.
static void LowLatencyClass(bool on)
{
    typedef LONG(APIENTRY *SetClassFn)(HANDLE, INT);
    static SetClassFn set_class = [] {
        HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
        if (!gdi) gdi = LoadLibraryW(L"gdi32.dll");
        return gdi ? reinterpret_cast<SetClassFn>(GetProcAddress(gdi, "D3DKMTSetProcessSchedulingPriorityClass")) : nullptr;
    }();
    const INT level = on ? 5 : 2;   // D3DKMT_SCHEDULINGPRIORITYCLASS_REALTIME / _NORMAL
    const LONG status = set_class ? set_class(GetCurrentProcess(), level) : -1;
    if (status != 0)
        Trace("[latency] the card's scheduling class could not be set to %s (0x%08lX)", on ? "realtime" : "normal",
              (unsigned long)status);
}

// Which class, measured on the game in front of it. When the switch goes on,
// two summaries of [latency] (every two seconds) are taken at each class - the
// first after a change is passed over, it holds frames from before it - and
// realtime is kept only when it is clearly quicker (3 ms and 8%): it takes the
// card from the game, the normal class takes nothing. Measured again when the
// age drifts well away from the one chosen on (a game started or closed, a
// menu became a level) and every two minutes. While there is nothing to
// measure - a still screen - the trial simply waits for frames.
static struct ClassChoice
{
    std::atomic<int> want{0};   // 1 realtime, 0 normal: set here, applied by the loop's thread
    int applied = -1;           // the loop's thread only; -1: never set (the process's own, normal)
    int phase = 0;              // 0 chosen, 1 measuring the normal class, 2 measuring realtime
    int settle = 0;             // summaries still to pass over after a change
    double normal_ms = 0.0, chosen_ms = 0.0;
    double normal_rate = 0.0;   // real frames shown a second at the normal class
    int drift = 0;
    ULONGLONG chosen_at = 0;
} g_class;

// Under g_lat.lock.
static void LowLatencyTrial(const char *why)
{
    g_class.phase = 1;
    g_class.settle = 1;
    g_class.want = 0;
    g_class.drift = 0;
    if (why) Trace("[latency] Low Latency measures the card's scheduling classes again - %s", why);
}

// Under g_lat.lock: each [latency] summary while the switch or a generator is
// on. real_rate: real frames shown a second in that summary.
//
// With a generator on, what counts first is how many of the game's frames
// reach the screen (round 27): at the normal class under a game that fills the
// card both generators got about half of them - DLSS FG 62 of 130 and showed
// 124, fewer than the game alone - where the game's own rate was left
// untouched and the result was of no use. Realtime is kept when it shows
// clearly more real frames (8%), or as many with clearly less delay; the
// frames it costs the game are not shown anyway - real frames are taken at
// most at the screen's rate divided by the multiplier (PaceCapture).
static void LowLatencyChoose(double avg_ms, double real_rate)
{
    ClassChoice &c = g_class;
    const ULONGLONG tick = GetTickCount64();
    // DLSS MFG keeps realtime and is never measured again (LowLatencyWait).
    if (FgRequested()) return;
    if (c.phase == 0)
    {
        // A generator that was given realtime keeps it (round 28): measuring
        // again means seconds at the normal class, and under a game that
        // fills the card those seconds are the lag that came and went.
        if (c.want.load() == 1 && FlowWanted()) return;
        if (c.chosen_ms > 0.0 && avg_ms > c.chosen_ms * 1.35 + 5.0)
        {
            if (++c.drift >= 2 && tick - c.chosen_at > 15000)
                LowLatencyTrial("the frames' age moved away from the one it chose on");
        }
        else c.drift = 0;
        if (c.phase == 0 && tick - c.chosen_at > 120000) LowLatencyTrial(nullptr);
        return;
    }
    if (c.settle > 0) { --c.settle; return; }
    if (c.phase == 1)
    {
        c.normal_ms = avg_ms;
        c.normal_rate = real_rate;
        c.phase = 2;
        c.settle = 1;
        c.want = 1;
        return;
    }
    const bool quicker = avg_ms < c.normal_ms - std::max(3.0, 0.08 * c.normal_ms);
    const bool generating = FlowWanted() || FgRequested();
    const bool realtime = generating
        ? (real_rate > c.normal_rate * 1.08 + 1.0 || (quicker && real_rate >= c.normal_rate * 0.97))
        : quicker;
    Trace("[latency] Low Latency measured the card's scheduling classes: normal %.1f ms, realtime %.1f ms - %s kept"
          " (real frames shown: %.0f and %.0f a second)",
          c.normal_ms, avg_ms, realtime ? "realtime (LIVE FLOW's work goes first)" : "normal", c.normal_rate, real_rate);
    c.want = realtime ? 1 : 0;
    c.chosen_ms = realtime ? avg_ms : c.normal_ms;
    c.phase = 0;
    c.drift = 0;
    c.chosen_at = tick;
}

// Before a picture is taken (the loop's thread).
static void LowLatencyWait()
{
    static int priority = -1, choosing = -1;
    const int want_priority = LowLatencyWanted() ? 1 : 0;
    if (want_priority != priority)
    {
        SetThreadPriority(GetCurrentThread(), want_priority ? THREAD_PRIORITY_ABOVE_NORMAL : THREAD_PRIORITY_NORMAL);
        if (want_priority)
            Trace("[latency] Low Latency: each new game frame is processed once, as it arrives; "
                  "the card's scheduling class is measured before one is kept");
        priority = want_priority;
    }
    // The class is measured for FLOW MFG as well, with or without the switch
    // (round 27): at the normal class under a game that fills the card FLOW
    // MFG got 38 of the game's 130 frames a second and showed 63 - the "base
    // 36, output 76" a person saw in a game - and DLSS FG 62.
    //
    // DLSS MFG is not measured at all (round 28): realtime from its first
    // frame, and kept. The measurement began at the normal class, and under a
    // game that fills the card DLSS MFG x3 got 5 real frames a second there -
    // too few for a [latency] summary, so the choice was never made and it
    // stayed at 5; and each later measurement was seconds of that again, the
    // lag that came in waves. LF_FG_CLASS=normal keeps the normal class (to
    // compare).
    static const bool fg_normal = [] {
        char text[16] = {};
        return GetEnvironmentVariableA("LF_FG_CLASS", text, sizeof(text)) > 0 && text[0] == 'n';
    }();
    const int want_choice = FgRequested() ? 2 : ((want_priority || FlowWanted()) ? 1 : 0);
    if (want_choice != choosing)
    {
        std::lock_guard<std::mutex> hold(g_lat.lock);
        if (want_choice == 2)
        {
            g_class.want = fg_normal ? 0 : 1;
            g_class.phase = 0;
            g_class.chosen_ms = 0.0;
            Trace("[latency] DLSS MFG: the card's scheduling class is %s while it runs",
                  fg_normal ? "normal (LF_FG_CLASS)" : "realtime - its frames go first");
        }
        else if (want_choice) LowLatencyTrial(nullptr);
        else { g_class.want = 0; g_class.phase = 0; g_class.chosen_ms = 0.0; }
        choosing = want_choice;
    }
    const int want_class = g_class.want.load();
    if (want_class != g_class.applied && (g_class.applied != -1 || want_class != 0))
    {
        LowLatencyClass(want_class != 0);
        g_class.applied = want_class;
    }
    // DLSS FG owns every Present and its frame-latency wait. Consuming the
    // same signal here starves its presenter: neither thread submits the
    // Present needed to release the next signal. Keep priority handling above.
    if (FgRequested() || !lfShow.low_latency || lfShow.quick_waitable == nullptr || lfShow.chain == nullptr) return;
    {
        std::lock_guard<std::mutex> hold(g_lat.lock);
        if (g_lat.chain != lfShow.chain) { g_lat.chain = lfShow.chain; g_lat.owed = false; }
        if (g_lat.owed) return;
        g_lat.owed = true;
    }
    // A refresh and a half at most: a screen that takes nothing (the window
    // hidden, the screen asleep) must not hold the loop up.
    const DWORD most = static_cast<DWORD>(std::clamp(1500.0 / std::max(OutputRefresh(), 24.0), 4.0, 63.0));
    const double start = LatencyNow();
    const DWORD got = WaitForSingleObject(lfShow.quick_waitable, most);
    std::lock_guard<std::mutex> hold(g_lat.lock);
    g_lat.waited += LatencyNow() - start;
    ++g_lat.waits;
    if (got != WAIT_OBJECT_0) ++g_lat.timeouts;
}

static double LatencyPercentile(std::vector<double> &values, double at)
{
    if (values.empty()) return 0.0;
    std::sort(values.begin(), values.end());
    return values[std::min(values.size() - 1, static_cast<size_t>(at * (values.size() - 1) + 0.5))];
}

// After every Present of the output window (either thread). source: when
// the shown picture's source reached the desktop, in QPC seconds; 0 when the
// frame has none of its own (FLOW MFG's frames in between).
static void LatencyPresented(double source)
{
    IDXGISwapChain3 *chain = lfShow.chain;
    if (chain == nullptr) return;
    const double now = LatencyNow();
    std::lock_guard<std::mutex> hold(g_lat.lock);
    if (lfShow.low_latency) g_lat.owed = false;
    if (g_lat.chain != chain) { g_lat.chain = chain; g_lat.owed = false; memset(g_lat.counts, 0, sizeof(g_lat.counts)); }
    UINT count = 0;
    // Each picture once, at its first Present: shown again with no new
    // capture behind it (the source did not change), its age says nothing
    // about the delay and only inflated the figure.
    const bool first = source > 0.0 && source != g_lat.last_source;
    if (first) { g_lat.last_source = source; ++g_lat.reals; }
    if (first && now - source < 1.0 && SUCCEEDED(chain->GetLastPresentCount(&count)) && count != 0)
    {
        const unsigned at = g_lat.next++ % LatencyState::kRing;
        g_lat.counts[at] = count;
        g_lat.sources[at] = source;
        g_lat.called[at] = now;
    }
    // The newest present the screen showed, and when.
    DXGI_FRAME_STATISTICS st = {};
    if (SUCCEEDED(chain->GetFrameStatistics(&st)) && st.PresentCount != 0 && st.SyncQPCTime.QuadPart != 0)
    {
        LARGE_INTEGER frequency;
        QueryPerformanceFrequency(&frequency);
        const double on_screen = double(st.SyncQPCTime.QuadPart) / double(frequency.QuadPart);
        for (unsigned i = 0; i < LatencyState::kRing; ++i)
        {
            if (g_lat.counts[i] != st.PresentCount) continue;
            const double age = (on_screen - g_lat.sources[i]) * 1000.0;
            if (age > 0.0 && age < 500.0)
            {
                g_lat.shown_ms.push_back(age);
                g_lat.queue_ms.push_back(std::max(0.0, (on_screen - g_lat.called[i]) * 1000.0));
            }
            g_lat.counts[i] = 0;
        }
    }
    const ULONGLONG tick = GetTickCount64();
    if (g_lat.since == 0) g_lat.since = tick;
    if (tick - g_lat.since < 2000) return;
    const bool low = lfShow.low_latency || (LowLatencyWanted() && FlowWanted());
    if (g_lat.shown_ms.size() >= 10)
    {
        const size_t n = g_lat.shown_ms.size();
        double sum = 0.0;
        for (double v : g_lat.shown_ms) sum += v;
        double queue_sum = 0.0;
        for (double v : g_lat.queue_ms) queue_sum += v;
        const double p95 = LatencyPercentile(g_lat.shown_ms, 0.95);
        Trace("[latency] desktop -> screen: avg %.1f ms, p95 %.1f ms over %zu frames; Present -> screen avg %.1f ms"
              "%s%s (waited for the screen %.1f ms per frame%s)",
              sum / n, p95, n, queue_sum / std::max<size_t>(1, g_lat.queue_ms.size()),
              low ? ", Low Latency on" : ", Low Latency off", FlowWanted() ? ", FLOW MFG" : "",
              g_lat.waits ? g_lat.waited * 1000.0 / g_lat.waits : 0.0,
              g_lat.timeouts ? ", some waits ran out" : "");
        if (LowLatencyWanted() || FlowWanted() || FgRequested())
            LowLatencyChoose(sum / n, double(g_lat.reals) * 1000.0 / double(std::max<ULONGLONG>(1, tick - g_lat.since)));
    }
    // Too few frames for a summary while the classes are being measured: a
    // generator starved at the normal class. That is an answer too - without
    // it the measurement never ended and it stayed starved (round 28).
    else if (g_class.phase != 0 && FlowWanted())
        LowLatencyChoose(1000.0, double(g_lat.reals) * 1000.0 / double(std::max<ULONGLONG>(1, tick - g_lat.since)));
    g_lat.shown_ms.clear();
    g_lat.queue_ms.clear();
    g_lat.waited = 0.0;
    g_lat.waits = g_lat.timeouts = 0;
    g_lat.since = tick;
    g_lat.reals = 0;
    g_lat.was_low = low;
}
