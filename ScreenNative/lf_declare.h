// lf_declare.h - every function of the worker, declared before any is
// defined, so the topic files do not depend on each other's order. Default
// arguments live here and only here.
#pragma once

static void Trace(const char *pixel_format, ...);
static const char *NgxCodeText(NVSDK_NGX_Result r);
static bool PatchBytes(void *at, const void *from, size_t bytes);
static bool ArchHookInstall();
static int __cdecl ArchAnswer(void *gpu_at, ArchRecord *info);
static bool ArchWanted();
static int ArchPrepare();
static bool NrForwarderLoad(const wchar_t *dll_file);
static bool NrDirectStart(const wchar_t *data_dir);
static void TimingFlush();
static void TimingCapture(double acquire_start, UINT64 source_qpc, const char *kind);
static bool CmdOpen();
static void ReportDeviceLoss(const char *where);
static UINT64 CmdSubmit();
static bool AwaitMark(ID3D12Fence *f, UINT64 v, DWORD timeout_ms);
static void CmdCloseSafe();
static void DropCommands();
static NVSDK_NGX_Result NgxCreateGuarded(NVSDK_NGX_DLSS_Create_Params *cp, DWORD *blob);
static NVSDK_NGX_Result NgxEvaluateGuarded(NVSDK_NGX_D3D12_DLSS_Eval_Params *ep, DWORD *blob);
static void NgxReleaseGuarded(NVSDK_NGX_Handle *f);
static int CardWanted();
static void ReportVram(const char *where);
static int CardInUse();
static bool GpuCoreStart();
static void NVSDK_CONV NgxQuietLog(const char *, NVSDK_NGX_Logging_Level,
                                          NVSDK_NGX_Feature);
static NVSDK_NGX_FeatureCommonInfo *NgxSearchInfo(const wchar_t *data_dir);
static bool NgxStart();
static UINT NrPresetWanted();
static bool NrExposureGiven();
static bool NetworkCreate(UINT w, UINT h_, int flags, NVSDK_NGX_Result *out_r, UINT screen_w = 0, UINT screen_h = 0);
static bool NgxRestart();
static bool NetworkRun(ID3D12Resource *pic, ID3D12Resource *output, ID3D12Resource *depth, ID3D12Resource *motion,
                     UINT w, UINT h_, int reset, float mvsx, float mvsy);
static ID3D12Resource *NewTexture(UINT w, UINT h_, DXGI_FORMAT pixel_format, bool uav_view);
static UINT CompareColumn(uint32_t reserved, UINT width);
static float EnvFraction(const char *name);
static float SharpnessNow();
static float EdgesNow();
static unsigned FlowExtraFrames();
static bool FlowViewShown();
static int ShownCap();
static float KeepColour();
static float LowLight();
static void ReadExtrasWord(uint32_t word);
static void PaceCapture();
static void SharedStop();
static void ShowAffinityApply();
static LRESULT CALLBACK ShowWndProc(HWND w, UINT m, WPARAM wp, LPARAM lp);
static void ShowOriginRead();
static DWORD WINAPI ShowThread(LPVOID);
static void ShowStop();
static bool ShowStart(UINT width, UINT height, uint32_t flags);
static bool ShowDirect(const Pipeline &pipe);
static float WindowCoverage(const RECT &target);
static bool WindowInView(const RECT &target);
static bool WindowWantsOverlay(const RECT &target);
static bool WindowBounds(RECT &r);
static void WindowTrack();
static void ShowStayOnTop();
static void ShowRevealOnce();
static bool ShowResult(HRESULT status, const char *where);
static bool ShowRebuildIfStale();
static bool ShowPicture(Pipeline &pipe, UINT64 *sent_mark = nullptr);
static bool ShowPassThrough(Pipeline &pipe);
static bool PipeRead(FILE *f, void *p, size_t n);
static void PipeClaim();
static bool PipeWrite(FILE *f, const void *p, size_t n);
static bool StagedCreate(StagedTexture &staged, UINT w, UINT hgt, DXGI_FORMAT pixel_format, UINT tight_pitch,
                           D3D12_RESOURCE_FLAGS tex_flags = D3D12_RESOURCE_FLAG_NONE);
static bool SmallNetworkWanted();
static bool DetailWanted();
static float DetailAmount();
static void ClampWorkSize(UINT src_w, UINT src_h, UINT &w, UINT &height);
static bool PipelineBuild(Pipeline &pipe, UINT w, UINT hgt, UINT full_w = 0, UINT full_h = 0,
                                 int small_wanted = -1);
static bool StagedFill(StagedTexture &staged, const BYTE *from, UINT from_pitch, UINT hgt);
static void StagedCopy(ID3D12GraphicsCommandList *cmdl, StagedTexture &staged);
static D3D12_RESOURCE_BARRIER Barrier(ID3D12Resource *r, D3D12_RESOURCE_STATES before,
                                         D3D12_RESOURCE_STATES after);
static void StagedRelease(StagedTexture &staged);
static void MotionResizeStop();
static bool ScalerReady();
static void ScalerBind(ID3D12Resource *from, ID3D12Resource *into);
static void ScalerBindFine(ID3D12Resource *from, ID3D12Resource *into, UINT slot);
static void ResizePicture(ID3D12Resource *from, UINT src_w, UINT src_h,
                           ID3D12Resource *into, UINT dst_w, UINT dst_h, UINT slot);
static void DetailBind(ID3D12Resource *native, ID3D12Resource *net_in,
                                    ID3D12Resource *net_out, ID3D12Resource *into,
                                    ID3D12Resource *delta);
static void DetailCompose(ID3D12Resource *native, ID3D12Resource *net_in,
                            ID3D12Resource *net_out, UINT w, UINT h_,
                            ID3D12Resource *into, float strength,
                            ID3D12Resource *delta = nullptr, float halo = 0.0f);
static bool MotionResizeStart(UINT w, UINT hgt);
static bool ResizeMotion(Pipeline &pipe, const BYTE *mv, bool mv_was_ready);
static void LumaStop();
static void DeskStop();
static bool SwizzleReady();
static void SwizzleBind(ID3D12Resource *from);
static UINT FlowLumaDivisor();
static void FlowLumaStop();
static bool FlowLumaReady(UINT capture_w, UINT capture_h);
static bool LumaPipelineReady();
static void ExportStop();
static bool ExportStart(const ExportMsg &export_msg);
static bool ExportWanted();
static void ExportPicture(const BYTE *from, size_t pitch, UINT w, UINT hgt);
static bool SendPixels(const std::vector<BYTE> &output, uint32_t index,
                          int64_t when);
static bool LumaStart(const LumaMsg &luma_msg);
static bool LumaFromPicture();
static bool ExposureOn();
static float ExposureSetting(const char *name, float vram_heap);
static void ExposureUpdate();
static bool CaptureDeviceReady();
static HMONITOR MonitorByName(const wchar_t *name);
static IDXGIOutput *FindCaptureOutput(IDXGIAdapter1 *card, HMONITOR *elsewhere = nullptr);
static bool DeskStart(UINT w, UINT hgt);
static BridgeResult BridgeFrame(ID3D11Texture2D *frame, UINT *out_w, UINT *out_h,
                                      DXGI_FORMAT *out_format = nullptr);
static bool SwizzleIntoPicture(Pipeline &pipe);
static bool DeskTake(Pipeline &pipe);
static void WindowStop();
static bool SourceLive();
static bool WindowStart(HWND hwnd);
static void ScreenViaWgcStop();
static bool ScreenViaWgcStart(HMONITOR monitor);
static bool ScreenViaWgcTake(Pipeline &pipe);
static bool WindowTake(Pipeline &pipe);
static bool FeedFrame(Pipeline &pipe, const BYTE *color, const BYTE *mv, bool motion_small);
static bool FeedMotion(Pipeline &pipe, const BYTE *mv, bool motion_small,
                             UINT64 *sent_mark = nullptr);
static bool QueriesReady();
static bool GpuStageOpen(GpuStage stage);
static void GpuStageClose(GpuStage stage, unsigned query_count = 2);
static void QueryRead(int slot);
static void TimingCollect();
static bool GpuStageWait(GpuStage stage, UINT64 fence, DWORD timeout_ms);
static bool ParamSetF(NVSDK_NGX_Parameter *p, const char *name, float value);
static bool ParamSetU(NVSDK_NGX_Parameter *p, const char *name, unsigned int value);
static bool PipelineRun(Pipeline &pipe, int reset, UINT64 *sent_mark = nullptr);
static bool CompareTargetReady(ID3D12Resource *res);
static void CompareRecord(Pipeline &pipe, UINT split_x);
static bool PipelineReadback(Pipeline &pipe, std::vector<BYTE> &tight,
                               ID3D12Resource *from = nullptr,
                               D3D12_RESOURCE_STATES src_before = D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATES src_after = D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
static int ReadNextMessage(Pipeline &pipe, FrameMsg &frame_msg, std::vector<BYTE> &pic,
                            std::vector<BYTE> &motion, ReshapeMsg &reshape, SharedMsg &shared,
                            ScreenMsg &screen, MotionMsg &motion_msg, DesktopMsg &desk,
                            LumaMsg &luma_msg, ExportMsg &export_msg,
                            const BYTE **color_ptr, const BYTE **mv_ptr);
static void PipelineRelease(Pipeline &pipe);
static bool PhasesOn();
static double PhaseClock();
static void PhaseCount(int idx, double t0);
static void TimingResult(const Pipeline &pipe, const FrameMsg &frame_msg,
                               bool fresh, const char *outcome);
static void PhaseSummary(bool bypass);
static bool NrLive();
static int LiveLoop();
static void NgxShutdown();
static bool ShowHdr(Pipeline &v, bool bypass);
static bool ShowFormatReady(bool hdr, bool pq = false);
static ID3D12Resource *HdrExportFrame(ID3D12GraphicsCommandList *list, ID3D12Resource *source,
                                    D3D12_RESOURCE_STATES state);
static void HdrRelease();
static bool FlowWanted();
static bool FlowActive();
static bool FgRequested();
static unsigned FgExtraFrames();
static void CloseFgResources();
static bool FgPresent(Pipeline &v, ID3D12Resource *color, D3D12_RESOURCE_STATES state);
static ID3D12CommandQueue *FlowSwapQueue();
static void CloseFlow();
static bool FlowPresent(Pipeline &v, ID3D12Resource *color, D3D12_RESOURCE_STATES state);
static bool FlowToken();
static bool UpscaleSetting();
static void UpscaleFromResize(uint32_t flags);
static bool UpscaleWanted();
static bool FillWanted();
static void FillSet(HWND hwnd, bool on);
static bool MsmLinkWanted();
static void MsmLinkSet(HWND game, bool on, bool fill);
static void MsmLinkCopy(ID3D12GraphicsCommandList *list, ID3D12Resource *src, UINT w, UINT h);
static void MsmLinkSent();
static void MsmLinkTick();
static void MsmLinkAsideReset();
static bool UpscaleMonitor(RECT &monitor);
static void UpscaleTrack(const RECT &frame);
static bool UpscaleWantsOverlay();
static void UpscaleFollowStart();
static void ShowBlit(ID3D12GraphicsCommandList *list, ID3D12Resource *back_buffer, ID3D12Resource *src);
static bool VrrSetting();
static void VrrFromResize(uint32_t flags);
static HRESULT ShowPresentNow(double source = -1.0);
// The FPS counter, drawn into the frame (lf_counter.inl).
static void CounterPresented(double source, bool own_capture);
static void CounterStamp(ID3D12GraphicsCommandList *list, ID3D12Resource *back_buffer);
static void ShowProof(ID3D12GraphicsCommandList *list, ID3D12Resource *back_buffer, bool real);
static bool ShowProofWaiting();
static double LatencyNow();
static bool VrrWanted();
static void VrrObserve();
static bool SuperResWanted();
static bool SuperResReady();
static UINT SuperResSize(UINT display, UINT work);
static void SuperResNote(const Pipeline &pipe);
static bool SuperResDownscale(ID3D12GraphicsCommandList *list, ID3D12Resource *src, D3D12_RESOURCE_STATES src_state,
                              Pipeline &pipe);
static bool FeedFrameSuperRes(Pipeline &pipe, const BYTE *color, const BYTE *mv, bool motion_small);
static ID3D12Resource *SuperResExport(ID3D12GraphicsCommandList *list, ID3D12Resource *from, UINT w, UINT hgt, bool flow);
static void SuperResExportDone(ID3D12GraphicsCommandList *list, bool flow);
static void SuperResExportRelease(bool flow);
static bool LowLatencyWanted();
static void LowLatencyFromResize(uint32_t flags);
static void LowLatencyWait();
static void LatencyPresented(double source);
static double LumaChangeNow();
static bool SceneCutNow(double change);
static void SourceSeen(bool repeated);
static void FgGateRepeat(double now);
static void FgGateFresh(double stamp);
static bool NetMotionRun(Pipeline &pipe, bool reset, UINT64 *sent_mark);
static bool NetMotionForFlow(ID3D12Resource *&backward, ID3D12Resource *&forward);
static void MotionSettle(Pipeline &pipe, bool reset);
static double OutputRefresh();
static double VrrCeiling(double refresh);
static int VrrCap();
