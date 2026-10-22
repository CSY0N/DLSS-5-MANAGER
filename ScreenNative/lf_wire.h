// lf_wire.h - what travels over the pipe between the engine (transport.py)
// and the worker: the message tags, the flag bits, and the packed structs.
// The sizes are pinned by static_asserts; transport.py keeps the same
// layouts, byte for byte.
#pragma once

static constexpr uint32_t kHelloTag     = 0x3248464Cu;
static constexpr uint32_t kHelloTagLong = 0x3348464Cu;
static constexpr uint32_t kTakeTag = 0x4B54464Cu;
static constexpr uint32_t kFramePrepared = 0x1000u;
static constexpr uint32_t kFrameTag = 0x5246464Cu;
static constexpr uint32_t kResultTag   = 0x5352464Cu;
static constexpr uint32_t kReshapeTag    = 0x5A52464Cu;
static constexpr uint32_t kReshapeOk = 0x4B52464Cu;
static constexpr uint32_t kSharedTag     = 0x4D53464Cu;
static constexpr uint32_t kSharedOk = 0x4153464Cu;
static constexpr uint32_t kScreenTag     = 0x4353464Cu;
static constexpr uint32_t kScreenOk = 0x4B53464Cu;
static constexpr uint32_t kMotionTag     = 0x4F4D464Cu;
static constexpr uint32_t kMotionOk = 0x414D464Cu;
static constexpr uint32_t kDesktopTag        = 0x4B44464Cu;
static constexpr uint32_t kDesktopOk    = 0x4144464Cu;
static constexpr uint32_t kWindowTag        = 0x4E57464Cu;
static constexpr uint32_t kWindowOk    = 0x4157464Cu;
static constexpr uint32_t kLumaTag       = 0x554C464Cu;
static constexpr uint32_t kLumaOk   = 0x414C464Cu;
static constexpr uint32_t kExportTag       = 0x5845464Cu;
static constexpr uint32_t kExportOk   = 0x4145464Cu;
static constexpr uint32_t kPixelsShared = 0xFFFFFFFFu;
static constexpr uint32_t kScreenCapturable = 0x1u;
static constexpr uint32_t kScreenOff    = 0x2u;
static constexpr uint32_t kFrameShared = 0x1u;
static constexpr uint32_t kFrameWantsPixels = 0x2u;
static constexpr uint32_t kFrameSmallMotion = 0x4u;
static constexpr uint32_t kFrameNoPicture = 0x8u;
static constexpr uint32_t kFramePassThrough = 0x10u;
static constexpr uint32_t kFrameCompare = 0x20u;
static constexpr uint32_t kFrameIdleWhenStill = 0x40u;
static constexpr uint32_t kFrameRecording = 0x80u;
static constexpr size_t   kHelloShortBytes = 56;
static constexpr uint32_t kReshapeSmall = 0x1u;
static constexpr uint32_t kReshapeDirect = 0x2u;
// The output window's display switches (flow_upscaler.inl, lf_vrr.inl,
// lf_latency.inl). kReshapeDisplay says the bits after it mean something, so
// an engine that never sends them cannot read as "all off".
static constexpr uint32_t kReshapeDisplay    = 0x20000u;
static constexpr uint32_t kReshapeUpscale    = 0x40000u;   // Flow Super Resolution
static constexpr uint32_t kReshapeVrr        = 0x80000u;
static constexpr uint32_t kReshapeLowLatency = 0x100000u;
// FLOW MFG's performance mode (round 26, flow_mfg.inl).
static constexpr uint32_t kReshapeFlowFast   = 0x800000u;
// Bits 21..22 carried round 23's render scale; since round 28 they hold DLSS
// MFG's Queue target plus one (lf_fg_pace.inl), 0 = not said.
static constexpr uint32_t kReshapeFgQueueMask  = 0x600000u;
static constexpr uint32_t kReshapeFgQueueShift = 21u;
// Low-light noise (round 34, lf_settings.inl): bits 3-7 hold it in twentieths
// plus one (1..21), 0 = not said.
static constexpr uint32_t kReshapeLowLightMask  = 0xF8u;
static constexpr uint32_t kReshapeLowLightShift = 3u;
// A frame whose motion the worker finds itself (NVIDIA Optical Flow, or its
// own GPU estimate): no motion field follows the message, and the engine did
// not look for a scene cut either (round 23).
static constexpr uint32_t kFrameOwnMotion = 0x2000u;

#pragma pack(push, 1)
struct HelloMsg
{
    uint32_t magic, width, height, warmup, frame_count, profile, preset, style, auto_mask, ui_correction;
    float intensity, local_tone, local_structure, skin_structure;
    uint32_t full_w, full_h;
};

struct FrameMsg
{
    uint32_t magic, index, reset, reserved;
    int64_t pts;
};

struct ResultMsg
{
    uint32_t magic, index, ok, bytes, ngx_result;
    int64_t pts;
};

struct ReshapeMsg
{
    uint32_t magic, width, height, warmup;

    uint32_t flags;
    uint32_t profile, preset, style, auto_mask, ui_correction;
    float intensity, local_tone, local_structure, skin_structure;
    uint32_t full_w, full_h;
};

struct ReshapeReply
{
    uint32_t magic, ok, ngx_result, reserved;
    int64_t pts;
};

struct SharedMsg
{
    uint32_t magic, color_bytes, motion_bytes, flags;
    int64_t pts;
    char name[64];
};

struct SharedReply
{
    uint32_t magic, ok, reserved0, reserved1;
    int64_t pts;
};

struct ScreenMsg
{
    uint32_t magic, width, height, flags;
    int64_t pts;
};

struct ScreenReply
{
    uint32_t magic, ok, reserved0, reserved1;
    int64_t pts;
};

struct MotionMsg
{
    uint32_t magic, width, height, flags;
    int64_t pts;
};

struct MotionReply
{
    uint32_t magic, ok, reserved0, reserved1;
    int64_t pts;
};

// DesktopMsg.flags: every finished frame of the whole screen is handed to
// SMFG (MFG SUPER MOTION's engine; round 33, lf_msm_link.inl).
static constexpr uint32_t kDesktopLink = 0x1u;

struct DesktopMsg
{
    uint32_t magic, width, height, flags;
    int64_t pts;
};

struct DesktopReply
{
    uint32_t magic, ok, reserved0, reserved1;
    int64_t pts;
};

// WindowMsg.flags: the window keeps its own size and its picture is scaled onto
// the whole screen it is on (round 31, flow_upscaler.inl).
static constexpr uint32_t kWindowFill = 0x1u;
// WindowMsg.flags: every finished frame is handed to MFG SUPER MOTION (round
// 32, lf_msm_link.inl).
static constexpr uint32_t kWindowLink = 0x2u;

struct WindowMsg
{
    uint32_t magic, width, height, flags;
    int64_t pts;
    uint64_t hwnd;
};

struct WindowReply
{
    uint32_t magic, ok, width, height;
    int64_t pts;
};

struct LumaMsg
{
    uint32_t magic, width, height, flags;
    int64_t pts;
    char name[64];
};

struct ExportMsg
{
    uint32_t magic, width, height, flags;
    int64_t pts;
    char name[64];
};

struct ExportReply
{
    uint32_t magic, ok, reserved0, reserved1;
    int64_t pts;
};

struct LumaReply
{
    uint32_t magic, ok, reserved0, reserved1;
    int64_t pts;
};
#pragma pack(pop)

static_assert(sizeof(HelloMsg) == 64, "VideoHeader != HEADER_FMT");
static_assert(sizeof(FrameMsg) == 24, "VideoFrameHeader != FRAME_FMT");
static_assert(sizeof(ResultMsg) == 28, "VideoResultHeader != OUT_FMT");
static_assert(sizeof(ReshapeMsg) == 64, "VideoResizeCmd != RESIZE_FMT");
static_assert(sizeof(ReshapeReply) == 24, "VideoResizeAck != RACK_FMT");
static_assert(sizeof(SharedMsg) == 88, "VideoShmCmd != SHM_FMT");
static_assert(sizeof(SharedReply) == 24, "VideoShmAck != SHM_ACK_FMT");
static_assert(sizeof(ScreenMsg) == 24, "VideoWindowCmd != WINDOW_FMT");
static_assert(sizeof(ScreenReply) == 24, "VideoWindowAck != WINDOW_ACK_FMT");
static_assert(sizeof(MotionMsg) == 24, "VideoMotionCmd != MOTION_FMT");
static_assert(sizeof(MotionReply) == 24, "VideoMotionAck != MOTION_ACK_FMT");
static_assert(sizeof(DesktopMsg) == 24, "VideoDdaCmd != DDA_FMT");
static_assert(sizeof(DesktopReply) == 24, "VideoDdaAck != DDA_ACK_FMT");
static_assert(sizeof(WindowMsg) == 32, "VideoWgcCmd != WGC_FMT");
static_assert(sizeof(WindowReply) == 24, "VideoWgcAck != WGC_ACK_FMT");
static_assert(sizeof(LumaMsg) == 88, "VideoGrayCmd != GRAY_FMT");
static_assert(sizeof(LumaReply) == 24, "VideoGrayAck != GRAY_ACK_FMT");
static_assert(sizeof(ExportMsg) == 88, "VideoOutCmd != OUTS_FMT");
static_assert(sizeof(ExportReply) == 24, "VideoOutAck != OUTS_ACK_FMT");
static_assert(offsetof(FrameMsg, pts) == 16, "VideoFrameHeader is not packed");
static_assert(offsetof(ResultMsg, pts) == 20, "VideoResultHeader is not packed");
static_assert(offsetof(WindowMsg, hwnd) == 24, "VideoWgcCmd is not packed");
