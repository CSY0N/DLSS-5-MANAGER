// lf_hdr_show.inl - HDR output: the swap chain format and colour space, and
// the pass that puts the network's SDR result back on an HDR picture.
static ID3D12RootSignature *hdr_root = nullptr;
static ID3D12PipelineState *hdr_pso = nullptr;
static ID3D12DescriptorHeap *hdr_heap = nullptr;
static ID3D12Resource *hdr_target = nullptr;
static winrt::com_ptr<ID3D12RootSignature> hdr_export_root;
static winrt::com_ptr<ID3D12PipelineState> hdr_export_pso;
static winrt::com_ptr<ID3D12DescriptorHeap> hdr_export_heap;
static winrt::com_ptr<ID3D12Resource> hdr_export_target;

static void HdrRelease()
{
    if (hdr_target) { hdr_target->Release(); hdr_target = nullptr; }
    if (hdr_heap) { hdr_heap->Release(); hdr_heap = nullptr; }
    if (hdr_pso) { hdr_pso->Release(); hdr_pso = nullptr; }
    if (hdr_root) { hdr_root->Release(); hdr_root = nullptr; }
    hdr_export_target = nullptr; hdr_export_heap = nullptr;
    hdr_export_pso = nullptr; hdr_export_root = nullptr;
}

static ID3D12Resource *HdrExportFrame(ID3D12GraphicsCommandList *list, ID3D12Resource *source,
                                    D3D12_RESOURCE_STATES state)
{
    if (source->GetDesc().Format == DXGI_FORMAT_R8G8B8A8_UNORM) return source;
    if (source->GetDesc().Format != DXGI_FORMAT_R10G10B10A2_UNORM) return nullptr;
    if (!hdr_export_pso || !hdr_export_heap || !hdr_export_root) {
        hdr_export_heap = nullptr; hdr_export_pso = nullptr; hdr_export_root = nullptr;
        winrt::com_ptr<ID3DBlob> code, diag, signature;
        if (FAILED(CachedCompile(kHdrExportHlsl, sizeof(kHdrExportHlsl)-1, "lf-hdr-export",
            nullptr, nullptr, "CSMain", "cs_5_0", 0, 0, code.put(), diag.put()))) {
            Trace("[hdr] SDR export shader: %s", diag ? (char *)diag->GetBufferPointer() : "compile failed");
            return nullptr;
        }
        D3D12_DESCRIPTOR_RANGE ranges[] = {
            {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0},
            {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1}};
        D3D12_ROOT_PARAMETER parameter = {};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable = {2, ranges};
        D3D12_ROOT_SIGNATURE_DESC root = {1, &parameter};
        if (FAILED(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, signature.put(), diag.put())) ||
            FAILED(lfCore.device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(), IID_PPV_ARGS(hdr_export_root.put())))) return nullptr;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline = {};
        pipeline.pRootSignature = hdr_export_root.get(); pipeline.CS = {code->GetBufferPointer(), code->GetBufferSize()};
        if (FAILED(lfCore.device->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(hdr_export_pso.put())))) return nullptr;
        D3D12_DESCRIPTOR_HEAP_DESC heap = {};
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; heap.NumDescriptors = 2;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(lfCore.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(hdr_export_heap.put())))) return nullptr;
    }
    const UINT w = UINT(source->GetDesc().Width), h = source->GetDesc().Height;
    if (!hdr_export_target || hdr_export_target->GetDesc().Width != w || hdr_export_target->GetDesc().Height != h) {
        hdr_export_target.attach(NewTexture(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, true));
        if (!hdr_export_target) return nullptr;
        auto start = Barrier(hdr_export_target.get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1, &start);
    }
    auto cpu = hdr_export_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = DXGI_FORMAT_R10G10B10A2_UNORM; srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels = 1;
    lfCore.device->CreateShaderResourceView(source, &srv, cpu);
    cpu.ptr += lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    lfCore.device->CreateUnorderedAccessView(hdr_export_target.get(), nullptr, &uav, cpu);
    D3D12_RESOURCE_BARRIER before[] = {
        Barrier(source, state, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(hdr_export_target.get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    list->ResourceBarrier(2, before);
    ID3D12DescriptorHeap *heaps[] = {hdr_export_heap.get()};
    list->SetDescriptorHeaps(1, heaps); list->SetComputeRootSignature(hdr_export_root.get());
    list->SetPipelineState(hdr_export_pso.get());
    list->SetComputeRootDescriptorTable(0, hdr_export_heap->GetGPUDescriptorHandleForHeapStart());
    list->Dispatch((w+7)/8, (h+7)/8, 1);
    D3D12_RESOURCE_BARRIER after[] = {
        Barrier(source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, state),
        Barrier(hdr_export_target.get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE)};
    list->ResourceBarrier(2, after);
    return hdr_export_target.get();
}

static bool ShowFormatReady(bool hdr, bool pq)
{
    DXGI_SWAP_CHAIN_DESC1 desc = {};
    if (FAILED(lfShow.chain->GetDesc1(&desc))) return false;
    const auto format = hdr ? (pq ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT) : DXGI_FORMAT_R8G8B8A8_UNORM;
    const auto space = hdr ? (pq ? DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 : DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709) : DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    if (desc.Format != format)
    {
        CloseFgResources();
        CloseFlow();
        if (!AwaitMark(lfCore.timeline, lfCore.timeline_value, 30000)) return false;
        if (FAILED(lfShow.chain->ResizeBuffers(0, 0, 0, format, desc.Flags)))
        { Trace("[hdr] the swap chain kept its old format"); return false; }
        Trace("[hdr] output %s", hdr ? (pq ? "HDR10 PQ, 10 bits" : "half-float scRGB") : "SDR, 8 bits");
        lfShow.color_space_set = false;
    }

    if (lfShow.color_space_set && lfShow.color_space == space) return true;
    UINT support = 0;
    if (FAILED(lfShow.chain->CheckColorSpaceSupport(space, &support)) ||
        !(support & DXGI_SWAP_CHAIN_COLOR_SPACE_SUPPORT_FLAG_PRESENT) ||
        FAILED(lfShow.chain->SetColorSpace1(space)))
    {
        Trace("[hdr] the screen does not take colour space %u", (unsigned)space);

        return hdr ? false : true;
    }
    lfShow.color_space_set = true;
    lfShow.color_space = space;
    return true;
}

static bool HdrPipelineReady(UINT w, UINT height, DXGI_FORMAT format)
{
    if (hdr_target)
    {
        const auto d = hdr_target->GetDesc();
        if (d.Width == w && d.Height == height && d.Format == format) return true;
    }

    HdrRelease();
    winrt::com_ptr<ID3DBlob> blob, diag, signature;
    HRESULT status = CachedCompile(kHdrOutHlsl, sizeof(kHdrOutHlsl)-1,
        "lf-hdr-out", nullptr, nullptr, "CSMain", "cs_5_0", 0, 0, blob.put(), diag.put());
    if (FAILED(status))
    { Trace("[hdr] shader build %s", diag ? (char *)diag->GetBufferPointer() : "failed"); return false; }
    D3D12_DESCRIPTOR_RANGE table[2] = {
        {D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 3, 0, 0, 0},
        {D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 3}};
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable = {2, table};
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants = {0, 0, 4};
    D3D12_ROOT_SIGNATURE_DESC rs = {2, params};
    if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, signature.put(), diag.put())))
        return false;
    if (FAILED(lfCore.device->CreateRootSignature(0, signature->GetBufferPointer(), signature->GetBufferSize(),
                                         IID_PPV_ARGS(&hdr_root)))) return false;
    D3D12_COMPUTE_PIPELINE_STATE_DESC ps = {};
    ps.pRootSignature = hdr_root;
    ps.CS = {blob->GetBufferPointer(), blob->GetBufferSize()};
    if (FAILED(lfCore.device->CreateComputePipelineState(&ps, IID_PPV_ARGS(&hdr_pso)))) return false;
    D3D12_DESCRIPTOR_HEAP_DESC heap = {};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = 4;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(lfCore.device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&hdr_heap)))) return false;
    hdr_target = NewTexture(w, height, format, true);
    return hdr_target != nullptr;
}

static ID3D12Resource *HdrPrepareFrame(Pipeline &pipe, bool bypass, bool pq)
{
    const UINT w = pipe.stretched ? pipe.screen_w : pipe.work_w;
    const UINT height = pipe.stretched ? pipe.screen_h : pipe.work_h;
    if (!lfCap.bridge12 || !lfCap.has_frame) return nullptr;
    const auto native_desc = lfCap.bridge12->GetDesc();
    if (native_desc.Width != w || native_desc.Height != height)
    { Trace("[hdr] source and output sizes differ - old HDR frame not shown"); return nullptr; }
    if (!HdrPipelineReady(w, height, pq ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT)) return nullptr;

    const UINT step_bytes = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu_at = hdr_heap->GetCPUDescriptorHandleForHeapStart();

    ID3D12Resource *inputs[] = {lfCap.bridge12, pipe.picture.gpu_tex, bypass ? pipe.picture.gpu_tex : pipe.result};
    for (auto *input : inputs)
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = input->GetDesc().Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        lfCore.device->CreateShaderResourceView(input, &srv, cpu_at);
        cpu_at.ptr += step_bytes;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_view = {};
    uav_view.Format = hdr_target->GetDesc().Format;
    uav_view.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    lfCore.device->CreateUnorderedAccessView(hdr_target, nullptr, &uav_view, cpu_at);
    if (!CmdOpen()) return nullptr;
    D3D12_RESOURCE_BARRIER barriers_in[] = {
        Barrier(hdr_target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(lfCap.bridge12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(pipe.result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    lfCore.cmd->ResourceBarrier(bypass ? 2 : 3, barriers_in);
    lfCore.cmd->SetDescriptorHeaps(1, &hdr_heap);
    lfCore.cmd->SetComputeRootSignature(hdr_root);
    lfCore.cmd->SetPipelineState(hdr_pso);
    lfCore.cmd->SetComputeRootDescriptorTable(0, hdr_heap->GetGPUDescriptorHandleForHeapStart());
    struct { float white; UINT bypass, split, hdr; } constants = {
        lfHdr.paper_white, bypass ? 1u : 0u, lfHdr.compare_x,
        (lfHdr.keep ? 1u : 0u) | (pq ? 2u : 0u) | (lfHdr.linear ? 4u : 0u)};
    lfCore.cmd->SetComputeRoot32BitConstants(1, 4, &constants, 0);
    lfCore.cmd->Dispatch((w+7)/8, (height+7)/8, 1);
    D3D12_RESOURCE_BARRIER barriers_out[] = {
        Barrier(hdr_target, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON),
        Barrier(lfCap.bridge12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
        Barrier(pipe.result, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    lfCore.cmd->ResourceBarrier(bypass ? 2 : 3, barriers_out);
    if (!AwaitMark(lfCore.timeline, CmdSubmit(), 30000)) return nullptr;
    return hdr_target;
}

// Native DLSS-G takes HDR10/PQ. FLOW uses the same bounded HDR colour input,
// with FP16 interpolation scratch so its confidence alpha keeps precision.
static bool ShowHdr(Pipeline &pipe, bool bypass)
{
    const bool pq = lfHdr.keep && (FgRequested() || FlowWanted());
    if (!ShowFormatReady(true, pq)) return false;
    auto color = HdrPrepareFrame(pipe, bypass, pq);
    if (!color) return false;
    if (FgRequested() && FgPresent(pipe, color, D3D12_RESOURCE_STATE_COMMON)) return true;
    if (FlowActive()) return FlowPresent(pipe, color, D3D12_RESOURCE_STATE_COMMON) || FlowToken();
    winrt::com_ptr<ID3D12Resource> back_buffer;
    if (FAILED(lfShow.chain->GetBuffer(lfShow.chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(back_buffer.put()))) || !CmdOpen()) return false;
    D3D12_RESOURCE_BARRIER before[] = {
        Barrier(color, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Barrier(back_buffer.get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST)};
    lfCore.cmd->ResourceBarrier(2, before);
    ShowBlit(lfCore.cmd, back_buffer.get(), color);
    ShowProof(lfCore.cmd, back_buffer.get(), true);
    CounterStamp(lfCore.cmd, back_buffer.get());
    D3D12_RESOURCE_BARRIER after[] = {
        Barrier(color, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
        Barrier(back_buffer.get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT)};
    lfCore.cmd->ResourceBarrier(2, after);
    const UINT w = pipe.stretched ? pipe.screen_w : pipe.work_w;
    const UINT height = pipe.stretched ? pipe.screen_h : pipe.work_h;
    ID3D12Resource *export_src = bypass ? pipe.picture.gpu_tex : pipe.result;
    auto rest = bypass ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    auto export_pre = Barrier(export_src, rest, D3D12_RESOURCE_STATE_COPY_SOURCE);
    lfCore.cmd->ResourceBarrier(1, &export_pre);
    ShareCopy(lfCore.cmd, export_src, w, height);
    MsmLinkCopy(lfCore.cmd, export_src, w, height);
    auto export_post = Barrier(export_src, D3D12_RESOURCE_STATE_COPY_SOURCE, rest);
    lfCore.cmd->ResourceBarrier(1, &export_post);
    const auto fence = CmdSubmit();
    MsmLinkSent();
    if (!AwaitMark(lfCore.timeline, fence, 2000)) return false;

    const bool ok = ShowResult(ShowPresentNow(), "hdr output");
    if (ok) { ShowRevealOnce(); ShareSend(); }
    return ok;
}
