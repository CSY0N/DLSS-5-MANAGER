// lf_state.h - the worker's state, one struct per subject instead of loose
// globals. Each lives exactly once (lfShow, lfCap, ...); nothing here is
// shared with another process.
#pragma once

// The process: how it was started and whether the card is still with us.
struct RunState
{
    LARGE_INTEGER qpc_rate;
    bool gpu_lost;
    bool unbounded = false;
    FILE *reply = nullptr;
};
static RunState lfRun;

// The card the network, the source and the output all share.
struct CardState
{
    int index = -1;
    IDXGIAdapter3 *adapter = nullptr;
};
static CardState lfCard;

// The network runtime: its DLL, its entry points and the last answers.
struct NrState
{
    bool initialized = false;
    HMODULE module;
    NrInitFn init_ext;
    NrCreateFn create;
    NrEvaluateFn evaluate;
    NrReleaseFn release;
    uint32_t create_code;
    uint32_t evaluations;
    NVSDK_NGX_FeatureCommonInfo search_info = {};
    wchar_t search_dirs[2][MAX_PATH];
    const wchar_t *search_list[2] = { search_dirs[0], search_dirs[1] };
    uint32_t last_code = 0;
    bool direct = !DetailWanted();
};
static NrState lfNr;

// The architecture answer given to the runtime (see ArchPrepare).
struct ArchState
{
    void             *handles[kArchSlots];
    ArchRecord        cache[kArchSlots];
    int               count;
    bool              patched;
    unsigned          real_group;
    void *hook_site;
};
static ArchState lfArch;

// Card-side timings and per-frame records (LF_PHASE=1).
struct TimingState
{
    FrameTiming stamp;
    UINT64 capture_gen, capture_serial, last_source_qpc;
    char rows[256][480];
    unsigned row_count;
    double submit_sum[GS_COUNT], submit_peak[GS_COUNT];
    double wait_sum[GS_COUNT], wait_peak[GS_COUNT];
    double gpu_sum[GS_COUNT], gpu_peak[GS_COUNT];
    double gap_sum[GS_COUNT], gap_peak[GS_COUNT];
    unsigned submit_n[GS_COUNT], wait_n[GS_COUNT], gpu_n[GS_COUNT], gap_n[GS_COUNT];
    unsigned allocator_waits;
    double allocator_wait_sum, allocator_wait_peak;
    unsigned in_flight_peak;
    int stage_now = GS_NONE;
    int slot_stage[GpuCore::kRing] = { GS_NONE, GS_NONE, GS_NONE };
    UINT64 slot_mark[GpuCore::kRing];
    ID3D12QueryHeap *query_heap;
    ID3D12Resource  *query_readback;
    UINT64           tick_rate;
    int              query_state;
    double           query_total;
    double           query_peak;
    unsigned         query_n;
    UINT64 last_gpu_end;
};
static TimingState lfProf;

// CPU-side step timings and counters (LF_PHASE=1).
struct PhaseState
{
    double sum[PHASE_COUNT];
    double peak[PHASE_COUNT];
    unsigned count[PHASE_COUNT];
    unsigned bins[6];
    unsigned requests;
    unsigned fresh_in;
    unsigned evaluated;
    unsigned fresh_evaluated;
    unsigned processed;
    unsigned idle;
    UINT64 window_start;
    int enabled = -1;
};
static PhaseState lfPhase;

// HDR: what the source delivers and how the result goes back on it.
struct HdrState
{
    bool float_frames = false;
    bool keep = false;
    // The output goes out in half float, linear light, SDR range (the colour
    // space picked: linear, or scRGB on a screen that is not in HDR). Given
    // up for the session when the screen does not take it.
    bool linear = false, linear_failed = false;
    float paper_white = 1.0f;
    UINT compare_x = UINT_MAX;
};
static HdrState lfHdr;

// What was last sent or shown, so repeats can be skipped.
struct OutState
{
    bool passthrough_before = false;
    uint32_t still_count = 0;
    uint32_t still_runs = 0;      // network runs on a picture with nothing new after it
    bool     still_logged = false;
    bool     sent_passthrough = false;
    bool     sent_compare = false;
    uint32_t sent_compare_x = 0;
    bool     force_next = false;
    UINT compare_x = 0;
};
static OutState lfOut;

// The extras word from the engine (sharpness, edges, FLOW MFG, frame limit).
struct ExtrasState
{
    float sharpness = -1.0f;
    float edges = -1.0f;
    int   flow_count = -1;
    int   flow_view = -1;
    std::atomic<int> frame_limit{-1};
};
static ExtrasState lfExtras;

// The engine: its hello, the window message, the shared input section.
struct LinkState
{
    HelloMsg hello = {};
    HANDLE shared_file = nullptr;
    const BYTE *shared_view = nullptr;
    size_t shared_size = 0;
    size_t motion_offset = 0;
    WindowMsg             window_msg = {};
};
static LinkState lfLink;

// The output window, its swap chain and its thread.
struct ShowState
{
    bool history_restart = true;       // FLOW MFG starts its history again with the next frame
    bool on_flow_queue = false;
    UINT64 flow_mark = 0;
    HANDLE flow_waitable = nullptr;
    HWND                       hwnd;
    int                        x = 0;
    int                        y = 0;
    std::atomic<bool>          visible{false};
    std::atomic<bool>          revealed{false};
    RECT                       follow_rect = {};
    IDXGISwapChain3           *chain;
    // Low Latency (lf_latency.inl): a queue of one frame, and the capture waits
    // for it to be free.
    HANDLE                     quick_waitable = nullptr;
    bool                       low_latency = false;
    DXGI_COLOR_SPACE_TYPE      color_space = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    bool                       color_space_set = false;
    HANDLE                     thread;
    DWORD                      thread_id;
    UINT                       w, h;          // the picture's size
    UINT                       bw, bh;        // the swap chain's: the display's with Flow Super Resolution
    bool                       upscaling = false;   // the picture is scaled onto the display
    bool                       vrr = false;         // VRR: presents wait for the screen (sync 1), never tear
    bool                       capturable;
    uint32_t                   flags = 0;
    bool                       stale = false;
    volatile LONG              state;
};
static ShowState lfShow;

// The source: duplication, window or screen capture, and the D3D11->D3D12 bridge.
struct CaptureState
{
    HMONITOR monitor = nullptr;
    HdrDisplayState display;
    bool                       window_live = false;
    bool                       window_dxgi = false;
    RECT                       window_crop = {};
    HWND                       window = nullptr;
    bool                    opened_hdr = false;
    bool                    upside_down = false;
    bool                    reopen_tried = false;
    bool                    live = false;
    bool                    picture_changed = false;
    double                  stamp = 0.0;
    UINT                    w = 0, h = 0;
    ID3D11Device           *d11 = nullptr;
    ID3D11DeviceContext    *d11_ctx = nullptr;
    IDXGIOutputDuplication *duplication = nullptr;
    ID3D11Texture2D        *bridge11 = nullptr;
    HANDLE                  bridge_handle = nullptr;
    ID3D12Resource         *bridge12 = nullptr;
    ID3D12Resource         *rgba = nullptr;
    ID3D12Fence            *sync12 = nullptr;
    ID3D11Fence            *sync11 = nullptr;
    HANDLE                  sync_event = nullptr;
    UINT64                  sync_value = 1;
    bool                    has_frame = false;
    bool screen_via_wgc = false;
    ID3D12RootSignature  *swz_root = nullptr;
    ID3D12PipelineState  *swz_pso = nullptr;
    ID3D12DescriptorHeap *swz_heap = nullptr;
    bool luma_ok = true;
    WgcLink *window_link = nullptr;
    WgcLink *screen_link = nullptr;
};
static CaptureState lfCap;

// The luma sections for the engine and for NVIDIA Optical Flow.
struct LumaState
{
    ID3D12DescriptorHeap *heap = nullptr; // separate from capture conversion
    HANDLE                  file = nullptr;
    BYTE                   *view = nullptr;
    size_t                  size = 0;
    UINT                    w = 0, h = 0;
    UINT                    pitch = 0;
    ID3D12Resource         *readback = nullptr;
    ID3D12Resource         *target = nullptr;
    bool                    mapped = false;
    ID3D12RootSignature  *root = nullptr;
    ID3D12PipelineState  *pso = nullptr;
    ID3D12Resource       *flow_tex;
    ID3D12DescriptorHeap *flow_heap;
    UINT                  flow_w, flow_h;
};
static LumaState lfLuma;

// The resize, crisp and detail pipelines and what they are bound to.
struct ScalerState
{
    ID3D12RootSignature  *root;
    ID3D12PipelineState  *pso;
    ID3D12DescriptorHeap *heap;
    ID3D12Resource       *src_now;
    ID3D12Resource       *dst_now;
    ID3D12PipelineState  *fine_pso;
    ID3D12PipelineState  *crisp_pso;
    ID3D12DescriptorHeap *fine_heap;
    ID3D12Resource       *fine_src;
    ID3D12Resource       *fine_dst;
    ID3D12Resource       *fine_src2;
    ID3D12Resource       *fine_dst2;
    ID3D12RootSignature  *detail_root;
    ID3D12PipelineState  *detail_pso;
    ID3D12DescriptorHeap *detail_heap;
    ID3D12Resource       *detail_native;
    ID3D12Resource       *detail_in;
    ID3D12Resource       *detail_out;
    ID3D12Resource       *detail_dst;
    ID3D12Resource       *detail_delta;
    StagedTexture              motion_small;
    UINT                  motion_w, motion_h;
};
static ScalerState lfScaler;

// The export section finished pictures are published into.
struct ExportState
{
    HANDLE file;
    BYTE  *view;
    size_t size;
    uint64_t sequence;
    std::mutex lock;
    std::atomic<uint64_t> recording_since{0};
};
static ExportState lfExport;

// The exposure follower's gain and clock.
struct ExposureState
{
    float gain = 1.0f;
    double last = 0.0;
    bool   logged = false;
};
static ExposureState lfExposure;

// The compare wipe's descriptor heaps and target.
struct CompareState
{
    ID3D12DescriptorHeap *heap_gpu;
    ID3D12DescriptorHeap *heap_cpu;
    ID3D12Resource       *target;
};
static CompareState lfCompare;

struct TimingGuard
{
    const Pipeline &video;
    const FrameMsg &frame;
    ~TimingGuard()
    {
        if (PhasesOn() && !lfProf.stamp.reported)
        {
            TimingResult(video, frame, false, "failed");
            TimingFlush();
        }
    }
};
