// lf_gpu_core.inl - the D3D12 device and the one command list everything is
// recorded into: a ring of three allocators guarded by a timeline fence, and
// the card choice (LF_GPU, else the first card that can run the network).

// The device, its queue, the allocator ring and the fence, on the card the
// network can use (LF_GPU picks one; a Radeon in the Radeon build).
static bool GpuCoreStart()
{
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    HMODULE d3d12 = LoadLibraryW(L"d3d12.dll");
    auto create_device  = d3d12 ? reinterpret_cast<D3d12CreateFn>(GetProcAddress(d3d12, "D3D12CreateDevice")) : nullptr;
    auto create_factory = dxgi ? reinterpret_cast<DxgiFactoryFn>(GetProcAddress(dxgi, "CreateDXGIFactory1")) : nullptr;
    if (create_device == nullptr || create_factory == nullptr) { Trace("[card] dxgi.dll / d3d12.dll lack the exports we need"); return false; }

    IDXGIFactory2 *dxgi_factory = nullptr;
    HRESULT status = create_factory(__uuidof(IDXGIFactory2), reinterpret_cast<void **>(&dxgi_factory));
    if (FAILED(status)) { Trace("[card] no DXGI factory (0x%08X)", status); return false; }

    const int wanted = CardWanted();
    IDXGIAdapter1 *nvidia = nullptr;
    IDXGIAdapter1 *chosen = nullptr;
    int nvidia_idx = -1;
    int chosen_idx = -1;
#ifdef LF_RADEON
    IDXGIAdapter1 *fallback = nullptr;
    int fallback_idx = -1;
#endif
    for (UINT i = 0; ; ++i)
    {
        IDXGIAdapter1 *option = nullptr;
        if (dxgi_factory->EnumAdapters1(i, &option) == DXGI_ERROR_NOT_FOUND) break;
        DXGI_ADAPTER_DESC1 desc = {};
        option->GetDesc1(&desc);

        Trace("[card] #%u %ls, vendor 0x%04X, %llu MB, luid %08X:%08X",
            i, desc.Description, desc.VendorId,
            (unsigned long long)(desc.DedicatedVideoMemory >> 20),
            (unsigned)desc.AdapterLuid.HighPart, (unsigned)desc.AdapterLuid.LowPart);
#ifdef LF_RADEON
        const bool usable = desc.VendorId == 0x1002 &&
                            !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
        if (!usable && fallback == nullptr && !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
        {
            option->AddRef();
            fallback = option;
            fallback_idx = static_cast<int>(i);
        }
        if (usable && nvidia == nullptr)
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, g_radeon_card, sizeof(g_radeon_card),
                                nullptr, nullptr);
#else
        const bool usable = desc.VendorId == 0x10DE &&
                            !(desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE);
#endif
        if (wanted >= 0 && static_cast<int>(i) == wanted && usable)
        { chosen = option; chosen_idx = static_cast<int>(i); continue; }
        if (nvidia == nullptr && usable)
        { nvidia = option; nvidia_idx = static_cast<int>(i); continue; }
        option->Release();
    }
#ifdef LF_RADEON
    g_radeon_adapter = nvidia != nullptr || chosen != nullptr;
    if (!g_radeon_adapter && fallback != nullptr)
    {
        DXGI_ADAPTER_DESC1 fd = {};
        fallback->GetDesc1(&fd);
        WideCharToMultiByte(CP_UTF8, 0, fd.Description, -1, g_radeon_card, sizeof(g_radeon_card),
                            nullptr, nullptr);
        Trace("[card] no Radeon here - LIVE FLOW runs on card %d with the network off", fallback_idx);
        nvidia = fallback;
        nvidia_idx = fallback_idx;
        fallback = nullptr;
    }
    if (fallback != nullptr) fallback->Release();
#endif
    if (wanted >= 0 && chosen == nullptr)
        Trace("[card] LF_GPU=%d is no usable NVIDIA card - taking the first NVIDIA card",
            wanted);
    if (chosen != nullptr)
    {
        if (nvidia != nullptr) nvidia->Release();
        nvidia = chosen;
        nvidia_idx = chosen_idx;
        Trace("[card] card %d picked by LF_GPU", wanted);
    }
    if (nvidia == nullptr) { dxgi_factory->Release(); Trace("[card] no NVIDIA card in this machine"); return false; }

    lfCard.index = nvidia_idx;
    Trace("[card] card %d does both the network and the source", nvidia_idx);
    if (FAILED(nvidia->QueryInterface(__uuidof(IDXGIAdapter3),
                                      reinterpret_cast<void **>(&lfCard.adapter))))
        lfCard.adapter = nullptr;

    status = create_device(nvidia, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device),
                       reinterpret_cast<void **>(&lfCore.device));
    nvidia->Release();
    if (FAILED(status)) { Trace("[card] the D3D12 device did not start (0x%08X)", status); return false; }

    {
        ID3D12DeviceRemovedExtendedDataSettings1 *dred1 = nullptr;
        const HRESULT hr1 = lfCore.device->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedDataSettings1),
                                                  reinterpret_cast<void **>(&dred1));
        if (SUCCEEDED(hr1))
        {
            dred1->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred1->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred1->Release();
            Trace("[card] crash breadcrumbs on (DRED settings1)");
        }
        else
        {
            ID3D12DeviceRemovedExtendedDataSettings *dred = nullptr;
            const HRESULT hr0 = lfCore.device->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedDataSettings),
                                                      reinterpret_cast<void **>(&dred));
            if (SUCCEEDED(hr0))
            {
                dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
                dred->Release();
                Trace("[card] crash breadcrumbs on (DRED settings)");
            }
            else
            {
                Trace("[card] no crash breadcrumbs (settings1 0x%08X, settings 0x%08X)",
                    (unsigned)hr1, (unsigned)hr0);
            }
        }
    }

    dxgi_factory->Release();
    D3D12_COMMAND_QUEUE_DESC queue_desc = {};
    lfCore.device->CreateCommandQueue(&queue_desc, __uuidof(ID3D12CommandQueue), reinterpret_cast<void **>(&lfCore.direct_queue));
    if (lfCore.direct_queue == nullptr) { Trace("[card] no command queue"); return false; }
    Trace("[net] D3D12 device up on its own - no swap chain, no host process");

    for (int i = 0; i < GpuCore::kRing; ++i)
        lfCore.device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                      reinterpret_cast<void **>(&lfCore.allocators[i]));
    if (lfCore.allocators[0] != nullptr)
        lfCore.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, lfCore.allocators[0], nullptr,
                                 __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&lfCore.cmd));
    if (lfCore.cmd != nullptr) lfCore.cmd->Close();
    lfCore.device->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void **>(&lfCore.timeline));
    lfCore.timeline_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (lfCore.cmd == nullptr || lfCore.timeline == nullptr) { Trace("[card] no command list or fence"); return false; }

    return true;
}

// LF_GPU as a card number, or -1.
static int CardWanted()
{
    char text[16] = {};
    const DWORD len = GetEnvironmentVariableA("LF_GPU", text, sizeof(text));
    if (len == 0 || len >= sizeof(text)) return -1;
    char *end = nullptr;
    const long value = strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 0 || value > 63)
    {
        Trace("[card] LF_GPU=%s is not a card number - ignored", text);
        return -1;
    }
    return static_cast<int>(value);
}

// The card the device was really made on (LF_GPU before that).
static int CardInUse()
{
    return lfCard.index >= 0 ? lfCard.index : CardWanted();
}

// Open the command list on the next allocator of the ring, waiting for the
// card to be done with it first.
static bool CmdOpen()
{
    if (lfRun.gpu_lost || lfCore.cmd == nullptr) return false;
    const int slot = lfCore.ring_slot;
    const UINT64 done_mark = lfCore.allocator_mark[slot];
    if (done_mark != 0 && lfCore.timeline->GetCompletedValue() < done_mark)
    {
        const bool phase = PhasesOn();
        const double t_wait = phase ? PhaseClock() : 0.0;
        if (!AwaitMark(lfCore.timeline, done_mark, 2000))
        { Trace("[card] allocator %d is still busy on the card", slot); return false; }
        if (phase)
        {
            const double spent = PhaseClock() - t_wait;
            ++lfProf.allocator_waits;
            lfProf.allocator_wait_sum += spent;
            if (spent > lfProf.allocator_wait_peak) lfProf.allocator_wait_peak = spent;
        }
    }
    if (PhasesOn()) TimingCollect();
    if (FAILED(lfCore.allocators[slot]->Reset()))
    {
        ReportDeviceLoss("allocator reuse");
        return false;
    }
    return SUCCEEDED(lfCore.cmd->Reset(lfCore.allocators[slot], nullptr));
}

// Close and send the list; returns the fence value that marks its end
// (0 when the card refused it).
static UINT64 CmdSubmit()
{
    const bool phase = PhasesOn();
    const double t_submit = phase ? PhaseClock() : 0.0;
    const HRESULT closed = lfCore.cmd->Close();
    if (FAILED(closed))
    {
        Trace("[card] the command list did not close (0x%08X) - nothing sent", closed);
        lfRun.gpu_lost = true;
        lfProf.stage_now = GS_NONE;
        DropCommands();
        return 0;
    }
    ID3D12CommandList *lists[] = { lfCore.cmd };
    lfCore.direct_queue->ExecuteCommandLists(1, lists);
    const UINT64 v = ++lfCore.timeline_value;

    const HRESULT sig = lfCore.direct_queue->Signal(lfCore.timeline, v);
    if (FAILED(sig))
    {
        Trace("[card] the queue refused a signal (0x%08X)", sig);
        lfRun.gpu_lost = true;
        lfProf.stage_now = GS_NONE;
        return 0;
    }
    lfCore.allocator_mark[lfCore.ring_slot] = v;
    if (lfProf.stage_now != GS_NONE && lfProf.slot_stage[lfCore.ring_slot] != GS_NONE)
        lfProf.slot_mark[lfCore.ring_slot] = v;
    lfCore.ring_slot = (lfCore.ring_slot + 1) % GpuCore::kRing;
    if (phase)
    {
        lfProf.stamp.fence = v;
        if (lfProf.stage_now >= 0 && lfProf.stage_now < GS_COUNT)
        {
            const double spent = PhaseClock() - t_submit;
            lfProf.submit_sum[lfProf.stage_now] += spent;
            if (spent > lfProf.submit_peak[lfProf.stage_now]) lfProf.submit_peak[lfProf.stage_now] = spent;
            ++lfProf.submit_n[lfProf.stage_now];
        }
        const UINT64 reached = lfCore.timeline->GetCompletedValue();
        const UINT64 outstanding = reached == UINT64_MAX || reached >= v ? 0 : v - reached;
        if (outstanding > lfProf.in_flight_peak)
            lfProf.in_flight_peak = static_cast<unsigned>(outstanding);
    }
    lfProf.stage_now = GS_NONE;
    return v;
}

// Wait until a fence reaches a value, at most ms milliseconds. A removed
// device answers UINT64_MAX, which is a failure, not a success.
static bool AwaitMark(ID3D12Fence *f, UINT64 v, DWORD timeout_ms)
{
    if (v == 0) return false;

    if (f->GetCompletedValue() == UINT64_MAX) return false;
    if (f->GetCompletedValue() >= v) return true;

    ResetEvent(lfCore.timeline_event);
    if (f->GetCompletedValue() >= v) return true;
    if (FAILED(f->SetEventOnCompletion(v, lfCore.timeline_event))) return false;
    const ULONGLONG until = GetTickCount64() + timeout_ms;
    for (;;)
    {
        const ULONGLONG now_ms = GetTickCount64();
        const DWORD left = now_ms >= until ? 0 : (DWORD)(until - now_ms);
        if (WaitForSingleObject(lfCore.timeline_event, left) != WAIT_OBJECT_0) return false;
        const UINT64 now = f->GetCompletedValue();
        if (now == UINT64_MAX) return false;
        if (now >= v) return true;
    }
}

// Close the list even if a runtime left it in a state that throws.
static void CmdCloseSafe()
{
    __try { lfCore.cmd->Close(); } __except (EXCEPTION_EXECUTE_HANDLER) {}
}

// Throw away a list a runtime crashed in: wait for what was already sent,
// then start a fresh list.
static void DropCommands()
{
    if (lfCore.cmd == nullptr) return;
    UINT64 done_mark = 0;
    for (int i = 0; i < GpuCore::kRing; ++i)
        if (lfCore.allocator_mark[i] > done_mark) done_mark = lfCore.allocator_mark[i];
    if (done_mark && !AwaitMark(lfCore.timeline, done_mark, 60000))
    { lfRun.gpu_lost = true; Trace("[card] old commands still running - the list cannot be replaced"); return; }
    CmdCloseSafe();
    lfCore.cmd->Release();
    lfCore.cmd = nullptr;
    HRESULT status = lfCore.device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, lfCore.allocators[lfCore.ring_slot], nullptr,
                                        __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void **>(&lfCore.cmd));
    if (SUCCEEDED(status)) status = lfCore.cmd->Close();
    if (FAILED(status))
    {
        Trace("[card] no new command list (0x%08X)", status);
        if (lfCore.cmd) lfCore.cmd->Release();
        lfCore.cmd = nullptr;
        lfRun.gpu_lost = true;
    }
}

// A 2D texture in video memory.
static ID3D12Resource *NewTexture(UINT w, UINT h_, DXGI_FORMAT pixel_format, bool uav_view)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC res_desc = {};
    res_desc.Dimension        = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    res_desc.Width            = w;
    res_desc.Height           = h_;
    res_desc.DepthOrArraySize = 1;
    res_desc.MipLevels        = 1;
    res_desc.Format           = pixel_format;
    res_desc.SampleDesc.Count = 1;
    res_desc.Layout           = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    res_desc.Flags            = uav_view ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
    ID3D12Resource *t = nullptr;
    lfCore.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &res_desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                   __uuidof(ID3D12Resource), reinterpret_cast<void **>(&t));
    return t;
}

// A state transition for one resource.
static D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    return b;
}
