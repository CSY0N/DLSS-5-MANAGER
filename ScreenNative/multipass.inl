// Independent persistent histories for sequential NR layers. The original
// input is immutable; composition and the comparison wipe happen only once.
// How many layers the slider offers. The first is the feature the worker
// already has, so the extras are one fewer - and THIS is what the array below
// has to be sized from. It held two while the slider went to four, so a
// four-pass run wrote a feature handle past the end of it and then evaluated
// whatever was there: "[multipass] 4 independent layers ready" followed by a
// failed evaluate, every time (1.3.2). Six since the round after 1.3.2.
static constexpr unsigned kMaxPasses = 6;
static NVSDK_NGX_Handle* g_extra_features[kMaxPasses - 1] = {};
static ID3D12Resource* g_pass_scratch = nullptr;
static bool g_pass_ready = false;
static unsigned g_pass_count = 1;

static void ReleaseMultiPass()
{
    for (auto& feature : g_extra_features) {
        if (feature) NgxReleaseGuarded(feature);
        feature = nullptr;
    }
    if (g_pass_scratch) g_pass_scratch->Release();
    g_pass_scratch = nullptr;
    g_pass_ready = false;
    g_pass_count = 1;
}

static bool EnsureMultiPass(Pipeline& v)
{
    if (g_pass_ready) return true;
    char value[8] = {};
    GetEnvironmentVariableA("DLSS_MANAGER_PASSES", value, sizeof(value));
    const int count = atoi(value);
    // Four since 1.3.2, six since the round after: people with cards that can
    // afford it asked for it. Each pass is another whole run of the network,
    // so this is a straight multiplier on the frame time - the UI says so
    // beside the slider.
    g_pass_count = count >= 1 && count <= (int)kMaxPasses ? count : 1;
    if (g_pass_count == 1) { g_pass_ready = true; return true; }
    ID3D12Resource* result = v.net_small ? v.net_out : v.result;
    auto desc = result->GetDesc();
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(lfCore.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&g_pass_scratch))))
    {
        // No room even for the scratch texture: one layer, which is the
        // feature the worker already has and always works.
        Trace("[multipass] scratch texture failed - running 1 layer");
        g_pass_count = 1;
        g_pass_ready = true;
        return true;
    }
    const auto first = lfCore.ngx_feature;
    const int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure |
                      NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    const unsigned wanted = g_pass_count;
    for (unsigned i = 0; i < wanted - 1; ++i) {
        lfCore.ngx_feature = nullptr;
        NVSDK_NGX_Result resultCode = NVSDK_NGX_Result_Fail;
        const bool ok = NetworkCreate(v.work_w, v.work_h, flags, &resultCode,
            v.net_small || !v.stretched ? 0 : v.screen_w, v.net_small || !v.stretched ? 0 : v.screen_h);
        g_extra_features[i] = ok ? lfCore.ngx_feature : nullptr;
        lfCore.ngx_feature = first;
        if (!ok)
        {
            // Out of video memory is the usual reason at five or six layers
            // on an 8 GB card. The layers that were made are kept and run -
            // failing the whole evaluate here used to end the worker, so
            // asking for more than the card holds stopped the picture.
            g_pass_count = i + 1;
            Trace("[multipass] layer %u could not be created (0x%08X) - running %u of %u",
                i + 2, static_cast<unsigned>(resultCode), g_pass_count, wanted);
            break;
        }
    }
    if (g_pass_count == 1 && g_pass_scratch) { g_pass_scratch->Release(); g_pass_scratch = nullptr; }
    g_pass_ready = true;
    Trace("[multipass] %u independent layers ready", g_pass_count);
    return true;
}

static NVSDK_NGX_Result EvaluateExtraLayers(ID3D12Resource* result, int reset)
{
    NVSDK_NGX_Result status = NVSDK_NGX_Result_Success;
    for (unsigned i = 0; i < g_pass_count - 1; ++i) {
        auto read = Barrier(result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                               D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        lfCore.cmd->ResourceBarrier(1, &read);
        lfCore.ngx_params->Set("DLSSNR.Color", result);
        lfCore.ngx_params->Set("DLSSNR.Output", g_pass_scratch);
        lfCore.ngx_params->Set("DLSSNR.Reset", static_cast<unsigned>(reset));
        // Avoid repeatedly lifting shadows across layers.
        lfCore.ngx_params->Set("DLSSNR.LocalToneStrength", 0.0f);
        DWORD exception = 0;
        __try { status = lfNr.evaluate(lfCore.cmd, g_extra_features[i], lfCore.ngx_params, nullptr); }
        __except (EXCEPTION_EXECUTE_HANDLER) { exception = GetExceptionCode(); }
        if (exception || NVSDK_NGX_FAILED(status)) return NVSDK_NGX_Result_Fail;
        D3D12_RESOURCE_BARRIER copy[] = {
            Barrier(result, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
            Barrier(g_pass_scratch, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE)
        };
        lfCore.cmd->ResourceBarrier(2, copy);
        lfCore.cmd->CopyResource(result, g_pass_scratch);
        D3D12_RESOURCE_BARRIER back[] = {
            Barrier(result, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
            Barrier(g_pass_scratch, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)
        };
        lfCore.cmd->ResourceBarrier(2, back);
    }
    return status;
}
