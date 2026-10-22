// lf_trace.inl - how the worker talks: one timestamped line per event on
// stderr (the engine files each line under Screen.log), readable names for
// NGX answers, and what is known when the device is lost.

// One line on stderr with the local time in front; the engine reads stderr.
static void Trace(const char *pixel_format, ...)
{
    char text[2048];
    va_list args;
    va_start(args, pixel_format);
    _vsnprintf_s(text, sizeof(text), _TRUNCATE, pixel_format, args);
    va_end(args);
    SYSTEMTIME now;
    GetLocalTime(&now);
    fprintf(stderr, "%02u:%02u:%02u.%03u  %s\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, text);
    fflush(stderr);
}

// NGX answers by name, for the log.
static const char *NgxCodeText(NVSDK_NGX_Result r)
{
    struct Named { unsigned code; const char *text; };
    static const Named kKnown[] = {
        {0x00000001u, "Success"}, {0xBAD00001u, "FeatureNotSupported"}, {0xBAD00002u, "PlatformError"},
        {0xBAD00003u, "FeatureAlreadyExists"}, {0xBAD00004u, "FeatureNotFound"}, {0xBAD00005u, "InvalidParameter"},
        {0xBAD00007u, "NotInitialized"}, {0xBAD00008u, "UnsupportedInputFormat"}, {0xBAD0000Au, "MissingInput"},
        {0xBAD0000Bu, "UnableToInitializeFeature"}, {0xBAD0000Du, "OutOfGPUMemory"}, {0xBAD0000Eu, "UnsupportedFormat"},
    };
    for (const Named &known : kKnown)
        if (known.code == static_cast<unsigned>(r)) return known.text;
    return "?";
}

// The device is gone: say why, and what DRED recorded last.
static void ReportDeviceLoss(const char *where)
{
    if (lfCore.device == nullptr) return;
    const HRESULT reason = lfCore.device->GetDeviceRemovedReason();
    Trace("[card] the device was lost in %s: 0x%08X", where, (unsigned)reason);
    ID3D12DeviceRemovedExtendedData1 *dred = nullptr;
    if (FAILED(lfCore.device->QueryInterface(__uuidof(ID3D12DeviceRemovedExtendedData1),
                                     reinterpret_cast<void **>(&dred))))
        return;
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT crumbs = {};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&crumbs)) && crumbs.pHeadAutoBreadcrumbNode)
    {
        const D3D12_AUTO_BREADCRUMB_NODE *n = crumbs.pHeadAutoBreadcrumbNode;
        Trace("[card] breadcrumbs: %u, the last %u, list %p",
            n->BreadcrumbCount,
            n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0u,
            (void *)n->pCommandList);
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT pf = {};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)) && pf.PageFaultVA != 0)
        Trace("[card] breadcrumbs: page fault at 0x%llX", (unsigned long long)pf.PageFaultVA);
    dred->Release();
}

// How much video memory this process holds right now, and the budget.
static void ReportVram(const char *where)
{
    if (lfCard.adapter == nullptr) return;
    DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
    if (SUCCEEDED(lfCard.adapter->QueryVideoMemoryInfo(
            0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info)))
        Trace("[card] video memory after %s: %llu of %llu MB", where,
            (unsigned long long)(info.CurrentUsage >> 20),
            (unsigned long long)(info.Budget >> 20));
}
