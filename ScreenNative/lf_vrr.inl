// lf_vrr.inl - VRR for the output window: the LIVE FLOW switch under FLOW
// Upscaler. One mechanism for every screen with a variable refresh rate -
// G-SYNC, G-SYNC Compatible, FreeSync, HDMI 2.1 VRR.
//
// Round 23, from measurements on a real PC (scratchpad vrr_probe*): the output
// window reaches the screen through a hardware overlay plane, never through
// the desktop compositor, whatever its style. So a tearing present (round 22:
// DXGI_PRESENT_ALLOW_TEARING) really could tear there whenever the screen was
// not in its variable mode - tear lines drifting up the picture at 188 FPS on
// a 200 Hz screen. And on that PC no windowed program got a variable refresh
// at all, a plain focused full-screen window included: the driver was not
// applying VRR to windows.
//
// So now:
//   - VRR presents with sync interval 1 and never with tearing. On a screen
//     in VRR mode that is the standard "G-SYNC + V-SYNC": each frame is shown
//     when it is ready, inside the range. On one that is not, it is ordinary
//     V-SYNC - never a tear line. The chain itself does not change, so the
//     switch no longer rebuilds the window.
//   - What is shown stays a little under the screen's top rate
//     (refresh - refresh^2/3600: 138 on 144 Hz, 188 on 200, 224 on 240); a
//     lower frame limit wins; FLOW MFG caps only its own shown rate.
//   - It checks itself. The frame statistics say when each present reached
//     the screen: with VRR the gaps follow our frames, on a fixed refresh they
//     land on whole refresh periods. After a few seconds of frames that do not
//     fall on the refresh grid themselves it says which ("[vrr] the screen
//     follows the frames" / "[vrr] the screen stays at its fixed ..."), and
//     the app tells the person what to switch on when it is the second.

static int g_vrr = -1;   // -1 until LF_VRR or a reshape has said

static bool VrrSetting()
{
    if (g_vrr < 0)
    {
        char text[4] = {};
        g_vrr = GetEnvironmentVariableA("LF_VRR", text, sizeof(text)) > 0 && text[0] == '1' ? 1 : 0;
        if (g_vrr) Trace("[vrr] asked for from LF_VRR");
    }
    return g_vrr == 1;
}

static void VrrFromResize(uint32_t flags)
{
    if ((flags & kReshapeDisplay) == 0) return;
    const int want = (flags & kReshapeVrr) != 0 ? 1 : 0;
    if (want != (VrrSetting() ? 1 : 0)) Trace("[vrr] VRR %s", want ? "on" : "off");
    g_vrr = want;
}

static bool VrrWanted()
{
    return VrrSetting();
}

// The refresh of the screen the output window is on, asked at most once a
// second (the window can be dragged, or the mode changed).
static double OutputRefresh()
{
    static std::atomic<long long> asked{0};
    static std::atomic<int> hz{60};
    const long long now = static_cast<long long>(GetTickCount64());
    if (now - asked.load() >= 1000)
    {
        asked = now;
        const HWND at = lfShow.hwnd;
        MONITORINFOEXW monitor = {}; monitor.cbSize = sizeof(monitor);
        DEVMODEW mode = {}; mode.dmSize = sizeof(mode);
        if (at != nullptr && GetMonitorInfoW(MonitorFromWindow(at, MONITOR_DEFAULTTONEAREST), &monitor) &&
            EnumDisplaySettingsW(monitor.szDevice, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1)
            hz = static_cast<int>(mode.dmDisplayFrequency);
    }
    return static_cast<double>(hz.load());
}

// The highest rate that stays inside the VRR range.
static double VrrCeiling(double refresh)
{
    return std::max(30.0, refresh - refresh * refresh / 3600.0);
}

// The cap VRR puts on shown frames, 0 while VRR is off.
static int VrrCap()
{
    if (!lfShow.vrr) return 0;
    return static_cast<int>(VrrCeiling(OutputRefresh()));
}

// Does the screen follow our frames? Called after every present while VRR is
// on (one thread presents at a time; the lock only makes sure of it).
static void VrrObserve()
{
    static std::mutex lock;
    static IDXGISwapChain3 *chain = nullptr;
    static UINT last_count = 0;
    static LONGLONG last_sync = 0, last_call = 0;
    static unsigned shown = 0, shown_off = 0, sent = 0, sent_off = 0;
    static int said = -1;            // -1 nothing yet, 0 fixed, 1 follows
    std::lock_guard<std::mutex> hold(lock);
    LARGE_INTEGER freq, now;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    if (chain != lfShow.chain) { chain = lfShow.chain; last_count = 0; last_call = 0; shown = shown_off = sent = sent_off = 0; }
    if (chain == nullptr) return;
    const double period = 1000.0 / OutputRefresh();
    // Gaps past this are pauses (a still picture, a menu), not the pacing.
    const double longest = std::max(3.0 * period, 50.0);
    auto off_grid = [period](double gap_ms) {
        const double steps = std::round(gap_ms / period);
        return steps >= 1.0 && std::fabs(gap_ms - steps * period) > 0.2;
    };
    // Our own presents: do they fall on the refresh grid anyway (a frame rate
    // that divides the refresh tells nothing)?
    if (last_call != 0)
    {
        const double gap = double(now.QuadPart - last_call) * 1000.0 / double(freq.QuadPart);
        if (gap < longest) { ++sent; if (off_grid(gap)) ++sent_off; }
    }
    last_call = now.QuadPart;
    DXGI_FRAME_STATISTICS st = {};
    if (SUCCEEDED(chain->GetFrameStatistics(&st)) && st.PresentCount != last_count)
    {
        if (last_count != 0 && st.PresentCount == last_count + 1)
        {
            const double gap = double(st.SyncQPCTime.QuadPart - last_sync) * 1000.0 / double(freq.QuadPart);
            if (gap > 0.0 && gap < longest) { ++shown; if (off_grid(gap)) ++shown_off; }
        }
        last_count = st.PresentCount;
        last_sync = st.SyncQPCTime.QuadPart;
    }
    if (shown < 240) return;
    // Only frames that are off the grid themselves can show the difference.
    if (sent >= 120 && sent_off * 10 >= sent * 3)
    {
        const int follows = shown_off * 10 >= shown * 3 ? 1 : (shown_off * 20 <= shown ? 0 : -1);
        if (follows != -1 && follows != said)
        {
            said = follows;
            if (follows)
                Trace("[vrr] the screen follows the frames - VRR is working (%u of %u frames off the %.0f Hz grid)",
                    shown_off, shown, 1000.0 / period);
            else
                Trace("[vrr] the screen stays at its fixed %.0f Hz - VRR is not reaching it "
                      "(NVIDIA: G-SYNC for windowed and full screen mode; AMD: FreeSync in AMD Software; "
                      "and Adaptive-Sync on the monitor)", 1000.0 / period);
        }
    }
    shown = shown_off = sent = sent_off = 0;
}

// Present the output window's frame: sync interval 1 with VRR (never a
// tear), 0 otherwise (the compositor's newest-frame behaviour). source: when
// the shown picture's source reached the desktop (lf_latency.inl); -1 is the
// loop's own capture, 0 a frame without one.
static HRESULT ShowPresentNow(double source)
{
    const HRESULT hr = lfShow.chain->Present(lfShow.vrr ? 1u : 0u, 0u);
    if (lfShow.vrr && SUCCEEDED(hr)) VrrObserve();
    if (SUCCEEDED(hr)) LatencyPresented(source < 0.0 ? lfCap.stamp : source);
    if (SUCCEEDED(hr)) CounterPresented(source, source < 0.0);
    return hr;
}
