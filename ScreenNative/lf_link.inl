// lf_link.inl - the link with the engine: exact reads and writes on the pipe,
// the next message, the shared memory the pictures arrive in, and the export
// section finished pictures go out through.

// Keep the pipe for messages only: stdout goes binary, and anything else
// that writes to it (a runtime printing a line) is sent to stderr instead.
static void PipeClaim()
{
    {
        char v[8] = {};
        const DWORD len = GetEnvironmentVariableA("LF_SHARED_STDOUT", v, sizeof(v));
        if (len > 0 && len < sizeof(v) && v[0] == '1')
        {
            lfRun.reply = stdout;
            Trace("[pipe] LF_SHARED_STDOUT=1: messages and replies share stdout");
            return;
        }
    }
    const int copy = _dup(_fileno(stdout));
    if (copy < 0) { lfRun.reply = stdout; return; }
    lfRun.reply = _fdopen(copy, "wb");
    if (lfRun.reply == nullptr) { lfRun.reply = stdout; return; }
    _setmode(copy, _O_BINARY);
    setvbuf(lfRun.reply, nullptr, _IONBF, 0);

    _dup2(_fileno(stderr), _fileno(stdout));

    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err != nullptr) SetStdHandle(STD_OUTPUT_HANDLE, err);
}

// Read exactly n bytes from the pipe, or fail.
static bool PipeRead(FILE *f, void *p, size_t n)
{
    BYTE *into = static_cast<BYTE *>(p);
    while (n != 0)
    {
        const size_t len = fread(into, 1, n, f);
        if (len == 0) return false;
        into += len; n -= len;
    }
    return true;
}

// Write exactly n bytes to the pipe, or fail.
static bool PipeWrite(FILE *f, const void *p, size_t n)
{
    if (lfRun.gpu_lost) return false;
    const BYTE *from = static_cast<const BYTE *>(p);
    while (n != 0)
    {
        const size_t put = fwrite(from, 1, n, f);
        if (put == 0) return false;
        from += put; n -= put;
    }
    fflush(f);
    return true;
}

// Read one message and do what it asks. Frames come back to the loop;
// reshape, shared memory, source and output messages are handled here.
static int ReadNextMessage(Pipeline &pipe, FrameMsg &frame_msg, std::vector<BYTE> &pic,
                            std::vector<BYTE> &motion, ReshapeMsg &reshape, SharedMsg &shared,
                            ScreenMsg &screen, MotionMsg &motion_msg, DesktopMsg &desk,
                            LumaMsg &luma_msg, ExportMsg &export_msg,
                            const BYTE **color_ptr, const BYTE **mv_ptr)
{
    if (!PipeRead(stdin, &frame_msg, sizeof(frame_msg))) return 0;

    if (frame_msg.magic == kTakeTag) return 10;
    if (frame_msg.magic == kFrameTag)
    {
        if ((frame_msg.reserved & kFrameRecording) != 0)
            lfExport.recording_since.store(GetTickCount64(), std::memory_order_relaxed);
        bool no_color = (frame_msg.reserved & kFrameNoPicture) != 0;
        if (no_color && !SourceLive())
        {
            if (lfCap.window != nullptr)
            {
                if (!WindowStart(lfCap.window) || !lfCap.window_live) return 0;
            }
            else if (lfCap.w == 0 || lfCap.h == 0)
            { Trace("[source] a frame without a picture, and no source to reopen - the pipe is out of step"); return 0; }
            else if (!DeskStart(lfCap.w, lfCap.h) || !lfCap.live) return 0;
        }
        const size_t pic_w = pipe.stretched ? pipe.screen_w : pipe.work_w;
        const size_t pic_h = pipe.stretched ? pipe.screen_h : pipe.work_h;
        const size_t color_bytes = pic_w * pic_h * 4;

        const bool small_mv = (frame_msg.reserved & kFrameSmallMotion) != 0 && lfScaler.motion_w != 0;
        const size_t mv_bytes = small_mv
            ? static_cast<size_t>(lfScaler.motion_w) * lfScaler.motion_h * 4
            : static_cast<size_t>(pipe.work_w) * pipe.work_h * 4;
        if (no_color)
        {
            *color_ptr = nullptr;
            // The worker finds the motion itself: nothing follows (round 23).
            if ((frame_msg.reserved & kFrameOwnMotion) != 0) { *mv_ptr = nullptr; return 1; }
            motion.resize(mv_bytes);
            *mv_ptr = motion.data();
            return PipeRead(stdin, motion.data(), motion.size()) ? 1 : 0;
        }
        if ((frame_msg.reserved & kFrameShared) != 0)
        {
            if (lfLink.shared_view == nullptr || lfLink.motion_offset < color_bytes ||
                lfLink.shared_size < lfLink.motion_offset + mv_bytes)
            {
                Trace("[pipe] frame %u is in shared memory, but the view is missing or small "
                    "(%zu bytes mapped; picture %zu at 0, motion %zu at %zu)",
                    frame_msg.index, lfLink.shared_size, color_bytes, mv_bytes, lfLink.motion_offset);
                return 0;
            }
            *color_ptr = lfLink.shared_view;
            *mv_ptr = lfLink.shared_view + lfLink.motion_offset;
            return 1;
        }
        pic.resize(color_bytes); motion.resize(mv_bytes);
        *color_ptr = pic.data(); *mv_ptr = motion.data();
        return PipeRead(stdin, pic.data(), pic.size()) && PipeRead(stdin, motion.data(), motion.size()) ? 1 : 0;
    }
    if (frame_msg.magic == kSharedTag)
    {
        BYTE *p = reinterpret_cast<BYTE *>(&shared);
        memcpy(p, &frame_msg, sizeof(frame_msg));
        if (!PipeRead(stdin, p + sizeof(frame_msg), sizeof(shared) - sizeof(frame_msg))) return 0;
        return 3;
    }
    if (frame_msg.magic == kScreenTag)
    {
        memcpy(&screen, &frame_msg, sizeof(screen));
        return 4;
    }
    if (frame_msg.magic == kMotionTag)
    {
        memcpy(&motion_msg, &frame_msg, sizeof(motion_msg));
        return 5;
    }
    if (frame_msg.magic == kDesktopTag)
    {
        memcpy(&desk, &frame_msg, sizeof(desk));
        return 6;
    }
    if (frame_msg.magic == kWindowTag)
    {
        BYTE *p = reinterpret_cast<BYTE *>(&lfLink.window_msg);
        memcpy(p, &frame_msg, sizeof(frame_msg));
        if (!PipeRead(stdin, p + sizeof(frame_msg), sizeof(lfLink.window_msg) - sizeof(frame_msg))) return 0;
        return 9;
    }
    if (frame_msg.magic == kLumaTag)
    {
        BYTE *p = reinterpret_cast<BYTE *>(&luma_msg);
        memcpy(p, &frame_msg, sizeof(frame_msg));
        if (!PipeRead(stdin, p + sizeof(frame_msg), sizeof(luma_msg) - sizeof(frame_msg))) return 0;
        return 7;
    }
    if (frame_msg.magic == kExportTag)
    {
        BYTE *p = reinterpret_cast<BYTE *>(&export_msg);
        memcpy(p, &frame_msg, sizeof(frame_msg));
        if (!PipeRead(stdin, p + sizeof(frame_msg), sizeof(export_msg) - sizeof(frame_msg))) return 0;
        return 8;
    }
    if (frame_msg.magic == kReshapeTag)
    {
        BYTE *p = reinterpret_cast<BYTE *>(&reshape);
        memcpy(p, &frame_msg, sizeof(frame_msg));
        if (!PipeRead(stdin, p + sizeof(frame_msg), sizeof(reshape) - sizeof(frame_msg))) return 0;
        return 2;
    }
    return 0;
}

// Unmap and close the shared input section.
static void SharedStop()
{
    if (lfLink.shared_view != nullptr) { UnmapViewOfFile(lfLink.shared_view); lfLink.shared_view = nullptr; }
    if (lfLink.shared_file != nullptr) { CloseHandle(lfLink.shared_file); lfLink.shared_file = nullptr; }
    lfLink.shared_size = 0;
    lfLink.motion_offset = 0;
}

// Hand a finished picture back to the engine: through shared memory when
// there is room, through the pipe otherwise.
static bool SendPixels(const std::vector<BYTE> &output, uint32_t index,
                          int64_t when)
{
    if (lfExport.view != nullptr && !output.empty()
        && output.size() + 8 == lfExport.size)
    {
        std::lock_guard<std::mutex> lock(lfExport.lock);
        uint64_t seq = ++lfExport.sequence;
        if ((seq & 1) == 0) ++seq;
        memcpy(lfExport.view, &seq, sizeof(seq));
        memcpy(lfExport.view + 8, output.data(), output.size());
        ++seq;
        memcpy(lfExport.view, &seq, sizeof(seq));
        ResultMsg result_msg = { kResultTag, index, 1u, kPixelsShared,
                                  lfNr.last_code, when };
        return PipeWrite(lfRun.reply, &result_msg, sizeof(result_msg));
    }
    ResultMsg result_msg = { kResultTag, index, 1u,
                              static_cast<uint32_t>(output.size()),
                              lfNr.last_code, when };
    return PipeWrite(lfRun.reply, &result_msg, sizeof(result_msg))
        && PipeWrite(lfRun.reply, output.data(), output.size());
}

// Open the export section the engine named (for recording and screenshots).
static bool ExportStart(const ExportMsg &export_msg)
{
    ExportStop();
    if (export_msg.width == 0 || export_msg.height == 0) { Trace("[export] stopped"); return true; }
    char name[64] = {};
    memcpy(name, export_msg.name, sizeof(name) - 1);
    const size_t need = static_cast<size_t>(export_msg.width) * export_msg.height * 4 + 8;
    lfExport.file = OpenFileMappingA(FILE_MAP_WRITE, FALSE, name);
    if (lfExport.file == nullptr)
    { Trace("[export] section '%s' did not open (%lu)", name, GetLastError()); return false; }

    lfExport.view = static_cast<BYTE *>(MapViewOfFile(lfExport.file, FILE_MAP_WRITE, 0, 0, 0));
    if (lfExport.view == nullptr)
    {
        Trace("[export] section '%s' did not map (%lu)", name, GetLastError());
        CloseHandle(lfExport.file); lfExport.file = nullptr;
        return false;
    }
    MEMORY_BASIC_INFORMATION mbi = {};
    const size_t have = (VirtualQuery(lfExport.view, &mbi, sizeof(mbi)) == sizeof(mbi))
                            ? static_cast<size_t>(mbi.RegionSize) : 0u;
    if (have < need)
    {
        Trace("[export] section '%s' holds %zu bytes, %zu needed - not used", name, have, need);
        ExportStop();
        return false;
    }
    lfExport.size = need;
    Trace("[export] pictures go into '%s', %zu bytes", name, need);
    return true;
}

// Close the export section.
static void ExportStop()
{
    if (lfExport.view != nullptr) { UnmapViewOfFile(lfExport.view); lfExport.view = nullptr; }
    if (lfExport.file != nullptr) { CloseHandle(lfExport.file); lfExport.file = nullptr; }
    lfExport.size = 0;
}

// Is anyone reading the export section right now.
static bool ExportWanted()
{
    const uint64_t at = lfExport.recording_since.load(std::memory_order_relaxed);
    return at != 0 && GetTickCount64() - at < 1500;
}

// Copy a shown picture into the export section with a new sequence number.
static void ExportPicture(const BYTE *from, size_t pitch, UINT w, UINT hgt)
{
    const size_t row = static_cast<size_t>(w) * 4;
    std::lock_guard<std::mutex> lock(lfExport.lock);
    if (lfExport.view == nullptr || from == nullptr) return;
    if (row * hgt + 8 != lfExport.size) return;
    uint64_t seq = ++lfExport.sequence;
    if ((seq & 1) == 0) ++seq;
    memcpy(lfExport.view, &seq, sizeof(seq));
    if (pitch == row)
        memcpy(lfExport.view + 8, from, row * hgt);
    else
        for (UINT y = 0; y < hgt; ++y)
            memcpy(lfExport.view + 8 + row * y, from + pitch * y, row);
    ++seq;
    memcpy(lfExport.view, &seq, sizeof(seq));
}
