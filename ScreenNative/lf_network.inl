// lf_network.inl - the neural network. Loads the runtime (nvngx_dlssnr.dll,
// straight or through the optional relay DLL), starts NGX, answers the
// runtime's architecture question so every RTX card is accepted, creates
// the network at the working size and runs it; every call into the runtime
// is guarded so a crash inside it cannot take the worker down.

// Is the architecture answer wanted (on unless LF_ARCH_SPOOF=0).
static bool ArchWanted()
{
    char text[8] = {};
    const DWORD len = GetEnvironmentVariableA("LF_ARCH_SPOOF", text, sizeof(text));
    if (len > 0 && len < sizeof(text) && text[0] == '0') return false;
    return true;
}

// Ask NvAPI for every card's architecture and keep the answers.
static int ArchPrepare()
{
    if (!ArchWanted()) return 0;
    HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    if (nvapi == nullptr)
    { Trace("[gpuid] nvapi64.dll did not load (%lu)", GetLastError()); return -1; }
    auto qi = reinterpret_cast<NvQueryFn>(
        GetProcAddress(nvapi, "nvapi_QueryInterface"));
    if (qi == nullptr) { Trace("[gpuid] nvapi_QueryInterface is missing"); return -1; }

    auto init = reinterpret_cast<int (__cdecl *)()>(qi(kNvapiInit));
    auto enum_gpus = reinterpret_cast<int (__cdecl *)(void **, unsigned *)>(
        qi(kNvapiEnumGpus));
    auto get_arch = reinterpret_cast<NvArchFn>(qi(kNvapiArchInfo));
    if (init == nullptr || enum_gpus == nullptr || get_arch == nullptr)
    { Trace("[gpuid] the NvAPI calls we need did not resolve"); return -1; }
    if (init() != 0) { Trace("[gpuid] NvAPI did not start"); return -1; }

    void *handles[kArchSlots] = {};
    unsigned count = 0;
    if (enum_gpus(handles, &count) != 0 || count == 0)
    { Trace("[gpuid] no GPU list from NvAPI"); return -1; }
    if (count > static_cast<unsigned>(kArchSlots)) count = kArchSlots;

    lfArch.count = 0;
    for (unsigned i = 0; i < count; ++i)
    {
        ArchRecord one = {};
        one.version = static_cast<unsigned>(sizeof(ArchRecord)) | (2u << 16);
        if (get_arch(handles[i], &one) != 0)
        {
            one.version = static_cast<unsigned>(sizeof(ArchRecord)) | (1u << 16);
            if (get_arch(handles[i], &one) != 0) continue;
        }
        lfArch.handles[lfArch.count] = handles[i];
        lfArch.cache[lfArch.count] = one;
        ++lfArch.count;
    }
    if (lfArch.count == 0)
    { Trace("[gpuid] no card answered the architecture question"); return -1; }

    const ArchRecord info = lfArch.cache[0];
    lfArch.real_group = info.architecture & 0xFFFFFFF0u;

    char force[8] = {};
    const DWORD force_got = GetEnvironmentVariableA("LF_ARCH_FORCE", force,
                                                    sizeof(force));
    const bool forced = force_got > 0 && force_got < sizeof(force)
                        && force[0] == '1';
    if (lfArch.real_group >= kArchBlackwell && !forced)
    {
        Trace("[gpuid] architecture 0x%X is accepted as it is - nothing to change",
            info.architecture);
        return 0;
    }

    lfArch.hook_site = reinterpret_cast<void *>(get_arch);
    if (!ArchHookInstall())
    { Trace("[gpuid] the answer could not be put in place (%lu)", GetLastError()); return -1; }
    Trace("[gpuid] answer in place for %d card(s), architecture 0x%X%s",
        lfArch.count, info.architecture,
        forced ? " (LF_ARCH_FORCE=1, reported as it is)"
               : " (reported as one the runtime accepts)");
    return 1;
}

// Put our architecture answer in place of the runtime's question.
static bool ArchHookInstall()
{
    BYTE blob[12] = { 0x48, 0xB8 };
    void *into = reinterpret_cast<void *>(&ArchAnswer);
    memcpy(blob + 2, &into, sizeof(into));
    blob[10] = 0xFF; blob[11] = 0xE0;
    if (!PatchBytes(lfArch.hook_site, blob, sizeof(blob)))
        return false;
    lfArch.patched = true;
    return true;
}

// Write a few bytes of code in place.
static bool PatchBytes(void *at, const void *from, size_t bytes)
{
    DWORD old = 0;
    if (!VirtualProtect(at, bytes, PAGE_EXECUTE_READWRITE, &old)) return false;
    memcpy(at, from, bytes);
    DWORD tmp = 0;
    VirtualProtect(at, bytes, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), at, bytes);
    return true;
}

// Answer the runtime: a card it would refuse is reported as one it accepts.
static int __cdecl ArchAnswer(void *gpu_at, ArchRecord *info)
{
    if (info == nullptr) return -1;
    const unsigned wanted = info->version;

    int idx = -1;
    for (int i = 0; i < lfArch.count; ++i)
        if (lfArch.handles[i] == gpu_at) { idx = i; break; }
    if (idx < 0) idx = 0;

    *info = lfArch.cache[idx];
    info->version = wanted;

    const unsigned group = info->architecture & 0xFFFFFFF0u;
    if (idx == 0 && (group == kArchTuring || group == kArchAmpere
        || group == kArchAda))
    {
        info->architecture   = kArchBlackwell;
        info->implementation = 0x3u;
        info->revision       = 0xA1u;
    }
    return 0;
}

// Load the relay DLL and take the runtime's calls from it.
static bool NrForwarderLoad(const wchar_t *dll_file)
{
    wchar_t path[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LF_FORWARDER", path, MAX_PATH) == 0
        || wcscmp(path, L"1") == 0)
    {
        GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (wchar_t *s = wcsrchr(path, L'\\')) *(s + 1) = L'\0';
        wcsncat_s(path, L"nvngx.dll_lf-relay.dll", _TRUNCATE);
    }

    const HMODULE fwd = LoadLibraryW(path);
    if (fwd == nullptr)
    { Trace("[net] relay %ls did not load (%lu)", path, GetLastError()); return false; }

    const auto load = reinterpret_cast<int (*)(const wchar_t *)>(GetProcAddress(fwd, "LfRelayLoad"));
    const auto where = reinterpret_cast<void (*)(wchar_t *, unsigned int)>(GetProcAddress(fwd, "LfRelayPath"));
    const auto init_ext = reinterpret_cast<NrInitFn>(GetProcAddress(fwd, "LfRelayStart"));
    const auto create = reinterpret_cast<NrCreateFn>(GetProcAddress(fwd, "LfRelayCreate"));
    const auto evaluate = reinterpret_cast<NrEvaluateFn>(GetProcAddress(fwd, "LfRelayRun"));
    const auto release = reinterpret_cast<NrReleaseFn>(GetProcAddress(fwd, "LfRelayRelease"));
    if (load == nullptr || init_ext == nullptr || create == nullptr
        || evaluate == nullptr || release == nullptr)
    { Trace("[net] the relay lacks exports - an old build?"); return false; }

    const int lr = load(dll_file);
    if (lr != 0)
    { Trace("[net] the relay could not load %ls (%d)", dll_file, lr); return false; }

    lfNr.module = fwd;
    lfNr.init_ext = init_ext;
    lfNr.create = create;
    lfNr.evaluate = evaluate;
    lfNr.release = release;

    wchar_t actual[MAX_PATH] = {};
    if (where != nullptr) where(actual, MAX_PATH);
    Trace("[net] NGX calls pass through %ls", actual[0] != L'\0' ? actual : path);
    return true;
}

// Load nvngx_dlssnr.dll (the shipped one, or a signed user copy), find its
// entry points and start it.
static bool NrDirectStart(const wchar_t *data_dir)
{
    ArchPrepare();

    wchar_t core[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"LF_NGX_CORE", core, MAX_PATH) > 0)
    {
        const HMODULE m = LoadLibraryW(core);
        Trace("[net] NGX core loaded early: %s (%lu)",
            m != nullptr ? "yes" : "no", m != nullptr ? 0UL : GetLastError());
    }

    char via[8] = {};
    const DWORD via_got = GetEnvironmentVariableA("LF_NGX_VIA_CORE", via, sizeof(via));
    if (via_got > 0 && via_got < sizeof(via) && via[0] == '1')
    {
        lfNr.create = reinterpret_cast<NrCreateFn>(&NVSDK_NGX_D3D12_CreateFeature);
        lfNr.evaluate = reinterpret_cast<NrEvaluateFn>(&NVSDK_NGX_D3D12_EvaluateFeature);
        lfNr.release = reinterpret_cast<NrReleaseFn>(&NVSDK_NGX_D3D12_ReleaseFeature);
        Trace("[net] LF_NGX_VIA_CORE=1: the network is created through NGX core");
        return true;
    }

    wchar_t dll_path[MAX_PATH] = {};
    const wchar_t *dll_file = L"nvngx_dlssnr.dll";
    if (GetEnvironmentVariableW(L"LF_NR_DLL", dll_path, MAX_PATH) > 0)
    {
        dll_file = dll_path;
        Trace("[net] runtime LF_NR_DLL=%ls", dll_file);
    }
    else
    {
        wchar_t worker_dir[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, worker_dir, MAX_PATH);
        if (auto cut = wcsrchr(worker_dir, L'\\')) *(cut + 1) = 0;
        wchar_t option[MAX_PATH] = {};
        wcscpy_s(option, worker_dir);
        wcscat_s(option, L"libraries\\nvngx_dlssnr.dll");
        if (GetFileAttributesW(option) != INVALID_FILE_ATTRIBUTES)
        {
            if (!TrustUserDll(option, "nvngx_dlssnr.dll"))
            {
                Trace("[net] user copy refused: nvngx_dlssnr.dll is no "
                    "NVIDIA-signed runtime - using the shipped "
                    "one (%ls)", option);
            }
            else
            {
                dll_file = option;
                Trace("[net] runtime taken from native\\libraries\\ (user copy, signature checked)");
            }
        }
    }

    const DWORD fwd_got = GetEnvironmentVariableA("LF_FORWARDER", nullptr, 0);
    const bool asked_fwd = fwd_got > 0;

    const bool straight = !asked_fwd || !NrForwarderLoad(dll_file);
    if (straight)
    {
        if (!asked_fwd)
            Trace("[net] NGX calls come from the worker itself (its file is "
                "named nvngx.dll)");
        lfNr.module = LoadLibraryW(dll_file);
        if (!lfNr.module) { Trace("[net] %ls did not load (%lu)", dll_file, GetLastError()); return false; }
        lfNr.init_ext = reinterpret_cast<NrInitFn>(GetProcAddress(lfNr.module, "NVSDK_NGX_D3D12_Init_Ext"));
        lfNr.create = reinterpret_cast<NrCreateFn>(GetProcAddress(lfNr.module, "NVSDK_NGX_D3D12_CreateFeature"));
        lfNr.evaluate = reinterpret_cast<NrEvaluateFn>(GetProcAddress(lfNr.module, "NVSDK_NGX_D3D12_EvaluateFeature"));
        lfNr.release = reinterpret_cast<NrReleaseFn>(GetProcAddress(lfNr.module, "NVSDK_NGX_D3D12_ReleaseFeature"));
        if (!lfNr.init_ext || !lfNr.create || !lfNr.evaluate || !lfNr.release)
        { Trace("[net] nvngx_dlssnr.dll lacks calls: start %s, create %s, run %s, release %s (%lu)",
             lfNr.init_ext ? "found" : "MISSING", lfNr.create ? "found" : "MISSING",
             lfNr.evaluate ? "found" : "MISSING", lfNr.release ? "found" : "MISSING",
             GetLastError()); return false; }
    }
    const auto r = lfNr.init_ext(0x1000000ULL, data_dir, lfCore.device, NVSDK_NGX_Version_API, lfCore.ngx_params);
    Trace("[net] runtime start (%s) answered 0x%08X (%s)",
        straight ? "worker" : "relay", r, NgxCodeText(r));
    return NVSDK_NGX_SUCCEED(r);
}

// NGX's own log lines are not wanted.
static void NVSDK_CONV NgxQuietLog(const char *, NVSDK_NGX_Logging_Level,
                                          NVSDK_NGX_Feature) {}

// Where NGX looks for its runtimes.
static NVSDK_NGX_FeatureCommonInfo *NgxSearchInfo(const wchar_t *data_dir)
{
    lfNr.search_info = {};
    lfNr.search_info.LoggingInfo.LoggingCallback = NgxQuietLog;
    lfNr.search_info.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_OFF;
    lfNr.search_info.LoggingInfo.DisableOtherLoggingSinks = true;

    char v[8] = {};
    const DWORD len = GetEnvironmentVariableA("LF_NGX_PATHLIST", v, sizeof(v));
    if (len > 0 && len < sizeof(v) && v[0] == '1')
    {
        wcscpy_s(lfNr.search_dirs[0], MAX_PATH, data_dir);
        wchar_t exe_dir[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, exe_dir, MAX_PATH);
        if (wchar_t *s = wcsrchr(exe_dir, L'\\')) *(s + 1) = L'\0';
        wcscpy_s(lfNr.search_dirs[1], MAX_PATH, exe_dir);
        lfNr.search_info.PathListInfo.Path = lfNr.search_list;
        lfNr.search_info.PathListInfo.Length = 2;
        Trace("[card] LF_NGX_PATHLIST=1: NGX searches the exe and the worker folders");
    }
    return &lfNr.search_info;
}

// Start NGX on the device.
static bool NgxStart()
{
    lfNr.initialized = false;
    wchar_t data_dir[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, data_dir, MAX_PATH);
    if (wchar_t *s = wcsrchr(data_dir, L'\\')) *(s + 1) = L'\0';

    char iv[8] = {};
    if (GetEnvironmentVariableA("LF_NGX_INDICATOR", iv, sizeof(iv)) > 0 && iv[0] == '1')
    {
        SetEnvironmentVariableA("__NGX_SHOW_INDICATOR", "1024");
        Trace("[card] LF_NGX_INDICATOR=1: NGX draws its debug overlay");
    }
    char cv[8] = {};
    if (GetEnvironmentVariableA("LF_NGX_NO_CUBIN_CACHE", cv, sizeof(cv)) > 0 && cv[0] == '1')
    {
        SetEnvironmentVariableA("__NGX_CUBIN_DISABLE_RESOURCE_CACHE", "1");
        Trace("[card] LF_NGX_NO_CUBIN_CACHE=1: no kernel cache");
    }

    NVSDK_NGX_FeatureCommonInfo *common = NgxSearchInfo(data_dir);
    LiveFlowCachePath(data_dir);
    NVSDK_NGX_Result r = NVSDK_NGX_D3D12_Init(0x1000000ULL, data_dir, lfCore.device, common, NVSDK_NGX_Version_API);
    Trace("[card] NGX start answered 0x%08X (%s)", r, NgxCodeText(r));
    if (NVSDK_NGX_FAILED(r))
    {
        r = NVSDK_NGX_D3D12_Init_with_ProjectID("a0f57b54-1daf-4934-90ae-c4035c19df04", NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                "1.0", data_dir, lfCore.device, common, NVSDK_NGX_Version_API);
        Trace("[card] NGX start by project answered 0x%08X (%s)", r, NgxCodeText(r));
    }
    if (NVSDK_NGX_FAILED(r)) return false;
    lfCore.ngx_ready = true;

    r = NVSDK_NGX_D3D12_AllocateParameters(&lfCore.ngx_params);
    if (NVSDK_NGX_FAILED(r) || lfCore.ngx_params == nullptr) { Trace("[card] no NGX parameter block (0x%08X)", r); return false; }
    lfNr.initialized = NrDirectStart(data_dir);
    return lfNr.initialized;
}

// NGX failed again and again: shut it down and start it over.
static bool NgxRestart()
{
    Trace("[card] NGX failed again and again - starting it over");
    ShutdownFg();
    if (lfCore.ngx_params != nullptr) { NVSDK_NGX_D3D12_DestroyParameters(lfCore.ngx_params); lfCore.ngx_params = nullptr; }
    if (lfCore.ngx_ready) { NVSDK_NGX_D3D12_Shutdown1(lfCore.device); lfCore.ngx_ready = false; }
    lfCore.ngx_feature = nullptr;
    return NgxStart();
}

// Free the network and shut NGX down.
static void NgxShutdown()
{
    ShutdownFg();
    lfNr.initialized = false;
#ifdef LF_RADEON
    RadeonShutdown();
#endif
    CloseFlow();
    OfaStop();
    ReleaseMultiPass();
    if (lfCore.ngx_feature != nullptr)
    {
        NgxReleaseGuarded(lfCore.ngx_feature);
        lfCore.ngx_feature = nullptr;
        Trace("[pipe] network released");
    }
    if (lfCore.ngx_params != nullptr)
    {
        NVSDK_NGX_D3D12_DestroyParameters(lfCore.ngx_params);
        lfCore.ngx_params = nullptr;
    }
    if (lfCore.ngx_ready)
    {
        NVSDK_NGX_D3D12_Shutdown1(lfCore.device);
        lfCore.ngx_ready = false;
        Trace("[pipe] NGX shut down");
    }
}

// A network preset from LF_NR_PRESET, 0 for the default.
static UINT NrPresetWanted()
{
    static int cached = -1;
    if (cached < 0)
    {
        char text[16] = {};
        const DWORD len = GetEnvironmentVariableA("LF_NR_PRESET", text, sizeof(text));
        cached = (len > 0 && len < sizeof(text)) ? atoi(text) : 0;
        if (cached < 0) cached = 0;
    }
    return static_cast<UINT>(cached);
}

// Round 41: the exposure numbers (DLSS.Pre.Exposure, DLSS.Exposure.Scale) are
// given to the network only with LF_NR_EXPOSURE=1. NeuralScreen gives none.
static bool NrExposureGiven()
{
    static int cached = -1;
    if (cached < 0)
    {
        char text[8] = {};
        const DWORD len = GetEnvironmentVariableA("LF_NR_EXPOSURE", text, sizeof(text));
        cached = (len > 0 && len < sizeof(text) && text[0] == '1') ? 1 : 0;
    }
    return cached == 1;
}

// Create the network at w x h_ (and screen size when it runs small).
static bool NetworkCreate(UINT w, UINT h_, int flags, NVSDK_NGX_Result *out_r, UINT screen_w, UINT screen_h)
{
    (void)flags;
    if (!lfNr.initialized || !lfCore.ngx_ready || !lfCore.ngx_params || !lfNr.create) {
        if (out_r) *out_r = NVSDK_NGX_Result_Fail;
        Trace("[net] NR unavailable - the picture passes through");
        return false;
    }

    const bool upscale = (screen_w > 0 && screen_h > 0 && (screen_w != w || screen_h != h_));
    lfCore.ngx_params->Reset();
    lfCore.ngx_params->Set("CreationNodeMask", 1u);
    lfCore.ngx_params->Set("VisibilityNodeMask", 1u);
    lfCore.ngx_params->Set("DLSSNR.Width", w); lfCore.ngx_params->Set("DLSSNR.Height", h_);
    lfCore.ngx_params->Set("DLSSNR.InputWidth", upscale ? screen_w : w);
    lfCore.ngx_params->Set("DLSSNR.InputHeight", upscale ? screen_h : h_);
    lfCore.ngx_params->Set("DLSSNR.OutputWidth", upscale ? screen_w : w);
    lfCore.ngx_params->Set("DLSSNR.OutputHeight", upscale ? screen_h : h_);
    lfCore.ngx_params->Set("DLSSNR.Output.Width", upscale ? screen_w : w);
    lfCore.ngx_params->Set("DLSSNR.Output.Height", upscale ? screen_h : h_);
    lfCore.ngx_params->Set("DLSSNR.Upscaling", upscale ? 1u : 0u);
    lfCore.ngx_params->Set("DLSSNR.Scale", upscale ? static_cast<float>(w) / static_cast<float>(screen_w) : 1.0f);
    lfCore.ngx_params->Set("DLSSNR.ScalingRatio", upscale ? static_cast<float>(w) / static_cast<float>(screen_w) : 1.0f);
    lfCore.ngx_params->Set("DLSSNR.Hint.Render.Preset", NrPresetWanted());
    lfCore.ngx_params->Set("DLSS.Feature.Create.Flags", 0u);

    {
        wchar_t data_dir[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, data_dir, MAX_PATH);
        if (wchar_t *sl = wcsrchr(data_dir, L'\\')) *(sl + 1) = L'\0';
        NVSDK_NGX_FeatureDiscoveryInfo di = {};
        di.SDKVersion = NVSDK_NGX_Version_API;
        di.FeatureID = NVSDK_NGX_Feature_Reserved18;
        di.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
        di.Identifier.v.ProjectDesc.ProjectId = "a0f57b54-1daf-4934-90ae-c4035c19df04";
        di.Identifier.v.ProjectDesc.EngineType = NVSDK_NGX_ENGINE_TYPE_CUSTOM;
        di.Identifier.v.ProjectDesc.EngineVersion = "1.0";
        di.ApplicationDataPath = data_dir;
        di.FeatureInfo = &lfNr.search_info;
        NVSDK_NGX_FeatureRequirement req = {};
        const NVSDK_NGX_Result qrr = NVSDK_NGX_D3D12_GetFeatureRequirements(lfCard.adapter, &di, &req);
        if (!NVSDK_NGX_FAILED(qrr))
        {
            if (req.FeatureSupported == NVSDK_NGX_FeatureSupportResult_Supported)
                Trace("[card] the network is supported here (needs architecture 0x%X)", req.MinHWArchitecture);
            else
            {
                static const char *bits[] = {"check-not-present", "driver-unsupported",
                                             "adapter-unsupported", "os-below-minimum",
                                             "not-implemented"};
                char why[160] = {};
                int off = 0;
                for (int b = 0; b < 5; ++b)
                    if (req.FeatureSupported & (1 << b))
                        off += _snprintf_s(why + off, sizeof(why) - off, _TRUNCATE, "%s%s",
                                           off ? "+" : "", bits[b]);
                Trace("[card] the network is REFUSED (%s): needs architecture 0x%X, Windows %s",
                    why, req.MinHWArchitecture, req.MinOSVersion);
            }
        }
        else
            Trace("[card] could not ask what the network needs (0x%08X) - creating it anyway", qrr);
    }

    if (!CmdOpen()) return false;
    DWORD ccode = 0;
    NVSDK_NGX_Result rf = static_cast<NVSDK_NGX_Result>(0x7FFFFFFF);
    __try { rf = lfNr.create(lfCore.cmd, NVSDK_NGX_Feature_Reserved18, lfCore.ngx_params, &lfCore.ngx_feature); }
    __except (EXCEPTION_EXECUTE_HANDLER) { ccode = GetExceptionCode(); }
    lfNr.create_code = static_cast<uint32_t>(rf);
    if (out_r != nullptr) *out_r = rf;
    if (ccode != 0)
    {
        DropCommands();

        lfCore.ngx_feature = nullptr;
        Trace("[card] creating the network threw 0x%08X - caught, nothing sent", ccode);
        return false;
    }
    const UINT64 v = CmdSubmit();
    if (!AwaitMark(lfCore.timeline, v, 30000)) { Trace("[net] the network was not finished"); return false; }
    if (NVSDK_NGX_FAILED(rf) || lfCore.ngx_feature == nullptr)
    { Trace("[net] the network was refused 0x%08X (%s)", rf, NgxCodeText(rf)); lfCore.ngx_feature = nullptr; return false; }
    Trace("[net] network up: %ux%u%s, preset %u, answer 0x%08X", w, h_,
        upscale ? " (screen -> work size -> screen)" : "", NrPresetWanted(), rf);
    ReportVram("network create");
    return true;
}

// One network run through NGX.
static bool NetworkRun(ID3D12Resource *pic, ID3D12Resource *output, ID3D12Resource *depth, ID3D12Resource *motion,
                     UINT w, UINT h_, int reset, float mvsx, float mvsy)
{
    if (!CmdOpen()) return false;

    NVSDK_NGX_D3D12_DLSS_Eval_Params ep = {};
    ep.Feature.pInColor  = pic;
    ep.Feature.pInOutput = output;
    ep.pInDepth          = depth;
    ep.pInMotionVectors  = motion;
    ep.InRenderSubrectDimensions.Width  = w;
    ep.InRenderSubrectDimensions.Height = h_;
    ep.InReset           = reset;
    ep.InMVScaleX        = mvsx;
    ep.InMVScaleY        = mvsy;
    ep.InPreExposure     = 1.0f;
    ep.InExposureScale   = 1.0f;

    DWORD ecode = 0;
    NVSDK_NGX_Result re = NgxEvaluateGuarded(&ep, &ecode);
    if (ecode != 0) { DropCommands(); Trace("[card] a network run threw 0x%08X - caught, nothing sent", ecode); return false; }
    if (CmdSubmit() == 0) return false;
    if (NVSDK_NGX_FAILED(re)) { Trace("[card] a network run failed 0x%08X (%s)", re, NgxCodeText(re)); return false; }
    return true;
}

// Create the network with a crash inside the runtime caught.
static NVSDK_NGX_Result NgxCreateGuarded(NVSDK_NGX_DLSS_Create_Params *cp, DWORD *blob)
{
    *blob = 0;
    __try { return NGX_D3D12_CREATE_DLSS_EXT(lfCore.cmd, 1, 1, &lfCore.ngx_feature, lfCore.ngx_params, cp); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *blob = GetExceptionCode(); return static_cast<NVSDK_NGX_Result>(0x7FFFFFFF); }
}

// Run it the same way.
static NVSDK_NGX_Result NgxEvaluateGuarded(NVSDK_NGX_D3D12_DLSS_Eval_Params *ep, DWORD *blob)
{
    *blob = 0;
    __try { return NGX_D3D12_EVALUATE_DLSS_EXT(lfCore.cmd, lfCore.ngx_feature, lfCore.ngx_params, ep); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *blob = GetExceptionCode(); return static_cast<NVSDK_NGX_Result>(0x7FFFFFFF); }
}

// Release it the same way.
static void NgxReleaseGuarded(NVSDK_NGX_Handle *f)
{
    if (f == nullptr) return;

    __try
    {
        if (lfNr.release != nullptr) lfNr.release(f);
        else NVSDK_NGX_D3D12_ReleaseFeature(f);
    }
    __except (EXCEPTION_EXECUTE_HANDLER) { Trace("[card] releasing the network threw 0x%08X - ignored", GetExceptionCode()); }
}

// Set an NGX float and check that it stuck.
static bool ParamSetF(NVSDK_NGX_Parameter *p, const char *name, float value)
{
    p->Set(name, value);
    float len = -1.0f;
    return !NVSDK_NGX_FAILED(static_cast<NVSDK_NGX_Result>(p->Get(name, &len))) && len == value;
}

// Set an NGX integer and check that it stuck.
static bool ParamSetU(NVSDK_NGX_Parameter *p, const char *name, unsigned int value)
{
    p->Set(name, value);
    unsigned int len = 0xFFFFFFFFu;
    return !NVSDK_NGX_FAILED(static_cast<NVSDK_NGX_Result>(p->Get(name, &len))) && len == value;
}

// Is the network actually running (Radeon: is its engine up).
static bool NrLive()
{
#ifdef LF_RADEON
    return RadeonLive();
#else
    return lfCore.ngx_feature != nullptr;
#endif
}
