// lf_desktop.inl - where the picture comes from. Three sources, one bridge:
// Desktop Duplication for a screen on the network's card, Windows Graphics
// Capture for a single window, and Graphics Capture of a whole screen when
// that screen hangs on another card. Every source hands a D3D11 texture to
// BridgeFrame, which moves it to D3D12 through a shared texture and a shared
// fence; SwizzleIntoPicture then puts it in RGBA order in the pipeline.

// Is any source (desktop, window or screen) running.
static bool SourceLive()
{
    return lfCap.live || lfCap.window_live;
}

// The D3D11 device sources deliver on, made on the network's card.
static bool CaptureDeviceReady()
{
    if (lfCap.d11 != nullptr) return true;
    IDXGIFactory1 *dxgi_factory = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&dxgi_factory)))
    { Trace("[source] no DXGI factory"); return false; }

    const int wanted = CardInUse();
    IDXGIAdapter1 *card = nullptr;
    if (FAILED(dxgi_factory->EnumAdapters1(wanted >= 0 ? (UINT)wanted : 0, &card)))
    { Trace("[source] card %d is not there", wanted >= 0 ? wanted : 0); dxgi_factory->Release(); return false; }
    if (wanted >= 0) Trace("[source] capturing on card %d, next to the network", wanted);
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    const HRESULT status = D3D11CreateDevice(card, D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                                         D3D11_SDK_VERSION, &lfCap.d11, nullptr,
                                         &lfCap.d11_ctx);
    card->Release(); dxgi_factory->Release();
    if (FAILED(status)) { Trace("[source] the D3D11 capture device did not start (0x%08X)", status); return false; }
    return true;
}

// The screen called like DXGI's device name (\\.\DISPLAYn), or null.
static HMONITOR MonitorByName(const wchar_t *name)
{
    struct Search { const wchar_t *name; HMONITOR found; } search = { name, nullptr };
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
        auto *s = reinterpret_cast<Search *>(data);
        MONITORINFOEXW info = {};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info) && wcscmp(info.szDevice, s->name) == 0)
        { s->found = monitor; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return search.found;
}

// The output of the card that shows the named screen. When the screen
// exists but hangs on another card, *elsewhere gets it instead and the
// caller reads it through Graphics Capture (LF_MONITOR_WGC=1 forces that).
static HMONITOR g_capture_target_monitor = nullptr;
static IDXGIOutput *FindCaptureOutput(IDXGIAdapter1 *card, HMONITOR *elsewhere)
{
    if (g_capture_target_monitor) {
        for (UINT i = 0; ; ++i) {
            IDXGIOutput *output = nullptr;
            if (FAILED(card->EnumOutputs(i, &output))) break;
            DXGI_OUTPUT_DESC desc = {}; output->GetDesc(&desc);
            if (desc.Monitor == g_capture_target_monitor) return output;
            output->Release();
        }
        if (elsewhere) *elsewhere = g_capture_target_monitor;
        return nullptr;
    }
    wchar_t wanted[64] = {};
    const DWORD len = GetEnvironmentVariableW(L"LF_OUTPUT", wanted, 64);
    UINT index = 0;
    if (len > 0 && len < _countof(wanted))
    {
        bool found = false;
        for (UINT i = 0; ; ++i)
        {
            IDXGIOutput *option = nullptr;

            if (FAILED(card->EnumOutputs(i, &option)) || option == nullptr) break;
            DXGI_OUTPUT_DESC desc = {};
            option->GetDesc(&desc);
            const bool match = wcscmp(desc.DeviceName, wanted) == 0;
            option->Release();
            if (match) { index = i; found = true; break; }
        }

        char forced[4] = {};
        if (found && elsewhere && GetEnvironmentVariableA("LF_MONITOR_WGC", forced, sizeof(forced)) == 1 && forced[0] == '1')
        {
            Trace("[desk] LF_MONITOR_WGC=1: %ls goes through Windows Graphics Capture on purpose", wanted);
            *elsewhere = MonitorByName(wanted);
            if (*elsewhere) return nullptr;
        }
        if (found)
            Trace("[desk] screen %u picked by name (%ls)", index, wanted);
        else if (HMONITOR other = elsewhere ? MonitorByName(wanted) : nullptr)
        {
            Trace("[desk] %ls hangs on another card - taking it through Windows Graphics Capture", wanted);
            *elsewhere = other;
            return nullptr;
        }
        else
            Trace("[desk] no screen called %ls anywhere - taking this card's first screen", wanted);
    }
    IDXGIOutput *output = nullptr;
    if (FAILED(card->EnumOutputs(index, &output))) return nullptr;
    return output;
}

// Start taking the desktop at w x hgt: duplication of the screen named in
// LF_OUTPUT, in half float when that screen is in HDR.
static bool DeskStart(UINT w, UINT hgt)
{
    WindowStop();
    DeskStop();
    lfCap.window = nullptr;
    if (w == 0 || hgt == 0) { Trace("[desk] desktop source stopped"); return true; }
    if (!SwizzleReady()) return false;
    if (!CaptureDeviceReady()) return false;
    IDXGIFactory1 *dxgi_factory = nullptr;
    HRESULT status = CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&dxgi_factory);
    if (FAILED(status)) { Trace("[desk] no DXGI factory (0x%08X)", status); return false; }

    const int want_dda = CardInUse();
    IDXGIAdapter1 *card = nullptr;
    if (FAILED(dxgi_factory->EnumAdapters1(want_dda >= 0 ? (UINT)want_dda : 0, &card)))
    { Trace("[desk] card %d is not there", want_dda >= 0 ? want_dda : 0); dxgi_factory->Release(); return false; }
    HMONITOR elsewhere = nullptr;
    IDXGIOutput *output = FindCaptureOutput(card, &elsewhere);
    if (output == nullptr && elsewhere != nullptr)
    {
        card->Release(); dxgi_factory->Release();
        return ScreenViaWgcStart(elsewhere);
    }
    if (output == nullptr) { Trace("[desk] this card has no screen to duplicate"); card->Release(); dxgi_factory->Release(); return false; }

    {
        IDXGIOutput6 *out6 = nullptr;
        if (SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput6), (void **)&out6)))
        {
            DXGI_OUTPUT_DESC1 d1 = {};
            if (SUCCEEDED(out6->GetDesc1(&d1)))
            {
                const bool hdr =
                    d1.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 ||
                    d1.ColorSpace == DXGI_COLOR_SPACE_RGB_STUDIO_G2084_NONE_P2020 ||
                    d1.ColorSpace == DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_LEFT_P2020 ||
                    d1.ColorSpace == DXGI_COLOR_SPACE_YCBCR_STUDIO_G2084_TOPLEFT_P2020;

                Trace("[desk] screen colour space %d at %u bits%s",
                    (int)d1.ColorSpace, d1.BitsPerColor,
                    hdr ? " - this screen is in HDR" : "");
            }
            out6->Release();
        }
    }
    IDXGIOutput1 *output1 = nullptr;
    if (FAILED(output->QueryInterface(__uuidof(IDXGIOutput1), (void **)&output1)))
    { Trace("[desk] IDXGIOutput1 is missing"); output->Release(); card->Release(); dxgi_factory->Release(); return false; }
    DXGI_OUTPUT_DESC output_desc = {};
    output->GetDesc(&output_desc);
    lfCap.monitor = output_desc.Monitor;
    lfCap.display = HdrDisplayOf(lfCap.monitor);

    lfCap.opened_hdr = lfCap.display.hdr_on;
    IDXGIOutput5 *output5 = nullptr;
    status = E_FAIL;
    if (lfCap.opened_hdr && SUCCEEDED(output->QueryInterface(__uuidof(IDXGIOutput5), (void **)&output5)))
    {
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM};
        status = output5->DuplicateOutput1(lfCap.d11, 0, _countof(formats), formats, &lfCap.duplication);
        output5->Release();
        if (FAILED(status))
            Trace("[hdr] half-float duplication refused (0x%08X) - taking the screen in SDR", status);
    }
    else if (lfCap.opened_hdr)
        Trace("[hdr] IDXGIOutput5 is missing on this Windows - taking the screen in SDR");
    if (FAILED(status))
    {
        status = output1->DuplicateOutput(lfCap.d11, &lfCap.duplication);
    }
    output1->Release(); output->Release(); card->Release(); dxgi_factory->Release();
    if (FAILED(status)) { Trace("[desk] duplication refused (0x%08X)", status); return false; }

    {
        DXGI_OUTDUPL_DESC dd = {};
        lfCap.duplication->GetDesc(&dd);
        static const char *kRot[] = { "unspecified", "none", "90", "180", "270" };
        const unsigned r = (unsigned)dd.Rotation;

        lfCap.upside_down = (r == DXGI_MODE_ROTATION_ROTATE180);
        const bool unhandled = (r == DXGI_MODE_ROTATION_ROTATE90 ||
                                r == DXGI_MODE_ROTATION_ROTATE270);
        Trace("[desk] screen rotation %s%s", r < 5 ? kRot[r] : "?",
            lfCap.upside_down ? " - flipped upright while copying"
            : unhandled ? " - not supported: a portrait screen swaps "
                          "width and height, the picture will be wrong"
                        : "");
    }
    lfCap.w = w; lfCap.h = hgt; lfCap.live = true;
    Trace("[desk] taking the desktop at %ux%u", w, hgt);
    return true;
}

// Stop the desktop source and free the bridge it used.
static void DeskStop()
{
    // The HDR export textures can still be used by a generation presenter.
    CloseFgResources();
    CloseFlow();
    if (PhasesOn()) { ++lfProf.capture_gen; lfProf.last_source_qpc = 0; lfProf.stamp = {}; }
    ScreenViaWgcStop();
    lfCap.live = false;
    lfCap.has_frame = false;
    lfHdr.keep = false;
    lfHdr.float_frames = false;
    HdrRelease();
    if (lfCap.bridge12) { lfCap.bridge12->Release(); lfCap.bridge12 = nullptr; }
    if (lfCap.bridge_handle)  { CloseHandle(lfCap.bridge_handle); lfCap.bridge_handle = nullptr; }
    if (lfCap.bridge11) { lfCap.bridge11->Release(); lfCap.bridge11 = nullptr; }
    if (lfCap.duplication) { lfCap.duplication->Release(); lfCap.duplication = nullptr; }
    if (lfCap.d11_ctx) { lfCap.d11_ctx->Release(); lfCap.d11_ctx = nullptr; }
    if (lfCap.d11) { lfCap.d11->Release(); lfCap.d11 = nullptr; }
    if (lfCap.sync12) { lfCap.sync12->Release(); lfCap.sync12 = nullptr; }
    if (lfCap.sync11) { lfCap.sync11->Release(); lfCap.sync11 = nullptr; }
    if (lfCap.sync_event) { CloseHandle(lfCap.sync_event); lfCap.sync_event = nullptr; }

    if (lfCap.rgba) { lfCap.rgba->Release(); lfCap.rgba = nullptr; }
}

// Changes outside the selected client (taskbar, another window, etc.) do not
// need a new motion estimate or NGX evaluation. On metadata errors, capture.
static bool DxgiTouchesClient(const DXGI_OUTDUPL_FRAME_INFO &info)
{
    if (!lfCap.window_dxgi || !info.TotalMetadataBufferSize) return true;
    const RECT &client = lfCap.window_crop;
    auto touches = [&](const RECT &r) { RECT overlap = {}; return IntersectRect(&overlap, &r, &client) != FALSE; };
    std::vector<RECT> dirty((info.TotalMetadataBufferSize + sizeof(RECT)-1)/sizeof(RECT));
    UINT bytes = 0;
    if (FAILED(lfCap.duplication->GetFrameDirtyRects(UINT(dirty.size()*sizeof(RECT)), dirty.data(), &bytes))) return true;
    for (UINT i=0;i<bytes/sizeof(RECT);++i) if (touches(dirty[i])) return true;
    std::vector<DXGI_OUTDUPL_MOVE_RECT> moves((info.TotalMetadataBufferSize + sizeof(DXGI_OUTDUPL_MOVE_RECT)-1)/sizeof(DXGI_OUTDUPL_MOVE_RECT));
    if (FAILED(lfCap.duplication->GetFrameMoveRects(UINT(moves.size()*sizeof(DXGI_OUTDUPL_MOVE_RECT)), moves.data(), &bytes))) return true;
    for (UINT i=0;i<bytes/sizeof(DXGI_OUTDUPL_MOVE_RECT);++i) {
        const auto &m=moves[i];
        const RECT from={m.SourcePoint.x,m.SourcePoint.y,m.SourcePoint.x+m.DestinationRect.right-m.DestinationRect.left,
                         m.SourcePoint.y+m.DestinationRect.bottom-m.DestinationRect.top};
        if (touches(from) || touches(m.DestinationRect)) return true;
    }
    return false;
}

// The newest desktop picture into the pipeline; false when nothing changed.
static bool DeskTake(Pipeline &pipe)
{
    CaptureGuard guard;
    if (g_snapshot_frozen) return false;
    if (!lfCap.live) return false;
    lfCap.picture_changed = false;
    const HWND target = lfCap.window_dxgi ? lfCap.window : nullptr;
    auto reopen = [&](UINT w, UINT h) {
        return target ? WindowStart(target) : DeskStart(w, h);
    };
    static ULONGLONG last_mode_query = 0;
    if (GetTickCount64() - last_mode_query > 1000)
    {
        lfCap.display = HdrDisplayOf(lfCap.monitor);
        last_mode_query = GetTickCount64();
        if (lfCap.opened_hdr != lfCap.display.hdr_on)
        {
            Trace("[hdr] the screen switched HDR mode - reopening the source");
            reopen(lfCap.w, lfCap.h);
            return false;
        }
    }
    if (lfCap.screen_via_wgc) return ScreenViaWgcTake(pipe);
    IDXGIResource *res = nullptr;
    DXGI_OUTDUPL_FRAME_INFO fi = {};
    const double take_start = PhaseClock();
    HRESULT status = lfCap.duplication->AcquireNextFrame(100, &fi, &res);
    // DLSS MFG under a game faster than the screen can show multiplied takes
    // every second (third) frame, by the frames' own time stamps
    // (lf_fg_pace.inl): one that is not taken is let go here and the next one
    // waited for.
    static bool let_go = false;
    for (int tries = 0; SUCCEEDED(status) && FgRequested() && fi.LastPresentTime.QuadPart != 0 && tries < 8; ++tries)
    {
        if (lfCap.window_dxgi && !DxgiTouchesClient(fi)) break;
        LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
        // With VRR what is shown stays inside the screen's range (lf_vrr.inl):
        // its cap is the room there is, not the refresh rate and a little over.
        const int vrr_cap = lfShow.vrr ? VrrCap() : 0;
        const double room = vrr_cap > 0 ? vrr_cap / 1.04 : OutputRefresh();
        if (FgGateTakes(double(fi.LastPresentTime.QuadPart) / frequency.QuadPart, FgExtraFrames(), room,
                        ShownCap() > 0))
        { let_go = false; break; }
        if (res) { res->Release(); res = nullptr; }
        lfCap.duplication->ReleaseFrame();
        let_go = true;
        status = lfCap.duplication->AcquireNextFrame(50, &fi, &res);
    }
    PhaseCount(PHASE_TAKE, take_start);
    if (status == DXGI_ERROR_WAIT_TIMEOUT)
    {
        // The game stopped right after a frame that was let go: the picture
        // on the screen would stay one frame behind. The duplication is
        // opened again - its first frame is the desktop as it is now.
        if (let_go)
        {
            let_go = false;
            FgGateForget();
            reopen(lfCap.w, lfCap.h);
        }
        return false;
    }
    if (FAILED(status))
    {
        Trace("[desk] frame wait failed (0x%08X) - reopening", status);
        reopen(lfCap.w, lfCap.h);
        return false;
    }

    lfCap.picture_changed = fi.LastPresentTime.QuadPart != 0;
    if (lfCap.window_dxgi && (!lfCap.picture_changed || !DxgiTouchesClient(fi))) {
        if (res) res->Release();
        lfCap.duplication->ReleaseFrame();
        lfCap.picture_changed = false;
        return false;
    }
    if (lfCap.picture_changed) {
        LARGE_INTEGER frequency; QueryPerformanceFrequency(&frequency);
        lfCap.stamp = double(fi.LastPresentTime.QuadPart) / frequency.QuadPart;
    }
    TimingCapture(take_start, static_cast<UINT64>(fi.LastPresentTime.QuadPart), "dda");
    ID3D11Texture2D *frame = nullptr;
    if (FAILED(res->QueryInterface(__uuidof(ID3D11Texture2D), (void **)&frame)))
    {
        lfCap.duplication->ReleaseFrame();
        res->Release();
        return false;
    }
    UINT new_w = 0, new_h = 0;
    DXGI_FORMAT new_format = DXGI_FORMAT_UNKNOWN;
    const BridgeResult st = BridgeFrame(frame, &new_w, &new_h, &new_format);
    frame->Release();
    res->Release();

    lfCap.duplication->ReleaseFrame();

    if (st == BridgeResult::NewFormat) return false;
    if (st == BridgeResult::NewSize)
    {
        Trace("[desk] the desktop is now %ux%u, format %u - reopening",
            new_w, new_h, (unsigned)new_format);
        reopen(new_w, new_h);
        return false;
    }
    if (st != BridgeResult::Done) return false;
    return SwizzleIntoPicture(pipe);
}

// DXGI duplicates a monitor; restrict the GPU copy to the selected client's
// rectangle. Rotated, off-screen and cross-adapter windows retain WGC.
static bool WindowClientOnMonitor(HWND hwnd, RECT &crop, HMONITOR &monitor)
{
    RECT client = {}; POINT origin = {};
    MONITORINFO mi = {sizeof(mi)};
    monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
    if (!GetClientRect(hwnd, &client) || !ClientToScreen(hwnd, &origin) ||
        !GetMonitorInfoW(monitor, &mi) || client.right <= 0 || client.bottom <= 0) return false;
    crop = {origin.x - mi.rcMonitor.left, origin.y - mi.rcMonitor.top,
            origin.x - mi.rcMonitor.left + client.right, origin.y - mi.rcMonitor.top + client.bottom};
    return crop.left >= 0 && crop.top >= 0 &&
        crop.right <= mi.rcMonitor.right - mi.rcMonitor.left &&
        crop.bottom <= mi.rcMonitor.bottom - mi.rcMonitor.top;
}

// Does the cut-out take in the whole of its screen.
static bool CropIsWholeScreen(const RECT &crop, HMONITOR monitor)
{
    MONITORINFO mi = {sizeof(mi)};
    return GetMonitorInfoW(monitor, &mi) && crop.left == 0 && crop.top == 0 &&
        crop.right == mi.rcMonitor.right - mi.rcMonitor.left && crop.bottom == mi.rcMonitor.bottom - mi.rcMonitor.top;
}

static bool WindowDxgiStart(HWND hwnd)
{
    RECT crop = {}; HMONITOR monitor = nullptr;
    if (!WindowClientOnMonitor(hwnd, crop, monitor)) return false;
    // Only a window that is its whole screen (round 31). A cut-out of the
    // desktop counts whatever changes over the window's place as a frame of
    // the game - LIVE FLOW's own output among it, when the compositor draws
    // it there: a 15 FPS game was read as 50, and at 20 FPS the extra
    // "frames" were a wave in DLSS MFG's pace (4.6 ms, up to 20, against 0.3
    // for a window over the whole screen). Any other window is taken by
    // Graphics Capture, which hands over that window's own frames only.
    if (!CropIsWholeScreen(crop, monitor)) return false;
    g_capture_target_monitor = monitor;
    const bool opened = DeskStart(crop.right - crop.left, crop.bottom - crop.top);
    g_capture_target_monitor = nullptr;
    DXGI_OUTDUPL_DESC desc = {};
    if (lfCap.duplication) lfCap.duplication->GetDesc(&desc);
    if (!opened || !lfCap.duplication || (desc.Rotation != DXGI_MODE_ROTATION_IDENTITY &&
                                        desc.Rotation != DXGI_MODE_ROTATION_UNSPECIFIED)) {
        DeskStop(); return false;
    }
    lfCap.window = hwnd;
    lfCap.window_live = lfCap.window_dxgi = true;
    lfCap.window_crop = crop;
    ShowAffinityApply();
    Trace("[window] DXGI client capture %p at %ux%u (crop %ld,%ld)",
          (void *)hwnd, lfCap.w, lfCap.h, crop.left, crop.top);
    return true;
}

// Prefer DXGI for MFG's focused window; WGC remains the compatibility fallback.
static bool WindowStart(HWND hwnd)
{
    WindowStop();
    if (hwnd == nullptr) { Trace("[window] window source stopped"); return true; }
    if (!IsWindow(hwnd)) { Trace("[window] %p is no window", (void *)hwnd); return false; }
    char api[8] = {};
    GetEnvironmentVariableA("LF_WINDOW_API", api, sizeof(api));
    // Linked to MFG SUPER MOTION, whose output lies over the game: its own
    // frames only (lf_msm_link.inl).
    if (_stricmp(api, "wgc") != 0 && !SuperResWanted() && !FillWanted() && !MsmLinkWanted() && WindowDxgiStart(hwnd)) return true;
    Trace("[window] taken by Graphics Capture (the window's own frames)");
    if (!wgc_ns::GraphicsCaptureSession::IsSupported())
    { Trace("[window] this Windows has no Graphics Capture"); return false; }
    if (!SwizzleReady()) return false;
    if (!CaptureDeviceReady()) return false;

    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    catch (winrt::hresult_error const &) {}

    WgcLink *s = new WgcLink();
    try
    {
        IDXGIDevice *dxgi = nullptr;
        if (FAILED(lfCap.d11->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi)))
        { Trace("[window] IDXGIDevice is missing"); delete s; return false; }
        winrt::com_ptr<::IInspectable> insp;
        const HRESULT status = CreateDirect3D11DeviceFromDXGIDevice(dxgi, insp.put());
        dxgi->Release();
        if (FAILED(status))
        { Trace("[window] could not hand the device to WinRT (0x%08X)", status); delete s; return false; }
        s->device = insp.as<wgdx_ns::Direct3D11::IDirect3DDevice>();
        auto capture_interop = winrt::get_activation_factory<wgc_ns::GraphicsCaptureItem>()
                           .as<::IGraphicsCaptureItemInterop>();
        const HRESULT item_status = capture_interop->CreateForWindow(
            hwnd, winrt::guid_of<wgc_ns::GraphicsCaptureItem>(),
            winrt::put_abi(s->item));
        if (FAILED(item_status) || s->item == nullptr)
        { Trace("[window] the window cannot be captured (0x%08X)", item_status); delete s; return false; }
        const auto size = s->item.Size();
        if (size.Width <= 0 || size.Height <= 0)
        { Trace("[window] the window has no size - minimised?"); delete s; return false; }
        lfCap.monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        lfCap.display = HdrDisplayOf(lfCap.monitor);

        s->hdr = lfCap.display.hdr_on;
        s->pool = wgc_ns::Direct3D11CaptureFramePool::CreateFreeThreaded(
            s->device, s->hdr ? wgdx_ns::DirectXPixelFormat::R16G16B16A16Float
                                    : wgdx_ns::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        s->mailbox = std::make_shared<FrameDrop>();
        s->mailbox->latest_only = FlowWanted() || FgRequested() || LowLatencyWanted();
        if (s->mailbox->arrived) {
            s->arrival_token = s->pool.FrameArrived([box = s->mailbox](auto const &pool, auto const &) {
                try {
                    if (box->latest_only) {
                        std::lock_guard<std::mutex> lock(box->mutex);
                        for (int n = 0; n < 8; ++n) {
                            auto frame = pool.TryGetNextFrame();
                            if (!frame) break;
                            if (box->latest) box->latest.Close();
                            box->latest = frame;
                        }
                    }
                    SetEvent(box->arrived.get());
                } catch (winrt::hresult_error const &) {   }
            });
        }
        s->session = s->pool.CreateCaptureSession(s->item);
        try { s->session.IsCursorCaptureEnabled(false); }
        catch (winrt::hresult_error const &) { Trace("[window] the pointer stays in the picture"); }

        try { s->session.IsBorderRequired(false); }
        catch (winrt::hresult_error const &) { Trace("[window] Windows keeps its capture border"); }
        // Every frame the window shows, as soon as it shows it (round 31).
        // Left to itself Graphics Capture hands frames over on a slow beat of
        // its own: a game drawing evenly every 50 ms arrived 40, 60, 40, 60 ms
        // apart, and DLSS MFG's frames in between were paced to that.
        try { s->session.MinUpdateInterval(std::chrono::milliseconds(1)); }
        catch (winrt::hresult_error const &) { Trace("[window] this Windows sets the capture's pace itself"); }
        s->session.StartCapture();
        lfCap.window_link = s;
        lfCap.window = hwnd;
        lfCap.window_live = true;

        ShowAffinityApply();
        lfCap.w = (UINT)size.Width;
        lfCap.h = (UINT)size.Height;
        Trace("[window] taking window %p at %dx%d", (void *)hwnd, size.Width, size.Height);
        return true;
    }
    catch (winrt::hresult_error const &e)
    {
        Trace("[window] opening threw 0x%08X", (unsigned)e.code());
        delete s;
        return false;
    }
}

// Stop the window source.
static void WindowStop()
{
    lfCap.window_live = false;
    lfCap.window_dxgi = false;
    lfShow.follow_rect = RECT{};

    ShowAffinityApply();
    if (!lfShow.visible && lfShow.hwnd != nullptr && lfShow.revealed)
    {
        ShowWindow(lfShow.hwnd, SW_SHOWNOACTIVATE);
        lfShow.visible = true;
    }
    if (lfCap.window_link != nullptr)
    {
        try
        {
            if (lfCap.window_link->pool != nullptr && lfCap.window_link->arrival_token.value)
                lfCap.window_link->pool.FrameArrived(lfCap.window_link->arrival_token);
            if (lfCap.window_link->session != nullptr) lfCap.window_link->session.Close();
            if (lfCap.window_link->pool != nullptr) lfCap.window_link->pool.Close();
        }
        catch (winrt::hresult_error const &) {   }
        delete lfCap.window_link;
        lfCap.window_link = nullptr;
        Trace("[window] window source closed");
    }
    DeskStop();
}

// The newest picture of the window, always the newest of the pool.
static bool WindowTake(Pipeline &pipe)
{
    if (lfCap.window_live && lfCap.window_dxgi) {
        lfCap.picture_changed = false;
        WindowTrack();
        if (!IsWindow(lfCap.window) || IsIconic(lfCap.window)) { Sleep(8); return false; }
        const HWND front = GetForegroundWindow();
        DWORD source_pid = 0, front_pid = 0;
        GetWindowThreadProcessId(lfCap.window, &source_pid);
        GetWindowThreadProcessId(front, &front_pid);
        if (front_pid != source_pid) { Sleep(8); return false; }
        RECT crop = {}; HMONITOR monitor = nullptr;
        // No longer the whole screen (the game left its borderless size): it
        // is taken again, by Graphics Capture this time (WindowDxgiStart).
        if (!WindowClientOnMonitor(lfCap.window, crop, monitor) || monitor != lfCap.monitor ||
            !CropIsWholeScreen(crop, monitor)) {
            const HWND hwnd = lfCap.window; WindowStart(hwnd); return false;
        }
        lfCap.window_crop = crop;
        return DeskTake(pipe);
    }
    if (!lfCap.window_live || lfCap.window_link == nullptr) return false;
    lfCap.picture_changed = false;
    const HMONITOR monitor = MonitorFromWindow(lfCap.window, MONITOR_DEFAULTTONEAREST);
    static ULONGLONG last_mode_query = 0;
    if (monitor != lfCap.monitor || GetTickCount64() - last_mode_query > 1000)
    {
        lfCap.monitor = monitor;
        lfCap.display = HdrDisplayOf(monitor);
        last_mode_query = GetTickCount64();
        if (lfCap.window_link->hdr != lfCap.display.hdr_on)
        {
            const HWND hwnd = lfCap.window;
            Trace("[hdr] the window's screen switched HDR mode - reopening");
            WindowStart(hwnd);
            return false;
        }
    }
    try
    {
        const double take_start = PhaseClock();
        auto box = lfCap.window_link->mailbox;
        box->latest_only = FlowWanted() || FgRequested() || LowLatencyWanted();
        auto newest = [&]() {
            std::lock_guard<std::mutex> lock(box->mutex);
            auto frame = box->latest;
            box->latest = nullptr;

            for (int n = 0; n < 8; ++n) {
                auto next = lfCap.window_link->pool.TryGetNextFrame();
                if (!next) break;
                if (frame) frame.Close();
                frame = next;
            }
            return frame;
        };
        auto frame = newest();

        // FLOW MFG and Low Latency skip a frame with nothing new: wait a
        // little for one instead of answering at once (the engine would ask
        // again straight away - a loop spinning thousands of times a second).
        // Everything else waits for one too (round 31), as long as the desktop
        // source does (AcquireNextFrame): with nothing new the answer came at
        // once and the engine asked again - the network ran three times over
        // on the same picture of a window (156 a second for 51 shown), taking
        // the card from the game, and DLSS MFG's loop span between frames.
        if (frame == nullptr && box->arrived) {
            WaitForSingleObject(box->arrived.get(), (FlowWanted() || LowLatencyWanted()) ? 8 : 100);
            frame = newest();
        }

        PhaseCount(PHASE_TAKE, take_start);

        if (frame == nullptr) return false;
        lfCap.stamp = frame.SystemRelativeTime().count() * 1e-7;
        TimingCapture(take_start, 0, "wgc");
        lfCap.picture_changed = true;
        auto surface_access = frame.Surface().as<IDxgiAccessBridge>();
        ID3D11Texture2D *tex = nullptr;
        if (FAILED(surface_access->GetInterface(__uuidof(ID3D11Texture2D), (void **)&tex)) ||
            tex == nullptr)
        { frame.Close(); return false; }
        UINT new_w = 0, new_h = 0;
        DXGI_FORMAT new_format = DXGI_FORMAT_UNKNOWN;
        const BridgeResult st = BridgeFrame(tex, &new_w, &new_h, &new_format);
        tex->Release();
        frame.Close();
        if (st == BridgeResult::NewSize)
        {
            static UINT last_w = 0, last_h = 0;
            static ULONGLONG first_seen = 0;
            const ULONGLONG now = GetTickCount64();
            if (new_w != last_w || new_h != last_h)
            {
                last_w = new_w; last_h = new_h; first_seen = now;
                return false;
            }
            if (now - first_seen < 250)
                return false;
            Trace("[window] the window came to rest at %ux%u, format %u - reopening",
                new_w, new_h, (unsigned)new_format);
            last_w = last_h = 0; first_seen = 0;
            WindowStart(lfCap.window);
            return false;
        }
        if (st != BridgeResult::Done) return false;
        return SwizzleIntoPicture(pipe);
    }
    catch (winrt::hresult_error const &e)
    {
        Trace("[window] a frame threw 0x%08X - source closed", (unsigned)e.code());
        WindowStop();
        return false;
    }
}

// Take a whole screen through Graphics Capture: used for a screen the
// network's card does not drive, where duplication cannot reach.
// Windows moves the frames between the cards.
static bool ScreenViaWgcStart(HMONITOR monitor)
{
    if (!wgc_ns::GraphicsCaptureSession::IsSupported())
    { Trace("[desk] no Windows Graphics Capture here - that screen cannot be taken"); return false; }
    try { winrt::init_apartment(winrt::apartment_type::multi_threaded); }
    catch (winrt::hresult_error const &) {}
    WgcLink *s = new WgcLink();
    try
    {
        IDXGIDevice *dxgi = nullptr;
        if (FAILED(lfCap.d11->QueryInterface(__uuidof(IDXGIDevice), (void **)&dxgi)))
        { Trace("[desk] IDXGIDevice is missing for the screen source"); delete s; return false; }
        winrt::com_ptr<::IInspectable> insp;
        const HRESULT status = CreateDirect3D11DeviceFromDXGIDevice(dxgi, insp.put());
        dxgi->Release();
        if (FAILED(status)) { Trace("[desk] could not hand the device to WinRT (0x%08X)", status); delete s; return false; }
        s->device = insp.as<wgdx_ns::Direct3D11::IDirect3DDevice>();
        auto capture_interop = winrt::get_activation_factory<wgc_ns::GraphicsCaptureItem>()
                           .as<::IGraphicsCaptureItemInterop>();
        const HRESULT item_status = capture_interop->CreateForMonitor(
            monitor, winrt::guid_of<wgc_ns::GraphicsCaptureItem>(), winrt::put_abi(s->item));
        if (FAILED(item_status) || s->item == nullptr)
        { Trace("[desk] the screen cannot be captured (0x%08X)", item_status); delete s; return false; }
        const auto size = s->item.Size();
        if (size.Width <= 0 || size.Height <= 0)
        { Trace("[desk] the screen reports no size"); delete s; return false; }
        lfCap.monitor = monitor;
        lfCap.display = HdrDisplayOf(monitor);

        lfCap.opened_hdr = lfCap.display.hdr_on;
        s->hdr = lfCap.display.hdr_on;
        s->pool = wgc_ns::Direct3D11CaptureFramePool::CreateFreeThreaded(
            s->device, s->hdr ? wgdx_ns::DirectXPixelFormat::R16G16B16A16Float
                              : wgdx_ns::DirectXPixelFormat::B8G8R8A8UIntNormalized, 2, size);
        s->mailbox = std::make_shared<FrameDrop>();
        s->mailbox->latest_only = true;
        if (s->mailbox->arrived)
        {
            s->arrival_token = s->pool.FrameArrived([box = s->mailbox](auto const &pool, auto const &) {
                try
                {
                    std::lock_guard<std::mutex> lock(box->mutex);
                    for (int n = 0; n < 8; ++n)
                    {
                        auto frame = pool.TryGetNextFrame();
                        if (!frame) break;
                        if (box->latest) box->latest.Close();
                        box->latest = frame;
                    }
                    SetEvent(box->arrived.get());
                }
                catch (winrt::hresult_error const &) {   }
            });
        }
        s->session = s->pool.CreateCaptureSession(s->item);

        try { s->session.IsCursorCaptureEnabled(false); }
        catch (winrt::hresult_error const &) { Trace("[desk] the pointer stays in the screen picture"); }
        try { s->session.IsBorderRequired(false); }
        catch (winrt::hresult_error const &) { Trace("[desk] Windows keeps its capture border"); }
        s->session.StartCapture();
        lfCap.screen_link = s;
        lfCap.screen_via_wgc = true;
        lfCap.upside_down = false;
        lfCap.w = (UINT)size.Width;
        lfCap.h = (UINT)size.Height;
        lfCap.live = true;
        Trace("[desk] taking the desktop at %ux%u (through Windows Graphics Capture)", lfCap.w, lfCap.h);
        return true;
    }
    catch (winrt::hresult_error const &e)
    {
        Trace("[desk] the screen source threw 0x%08X", (unsigned)e.code());
        delete s;
        return false;
    }
}

// Stop the screen source.
static void ScreenViaWgcStop()
{
    lfCap.screen_via_wgc = false;
    if (lfCap.screen_link == nullptr) return;
    try
    {
        if (lfCap.screen_link->pool != nullptr && lfCap.screen_link->arrival_token.value)
            lfCap.screen_link->pool.FrameArrived(lfCap.screen_link->arrival_token);
        if (lfCap.screen_link->session != nullptr) lfCap.screen_link->session.Close();
        if (lfCap.screen_link->pool != nullptr) lfCap.screen_link->pool.Close();
    }
    catch (winrt::hresult_error const &) {   }
    delete lfCap.screen_link;
    lfCap.screen_link = nullptr;
    Trace("[desk] screen source (Graphics Capture) closed");
}

// The newest screen picture; waits up to 100 ms like duplication does.
static bool ScreenViaWgcTake(Pipeline &pipe)
{
    if (lfCap.screen_link == nullptr) return false;
    try
    {
        const double take_start = PhaseClock();
        auto box = lfCap.screen_link->mailbox;
        auto newest = [&]() {
            std::lock_guard<std::mutex> lock(box->mutex);
            auto frame = box->latest;
            box->latest = nullptr;
            for (int n = 0; n < 8; ++n)
            {
                auto next = lfCap.screen_link->pool.TryGetNextFrame();
                if (!next) break;
                if (frame) frame.Close();
                frame = next;
            }
            return frame;
        };
        auto frame = newest();

        if (frame == nullptr && box->arrived)
        {
            WaitForSingleObject(box->arrived.get(), 100);
            frame = newest();
        }
        PhaseCount(PHASE_TAKE, take_start);
        if (frame == nullptr) return false;
        lfCap.stamp = frame.SystemRelativeTime().count() * 1e-7;
        TimingCapture(take_start, 0, "wgc-monitor");
        lfCap.picture_changed = true;
        auto surface_access = frame.Surface().as<IDxgiAccessBridge>();
        ID3D11Texture2D *tex = nullptr;
        if (FAILED(surface_access->GetInterface(__uuidof(ID3D11Texture2D), (void **)&tex)) || tex == nullptr)
        { frame.Close(); return false; }
        UINT new_w = 0, new_h = 0;
        DXGI_FORMAT new_format = DXGI_FORMAT_UNKNOWN;
        const BridgeResult st = BridgeFrame(tex, &new_w, &new_h, &new_format);
        tex->Release();
        frame.Close();
        if (st == BridgeResult::NewFormat) return false;
        if (st == BridgeResult::NewSize)
        {
            Trace("[desk] the screen is now %ux%u, format %u - reopening",
                new_w, new_h, (unsigned)new_format);
            DeskStart(new_w, new_h);
            return false;
        }
        if (st != BridgeResult::Done) return false;
        return SwizzleIntoPicture(pipe);
    }
    catch (winrt::hresult_error const &e)
    {
        Trace("[desk] a screen frame threw 0x%08X - reopening", (unsigned)e.code());
        DeskStart(lfCap.w, lfCap.h);
        return false;
    }
}

// Copy a D3D11 source texture into the shared texture and signal the fence
// D3D12 waits on. A new size or format rebuilds the bridge.
static BridgeResult BridgeFrame(ID3D11Texture2D *frame, UINT *out_w, UINT *out_h,
                                      DXGI_FORMAT *out_format)
{
    D3D11_TEXTURE2D_DESC fd = {};
    frame->GetDesc(&fd);
    D3D11_BOX source_box = {0, 0, 0, fd.Width, fd.Height, 1};
    if (lfCap.window_dxgi) {
        const RECT &r = lfCap.window_crop;
        if (r.left < 0 || r.top < 0 || r.right <= r.left || r.bottom <= r.top ||
            r.right > LONG(fd.Width) || r.bottom > LONG(fd.Height)) return BridgeResult::Error;
        source_box = {UINT(r.left), UINT(r.top), 0, UINT(r.right), UINT(r.bottom), 1};
        fd.Width = r.right - r.left; fd.Height = r.bottom - r.top;
    }
    if (out_w != nullptr) *out_w = fd.Width;
    if (out_h != nullptr) *out_h = fd.Height;
    if (out_format != nullptr) *out_format = fd.Format;

    if (fd.Format != DXGI_FORMAT_B8G8R8A8_UNORM &&
        fd.Format != DXGI_FORMAT_R10G10B10A2_UNORM &&
        fd.Format != DXGI_FORMAT_R16G16B16A16_FLOAT)
    { Trace("[hdr] cannot use source format %u", fd.Format); return BridgeResult::Error; }
    if (lfCap.bridge11 == nullptr)
    {
        lfHdr.float_frames = fd.Format == DXGI_FORMAT_R16G16B16A16_FLOAT;

        lfHdr.keep = lfHdr.float_frames && HdrSwitchOn() && lfCap.display.hdr_on;
        lfHdr.linear = !lfHdr.keep && !lfHdr.linear_failed &&
                       (ColorSpaceWanted() == 2 || ColorSpaceWanted() == 3);
        if (ColorSpaceWanted() != 0)
            Trace("[hdr] colour space picked: %s - output %s",
                ColorSpaceWanted() == 1 ? "sRGB" : ColorSpaceWanted() == 2 ? "linear sRGB" : "scRGB",
                lfHdr.keep ? "HDR kept" : (lfHdr.linear ? "half float, linear, SDR range" : "8-bit sRGB"));

        Trace("[source] source format %u (%s)", (unsigned)fd.Format,
            fd.Format == DXGI_FORMAT_B8G8R8A8_UNORM ? "8-bit BGRA" :
            fd.Format == DXGI_FORMAT_R10G10B10A2_UNORM ? "10-bit RGB10A2 screen" :
            "half float");
        Trace("[hdr] source %s; the network sees SDR; export stays SDR",
            lfHdr.float_frames ? (lfHdr.keep ? "half-float scRGB"
                                             : "half float, tone-mapped to SDR")
                            : "SDR");
        D3D11_TEXTURE2D_DESC view_desc = {};
        view_desc.Width = fd.Width; view_desc.Height = fd.Height; view_desc.MipLevels = 1; view_desc.ArraySize = 1;
        view_desc.Format = fd.Format; view_desc.SampleDesc.Count = 1; view_desc.Usage = D3D11_USAGE_DEFAULT;
        view_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        view_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        if (FAILED(lfCap.d11->CreateTexture2D(&view_desc, nullptr, &lfCap.bridge11)))
        { Trace("[source] no shared texture"); return BridgeResult::Error; }
        IDXGIResource1 *r1 = nullptr;
        lfCap.bridge11->QueryInterface(__uuidof(IDXGIResource1), (void **)&r1);
        if (!r1 || FAILED(r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ, nullptr, &lfCap.bridge_handle)))
        { Trace("[source] no NT handle for the shared texture"); if (r1) r1->Release(); goto fail_capture; }
        r1->Release();
        if (FAILED(lfCore.device->OpenSharedHandle(lfCap.bridge_handle, __uuidof(ID3D12Resource),
                                           (void **)&lfCap.bridge12)))
        { Trace("[source] D3D12 could not open the shared texture"); goto fail_capture; }

        lfCap.sync_value = 1;
        if (FAILED(lfCore.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence),
                                      reinterpret_cast<void **>(&lfCap.sync12))))
        { Trace("[source] no shared fence"); goto fail_capture; }
        HANDLE nt_f = nullptr;
        lfCore.device->CreateSharedHandle(lfCap.sync12, nullptr, GENERIC_ALL, nullptr, &nt_f);
        ID3D11Device5 *d5 = nullptr;
        if (lfCap.d11->QueryInterface(__uuidof(ID3D11Device5), (void **)&d5) == S_OK)
        {
            d5->OpenSharedFence(nt_f, __uuidof(ID3D11Fence), (void **)&lfCap.sync11);
            d5->Release();
        }
        if (nt_f) CloseHandle(nt_f);
        if (!lfCap.sync11)
        { Trace("[source] the D3D11 side has no fence - source unusable"); goto fail_capture; }
        D3D12_HEAP_PROPERTIES vram_heap = { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC tex_desc = {};
        tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        tex_desc.Width = fd.Width; tex_desc.Height = fd.Height; tex_desc.DepthOrArraySize = 1; tex_desc.MipLevels = 1;
        tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; tex_desc.SampleDesc.Count = 1;
        tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(lfCore.device->CreateCommittedResource(&vram_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
                                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                                  __uuidof(ID3D12Resource),
                                                  reinterpret_cast<void **>(&lfCap.rgba))))
        { Trace("[source] no RGBA target"); goto fail_capture; }
        Trace("[source] shared picture %ux%u ready", (UINT)fd.Width, (UINT)fd.Height);
    }

    {
        D3D11_TEXTURE2D_DESC view_desc = {};
        lfCap.bridge11->GetDesc(&view_desc);
        if (view_desc.Width != fd.Width || view_desc.Height != fd.Height)
            return BridgeResult::NewSize;
        if (view_desc.Format != fd.Format)
        {
            Trace("[source] source format %u -> %u - new bridge",
                (unsigned)view_desc.Format, (unsigned)fd.Format);
            CloseFgResources();
            CloseFlow();
            HdrRelease();
            if (lfCap.bridge12) { lfCap.bridge12->Release(); lfCap.bridge12 = nullptr; }
            if (lfCap.bridge_handle)  { CloseHandle(lfCap.bridge_handle); lfCap.bridge_handle = nullptr; }
            if (lfCap.sync12) { lfCap.sync12->Release(); lfCap.sync12 = nullptr; }
            if (lfCap.sync11) { lfCap.sync11->Release(); lfCap.sync11 = nullptr; }
            if (lfCap.rgba) { lfCap.rgba->Release(); lfCap.rgba = nullptr; }
            if (lfCap.bridge11) { lfCap.bridge11->Release(); lfCap.bridge11 = nullptr; }
            return BridgeResult::NewFormat;
        }
        lfCap.d11_ctx->CopySubresourceRegion(lfCap.bridge11, 0, 0, 0, 0, frame, 0, &source_box);
    }
    ID3D11DeviceContext4 *ctx4 = nullptr;
    if (FAILED(lfCap.d11_ctx->QueryInterface(__uuidof(ID3D11DeviceContext4), (void **)&ctx4)))
        return BridgeResult::Error;
    const HRESULT signaled = ctx4->Signal(lfCap.sync11, lfCap.sync_value);
    ctx4->Release();
    if (FAILED(signaled)) return BridgeResult::Error;
    lfCap.d11_ctx->Flush();

    const UINT64 wanted = lfCap.sync_value;
    // The conversion is submitted next on this queue. GPU-to-GPU ordering
    // replaces a CPU sleep/wakeup between the shared copy and conversion.
    if (FAILED(lfCore.direct_queue->Wait(lfCap.sync12, wanted))) return BridgeResult::Error;
    ++lfCap.sync_value;
    return BridgeResult::Done;
fail_capture:

    if (lfCap.bridge12) { lfCap.bridge12->Release(); lfCap.bridge12 = nullptr; }
    if (lfCap.bridge_handle)  { CloseHandle(lfCap.bridge_handle); lfCap.bridge_handle = nullptr; }
    if (lfCap.sync12) { lfCap.sync12->Release(); lfCap.sync12 = nullptr; }
    if (lfCap.sync11) { lfCap.sync11->Release(); lfCap.sync11 = nullptr; }
    if (lfCap.rgba) { lfCap.rgba->Release(); lfCap.rgba = nullptr; }
    if (lfCap.bridge11) { lfCap.bridge11->Release(); lfCap.bridge11 = nullptr; }
    return BridgeResult::Error;
}

// The small pass that puts BGRA sources in RGBA order.
static bool SwizzleReady()
{
    if (lfCap.swz_pso) return true;
    ID3DBlob *blob = nullptr, *err = nullptr;
    HRESULT status = CachedCompile(kHdrInHlsl, sizeof(kHdrInHlsl) - 1, "lf-order.hlsl",
                            nullptr, nullptr, "CSMain", "cs_5_0", 0, 0, &blob, &err);
    if (FAILED(status)) { Trace("[desk] channel-order shader did not compile: %s", err ? (char *)err->GetBufferPointer() : "?"); return false; }
    D3D12_ROOT_PARAMETER prm_desc[3] = {};
    D3D12_DESCRIPTOR_RANGE r0 = { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0 };
    D3D12_DESCRIPTOR_RANGE r1 = { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0 };
    prm_desc[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    prm_desc[0].DescriptorTable.NumDescriptorRanges = 1; prm_desc[0].DescriptorTable.pDescriptorRanges = &r0;
    prm_desc[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    prm_desc[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    prm_desc[1].DescriptorTable.NumDescriptorRanges = 1; prm_desc[1].DescriptorTable.pDescriptorRanges = &r1;
    prm_desc[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    prm_desc[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    prm_desc[2].Constants.Num32BitValues = 3;
    prm_desc[2].Constants.ShaderRegister = 0;
    root_desc.NumParameters = 3; root_desc.pParameters = prm_desc;
    ID3DBlob *sig = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig, &err)))
    { Trace("[desk] channel-order root signature did not serialize"); return false; }
    if (FAILED(lfCore.device->CreateRootSignature(0, sig->GetBufferPointer(), sig->GetBufferSize(),
                                          __uuidof(ID3D12RootSignature),
                                          reinterpret_cast<void **>(&lfCap.swz_root))))
    { Trace("[desk] channel-order root signature refused"); return false; }
    sig->Release();
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = lfCap.swz_root;
    pd.CS.pShaderBytecode = blob->GetBufferPointer(); pd.CS.BytecodeLength = blob->GetBufferSize();
    if (FAILED(lfCore.device->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState),
                                                 reinterpret_cast<void **>(&lfCap.swz_pso))))
    { Trace("[desk] channel-order pipeline refused"); return false; }
    blob->Release();
    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 2; heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(lfCore.device->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap),
                                           reinterpret_cast<void **>(&lfCap.swz_heap))))
    { Trace("[desk] channel-order descriptor heap refused"); return false; }
    Trace("[desk] channel-order pipeline ready");
    return true;
}

// Point the channel-order pass at its source.
static void SwizzleBind(ID3D12Resource *from)
{
    const UINT step_bytes = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_at = lfCap.swz_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC view_desc = {};
    view_desc.Format = from->GetDesc().Format; view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view_desc.Texture2D.MipLevels = 1;
    lfCore.device->CreateShaderResourceView(from, &view_desc, cpu_at);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
    uav_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    cpu_at.ptr += step_bytes;
    lfCore.device->CreateUnorderedAccessView(lfCap.rgba, nullptr, &uav_desc, cpu_at);
}

// Run the channel-order pass from the bridge into the pipeline's picture.
static bool SwizzleIntoPicture(Pipeline &pipe)
{
    static ULONGLONG last_display_query = 0;
    if (GetTickCount64() - last_display_query > 1000 || last_display_query == 0)
    {
        lfCap.display = HdrDisplayOf(lfCap.monitor);
        last_display_query = GetTickCount64();
    }
    lfHdr.paper_white = lfCap.display.sdr_white;
    if (!CmdOpen()) return false;
    GpuStageOpen(GS_CONVERT);
    D3D12_RESOURCE_BARRIER to_srv = Barrier(lfCap.bridge12, D3D12_RESOURCE_STATE_COMMON,
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    lfCore.cmd->ResourceBarrier(1, &to_srv);
    SwizzleBind(lfCap.bridge12);
    ID3D12DescriptorHeap *heap_list[] = { lfCap.swz_heap };
    lfCore.cmd->SetDescriptorHeaps(1, heap_list);
    lfCore.cmd->SetComputeRootSignature(lfCap.swz_root);
    lfCore.cmd->SetPipelineState(lfCap.swz_pso);
    D3D12_GPU_DESCRIPTOR_HANDLE g0 = lfCap.swz_heap->GetGPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE g1 = g0;
    g1.ptr += lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    lfCore.cmd->SetComputeRootDescriptorTable(0, g0);
    lfCore.cmd->SetComputeRootDescriptorTable(1, g1);

    struct { UINT is_float; float white; UINT rotate180; } hdr = {
        lfHdr.float_frames ? (lfHdr.keep ? 1u : 2u) : 0u, lfHdr.paper_white,
        (lfCap.live && lfCap.upside_down) ? 1u : 0u };
    lfCore.cmd->SetComputeRoot32BitConstants(2, 3, &hdr, 0);
    lfCore.cmd->Dispatch((lfCap.w + 7) / 8, (lfCap.h + 7) / 8, 1);

    // Flow Super Resolution: the picture is the internal size - the capture
    // is brought down to it here, once (flow_upscaler.inl).
    if (pipe.super_res)
    {
        if (!SuperResDownscale(lfCore.cmd, lfCap.rgba, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, pipe))
        { DropCommands(); Trace("[source] the capture could not be scaled to the internal size - frame dropped"); return false; }
        D3D12_RESOURCE_BARRIER back_common = Barrier(lfCap.bridge12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                        D3D12_RESOURCE_STATE_COMMON);
        lfCore.cmd->ResourceBarrier(1, &back_common);
    }
    else
    {
    D3D12_RESOURCE_BARRIER pre_color = Barrier(pipe.picture.gpu_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                  D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_RESOURCE_BARRIER to_copy = Barrier(lfCap.rgba, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                                D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_RESOURCE_BARRIER pre_c[2] = { to_copy, pre_color };
    lfCore.cmd->ResourceBarrier(2, pre_c);
    D3D12_TEXTURE_COPY_LOCATION from = {}, into = {};
    from.pResource = lfCap.rgba; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; from.SubresourceIndex = 0;
    into.pResource = pipe.picture.gpu_tex; into.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; into.SubresourceIndex = 0;

    {
        const D3D12_RESOURCE_DESC view_desc = lfCap.rgba->GetDesc();
        const D3D12_RESOURCE_DESC dd = pipe.picture.gpu_tex->GetDesc();
        const UINT pic_w = (UINT)((view_desc.Width < dd.Width) ? view_desc.Width : dd.Width);
        const UINT pic_h = (UINT)((view_desc.Height < dd.Height) ? view_desc.Height : dd.Height);
        if (pic_w != view_desc.Width || pic_h != view_desc.Height)
            Trace("[source] sizes differ, %llux%llu against %llux%llu - cropped",
                (unsigned long long)view_desc.Width, (unsigned long long)view_desc.Height,
                (unsigned long long)dd.Width, (unsigned long long)dd.Height);
        if (pic_w == 0 || pic_h == 0)
        { Trace("[source] nothing to copy - frame dropped"); return false; }
        D3D12_BOX box = { 0, 0, 0, pic_w, pic_h, 1 };
        lfCore.cmd->CopyTextureRegion(&into, 0, 0, 0, &from, &box);
    }
    D3D12_RESOURCE_BARRIER to_uav = Barrier(lfCap.rgba, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_RESOURCE_BARRIER to_nps = Barrier(pipe.picture.gpu_tex, D3D12_RESOURCE_STATE_COPY_DEST,
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    D3D12_RESOURCE_BARRIER to_common = Barrier(lfCap.bridge12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                  D3D12_RESOURCE_STATE_COMMON);
    D3D12_RESOURCE_BARRIER post_c[3] = { to_uav, to_nps, to_common };
    lfCore.cmd->ResourceBarrier(3, post_c);
    }
    GpuStageClose(GS_CONVERT);
    const UINT64 fence = CmdSubmit();
    if ((FlowActive() || FgRequested()) && lfOut.sent_passthrough && lfLuma.mapped)
    {
        // Luma has its own descriptors: conversion and luma may be queued
        // together without rewriting descriptors still in use. Its final
        // readback fence covers BOTH jobs, avoiding a GPU time-slice wait
        // between two dependent pieces of the same captured picture.
        const bool luma_ok = fence && LumaFromPicture();
        if (!luma_ok && !GpuStageWait(GS_CONVERT, fence, 10000)) return false;
        lfCap.luma_ok = luma_ok;
        if (lfRun.gpu_lost) return false;
        ExposureUpdate();
        lfCap.has_frame = true;
        lfCap.reopen_tried = false;
        return true;
    }
    if (!GpuStageWait(GS_CONVERT, fence, 10000)) { Trace("[source] waited too long for the channel-order pass"); return false; }

    if (!LumaFromPicture()) {   }
    lfCap.luma_ok = lfLuma.mapped;
    if (lfRun.gpu_lost) return false;
    ExposureUpdate();
    lfCap.has_frame = true;
    lfCap.reopen_tried = false;
    return true;
}
