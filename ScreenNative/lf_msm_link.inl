// lf_msm_link.inl - LIVE FLOW hands its finished frames to MFG SUPER MOTION
// (round 32; the layout is lf_msm_link.h).
//
// Asked for with the window (WindowMsg.flags, kWindowLink): "Link to MFG SUPER
// MOTION" in the app. While the link is on:
//   - the game is taken by Graphics Capture, its window's own frames. MFG
//     SUPER MOTION's output lies over the game and is drawn anew several times
//     for each frame of the game; taken from the desktop, every one of those
//     would come back as a picture to look at (round 31, SourceSeen);
//   - every finished frame - DLSS 5 on it, or untouched while DLSS 5 is off -
//     is copied into the next of the link's textures in the same piece of work
//     that puts it on LIVE FLOW's own window (MsmLinkCopy, beside ShareCopy),
//     and handed over once that is sent (MsmLinkSent);
//   - while MFG SUPER MOTION takes the frames, LIVE FLOW's own window steps
//     aside (fully see-through): what reaches the screen is MFG SUPER MOTION's.
//     When it stops taking them, LIVE FLOW shows its frames itself again.
// Spout (OBS) stays LIVE FLOW's: MFG SUPER MOTION sends none while linked.

static struct MsmLinkState
{
    bool wanted = false;
    HWND game = nullptr;
    bool fill = false;
    HANDLE map = nullptr;
    LinkHeader *head = nullptr;
    HANDLE event = nullptr;
    ID3D12Resource *slot[kLinkSlots] = {};
    HANDLE slot_handle[kLinkSlots] = {};
    ID3D12Fence *ready = nullptr, *taken = nullptr;
    HANDLE ready_handle = nullptr, taken_handle = nullptr;
    UINT w = 0, h = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    uint32_t generation = 0;
    int64_t sequence = 0;
    int pending = -1;            // the slot copied into in the list not yet sent
    double pending_stamp = 0.0;
    bool aside = false;          // LIVE FLOW's own window is see-through
    bool consumer = false;       // MFG SUPER MOTION was taking frames at the last look
    unsigned skipped = 0;
    double beat_at = 0.0;
} g_msm;

static int64_t MsmTicks(double seconds)
{
    static LARGE_INTEGER frequency = {};
    if (frequency.QuadPart == 0) QueryPerformanceFrequency(&frequency);
    return static_cast<int64_t>(seconds * double(frequency.QuadPart));
}

static int64_t MsmNowTicks()
{
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return now.QuadPart;
}

static bool MsmLinkWanted() { return g_msm.wanted; }

static void MsmLinkFreeTextures()
{
    for (UINT i = 0; i < kLinkSlots; ++i)
    {
        if (g_msm.slot[i]) { g_msm.slot[i]->Release(); g_msm.slot[i] = nullptr; }
        if (g_msm.slot_handle[i]) { CloseHandle(g_msm.slot_handle[i]); g_msm.slot_handle[i] = nullptr; }
    }
    if (g_msm.ready) { g_msm.ready->Release(); g_msm.ready = nullptr; }
    if (g_msm.taken) { g_msm.taken->Release(); g_msm.taken = nullptr; }
    if (g_msm.ready_handle) { CloseHandle(g_msm.ready_handle); g_msm.ready_handle = nullptr; }
    if (g_msm.taken_handle) { CloseHandle(g_msm.taken_handle); g_msm.taken_handle = nullptr; }
    g_msm.w = g_msm.h = 0;
    g_msm.format = DXGI_FORMAT_UNKNOWN;
    g_msm.pending = -1;
}

// LIVE FLOW's own window as it should be: see-through while MFG SUPER MOTION
// shows the frames, itself otherwise.
static void MsmLinkAside(bool aside)
{
    if (aside == g_msm.aside || lfShow.hwnd == nullptr) return;
    if (SetLayeredWindowAttributes(lfShow.hwnd, 0, aside ? 0 : 255, LWA_ALPHA))
    {
        g_msm.aside = aside;
        Trace(aside ? "[msm] MFG SUPER MOTION takes the frames - LIVE FLOW's own window steps aside"
                    : "[msm] LIVE FLOW shows its frames itself again");
    }
}

// A new output window starts as itself (ShowStart).
static void MsmLinkAsideReset() { g_msm.aside = false; }

static void MsmLinkClose()
{
    MsmLinkAside(false);
    MsmLinkFreeTextures();
    if (g_msm.head)
    {
        g_msm.head->magic = 0;
        g_msm.head->latest = 0;
        UnmapViewOfFile(g_msm.head);
        g_msm.head = nullptr;
    }
    if (g_msm.map) { CloseHandle(g_msm.map); g_msm.map = nullptr; }
    if (g_msm.event) { CloseHandle(g_msm.event); g_msm.event = nullptr; }
    g_msm.consumer = false;
}

// The window message: the game, and whether the link is wanted for it.
// game null: the whole screen (round 33 - LIVE FLOW takes the whole screen
// again, and SMFG with it).
static void MsmLinkSet(HWND game, bool on, bool fill)
{
    if (!on)
    {
        if (g_msm.wanted) Trace("[msm] the link to MFG SUPER MOTION is off");
        g_msm.wanted = false;
        g_msm.game = nullptr;
        MsmLinkClose();
        return;
    }
    if (!g_msm.wanted) Trace("[msm] the link to MFG SUPER MOTION is on - every finished frame is handed over");
    g_msm.wanted = true;
    g_msm.game = game;
    g_msm.fill = fill;
    if (g_msm.map == nullptr)
    {
        g_msm.map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, kLinkMemory);
        if (g_msm.map != nullptr)
            g_msm.head = static_cast<LinkHeader *>(MapViewOfFile(g_msm.map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(LinkHeader)));
        if (g_msm.head == nullptr)
        {
            Trace("[msm] no shared memory for the link (%lu)", GetLastError());
            if (g_msm.map) { CloseHandle(g_msm.map); g_msm.map = nullptr; }
            g_msm.wanted = false;
            return;
        }
        g_msm.event = CreateEventW(nullptr, FALSE, FALSE, kLinkEvent);
        // Round 42 ("DLSS MFG stops"): a worker started again under a running
        // engine (another pass count, network model, screen) found the engine
        // still holding the memory and the last worker's textures, and began
        // again at generation 1 - the names the engine already had open. It
        // took the link back, got nothing for 4 s and fell back to taking the
        // desktop itself, without DLSS 5, for good. The generation goes on from
        // the one there, so the names are new and the change is seen.
        const uint32_t before = g_msm.head->generation;
        if (before > g_msm.generation) g_msm.generation = before;
        // Whoever was there before (an earlier run) is forgotten; a reader of
        // that time sees another producer and opens everything again.
        memset(g_msm.head, 0, sizeof(LinkHeader));
        g_msm.head->version = kLinkVersion;
        g_msm.head->producer_pid = GetCurrentProcessId();
    }
    g_msm.head->game_hwnd = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(game));
    g_msm.head->fill = fill ? 1u : 0u;
    g_msm.head->producer_beat = MsmNowTicks();
    g_msm.head->magic = kLinkMagic;
}

// The textures and fences for frames of this size and format.
static bool MsmLinkReady(UINT w, UINT h, DXGI_FORMAT format)
{
    if (g_msm.slot[0] != nullptr && g_msm.w == w && g_msm.h == h && g_msm.format == format) return true;
    MsmLinkFreeTextures();
    const uint32_t generation = g_msm.generation + 1;
    D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = w; desc.Height = h; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.Format = format; desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    wchar_t name[160] = {};
    for (UINT i = 0; i < kLinkSlots; ++i)
    {
        if (FAILED(lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_SHARED, &desc, D3D12_RESOURCE_STATE_COMMON,
                                                          nullptr, __uuidof(ID3D12Resource),
                                                          reinterpret_cast<void **>(&g_msm.slot[i]))))
        { Trace("[msm] the link's %ux%u textures could not be made", w, h); MsmLinkFreeTextures(); return false; }
        swprintf_s(name, L"%ls_g%u_s%u", kLinkName, generation, i);
        if (FAILED(lfCore.device->CreateSharedHandle(g_msm.slot[i], nullptr, GENERIC_ALL, name, &g_msm.slot_handle[i])))
        { Trace("[msm] the link's texture could not be shared"); MsmLinkFreeTextures(); return false; }
    }
    ID3D12Fence **fences[2] = { &g_msm.ready, &g_msm.taken };
    HANDLE *handles[2] = { &g_msm.ready_handle, &g_msm.taken_handle };
    const wchar_t *what[2] = { L"ready", L"taken" };
    for (int i = 0; i < 2; ++i)
    {
        swprintf_s(name, L"%ls_g%u_%ls", kLinkName, generation, what[i]);
        if (FAILED(lfCore.device->CreateFence(0, D3D12_FENCE_FLAG_SHARED, __uuidof(ID3D12Fence),
                                              reinterpret_cast<void **>(fences[i]))) ||
            FAILED(lfCore.device->CreateSharedHandle(*fences[i], nullptr, GENERIC_ALL, name, handles[i])))
        { Trace("[msm] the link's fences could not be made"); MsmLinkFreeTextures(); return false; }
    }
    g_msm.generation = generation;
    g_msm.w = w; g_msm.h = h; g_msm.format = format;
    g_msm.sequence = 0;
    LinkHeader *head = g_msm.head;
    head->latest = 0;
    for (UINT i = 0; i < kLinkSlots; ++i) head->frame[i] = LinkFrame{};
    head->consumer_taken = 0;
    head->width = w; head->height = h; head->format = static_cast<uint32_t>(format);
    MemoryBarrier();
    head->generation = generation;
    Trace("[msm] frames of %ux%u (format %u) for MFG SUPER MOTION, generation %u", w, h, (unsigned)format, generation);
    return true;
}

// Is MFG SUPER MOTION taking the frames: its engine there and heard from
// within the last second.
static bool MsmConsumerHere()
{
    const LinkHeader *head = g_msm.head;
    if (head == nullptr || head->consumer_pid == 0) return false;
    return MsmNowTicks() - head->consumer_beat < MsmTicks(1.0);
}

// Once a loop turn: the beat, and LIVE FLOW's own window aside or not.
static void MsmLinkTick()
{
    if (!g_msm.wanted || g_msm.head == nullptr) return;
    const double now = LatencyNow();
    if (now - g_msm.beat_at > 0.25)
    {
        g_msm.beat_at = now;
        g_msm.head->producer_beat = MsmNowTicks();
    }
    const bool here = MsmConsumerHere();
    if (here != g_msm.consumer)
    {
        g_msm.consumer = here;
        Trace(here ? "[msm] MFG SUPER MOTION is taking the frames (process %lu)"
                   : "[msm] MFG SUPER MOTION stopped taking the frames", (unsigned long)g_msm.head->consumer_pid);
    }
    MsmLinkAside(here);
}

// In the list that puts a finished frame on LIVE FLOW's window: src (in
// COPY_SOURCE, left so) into the next texture of the link.
static void MsmLinkCopy(ID3D12GraphicsCommandList *list, ID3D12Resource *src, UINT w, UINT h)
{
    g_msm.pending = -1;
    if (!g_msm.wanted || g_msm.head == nullptr || src == nullptr || w == 0 || h == 0) return;
    MsmLinkTick();
    const D3D12_RESOURCE_DESC sd = src->GetDesc();
    if (!MsmLinkReady(w, h, sd.Format)) return;
    const int64_t next = g_msm.sequence + 1;
    const UINT slot = static_cast<UINT>(next % kLinkSlots);
    // The slot about to be written held frame next - kLinkSlots. While MFG
    // SUPER MOTION's copy of it may still be running on its card, this frame
    // is not handed over: the next one will be.
    const int64_t asked = g_msm.head->consumer_taken;
    if (MsmConsumerHere() && asked >= next - static_cast<int64_t>(kLinkSlots) &&
        g_msm.taken->GetCompletedValue() < static_cast<UINT64>(asked))
    {
        if (++g_msm.skipped == 1 || g_msm.skipped % 600 == 0)
            Trace("[msm] MFG SUPER MOTION was still copying an earlier frame - %u frames not handed over", g_msm.skipped);
        return;
    }
    D3D12_RESOURCE_BARRIER in = Barrier(g_msm.slot[slot], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list->ResourceBarrier(1, &in);
    D3D12_TEXTURE_COPY_LOCATION into = {}, from = {};
    into.pResource = g_msm.slot[slot]; into.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    from.pResource = src; from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    const D3D12_BOX box = { 0, 0, 0, static_cast<UINT>(std::min<UINT64>(w, sd.Width)), std::min<UINT>(h, sd.Height), 1 };
    list->CopyTextureRegion(&into, 0, 0, 0, &from, &box);
    D3D12_RESOURCE_BARRIER out = Barrier(g_msm.slot[slot], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    list->ResourceBarrier(1, &out);
    g_msm.pending = static_cast<int>(slot);
    g_msm.pending_stamp = lfCap.stamp > 0.0 ? lfCap.stamp : LatencyNow();
}

// The list with the copy was sent: the frame is handed over once the card has
// done it (the ready fence), and MFG SUPER MOTION is told.
static void MsmLinkSent()
{
    if (g_msm.pending < 0 || g_msm.head == nullptr) return;
    const UINT slot = static_cast<UINT>(g_msm.pending);
    g_msm.pending = -1;
    const int64_t next = g_msm.sequence + 1;
    if (FAILED(lfCore.direct_queue->Signal(g_msm.ready, static_cast<UINT64>(next)))) return;
    g_msm.sequence = next;
    LinkHeader *head = g_msm.head;
    head->frame[slot].stamp = MsmTicks(g_msm.pending_stamp);
    head->frame[slot].sequence = next;
    InterlockedExchange64(&head->latest, next);
    head->producer_beat = MsmNowTicks();
    if (g_msm.event) SetEvent(g_msm.event);
}
