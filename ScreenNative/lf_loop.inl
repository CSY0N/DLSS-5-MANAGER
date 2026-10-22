// lf_loop.inl - the live loop: read the hello, build the pipeline, then for
// every message take a picture, run it through the network and show it,
// until the engine hangs up.

// The worker's life: hello, pipeline, then one message after another.
static int LiveLoop()
{
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
    PipeClaim();
    {
        char pipe[8] = {};
        const DWORD len = GetEnvironmentVariableA("LF_STDOUT_NOISE", pipe, sizeof(pipe));
        if (len > 0 && len < sizeof(pipe) && pipe[0] == '1')
        {
            printf("[2026-09-13 00:00:00.000][NOISE] a stray line on stdout\n");
            fflush(stdout);
        }
    }
    HelloMsg hello = {};

    BYTE raw[sizeof(HelloMsg)] = {};
    if (!PipeRead(stdin, raw, kHelloShortBytes)) { Trace("[pipe] the engine sent no hello"); return 2; }
    memcpy(&hello, raw, kHelloShortBytes);
    if (hello.magic == kHelloTagLong)
    {
        if (!PipeRead(stdin, raw + kHelloShortBytes, sizeof(HelloMsg) - kHelloShortBytes))
        { Trace("[pipe] the long hello was cut short"); return 2; }
        memcpy(&hello, raw, sizeof(HelloMsg));
    }
    if ((hello.magic != kHelloTag && hello.magic != kHelloTagLong) || hello.width < 64 || hello.height < 64 ||
        hello.width > 7680 || hello.height > 4320)
    { Trace("[pipe] not a hello we know (tag 0x%08X)", hello.magic); return 2; }
    const bool upscale = (hello.magic == kHelloTagLong) && hello.full_w > 0 && hello.full_h > 0 &&
                         (hello.full_w != hello.width || hello.full_h != hello.height);
    if (upscale && (hello.full_w < hello.width || hello.full_h < hello.height || hello.full_w > 7680 || hello.full_h > 4320))
    { Trace("[pipe] the screen size %ux%u makes no sense", hello.full_w, hello.full_h); return 2; }
    lfLink.hello = hello;
    const bool live = lfRun.unbounded || (hello.frame_count == 0);
    std::string full_note;
    if (upscale) full_note = " (screen frames " + std::to_string(hello.full_w) + "x" + std::to_string(hello.full_h) + ")";
    Trace("[pipe] work %ux%u%s, %s, %u warm-up frames", hello.width, hello.height, full_note.c_str(),
        live ? "live, no frame limit" : "limited", hello.warmup);

    Pipeline pipe;
    if (!PipelineBuild(pipe, hello.width, hello.height, upscale ? hello.full_w : 0, upscale ? hello.full_h : 0))
    { Trace("[pipe] the working textures did not come up"); return 3; }
    int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    NVSDK_NGX_Result create_result = NVSDK_NGX_Result_Fail;
#ifdef LF_RADEON
    (void)flags;
    if (!NrLive())
        Trace("[pipe] the Radeon network is off - the picture passes through untouched");
    else if (false)
#else
    // The network upscales only in the old stretched mode, onto the picture
    // (the display's size, or Flow Super Resolution's); with Enhance it works
    // at the working size and its detail is added back.
    if (!NetworkCreate(hello.width, hello.height, flags, &create_result,
                       (pipe.net_small || !upscale) ? 0 : pipe.screen_w,
                       (pipe.net_small || !upscale) ? 0 : pipe.screen_h))
#endif
    {
        if (lfRun.gpu_lost) return 3;

        lfCore.ngx_feature = nullptr;
        Trace("[pipe] no network (0x%08X) - the picture passes through untouched",
            static_cast<uint32_t>(create_result));
    }

    FrameMsg frame_msg = {};
    std::vector<BYTE> color, motion, output;
    bool prepared = false, prepared_got = false;
    uint32_t prepared_index = 0;
    bool warmup_done = false;
    uint32_t frame = 0;
    for (;;)
    {
        ReshapeMsg reshape = {};
        SharedMsg shared = {};
        ScreenMsg screen = {};
        MotionMsg motion_msg = {};
        DesktopMsg desk = {};
        LumaMsg luma_msg = {};
        ExportMsg export_msg = {};
        const BYTE *color_ptr = nullptr;
        const BYTE *mv_ptr = nullptr;
        const int msg = ReadNextMessage(pipe, frame_msg, color, motion, reshape, shared, screen, motion_msg, desk, luma_msg,
                                         export_msg, &color_ptr, &mv_ptr);
        if (msg == 1 || msg == 10) ConfigureFgFrame(frame_msg.reserved);
        if (msg != 1 && msg != 10) prepared = false;
        if (msg == 0)
        {
            if (live)
            {
                TimingFlush();
                Trace("[pipe] the engine hung up after %u frames, %u network runs", frame, lfNr.evaluations);

                ShowStop();
                NgxShutdown();
                SharedStop();
                MotionResizeStop();
                DeskStop();
                LumaStop();

                ShareStop();
                return 0;
            }
            Trace("[pipe] frame %u was cut short", frame); return 5;
        }
        if (msg == 2)
        {
            if (reshape.width < 64 || reshape.height < 64 || reshape.width > 7680 || reshape.height > 4320)
            {
                Trace("[pipe] reshape refused: work size %ux%u makes no sense", reshape.width, reshape.height);
                ReshapeReply reply = { kReshapeOk, 0u, 0xBAD00005u, 0u, frame_msg.pts };
                if (!PipeWrite(lfRun.reply, &reply, sizeof(reply))) return 10;
                continue;
            }
            const bool rup = reshape.full_w > 0 && reshape.full_h > 0 &&
                             (reshape.full_w != reshape.width || reshape.full_h != reshape.height);
            if (rup && (reshape.full_w < reshape.width || reshape.full_h < reshape.height ||
                        reshape.full_w > 7680 || reshape.full_h > 4320))
            {
                Trace("[pipe] reshape refused: screen size %ux%u makes no sense", reshape.full_w, reshape.full_h);
                ReshapeReply reply = { kReshapeOk, 0u, 0xBAD00005u, 0u, frame_msg.pts };
                if (!PipeWrite(lfRun.reply, &reply, sizeof(reply))) return 10;
                continue;
            }

            const int want_small_now = (reshape.flags & kReshapeSmall) != 0 ? 1 : 0;
            // Flow Super Resolution decides the picture's size, so its switch
            // is read first: turning it on or off builds the pipeline again.
            UpscaleFromResize(reshape.flags);
            const bool sr_next = rup && SuperResWanted();
            const bool same_size = reshape.width == pipe.work_w && reshape.height == pipe.work_h &&
                                   sr_next == pipe.super_res && want_small_now == (pipe.net_small ? 1 : 0) &&
                                   (sr_next ? (reshape.full_w == pipe.display_w && reshape.full_h == pipe.display_h)
                                            : (rup ? (pipe.stretched && reshape.full_w == pipe.screen_w &&
                                                      reshape.full_h == pipe.screen_h)
                                                   : !pipe.stretched));
            if (same_size && NrLive())
            {
                memcpy(&lfLink.hello, &reshape, sizeof(lfLink.hello));
                HaloFromResizeFlags(reshape.flags);
                ReadExtrasWord(reshape.profile);
                ColorsFromResize(reshape.flags, reshape.preset);
                VrrFromResize(reshape.flags);
                LowLatencyFromResize(reshape.flags);
                FlowFastFromResize(reshape.flags);
                FgQueueFromResize(reshape.flags);
                KeepColourFromResize(reshape.flags);
                LowLightFromResize(reshape.flags);

                lfNr.direct = (reshape.flags & kReshapeDirect) != 0;
                pipe.add_detail = pipe.net_small && !lfNr.direct;
                pipe.detail_gain = pipe.add_detail ? DetailAmount() * EffectGain() : 1.0f;
                lfOut.force_next = true;
                ReshapeReply ok = { kReshapeOk, 1u,
                                      static_cast<uint32_t>(NVSDK_NGX_Result_Success),
                                      0u, frame_msg.pts };
                if (!PipeWrite(lfRun.reply, &ok, sizeof(ok))) return 10;
                Trace("[pipe] reshape at %ux%u only moved settings - the network stays"
                    " (%s)", pipe.work_w, pipe.work_h,
                    pipe.super_res ? "Flow Super Resolution"
                    : !pipe.net_small ? "the network is the output"
                                : (pipe.add_detail ? "detail added back"
                                              : "direct picture"));
                continue;
            }
            Trace("[pipe] reshape: work %ux%u -> %ux%u (screen %ux%u), %u warm-up frames",
                pipe.work_w, pipe.work_h, reshape.width, reshape.height, reshape.full_w, reshape.full_h, reshape.warmup);

            AwaitMark(lfCore.timeline, lfCore.timeline_value, 2000);

            NgxReleaseGuarded(lfCore.ngx_feature);
            lfCore.ngx_feature = nullptr;
            PipelineRelease(pipe);

            memcpy(&lfLink.hello, &reshape, sizeof(lfLink.hello));
            HaloFromResizeFlags(reshape.flags);
            ReadExtrasWord(reshape.profile);
            ColorsFromResize(reshape.flags, reshape.preset);
            VrrFromResize(reshape.flags);
            LowLatencyFromResize(reshape.flags);
            FlowFastFromResize(reshape.flags);
            FgQueueFromResize(reshape.flags);
            KeepColourFromResize(reshape.flags);
            LowLightFromResize(reshape.flags);

            lfNr.direct = (reshape.flags & kReshapeDirect) != 0;
            const int small_wanted = (reshape.flags & kReshapeSmall) != 0 ? 1 : 0;
            if (!PipelineBuild(pipe, reshape.width, reshape.height, rup ? reshape.full_w : 0,
                                      rup ? reshape.full_h : 0, small_wanted))
            {
                Trace("[pipe] reshape: working textures failed at %ux%u", reshape.width, reshape.height);
                ReshapeReply reply = { kReshapeOk, 0u, 0x7FFFFFFFu, 0u, frame_msg.pts };
                if (!PipeWrite(lfRun.reply, &reply, sizeof(reply))) return 3;
            }
            NVSDK_NGX_Result rr = NVSDK_NGX_Result_Fail;
#ifdef LF_RADEON
            if (NrLive()) rr = NVSDK_NGX_Result_Success;
            else
#else
            if (!NetworkCreate(reshape.width, reshape.height, flags, &rr,
                               (pipe.net_small || !rup) ? 0 : pipe.screen_w,
                               (pipe.net_small || !rup) ? 0 : pipe.screen_h))
#endif
            {
                if (lfRun.gpu_lost) return 3;
                lfCore.ngx_feature = nullptr;
                Trace("[pipe] reshape: no network at %ux%u - the picture passes through untouched",
                    reshape.width, reshape.height);
            }
            warmup_done = !NrLive();
            ReshapeReply ack = { kReshapeOk, 1u, static_cast<uint32_t>(rr), 0u, frame_msg.pts };
            if (!PipeWrite(lfRun.reply, &ack, sizeof(ack))) return 10;
            lfOut.force_next = true;

            Trace("[pipe] reshape done at %ux%u: %s, %s", reshape.width, reshape.height,
                NrLive() ? "network ready" : "passing through untouched",
                pipe.super_res ? "Flow Super Resolution"
                : !pipe.net_small ? "the network is the output"
                            : (pipe.add_detail ? "detail added back"
                                          : "direct picture"));
            continue;
        }
        if (msg == 3)
        {
            shared.name[sizeof(shared.name) - 1] = '\0';
            SharedStop();
            const size_t need = static_cast<size_t>(shared.color_bytes) +
                                static_cast<size_t>(shared.motion_bytes);
            uint32_t ok = 0;
            if (shared.color_bytes == 0 || shared.motion_bytes == 0 || need > (size_t)1 << 31)
                Trace("[pipe] shared memory refused: picture %u, motion %u make no sense",
                    shared.color_bytes, shared.motion_bytes);
            else
            {
                lfLink.shared_file = OpenFileMappingA(FILE_MAP_READ, FALSE, shared.name);
                if (lfLink.shared_file == nullptr)
                    Trace("[pipe] shared memory '%s' did not open (%lu)",
                        shared.name, GetLastError());
                else
                {
                    lfLink.shared_view = static_cast<const BYTE *>(
                        MapViewOfFile(lfLink.shared_file, FILE_MAP_READ, 0, 0, need));
                    if (lfLink.shared_view == nullptr)
                    {
                        Trace("[pipe] shared memory (%zu bytes) did not map (%lu)", need, GetLastError());
                        CloseHandle(lfLink.shared_file);
                        lfLink.shared_file = nullptr;
                    }
                    else
                    {
                        lfLink.shared_size = need;
                        lfLink.motion_offset = shared.color_bytes;
                        ok = 1;
                        Trace("[pipe] shared memory '%s' in use, %zu bytes (motion from +%zu)",
                            shared.name, need, lfLink.motion_offset);
                    }
                }
            }
            SharedReply ack = { kSharedOk, ok, 0u, 0u, shared.pts };
            if (!PipeWrite(lfRun.reply, &ack, sizeof(ack))) return 10;
            continue;
        }
        if (msg == 4)
        {
            uint32_t ok = 0;
            if ((screen.flags & kScreenOff) != 0 || screen.width == 0 || screen.height == 0)
            {
                ShowStop();
                Trace("[screen] output window closed - pictures go back to the engine");
                ok = 1;
            }
            else
                ok = ShowStart(screen.width, screen.height, screen.flags) ? 1u : 0u;
            if (ok) lfOut.force_next = true;
            ScreenReply ack = { kScreenOk, ok, 0u, 0u, screen.pts };
            if (!PipeWrite(lfRun.reply, &ack, sizeof(ack))) return 10;
            continue;
        }
        if (msg == 5)
        {
            const uint32_t ok = MotionResizeStart(motion_msg.width, motion_msg.height) ? 1u : 0u;
            MotionReply ack = { kMotionOk, ok, 0u, 0u, motion_msg.pts };
            if (!PipeWrite(lfRun.reply, &ack, sizeof(ack))) return 10;
            continue;
        }
        if (msg == 6)
        {
            uint32_t ok = 0;
            if (desk.width == 0 || desk.height == 0)
            {
                DeskStop();
                ok = 1;
            }
            else
            {
                MsmLinkSet(nullptr, (desk.flags & kDesktopLink) != 0, false);
                ok = DeskStart(desk.width, desk.height) ? 1u : 0u;
            }
            Trace("[pipe] desktop source %s (%ux%u)", ok ? "up" : "failed", desk.width, desk.height);
            DesktopReply ack = { kDesktopOk, ok, 0u, 0u, desk.pts };
            if (!PipeWrite(lfRun.reply, &ack, sizeof(ack))) return 10;
            continue;
        }
        if (msg == 7)
        {
            uint32_t ok = 0;
            if (luma_msg.width == 0 || luma_msg.height == 0)
            {
                LumaStop();
                ok = 1;
            }
            else
                ok = LumaStart(luma_msg) ? 1u : 0u;
            Trace("[pipe] luma channel %s (%ux%u)", ok ? "up" : "failed", luma_msg.width, luma_msg.height);
            LumaReply ack = { kLumaOk, ok, 0u, 0u, luma_msg.pts };
            if (!PipeWrite(lfRun.reply, &ack, sizeof(ack))) return 10;
            continue;
        }
        if (msg == 8)
        {
            const uint32_t ok = ExportStart(export_msg) ? 1u : 0u;
            Trace("[pipe] picture export %s (%ux%u)", ok ? "up" : "failed", export_msg.width, export_msg.height);
            ExportReply ack = { kExportOk, ok, 0u, 0u, export_msg.pts };
            if (!PipeWrite(lfRun.reply, &ack, sizeof(ack))) return 10;
            continue;
        }
        if (msg == 9)
        {
            const HWND hwnd = reinterpret_cast<HWND>((uintptr_t)lfLink.window_msg.hwnd);
            uint32_t ok = 0;
            FillSet(hwnd, hwnd != nullptr && (lfLink.window_msg.flags & kWindowFill) != 0);
            MsmLinkSet(hwnd, (lfLink.window_msg.flags & kWindowLink) != 0, (lfLink.window_msg.flags & kWindowFill) != 0);
            if (hwnd == nullptr) { WindowStop(); ok = 1; }
            else ok = WindowStart(hwnd) ? 1u : 0u;
            if (ok) lfOut.force_next = true;

            WindowReply ack = { kWindowOk, ok, ok ? lfCap.w : 0u,
                                ok ? lfCap.h : 0u, lfLink.window_msg.pts };
            Trace("[pipe] window source %s (%p -> %ux%u)", ok ? "up" : "failed",
                (void *)hwnd, ack.width, ack.height);
            if (!PipeWrite(lfRun.reply, &ack, sizeof(ack))) return 10;
            continue;
        }
        if ((msg == 1 || msg == 10) && g_snapshot_frozen && lfShow.hwnd) {
            prepared = (msg == 10);
            prepared_index = frame_msg.index;
            prepared_got = false;
            Sleep(15);
            ResultMsg result_msg = { kResultTag, frame_msg.index, 1u, 0u, lfNr.last_code, frame_msg.pts };
            if (!PipeWrite(lfRun.reply, &result_msg, sizeof(result_msg))) return 10;
            continue;
        }
        if (msg == 10)
        {
            if (!SourceLive()) { Trace("[source] a take was asked for with no source running"); return 10; }
            PaceCapture();
            LowLatencyWait();
            prepared_got = lfCap.window_live ? WindowTake(pipe) : DeskTake(pipe);
            if (prepared_got && lfCap.picture_changed) PaceTaken();
            if (lfCap.has_frame && lfLuma.mapped && !lfCap.luma_ok)
            { Trace("[source] the luma copy failed - motion from another frame refused"); return 10; }
            prepared_index = frame_msg.index;
            prepared = true;
            ResultMsg ack = {kResultTag, frame_msg.index, 1u, 0u, 0u, frame_msg.pts};
            if (!PipeWrite(lfRun.reply, &ack, sizeof(ack))) return 10;
            continue;
        }
        const double t_frame = PhaseClock();
        const bool phase_on = PhasesOn();
        if (phase_on) lfProf.stamp = {};
        TimingGuard profile_request{ pipe, frame_msg };
        if (phase_on) ++lfPhase.requests;
        bool source_fresh = true;
        bool defer_tail = false;
        UINT64 upload_done = 0, eval_done = 0, present_done = 0;

        static ULONGLONG last_fresh_tick = GetTickCount64();
        static bool stall_pending = false;
        const ULONGLONG now_tick = GetTickCount64();
        if (SourceLive())
        {
            const double t_dda = PhaseClock();
            const bool use_prepared = (frame_msg.reserved & kFramePrepared) != 0;
            if (use_prepared && (!prepared || prepared_index != frame_msg.index))
            { Trace("[source] the frame taken earlier is not this one - its motion refused"); return 10; }
            if (!use_prepared) { PaceCapture(); LowLatencyWait(); }
            const double stamp_before = lfCap.stamp;
            bool len = use_prepared ? prepared_got : (lfCap.window_live ? WindowTake(pipe) : DeskTake(pipe));
            prepared = false;
            if (lfRun.gpu_lost) return 6;
            if (!len && !lfCap.has_frame && !use_prepared &&
                (frame_msg.reserved & kFrameWantsPixels) != 0)
            {
                Sleep(8);
                len = lfCap.window_live ? WindowTake(pipe) : DeskTake(pipe);
                if (lfRun.gpu_lost) return 6;
                if (!len && !lfCap.reopen_tried)
                {
                    lfCap.reopen_tried = true;
                    const bool reopened = lfCap.window_live
                        ? WindowStart(lfCap.window) : DeskStart(lfCap.w, lfCap.h);

                    for (int i = 0; reopened && !len && i < 12; ++i)
                    {
                        Sleep(10);
                        len = lfCap.window_live ? WindowTake(pipe) : DeskTake(pipe);
                        if (lfRun.gpu_lost) return 6;
                    }
                    Trace("[pipe] a picture was wanted before the source gave one; "
                        "the source was reopened and %s",
                        len ? "answered" : "stayed silent");
                }
            }
            source_fresh = len && lfCap.picture_changed;
            // The picture before over again is nothing new (lf_own_motion.inl,
            // SourceSeen): it keeps the time stamp of the frame it repeats.
            const double change = source_fresh ? LumaChangeNow() : -1.0;
            bool repeated = false;
            if (source_fresh && !use_prepared) SourceSeen(change == 0.0);
            if (source_fresh && change == 0.0 && !use_prepared)
            {
                repeated = true;
                lfCap.stamp = stamp_before;
                lfCap.picture_changed = false;
                source_fresh = false;
                len = false;
            }
            if (source_fresh && !use_prepared) { PaceTaken(); FgGateFresh(lfCap.stamp); }
            if (phase_on && source_fresh) ++lfPhase.fresh_in;            PhaseCount(PHASE_SOURCE, t_dda);
            // The scene cut is found here, not in the engine (lf_own_motion.inl).
            if (source_fresh && SceneCutNow(change)) frame_msg.reset = 1;

            if (source_fresh)
            {
                if (stall_pending && now_tick - last_fresh_tick > 1000)
                    Trace("[history] the source came back after %lu ms - network and generator start fresh",
                        (unsigned long)(now_tick - last_fresh_tick));
                stall_pending = false;
                last_fresh_tick = now_tick;
            }
            else if (!len && now_tick - last_fresh_tick > 1000)
                stall_pending = true;
            if (!len && !lfCap.has_frame)
            {
                ResultMsg empty = { kResultTag, frame_msg.index, 1u, 0u, lfNr.last_code, frame_msg.pts };
                if (!PipeWrite(lfRun.reply, &empty, sizeof(empty))) return 10;
                TimingResult(pipe, frame_msg, false, "idle");
                if (phase_on) ++lfPhase.idle;
                PhaseSummary((frame_msg.reserved & kFramePassThrough) != 0 || !NrLive());
                continue;
            }
            if (!len)
            {
                const bool want_bypass = (frame_msg.reserved & kFramePassThrough) != 0 ||
                                         !NrLive();
                const bool split_on = (frame_msg.reserved & kFrameCompare) != 0;
                const UINT split_cw = pipe.stretched ? pipe.screen_w : pipe.work_w;
                const uint32_t split_x = split_on
                    ? CompareColumn(frame_msg.reserved, split_cw) : 0u;
                const bool out_changed = want_bypass != lfOut.sent_passthrough ||
                                         split_on != lfOut.sent_compare ||
                                         (split_on && split_x != lfOut.sent_compare_x) ||
                                         lfOut.force_next;

                // Low Latency never processes the same picture twice: the card
                // goes back to the game until a new frame arrives (lf_latency.inl).
                // A repeat is passed over whatever is on: worked on, its result
                // shown would be handed over as the next "new" frame at once.
                // Round 41: a picture with nothing new after it (a paused video, a
                // still desktop) is worked on a few times and then held, as
                // NeuralScreen does. Run on the same picture over and over the
                // network drifts into a greyer, darker look (skin 0.73 of its
                // colour) within seconds. Not while MFG SUPER MOTION takes the
                // frames (round 42): its hand-over stays as it was.
                const bool held = lfOut.still_runs >= 4 && frame_msg.reset == 0 && !MsmLinkWanted() && !ShowProofWaiting();
                const bool skip = ((frame_msg.reserved & kFrameIdleWhenStill) != 0 || FlowWanted() || FgRequested() || LowLatencyWanted() ||
                                   repeated || held) &&
                                  (frame_msg.reserved & kFrameWantsPixels) == 0 &&
                                  !out_changed;
                if (skip)
                {
                    ++lfOut.still_count;

                    if (!lfOut.still_logged && lfOut.still_count >= 8)
                    {
                        lfOut.still_logged = true;
                        Trace("[idle] nothing new on screen - the network rests until something moves");
                    }

                    WindowTrack();
                    ShowStayOnTop();
                    ResultMsg idle = { kResultTag, frame_msg.index, 1u, 0u, lfNr.last_code, frame_msg.pts };
                    if (!PipeWrite(lfRun.reply, &idle, sizeof(idle))) return 10;
                    TimingResult(pipe, frame_msg, false, "idle");
                    if (phase_on) ++lfPhase.idle;
                    PhaseSummary(want_bypass);

                    continue;
                }
                ++lfOut.still_runs;
            }
            else if (lfOut.still_count != 0)
            {
                if (lfOut.still_logged)
                    Trace("[idle] the screen moved again after %u quiet frames; "
                        "the network is back", lfOut.still_count);
                lfOut.still_count = 0;
                lfOut.still_logged = false;
            }
            if (len) lfOut.still_runs = 0;

            defer_tail = !lfHdr.keep && warmup_done && NrLive() &&
                ShowDirect(pipe) &&
                (frame_msg.reserved & (kFramePassThrough | kFrameWantsPixels)) == 0;

            lfOut.sent_passthrough = (frame_msg.reserved & kFramePassThrough) != 0 || !NrLive();
            lfOut.sent_compare = (frame_msg.reserved & kFrameCompare) != 0;
            lfOut.sent_compare_x = lfOut.sent_compare
                ? CompareColumn(frame_msg.reserved, pipe.stretched ? pipe.screen_w : pipe.work_w) : 0u;
            lfOut.force_next = false;
            const double t_up = PhaseClock();
            // Interpolation consumes the raw pair on its own queue. With NR
            // bypassed, do not wait here or run NR's still-motion filter.
            const bool flow_only = FlowActive() && lfOut.sent_passthrough;
            const bool fg_only = FgRequested() && lfOut.sent_passthrough;
            // DLSS consumes the motion on this same GPU queue. Waiting on the
            // CPU before submitting NGX inserts an unnecessary round trip.
            UINT64 *motion_mark = defer_tail || flow_only || fg_only ? &upload_done : nullptr;
            const bool try_nvofa = OfaWanted() && !ofa.failed && lfLuma.mapped;
            const bool nvofa_used = try_nvofa && OfaRun(pipe, frame_msg.reset != 0, motion_mark);
            if (try_nvofa && !nvofa_used) frame_msg.reset = 1;
            // No field from the engine (kFrameOwnMotion): where NVIDIA Optical
            // Flow did not run, the GPU estimate finds it (lf_own_motion.inl).
            const bool own_motion = (frame_msg.reserved & kFrameOwnMotion) != 0;
            const bool up_ok = nvofa_used ||
                (own_motion ? NetMotionRun(pipe, frame_msg.reset != 0, motion_mark)
                            : FeedMotion(pipe, mv_ptr,
                                  (frame_msg.reserved & kFrameSmallMotion) != 0 && lfScaler.motion_w != 0,
                                  motion_mark));
            PhaseCount(PHASE_FEED, t_up);
            if (!up_ok || (defer_tail && upload_done == 0)) return 6;
            // The still check the engine's CPU field always had, for the
            // motion found here (lf_own_motion.inl).
            if (FgRequested() && !fg_only && !FgKeepMotion(pipe)) return 6;
            if (!flow_only && !fg_only && (nvofa_used || own_motion))
                MotionSettle(pipe, frame_msg.reset != 0 || (FgRequested() && lfOut.passthrough_before));
        }
        else
        {
            if (phase_on) ++lfPhase.fresh_in;
            const double t_up = PhaseClock();
            const bool up_ok = FeedFrame(pipe, color_ptr, mv_ptr,
                                   (frame_msg.reserved & kFrameSmallMotion) != 0 && lfScaler.motion_w != 0);
            PhaseCount(PHASE_FEED, t_up);
            if (!up_ok) return 6;
        }
        if (!warmup_done)
        {
#ifdef LF_RADEON
            if (true)
#else
            if (!NrLive())
#endif
            {
                warmup_done = true;
            }
            else
            {
                const uint32_t warmup = (std::min)(240u, (std::max)(1u, lfLink.hello.warmup));
                for (uint32_t i = 0; i < warmup; ++i)
                {
                    if (!PipelineRun(pipe, i == 0 ? 1 : 0)) return 7;
                }
                warmup_done = true;
                Trace("[net] network confirmed after %u warm-up frames thrown away", warmup);
            }
        }
        const UINT previous_hdr_split = lfHdr.compare_x;
        lfHdr.compare_x = (frame_msg.reserved & kFrameCompare) ?
            CompareColumn(frame_msg.reserved, pipe.stretched ? pipe.screen_w : pipe.work_w) : UINT_MAX;
        const bool bypass = (frame_msg.reserved & kFramePassThrough) != 0 || !NrLive();
        // Round 41: the network starts over when it comes back on. Without it the
        // first run after DLSS 5 was off went on from the warm-up frame (the
        // picture at the worker's start), and the network kept, for the whole
        // session, one of two looks picked by chance there - the right skin, or a
        // greyer, darker one (skin 0.73 of its colour, 3 levels darker).
        const bool network_back = !bypass && lfOut.passthrough_before;
        lfShow.history_restart = frame == 0 || frame_msg.reset != 0 || bypass != lfOut.passthrough_before
                     || previous_hdr_split != lfHdr.compare_x
                     || (stall_pending && source_fresh);
        lfOut.passthrough_before = bypass;

        const bool stall_reset = stall_pending && source_fresh;
        if (stall_reset) { frame_msg.reset = 1; stall_pending = false; Trace("[pipe] history cleared after the source paused"); }
        if (!bypass)
        {
            const double t_eval = PhaseClock();

            lfOut.compare_x = (frame_msg.reserved & kFrameCompare) != 0
                ? CompareColumn(frame_msg.reserved, pipe.stretched ? pipe.screen_w : pipe.work_w) : 0u;
            if (network_back && frame != 0) Trace("[net] network on again - its history starts over");
            const bool ev_ok = PipelineRun(pipe, (frame == 0 || frame_msg.reset != 0 || network_back) ? 1 : 0,
                                             defer_tail ? &eval_done : nullptr);
            lfOut.compare_x = 0;
            PhaseCount(PHASE_RUN, t_eval);
            if (!ev_ok) return 9;
            if (defer_tail && eval_done <= upload_done)
            { Trace("[pipe] tail tokens out of order: upload %llu, network %llu", upload_done, eval_done); return 9; }
            if (phase_on && !defer_tail)
            {
                ++lfPhase.evaluated;
                if (source_fresh) ++lfPhase.fresh_evaluated;
            }
        }
        if (ShowDirect(pipe))
        {
            const double t_pres = PhaseClock();

            WindowTrack();
            ShowStayOnTop();
            const bool pres_ok = bypass ? ShowPassThrough(pipe) : ShowPicture(pipe, defer_tail ? &present_done : nullptr);
            PhaseCount(PHASE_SHOW, t_pres);
            if (!pres_ok) return 9;
            if (defer_tail)
            {
                if (present_done <= eval_done)
                { Trace("[pipe] tail tokens out of order: network %llu, show %llu", eval_done, present_done); return 9; }
                ++lfNr.evaluations;
                if (phase_on)
                {
                    ++lfPhase.evaluated;
                    if (source_fresh) ++lfPhase.fresh_evaluated;
                }
            }

            if ((frame_msg.reserved & kFrameWantsPixels) != 0)
            {
                bool dl_ok = false;
                if (bypass)
                    dl_ok = PipelineReadback(pipe, output, pipe.picture.gpu_tex,
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                else
                    dl_ok = PipelineReadback(pipe, output);
                if (!dl_ok) return 9;
                if (!SendPixels(output, frame_msg.index, frame_msg.pts)) return 10;
            }
            else
            {
                ResultMsg out = { kResultTag, frame_msg.index, 1u, 0u, lfNr.last_code, frame_msg.pts };
                if (!PipeWrite(lfRun.reply, &out, sizeof(out))) return 10;
            }
        }
        else
        {
            if (bypass)
            {
                if (!PipelineReadback(pipe, output, pipe.picture.gpu_tex,
                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE))
                    return 9;
            }
            else if (!PipelineReadback(pipe, output)) return 9;
            if (!SendPixels(output, frame_msg.index, frame_msg.pts)) return 10;
        }
        PhaseCount(PHASE_WHOLE, t_frame);
        TimingResult(pipe, frame_msg, source_fresh && !bypass, bypass ? "bypass" : "enhanced");
        if (phase_on) ++lfPhase.processed;
        PhaseSummary(bypass);
        if (frame < 3 || ((frame + 1) % 30) == 0)
            Trace("[pipe] heartbeat: frame %u out%s", frame + 1, live ? " (live)" : "");
        ++frame;
        if (!live && frame >= hello.frame_count) break;
    }
    TimingFlush();
    Trace("[net] finished: %u frames out, %u network runs", frame, lfNr.evaluations);
    NgxShutdown();
    ExportStop();
    SharedStop();
    ShowStop();
    MotionResizeStop();
    DeskStop();
    LumaStop();
    ShareStop();
    return 0;
}
