// lf_settings.inl - settings the worker reads itself: the LF_ switches the
// engine puts in the environment, the extras word that every reshape
// message carries (sharpness, edges, FLOW MFG, FLOW VIEW, frame limit), the
// capture pacing that the frame limit asks for, and the exposure follower.

// A 0..1 value from the environment (0 when missing).
static float EnvFraction(const char *name)
{
    char text[16] = {};
    const DWORD len = GetEnvironmentVariableA(name, text, sizeof(text));
    const float f = (len > 0 && len < sizeof(text)) ? static_cast<float>(atof(text)) : 0.0f;
    return f > 0.0f ? (f < 1.0f ? f : 1.0f) : 0.0f;
}

// Crisping strength, 0..1.
static float SharpnessNow()
{
    if (lfExtras.sharpness < 0.0f) lfExtras.sharpness = EnvFraction("LF_SHARPNESS");
    return lfExtras.sharpness;
}

// Edge smoothing strength, 0..1.
static float EdgesNow()
{
    if (lfExtras.edges < 0.0f) lfExtras.edges = EnvFraction("LF_EDGE_SMOOTH");
    return lfExtras.edges;
}

// FLOW MFG: frames made between two real ones (0 = off, 1..5 = x2..x6).
static unsigned FlowExtraFrames()
{
    if (lfExtras.flow_count < 0)
    {
        char text[8] = {};
        const DWORD len = GetEnvironmentVariableA("LF_FLOW_MFG", text, sizeof(text));
        const int multiplier = (len > 0 && len < sizeof(text)) ? atoi(text) : 0;
        lfExtras.flow_count = (multiplier >= 2 && multiplier <= 6) ? multiplier - 1 : 0;
    }
    return static_cast<unsigned>(lfExtras.flow_count);
}

// FLOW VIEW: FLOW MFG draws what it sees instead of the picture.
static bool FlowViewShown()
{
    if (lfExtras.flow_view < 0)
    {
        char text[4] = {};
        lfExtras.flow_view = GetEnvironmentVariableA("LF_FLOW_VIEW", text, sizeof(text)) > 0 && text[0] == '1' ? 1 : 0;
    }
    return lfExtras.flow_view != 0;
}

// Keep colours, 0..1 (round 29): how much of the picture's own colour is kept
// where the network's result is put back on it (kDetailHlsl). 0 unless the
// engine said otherwise (round 41; it was 1): LF_KEEP_COLOR at start, bits 24-31 of a reshape's
// flags afterwards (the value in two-hundredths plus one; 0 = not said).
static float g_keep_colour = -1.0f;

static float KeepColour()
{
    if (g_keep_colour < 0.0f)
    {
        char text[16] = {};
        const DWORD len = GetEnvironmentVariableA("LF_KEEP_COLOR", text, sizeof(text));
        g_keep_colour = (len > 0 && len < sizeof(text)) ? std::clamp(static_cast<float>(atof(text)), 0.0f, 1.0f) : 0.0f;
        Trace("[finish] keep colours %.0f%%", g_keep_colour * 100.0f);
    }
    return g_keep_colour;
}

static void KeepColourFromResize(uint32_t flags)
{
    const uint32_t said = flags >> 24;
    if ((flags & kReshapeDisplay) == 0 || said == 0) return;
    const float want = std::clamp((said - 1u) / 200.0f, 0.0f, 1.0f);
    if (std::fabs(want - KeepColour()) > 0.002f) Trace("[finish] keep colours %.0f%%", want * 100.0f);
    g_keep_colour = want;
}

// Low-light noise, 0..1 (round 34): how far the dark parts of the picture,
// and what the network changed there, are smoothed edge-aware before they are
// put together (kDetailHlsl). 0 unless the engine said otherwise: LF_LOW_LIGHT
// at start, bits 3-7 of a reshape's flags afterwards (twentieths plus one).
static float g_low_light = -1.0f;

static float LowLight()
{
    if (g_low_light < 0.0f)
    {
        g_low_light = EnvFraction("LF_LOW_LIGHT");
        if (g_low_light > 0.0f) Trace("[finish] low-light noise %.0f%%", g_low_light * 100.0f);
    }
    return g_low_light;
}

static void LowLightFromResize(uint32_t flags)
{
    const uint32_t said = (flags & kReshapeLowLightMask) >> kReshapeLowLightShift;
    if (said == 0) return;
    const float want = std::clamp((said - 1u) / 20.0f, 0.0f, 1.0f);
    if (std::fabs(want - LowLight()) > 0.002f) Trace("[finish] low-light noise %.0f%%", want * 100.0f);
    g_low_light = want;
}

// The frame limit: most frames a second shown, 0 for none.
static int ShownCap()
{
    int limit = lfExtras.frame_limit.load();
    if (limit < 0)
    {
        char text[16] = {};
        const DWORD len = GetEnvironmentVariableA("LF_FRAME_LIMIT", text, sizeof(text));
        limit = (len > 0 && len < sizeof(text)) ? atoi(text) : 0;
        if (limit < 20 || limit > 360) limit = 0;
        lfExtras.frame_limit = limit;
    }
    return limit;
}

// The extras word of a reshape message: bits 0-7 crisping, 8-15 edges,
// 16-19 FLOW MFG, 20 FLOW VIEW, 21-29 the frame limit, 31 = valid.
static void ReadExtrasWord(uint32_t word)
{
    if ((word & kExtrasValid) == 0) return;
    {
        int limit = static_cast<int>((word >> 21) & 0x1FFu);
        if (limit < 20 || limit > 360) limit = 0;
        if (limit != ShownCap())
            Trace(limit ? "[limit] frame limit %d FPS" : "[limit] no frame limit", limit);
        lfExtras.frame_limit = limit;
    }
    const float sharp = static_cast<float>(word & 0xFFu) / 255.0f;
    const float edge = static_cast<float>((word >> 8) & 0xFFu) / 255.0f;
    const int flow = static_cast<int>(std::min(5u, (word >> 16) & 0xFu));
    if (sharp != SharpnessNow() || edge != EdgesNow())
        Trace("[finish] sharpness %.2f, edge smoothing %.2f", sharp, edge);
    if (flow != static_cast<int>(FlowExtraFrames()))
        Trace(flow > 0 ? "[flow] FLOW MFG x%d requested" : "[flow] off", flow + 1);
    const int view = (word & 0x100000u) != 0 ? 1 : 0;
    if (view != (FlowViewShown() ? 1 : 0)) Trace("[flow] FLOW VIEW %s", view ? "on" : "off");
    lfExtras.sharpness = sharp;
    lfExtras.edges = edge;
    lfExtras.flow_count = flow;
    lfExtras.flow_view = view;
}

// The frame limit's capture side: wait until the next picture may be taken.
// The limit counts shown frames, so with FLOW MFG the network runs at the
// limit divided by its multiplier. Taking the picture only
// when its turn comes keeps it fresh - nothing waits in a queue - which is
// the latency idea behind Reflex and Anti-Lag 2, and leaves the card's
// spare time to the game.
// When the last new picture was taken (QPC ticks) - the loop says so.
static LONGLONG g_pace_taken = 0;
static void PaceTaken()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g_pace_taken = now.QuadPart;
}

static void PaceCapture()
{
    static LARGE_INTEGER frequency = {}, next = {};
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    // DLSS FG limits presentation separately. Dividing capture by x6 made a
    // 45-FPS cap capture at 7.5 FPS, starving motion estimation of real frames.
    const unsigned shown = FlowWanted() ? FlowExtraFrames() + 1 : 1u;
    int limit = ShownCap();
    // VRR keeps shown frames inside the screen's range (lf_vrr.inl). Only
    // when every processed frame is a shown one: FLOW MFG caps its own shown
    // rate instead, and the real frames are never held back for it.
    const int vrr = shown == 1 ? VrrCap() : 0;
    if (vrr > 0 && (limit <= 0 || vrr < limit)) limit = vrr;
    // No limit set and a generator on (round 27): the screen is the limit. A
    // game faster than refresh / multiplier gave more pairs than the screen
    // can show - FLOW MFG dropped a third of them and showed 72 real + 40
    // generated of a 200 FPS game on a 200 Hz screen, less than the game
    // alone and uneven; DLSS FG presented 263. Real frames are taken evenly at
    // what the screen can show multiplied (never under 30 a second), so every
    // pair is shown whole.
    unsigned per_real = shown;
    bool from_last = false;
    // DLSS MFG on a monitor's duplication picks its frames by their own time
    // stamps instead (round 28, lf_fg_pace.inl): the clock here took a game
    // just over the limit later and later until a frame was lost.
    if (limit <= 0 && FgRequested() && lfCap.live && (lfCap.window_dxgi || !lfCap.window_live) && !lfCap.screen_via_wgc)
    { next = now; return; }
    if (limit <= 0)
    {
        const unsigned multiplied = FlowWanted() ? shown : (FgRequested() ? FgExtraFrames() + 1 : 1u);
        const double hz = OutputRefresh();
        if (multiplied > 1 && hz >= 24.0)
        {
            per_real = multiplied;
            // A little under the refresh rate: at exactly one frame a refresh
            // there is no slack at all, and every hiccup costs a frame.
            limit = std::max(static_cast<int>(hz * 0.97), 30 * static_cast<int>(multiplied));
            from_last = true;
        }
    }
    if (limit <= 0) { next = now; return; }
    const LONGLONG step = static_cast<LONGLONG>(frequency.QuadPart * double(per_real) / double(limit));
    // The screen's own limit spaces the pictures taken, counted from the last
    // new one - not a fixed grid: against a grid a game a little slower than
    // it (60 FPS under a 64.7 grid at x3 on 200 Hz) lost every slot that came
    // up empty and was taken at 52.
    if (from_last)
    {
        next.QuadPart = g_pace_taken + step;
        if (now.QuadPart >= next.QuadPart || next.QuadPart - now.QuadPart > step) return;
    }
    else if (next.QuadPart == 0 || now.QuadPart - next.QuadPart > step) next = now;
    static HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
    while (now.QuadPart < next.QuadPart)
    {
        const double left = double(next.QuadPart - now.QuadPart) / double(frequency.QuadPart);
        if (timer && left > .0006)
        {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>((left - .0004) * 1e7);
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 50);
            else Sleep(0);
        }
        else SwitchToThread();
        QueryPerformanceCounter(&now);
    }
    if (!from_last) next.QuadPart += step;
}

// LF_NR_SMALL=1: run the network at the working size and stretch it back.
static bool SmallNetworkWanted()
{
    static int cached = -1;
    if (cached < 0)
    {
        char text[8] = {};
        const DWORD len = GetEnvironmentVariableA("LF_NR_SMALL", text, sizeof(text));
        cached = (len > 0 && len < sizeof(text) && text[0] == '1') ? 1 : 0;
    }
    return cached == 1;
}

// Add the network's change back onto the screen picture (default) instead
// of showing the stretched network output itself.
static bool DetailWanted()
{
    char text[8] = {};
    const DWORD len = GetEnvironmentVariableA("LF_NR_RESIDUAL", text, sizeof(text));
    return !(len > 0 && len < sizeof(text) && text[0] == '0');
}

// How much of that change, 0..2 (1 by default).
// Intensity past 1 (round 38). The network takes 1 at most - measured, 2 gives
// exactly the picture 1 gives - so what is past it scales the change the network
// made, where that change is put back on the picture (kDetailHlsl): 2 shows
// twice what the network did, 3 three times. Up to 1 it is the network's own.
static float NetworkIntensity() { return std::min(lfLink.hello.intensity, 1.0f); }
static float EffectGain() { return std::clamp(lfLink.hello.intensity, 1.0f, 3.0f); }

static float DetailAmount()
{
    char text[16] = {};
    const DWORD len = GetEnvironmentVariableA("LF_NR_RESIDUAL_STRENGTH", text, sizeof(text));
    if (len > 0 && len < sizeof(text))
    {
        const float f = static_cast<float>(atof(text));
        if (f >= 0.0f && f <= 2.0f) return f;
    }
    return 1.0f;
}

// Never let the working size fall under 256x144 (or the screen, if smaller).
static void ClampWorkSize(UINT src_w, UINT src_h, UINT &w, UINT &height)
{
    const UINT mw=std::min(256u,src_w), mh=std::min(144u,src_h);
    if (w>=mw && height>=mh) return;
    const double ratio=std::min(1.,std::max({double(mw)/src_w,double(mh)/src_h,double(w)/src_w,double(height)/src_h}));
    w=std::min(src_w,UINT(ceil(src_w*ratio/2))*2);
    height=std::min(src_h,UINT(ceil(src_h*ratio/2))*2);
}

// The exposure follower, on unless LF_PW=0.
static bool ExposureOn()
{
    char text[8] = {};
    const DWORD len = GetEnvironmentVariableA("LF_PW", text, sizeof(text));
    return !(len > 0 && len < sizeof(text) && text[0] == '0');
}

// One of the follower's LF_PW_* numbers.
static float ExposureSetting(const char *name, float vram_heap)
{
    char text[32] = {};
    const DWORD len = GetEnvironmentVariableA(name, text, sizeof(text));
    if (len > 0 && len < sizeof(text))
    {
        const float f = static_cast<float>(atof(text));
        if (f >= 0.0f) return f;
    }
    return vram_heap;
}

// Follow the screen's average light from the luma section: dark scenes get
// a little more gain, bright ones none, eased over time.
static void ExposureUpdate()
{
    if (!ExposureOn() || !lfLuma.mapped || lfLuma.w == 0 || lfLuma.h == 0)
    {
        lfExposure.gain = 1.0f;
        return;
    }

    static float dark = 0.0f, lit = 0.0f, mn = 0.0f, mx = 0.0f, tau = 0.0f;
    static bool  env_read = false;
    if (!env_read)
    {
        dark = ExposureSetting("LF_PW_DARK", 0.10f);
        lit  = ExposureSetting("LF_PW_LIT", 0.40f);
        mn   = ExposureSetting("LF_PW_MIN", 1.00f);
        mx   = ExposureSetting("LF_PW_MAX", 1.10f);
        tau  = ExposureSetting("LF_PW_TAU", 0.50f);

        if (lit <= dark) lit = dark + 0.30f;
        env_read = true;
    }
    if (!lfExposure.logged)
    {
        Trace("[exposure] following the light: dark %.2f, lit %.2f, range %.2f..%.2f, speed %.2f",
            dark, lit, mn, mx, tau);
        lfExposure.logged = true;
    }

    uint64_t sum = 0;
    const size_t n = static_cast<size_t>(lfLuma.w) * lfLuma.h;
    for (size_t i = 0; i < n; ++i) sum += lfLuma.view[i];
    const float avg = static_cast<float>(
        static_cast<double>(sum) / (255.0 * static_cast<double>(n)));

    float t = (avg - dark) / (lit - dark);
    t = (t < 0.0f) ? 0.0f : (t > 1.0f) ? 1.0f : t;
    const float target = mx - (mx - mn) * (t * t * (3.0f - 2.0f * t));

    const double now = static_cast<double>(GetTickCount64());
    if (tau <= 0.0f || lfExposure.last == 0.0)
    {
        lfExposure.gain = target;
    }
    else
    {
        const float dt = static_cast<float>((now - lfExposure.last) / 1000.0);
        const float a = 1.0f - expf(-dt / tau);
        lfExposure.gain += (target - lfExposure.gain) * a;
    }
    lfExposure.last = now;
}
