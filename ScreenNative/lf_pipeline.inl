// lf_pipeline.inl - the picture's path through the card: staging textures,
// the working size the network runs at, the resize and detail passes around
// it, the network pass itself, the compare wipe, and the read-back when the
// engine asks for pixels.

// A texture plus the upload buffer that feeds it.
static bool StagedCreate(StagedTexture &staged, UINT w, UINT hgt, DXGI_FORMAT pixel_format, UINT tight_pitch,
                           D3D12_RESOURCE_FLAGS tex_flags)
{
    D3D12_HEAP_PROPERTIES vram_heap = {};
    vram_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC tex_desc = {};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = w; tex_desc.Height = hgt; tex_desc.DepthOrArraySize = 1; tex_desc.MipLevels = 1;
    tex_desc.Format = pixel_format; tex_desc.SampleDesc.Count = 1; tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    tex_desc.Flags = tex_flags;
    if (FAILED(lfCore.device->CreateCommittedResource(&vram_heap, D3D12_HEAP_FLAG_NONE, &tex_desc, D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr, __uuidof(ID3D12Resource), reinterpret_cast<void **>(&staged.gpu_tex)))) return false;

    UINT64 total_bytes = 0;
    lfCore.device->GetCopyableFootprints(&tex_desc, 0, 1, 0, &staged.layout, &staged.row_count, &staged.row_bytes, &total_bytes);
    if (staged.row_count != hgt || staged.row_bytes < tight_pitch) return false;
    D3D12_HEAP_PROPERTIES upload_heap = {};
    upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buf_desc = {};
    buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf_desc.Width = total_bytes; buf_desc.Height = 1; buf_desc.DepthOrArraySize = 1; buf_desc.MipLevels = 1;
    buf_desc.SampleDesc.Count = 1; buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return SUCCEEDED(lfCore.device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, __uuidof(ID3D12Resource),
        reinterpret_cast<void **>(&staged.staging)));
}

// Copy rows from memory into the upload buffer.
static bool StagedFill(StagedTexture &staged, const BYTE *from, UINT from_pitch, UINT hgt)
{
    BYTE *into = nullptr;
    if (FAILED(staged.staging->Map(0, nullptr, reinterpret_cast<void **>(&into)))) return false;
    for (UINT y = 0; y < hgt; ++y)
        memcpy(into + static_cast<size_t>(y) * staged.layout.Footprint.RowPitch,
               from + static_cast<size_t>(y) * from_pitch, from_pitch);
    staged.staging->Unmap(0, nullptr);
    return true;
}

// Record the upload buffer -> texture copy.
static void StagedCopy(ID3D12GraphicsCommandList *cmdl, StagedTexture &staged)
{
    D3D12_TEXTURE_COPY_LOCATION from = {}, into = {};
    from.pResource = staged.staging;
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = staged.layout;
    into.pResource = staged.gpu_tex;
    into.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    cmdl->CopyTextureRegion(&into, 0, 0, 0, &from, nullptr);
}

// Free a staged texture.
static void StagedRelease(StagedTexture &staged)
{
    if (staged.gpu_tex != nullptr) { staged.gpu_tex->Release(); staged.gpu_tex = nullptr; }
    if (staged.staging != nullptr) { staged.staging->Release(); staged.staging = nullptr; }
}

// All textures the pipeline needs at a working size (and screen size).
static bool PipelineBuild(Pipeline &pipe, UINT w, UINT hgt, UINT full_w, UINT full_h,
                                 int small_wanted)
{
    pipe.work_w = w; pipe.work_h = hgt;
    // Flow Super Resolution: with a display larger than the working size, the
    // picture is made smaller than the display - a fifth of the way down to
    // the working size since round 25 (SuperResSize) - the capture is scaled
    // down to it once, everything after runs there (the network still at the
    // working size, its detail added back as always), and only the output
    // (and what is read back) is scaled up to the display (flow_upscaler.inl).
    pipe.display_w = full_w; pipe.display_h = full_h;
    pipe.super_res = false;
    if (full_w > 0 && full_h > 0 && (full_w != w || full_h != hgt) && SuperResWanted() && SuperResReady())
    {
        const UINT sr_w = SuperResSize(full_w, w), sr_h = SuperResSize(full_h, hgt);
        if (sr_w < full_w || sr_h < full_h)
        {
            pipe.super_res = true;
            full_w = sr_w;
            full_h = sr_h;
        }
    }
    pipe.picture_new = true;
    pipe.screen_w = full_w; pipe.screen_h = full_h;
    pipe.stretched = (full_w > 0 && full_h > 0 && (full_w != w || full_h != hgt));
    SuperResNote(pipe);

    const bool asked = (small_wanted < 0) ? SmallNetworkWanted() : (small_wanted != 0);
    pipe.net_small = pipe.stretched && asked;
    pipe.net_w = pipe.net_small ? w : 0;
    pipe.net_h = pipe.net_small ? hgt : 0;

    pipe.add_detail = pipe.net_small && !lfNr.direct;
    pipe.detail_gain = pipe.add_detail ? DetailAmount() * EffectGain() : 1.0f;
    const UINT pic_w = pipe.stretched ? full_w : w;
    const UINT pic_h = pipe.stretched ? full_h : hgt;
    if (pipe.net_small) ClampWorkSize(pic_w,pic_h,pipe.net_w,pipe.net_h);
    // With Flow Super Resolution the capture is scaled into the picture by a
    // shader (SuperResDownscale), not copied.
    if (!StagedCreate(pipe.picture, pic_w, pic_h, DXGI_FORMAT_R8G8B8A8_UNORM, pic_w * 4,
                      pipe.super_res ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE) ||

        !StagedCreate(pipe.motion, w, hgt, DXGI_FORMAT_R16G16_FLOAT, w * 4,
                        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        return false;

    D3D12_HEAP_PROPERTIES vram_heap = {};
    vram_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC tex_desc = {};
    tex_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    tex_desc.Width = pic_w; tex_desc.Height = pic_h; tex_desc.DepthOrArraySize = 1; tex_desc.MipLevels = 1;
    tex_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; tex_desc.SampleDesc.Count = 1;
    tex_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    tex_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(lfCore.device->CreateCommittedResource(&vram_heap, D3D12_HEAP_FLAG_NONE, &tex_desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, __uuidof(ID3D12Resource),
        reinterpret_cast<void **>(&pipe.result)))) return false;

    if (pipe.net_small)
    {
        if (!ScalerReady()) { pipe.net_small = false; }
        else
        {
            D3D12_RESOURCE_DESC net_desc = tex_desc;
            net_desc.Width = pipe.net_w; net_desc.Height = pipe.net_h;
            const bool ok =
                SUCCEEDED(lfCore.device->CreateCommittedResource(
                    &vram_heap, D3D12_HEAP_FLAG_NONE, &net_desc,
                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
                    __uuidof(ID3D12Resource), reinterpret_cast<void **>(&pipe.net_in))) &&
                SUCCEEDED(lfCore.device->CreateCommittedResource(
                    &vram_heap, D3D12_HEAP_FLAG_NONE, &net_desc,
                    D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                    __uuidof(ID3D12Resource), reinterpret_cast<void **>(&pipe.net_out)));
            if (!ok)
            {
                Trace("[net] no %ux%u working textures - the network stays at screen size",
                    pipe.net_w, pipe.net_h);
                if (pipe.net_in != nullptr) { pipe.net_in->Release(); pipe.net_in = nullptr; }
                if (pipe.net_out != nullptr) { pipe.net_out->Release(); pipe.net_out = nullptr; }
                pipe.net_small = false;
            }
            else
                Trace("[net] the network works at %ux%u, the picture goes back to %ux%u",
                    pipe.net_w, pipe.net_h, pic_w, pic_h);
        }
    }

    UINT64 total_bytes = 0;
    // What is read back is what is shown: at the display's size with Flow
    // Super Resolution, so the engine, the recording and the screenshots get
    // the size they always did.
    D3D12_RESOURCE_DESC read_desc = tex_desc;
    if (pipe.super_res) { read_desc.Width = pipe.display_w; read_desc.Height = pipe.display_h; }
    lfCore.device->GetCopyableFootprints(&read_desc, 0, 1, 0, &pipe.result_layout, &pipe.result_rows, &pipe.result_row_bytes, &total_bytes);
    D3D12_HEAP_PROPERTIES readback_heap = {};
    readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buf_desc = {};
    buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buf_desc.Width = total_bytes; buf_desc.Height = 1; buf_desc.DepthOrArraySize = 1; buf_desc.MipLevels = 1;
    buf_desc.SampleDesc.Count = 1; buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return SUCCEEDED(lfCore.device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE, &buf_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, __uuidof(ID3D12Resource),
        reinterpret_cast<void **>(&pipe.result_cpu)));
}

// Free them again.
static void PipelineRelease(Pipeline &pipe)
{
    OfaStop();
    OfaClearLatch();
    if (PhasesOn()) { ++lfProf.capture_gen; lfProf.last_source_qpc = 0; lfProf.stamp = {}; }
    ReleaseMultiPass();

    if (pipe.motion.gpu_tex == lfScaler.dst_now) lfScaler.dst_now = nullptr;
    if (pipe.picture.gpu_tex != nullptr) { pipe.picture.gpu_tex->Release(); pipe.picture.gpu_tex = nullptr; }
    if (pipe.picture.staging != nullptr) { pipe.picture.staging->Release(); pipe.picture.staging = nullptr; }
    if (pipe.motion.gpu_tex != nullptr) { pipe.motion.gpu_tex->Release(); pipe.motion.gpu_tex = nullptr; }
    if (pipe.motion.staging != nullptr) { pipe.motion.staging->Release(); pipe.motion.staging = nullptr; }
    if (pipe.result != nullptr) { pipe.result->Release(); pipe.result = nullptr; }
    if (pipe.result_cpu != nullptr) { pipe.result_cpu->Release(); pipe.result_cpu = nullptr; }
    StagedRelease(pipe.feed_full);
    SuperResExportRelease(false);

    lfScaler.fine_src = lfScaler.fine_dst = nullptr;
    lfScaler.fine_src2 = lfScaler.fine_dst2 = nullptr;

    lfScaler.detail_native = lfScaler.detail_in = nullptr;
    lfScaler.detail_out = lfScaler.detail_dst = nullptr;
    lfScaler.detail_delta = nullptr;
    CloseHaloTargets();
    CloseDenoiseTargets();
    CloseFinishTargets();
    if (pipe.net_in != nullptr) { pipe.net_in->Release(); pipe.net_in = nullptr; }
    if (pipe.net_out != nullptr) { pipe.net_out->Release(); pipe.net_out = nullptr; }
    pipe.net_small = false;
    pipe.fed = false;

    lfCap.has_frame = false;
}

// The same with Flow Super Resolution: the engine's picture comes at the
// display's size and is brought down to the internal size on the card.
static bool FeedFrameSuperRes(Pipeline &pipe, const BYTE *color, const BYTE *mv, bool motion_small)
{
    if (pipe.feed_full.gpu_tex == nullptr &&
        !StagedCreate(pipe.feed_full, pipe.display_w, pipe.display_h, DXGI_FORMAT_R8G8B8A8_UNORM, pipe.display_w * 4))
    {
        StagedRelease(pipe.feed_full);
        Trace("[upscale] no %ux%u texture for the engine's pictures", pipe.display_w, pipe.display_h);
        return false;
    }
    if (!StagedFill(pipe.feed_full, color, pipe.display_w * 4, pipe.display_h)) return false;
    if (!motion_small && !StagedFill(pipe.motion, mv, pipe.work_w * 4, pipe.work_h)) return false;
    if (!CmdOpen()) return false;
    GpuStageOpen(GS_MOVE);
    // feed_full rests in COPY_DEST, the state it was made in.
    StagedCopy(lfCore.cmd, pipe.feed_full);
    if (!SuperResDownscale(lfCore.cmd, pipe.feed_full.gpu_tex, D3D12_RESOURCE_STATE_COPY_DEST, pipe))
    { DropCommands(); return false; }
    if (motion_small)
    {
        if (!ResizeMotion(pipe, mv, pipe.fed)) return false;
    }
    else
    {
        if (pipe.fed)
        {
            D3D12_RESOURCE_BARRIER to_copy = Barrier(pipe.motion.gpu_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                        D3D12_RESOURCE_STATE_COPY_DEST);
            lfCore.cmd->ResourceBarrier(1, &to_copy);
        }
        StagedCopy(lfCore.cmd, pipe.motion);
        D3D12_RESOURCE_BARRIER to_read = Barrier(pipe.motion.gpu_tex, D3D12_RESOURCE_STATE_COPY_DEST,
                                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        lfCore.cmd->ResourceBarrier(1, &to_read);
    }
    GpuStageClose(GS_MOVE);
    const UINT64 fence = CmdSubmit();
    if (fence == 0) return false;
    pipe.fed = true;
    return GpuStageWait(GS_MOVE, fence, 30000);
}

// A picture and motion from memory into the pipeline (the engine's path).
static bool FeedFrame(Pipeline &pipe, const BYTE *color, const BYTE *mv, bool motion_small)
{
    if (pipe.super_res) return FeedFrameSuperRes(pipe, color, mv, motion_small);
    const UINT pic_w = pipe.stretched ? pipe.screen_w : pipe.work_w;
    const UINT pic_h = pipe.stretched ? pipe.screen_h : pipe.work_h;
    if (!StagedFill(pipe.picture, color, pic_w * 4, pic_h)) return false;
    if (!motion_small && !StagedFill(pipe.motion, mv, pipe.work_w * 4, pipe.work_h)) return false;
    if (!CmdOpen()) return false;
    GpuStageOpen(GS_MOVE);
    if (pipe.fed)
    {
        D3D12_RESOURCE_BARRIER barriers_in[] = {
            Barrier(pipe.picture.gpu_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
            Barrier(pipe.motion.gpu_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
        };
        lfCore.cmd->ResourceBarrier(motion_small ? 1 : _countof(barriers_in), barriers_in);
    }
    StagedCopy(lfCore.cmd, pipe.picture);
    if (motion_small)
    {
        if (!ResizeMotion(pipe, mv, pipe.fed)) return false;
        D3D12_RESOURCE_BARRIER barriers_out = Barrier(pipe.picture.gpu_tex, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        lfCore.cmd->ResourceBarrier(1, &barriers_out);
    }
    else
    {
        StagedCopy(lfCore.cmd, pipe.motion);
        D3D12_RESOURCE_BARRIER barriers_out[] = {
            Barrier(pipe.picture.gpu_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Barrier(pipe.motion.gpu_tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        };
        lfCore.cmd->ResourceBarrier(_countof(barriers_out), barriers_out);
    }
    GpuStageClose(GS_MOVE);
    const UINT64 fence = CmdSubmit();
    if (fence == 0) return false;
    pipe.fed = true;
    return GpuStageWait(GS_MOVE, fence, 30000);
}

// Only the motion field, when the picture came from the worker's own source.
static bool FeedMotion(Pipeline &pipe, const BYTE *mv, bool motion_small,
                             UINT64 *sent_mark)
{
    if (sent_mark) *sent_mark = 0;
    if (!motion_small && !StagedFill(pipe.motion, mv, pipe.work_w * 4, pipe.work_h)) return false;
    if (!CmdOpen()) return false;
    GpuStageOpen(GS_MOVE);
    if (pipe.fed)
    {
        D3D12_RESOURCE_BARRIER barriers_in = Barrier(pipe.motion.gpu_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                                D3D12_RESOURCE_STATE_COPY_DEST);
        lfCore.cmd->ResourceBarrier(1, &barriers_in);
    }
    if (motion_small)
    {
        if (!ResizeMotion(pipe, mv, pipe.fed)) return false;
    }
    else
    {
        StagedCopy(lfCore.cmd, pipe.motion);
        D3D12_RESOURCE_BARRIER barriers_out = Barrier(pipe.motion.gpu_tex, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        lfCore.cmd->ResourceBarrier(1, &barriers_out);
    }
    GpuStageClose(GS_MOVE);
    const UINT64 fence = CmdSubmit();
    if (fence == 0) return false;
    pipe.fed = true;
    if (sent_mark) { *sent_mark = fence; return true; }
    return GpuStageWait(GS_MOVE, fence, 30000);
}

// The resize, crisp and detail pipelines; compiled once.
static bool ScalerReady()
{
    if (lfScaler.pso != nullptr) return lfScaler.fine_pso && lfScaler.crisp_pso && lfScaler.detail_pso;

    HMODULE compiler_dll = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler_dll ? static_cast<CompileFn>(&CachedCompile) : nullptr;
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    auto serialize = d3d12 ? reinterpret_cast<SerializeRootFn>(
                                 GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
    if (compile == nullptr || serialize == nullptr)
    { Trace("[resize] the shader compiler or root signature call is missing"); return false; }

    ID3DBlob *blob = nullptr, *diag = nullptr;
    HRESULT status = compile(kResizeHlsl, sizeof(kResizeHlsl) - 1, "lf-resize.hlsl", nullptr, nullptr,
                         "CSMain", "cs_5_0", 0, 0, &blob, &diag);
    if (FAILED(status) || blob == nullptr)
    {
        Trace("[resize] shader did not compile 0x%08X: %s", status,
            diag ? static_cast<const char *>(diag->GetBufferPointer()) : "(nothing said)");
        if (diag) diag->Release();
        return false;
    }
    if (diag) diag->Release();

    D3D12_DESCRIPTOR_RANGE table[2] = {};
    table[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    table[0].NumDescriptors = 1;
    table[0].BaseShaderRegister = 0;
    table[0].OffsetInDescriptorsFromTableStart = 0;
    table[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    table[1].NumDescriptors = 1;
    table[1].BaseShaderRegister = 0;
    table[1].OffsetInDescriptorsFromTableStart = 1;

    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = table;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.MaxLOD = D3D12_FLOAT32_MAX;
    samp.ShaderRegister = 0;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC root_desc = {};
    root_desc.NumParameters = _countof(params);
    root_desc.pParameters = params;
    root_desc.NumStaticSamplers = 1;
    root_desc.pStaticSamplers = &samp;

    ID3DBlob *rs_blob = nullptr;
    status = serialize(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &diag);
    if (FAILED(status) || rs_blob == nullptr)
    {
        Trace("[resize] root signature did not serialize 0x%08X: %s", status,
            diag ? static_cast<const char *>(diag->GetBufferPointer()) : "(nothing said)");
        if (diag) diag->Release();
        blob->Release();
        return false;
    }
    if (diag) diag->Release();
    status = lfCore.device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                    __uuidof(ID3D12RootSignature),
                                    reinterpret_cast<void **>(&lfScaler.root));
    rs_blob->Release();
    if (FAILED(status)) { Trace("[resize] root signature refused 0x%08X", status); blob->Release(); return false; }

    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = lfScaler.root;
    pd.CS.pShaderBytecode = blob->GetBufferPointer();
    pd.CS.BytecodeLength = blob->GetBufferSize();
    status = lfCore.device->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState),
                                           reinterpret_cast<void **>(&lfScaler.pso));
    blob->Release();
    if (FAILED(status)) { Trace("[resize] pipeline refused 0x%08X", status); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 2;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    status = lfCore.device->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap),
                                     reinterpret_cast<void **>(&lfScaler.heap));
    if (FAILED(status)) { Trace("[resize] descriptor heap refused 0x%08X", status); return false; }

    ID3DBlob *code4 = nullptr;
    status = compile(kResizeFineHlsl, sizeof(kResizeFineHlsl) - 1, "lf-resize-fine.hlsl", nullptr, nullptr,
                 "CSMain", "cs_5_0", 0, 0, &code4, &diag);
    if (FAILED(status) || code4 == nullptr)
    {
        Trace("[resize] picture shader did not compile 0x%08X: %s", status,
            diag ? static_cast<const char *>(diag->GetBufferPointer()) : "(nothing said)");
        if (diag) diag->Release();
        return false;
    }
    if (diag) diag->Release();
    pd.CS.pShaderBytecode = code4->GetBufferPointer();
    pd.CS.BytecodeLength = code4->GetBufferSize();
    status = lfCore.device->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState),
                                           reinterpret_cast<void **>(&lfScaler.fine_pso));
    code4->Release();
    if (FAILED(status)) { Trace("[resize] picture pipeline refused 0x%08X", status); return false; }
    ID3DBlob *sharp_code=nullptr; diag=nullptr;
    status=compile(kCrispHlsl,sizeof(kCrispHlsl)-1,"lf-crisp.hlsl",nullptr,nullptr,
               "CSMain","cs_5_0",0,0,&sharp_code,&diag);
    if (FAILED(status) || !sharp_code) {
        Trace("[resize] crisp shader did not compile 0x%08X: %s",status,
            diag ? static_cast<const char*>(diag->GetBufferPointer()) : "nothing said");
        if(diag) diag->Release();
        return false;
    }
    if(diag) diag->Release();
    pd.CS.pShaderBytecode=sharp_code->GetBufferPointer();pd.CS.BytecodeLength=sharp_code->GetBufferSize();
    status=lfCore.device->CreateComputePipelineState(&pd,__uuidof(ID3D12PipelineState),reinterpret_cast<void**>(&lfScaler.crisp_pso));
    sharp_code->Release();
    if(FAILED(status)) return false;
    heap_desc.NumDescriptors = 4;
    status = lfCore.device->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap),
                                     reinterpret_cast<void **>(&lfScaler.fine_heap));
    if (FAILED(status)) { Trace("[resize] picture descriptor heap refused 0x%08X", status); return false; }

    Trace("[resize] pipelines ready: area average down, bilinear up, limited crisping");

    ID3DBlob *res_code = nullptr;
    status = compile(kDetailHlsl, sizeof(kDetailHlsl) - 1, "lf-detail.hlsl", nullptr, nullptr,
                 "CSMain", "cs_5_0", 0, 0, &res_code, &diag);
    if (FAILED(status) || res_code == nullptr)
    {
        Trace("[detail] shader did not compile 0x%08X: %s", status,
            diag ? static_cast<const char *>(diag->GetBufferPointer()) : "(nothing said)");
        if (diag) diag->Release();
        return false;
    }
    if (diag) diag->Release();

    D3D12_DESCRIPTOR_RANGE rres[2] = {};
    rres[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    rres[0].NumDescriptors = 4;
    rres[0].BaseShaderRegister = 0;
    rres[0].OffsetInDescriptorsFromTableStart = 0;
    rres[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    rres[1].NumDescriptors = 1;
    rres[1].BaseShaderRegister = 0;
    rres[1].OffsetInDescriptorsFromTableStart = 4;

    D3D12_ROOT_PARAMETER rparams[2] = {};
    rparams[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rparams[0].Constants.ShaderRegister = 0;
    rparams[0].Constants.Num32BitValues = 6;   // the fifth: keep colours (round 29); the sixth: low-light noise (round 34)
    rparams[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    rparams[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rparams[1].DescriptorTable.NumDescriptorRanges = 2;
    rparams[1].DescriptorTable.pDescriptorRanges = rres;
    rparams[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_ROOT_SIGNATURE_DESC rrsd = {};
    rrsd.NumParameters = _countof(rparams);
    rrsd.pParameters = rparams;
    rrsd.NumStaticSamplers = 1;
    rrsd.pStaticSamplers = &samp;

    ID3DBlob *rs_res = nullptr;
    status = serialize(&rrsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_res, &diag);
    if (FAILED(status) || rs_res == nullptr)
    {
        Trace("[detail] root signature did not serialize 0x%08X", status);
        if (diag) diag->Release();
        res_code->Release();
        return false;
    }
    if (diag) diag->Release();
    status = lfCore.device->CreateRootSignature(0, rs_res->GetBufferPointer(), rs_res->GetBufferSize(),
                                    __uuidof(ID3D12RootSignature),
                                    reinterpret_cast<void **>(&lfScaler.detail_root));
    rs_res->Release();
    if (FAILED(status)) { Trace("[detail] root signature refused 0x%08X", status); res_code->Release(); return false; }

    D3D12_COMPUTE_PIPELINE_STATE_DESC rpd = {};
    rpd.pRootSignature = lfScaler.detail_root;
    rpd.CS.pShaderBytecode = res_code->GetBufferPointer();
    rpd.CS.BytecodeLength = res_code->GetBufferSize();
    status = lfCore.device->CreateComputePipelineState(&rpd, __uuidof(ID3D12PipelineState),
                                           reinterpret_cast<void **>(&lfScaler.detail_pso));
    res_code->Release();
    if (FAILED(status)) { Trace("[detail] pipeline refused 0x%08X", status); return false; }

    D3D12_DESCRIPTOR_HEAP_DESC rhd = {};
    rhd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    rhd.NumDescriptors = 5;
    rhd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    status = lfCore.device->CreateDescriptorHeap(&rhd, __uuidof(ID3D12DescriptorHeap),
                                     reinterpret_cast<void **>(&lfScaler.detail_heap));
    if (FAILED(status)) { Trace("[detail] descriptor heap refused 0x%08X", status); return false; }

    Trace("[detail] pipeline ready: screen picture plus what the network changed, scaled");
    return true;
}

// Bind the motion resize to its source and target.
static void ScalerBind(ID3D12Resource *from, ID3D12Resource *into)
{
    if (from == lfScaler.src_now && into == lfScaler.dst_now) return;
    const UINT step_bytes = lfCore.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_at = lfScaler.heap->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC view_desc = {};
    view_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view_desc.Texture2D.MipLevels = 1;
    lfCore.device->CreateShaderResourceView(from, &view_desc, cpu_at);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
    uav_desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    cpu_at.ptr += step_bytes;
    lfCore.device->CreateUnorderedAccessView(into, nullptr, &uav_desc, cpu_at);

    lfScaler.src_now = from;
    lfScaler.dst_now = into;
}

// Bind the picture resize to its source and target.
static void ScalerBindFine(ID3D12Resource *from, ID3D12Resource *into, UINT slot)
{
    ID3D12Resource **bound_src = (slot == 0) ? &lfScaler.fine_src : &lfScaler.fine_src2;
    ID3D12Resource **bound_dst = (slot == 0) ? &lfScaler.fine_dst : &lfScaler.fine_dst2;
    if (from == *bound_src && into == *bound_dst) return;
    const UINT step_bytes = lfCore.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_at = lfScaler.fine_heap->GetCPUDescriptorHandleForHeapStart();
    cpu_at.ptr += static_cast<SIZE_T>(slot) * 2 * step_bytes;

    D3D12_SHADER_RESOURCE_VIEW_DESC view_desc = {};
    view_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view_desc.Texture2D.MipLevels = 1;
    lfCore.device->CreateShaderResourceView(from, &view_desc, cpu_at);

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
    uav_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    cpu_at.ptr += step_bytes;
    lfCore.device->CreateUnorderedAccessView(into, nullptr, &uav_desc, cpu_at);

    *bound_src = from;
    *bound_dst = into;
}

// Resize a picture: area average down, bilinear up, optional crisping.
static void ResizePicture(ID3D12Resource *from, UINT src_w, UINT src_h,
                           ID3D12Resource *into, UINT dst_w, UINT dst_h, UINT slot)
{
    ScalerBindFine(from, into, slot);
    const UINT step_bytes = lfCore.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ID3D12DescriptorHeap *heap_list[] = { lfScaler.fine_heap };
    lfCore.cmd->SetDescriptorHeaps(1, heap_list);
    lfCore.cmd->SetComputeRootSignature(lfScaler.root);
    lfCore.cmd->SetPipelineState(lfScaler.fine_pso);
    const UINT sizes[4] = { dst_w, dst_h, src_w, src_h };
    lfCore.cmd->SetComputeRoot32BitConstants(0, 4, sizes, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_at = lfScaler.fine_heap->GetGPUDescriptorHandleForHeapStart();
    gpu_at.ptr += static_cast<UINT64>(slot) * 2 * step_bytes;
    lfCore.cmd->SetComputeRootDescriptorTable(1, gpu_at);
    lfCore.cmd->Dispatch((dst_w + 7) / 8, (dst_h + 7) / 8, 1);
}

// Bind the detail pass to its four inputs.
static void DetailBind(ID3D12Resource *native, ID3D12Resource *net_in,
                                    ID3D12Resource *net_out, ID3D12Resource *into,
                                    ID3D12Resource *delta)
{
    if (native == lfScaler.detail_native && net_in == lfScaler.detail_in &&
        net_out == lfScaler.detail_out && into == lfScaler.detail_dst && delta == lfScaler.detail_delta)
        return;
    const UINT step_bytes = lfCore.device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_at = lfScaler.detail_heap->GetCPUDescriptorHandleForHeapStart();

    D3D12_SHADER_RESOURCE_VIEW_DESC view_desc = {};
    view_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    view_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view_desc.Texture2D.MipLevels = 1;
    for (int i = 0; i < 3; ++i)
    {
        lfCore.device->CreateShaderResourceView(i == 0 ? native : (i == 1 ? net_in : net_out),
                                        &view_desc, cpu_at);
        cpu_at.ptr += step_bytes;
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC dd = view_desc;
    dd.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    lfCore.device->CreateShaderResourceView(delta, &dd, cpu_at);
    cpu_at.ptr += step_bytes;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
    uav_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    lfCore.device->CreateUnorderedAccessView(into, nullptr, &uav_desc, cpu_at);

    lfScaler.detail_native = native;
    lfScaler.detail_in = net_in;
    lfScaler.detail_out = net_out;
    lfScaler.detail_dst = into;
    lfScaler.detail_delta = delta;
}

// Screen picture plus what the network changed at its working size.
static void DetailCompose(ID3D12Resource *native, ID3D12Resource *net_in,
                            ID3D12Resource *net_out, UINT w, UINT h_,
                            ID3D12Resource *into, float strength,
                            ID3D12Resource *delta, float halo)
{
    if (delta == nullptr) halo = 0.0f;
    DetailBind(native, net_in, net_out, into, delta);
    ID3D12DescriptorHeap *heap_list[] = { lfScaler.detail_heap };
    lfCore.cmd->SetDescriptorHeaps(1, heap_list);
    lfCore.cmd->SetComputeRootSignature(lfScaler.detail_root);
    lfCore.cmd->SetPipelineState(lfScaler.detail_pso);
    UINT32 rc[6] = { w, h_, 0, 0, 0, 0 };
    float str = strength;
    const float keep = KeepColour(), low_light = LowLight();
    memcpy(&rc[2], &str, sizeof(float));
    memcpy(&rc[3], &halo, sizeof(float));
    memcpy(&rc[4], &keep, sizeof(float));
    memcpy(&rc[5], &low_light, sizeof(float));
    lfCore.cmd->SetComputeRoot32BitConstants(0, 6, rc, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_at = lfScaler.detail_heap->GetGPUDescriptorHandleForHeapStart();
    lfCore.cmd->SetComputeRootDescriptorTable(1, gpu_at);
    lfCore.cmd->Dispatch((w + 7) / 8, (h_ + 7) / 8, 1);
}

// A motion field arriving at another size gets its own texture.
static bool MotionResizeStart(UINT w, UINT hgt)
{
    MotionResizeStop();
    if (w == 0 || hgt == 0) return true;
    if (w < 8 || hgt < 8 || w > 7680 || hgt > 4320)
    { Trace("[resize] motion size %ux%u makes no sense - refused", w, hgt); return false; }
    if (!ScalerReady()) return false;
    if (!StagedCreate(lfScaler.motion_small, w, hgt, DXGI_FORMAT_R16G16_FLOAT, w * 4))
    { Trace("[resize] no %ux%u motion texture", w, hgt); return false; }
    lfScaler.motion_w = w;
    lfScaler.motion_h = hgt;
    Trace("[resize] motion comes at %ux%u and is stretched on the card", w, hgt);
    return true;
}

// Free it.
static void MotionResizeStop()
{
    StagedRelease(lfScaler.motion_small);
    lfScaler.motion_w = lfScaler.motion_h = 0;
    lfScaler.src_now = nullptr;
    lfScaler.dst_now = nullptr;
}

// Stretch the motion field to the working size on the card.
static bool ResizeMotion(Pipeline &pipe, const BYTE *mv, bool mv_was_ready)
{
    if (!StagedFill(lfScaler.motion_small, mv, lfScaler.motion_w * 4, lfScaler.motion_h)) return false;
    D3D12_RESOURCE_BARRIER to_dst = Barrier(
        lfScaler.motion_small.gpu_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
        D3D12_RESOURCE_STATE_COPY_DEST);
    if (lfScaler.src_now == lfScaler.motion_small.gpu_tex)
        lfCore.cmd->ResourceBarrier(1, &to_dst);
    StagedCopy(lfCore.cmd, lfScaler.motion_small);

    D3D12_RESOURCE_BARRIER barriers_in[] = {
        Barrier(lfScaler.motion_small.gpu_tex, D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(pipe.motion.gpu_tex,
                   mv_was_ready ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                : D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    lfCore.cmd->ResourceBarrier(_countof(barriers_in), barriers_in);

    ScalerBind(lfScaler.motion_small.gpu_tex, pipe.motion.gpu_tex);
    ID3D12DescriptorHeap *heap_list[] = { lfScaler.heap };
    lfCore.cmd->SetDescriptorHeaps(1, heap_list);
    lfCore.cmd->SetComputeRootSignature(lfScaler.root);
    lfCore.cmd->SetPipelineState(lfScaler.pso);
    const UINT sizes[4] = { pipe.work_w, pipe.work_h, lfScaler.motion_w, lfScaler.motion_h };
    lfCore.cmd->SetComputeRoot32BitConstants(0, 4, sizes, 0);
    lfCore.cmd->SetComputeRootDescriptorTable(
        1, lfScaler.heap->GetGPUDescriptorHandleForHeapStart());
    lfCore.cmd->Dispatch((pipe.work_w + 7) / 8, (pipe.work_h + 7) / 8, 1);

    D3D12_RESOURCE_BARRIER barriers_out = Barrier(pipe.motion.gpu_tex, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                             D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    lfCore.cmd->ResourceBarrier(1, &barriers_out);
    return true;
}

// Where the compare divider stands, in pixels.
static UINT CompareColumn(uint32_t reserved, UINT width)
{
    const uint32_t frac = (reserved >> 16) & 0xFFFFu;
    return static_cast<UINT>((static_cast<uint64_t>(width) * frac) / 0xFFFFu);
}

// The compare wipe's target view.
static bool CompareTargetReady(ID3D12Resource *res)
{
    if (res == nullptr) return false;
    if (lfCompare.target == res && lfCompare.heap_gpu != nullptr) return true;
    if (lfCompare.heap_gpu != nullptr) { lfCompare.heap_gpu->Release(); lfCompare.heap_gpu = nullptr; }
    if (lfCompare.heap_cpu != nullptr) { lfCompare.heap_cpu->Release(); lfCompare.heap_cpu = nullptr; }
    lfCompare.target = nullptr;

    D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
    heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap_desc.NumDescriptors = 1;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(lfCore.device->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap),
            reinterpret_cast<void **>(&lfCompare.heap_gpu)))) return false;
    heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    if (FAILED(lfCore.device->CreateDescriptorHeap(&heap_desc, __uuidof(ID3D12DescriptorHeap),
            reinterpret_cast<void **>(&lfCompare.heap_cpu)))) return false;

    D3D12_UNORDERED_ACCESS_VIEW_DESC uav_desc = {};
    uav_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    lfCore.device->CreateUnorderedAccessView(res, nullptr, &uav_desc,
        lfCompare.heap_gpu->GetCPUDescriptorHandleForHeapStart());
    lfCore.device->CreateUnorderedAccessView(res, nullptr, &uav_desc,
        lfCompare.heap_cpu->GetCPUDescriptorHandleForHeapStart());
    lfCompare.target = res;
    return true;
}

// Draw the untouched picture left of the divider, in the same command list.
static void CompareRecord(Pipeline &pipe, UINT split_x)
{
    const UINT pic_w = pipe.stretched ? pipe.screen_w : pipe.work_w;
    const UINT pic_h = pipe.stretched ? pipe.screen_h : pipe.work_h;
    if (split_x == 0 || pic_w == 0 || pic_h == 0) return;
    if (split_x > pic_w) split_x = pic_w;
    D3D12_RESOURCE_BARRIER barriers_in[] = {
        Barrier(pipe.picture.gpu_tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COPY_SOURCE),
        Barrier(pipe.result, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                   D3D12_RESOURCE_STATE_COPY_DEST),
    };
    lfCore.cmd->ResourceBarrier(_countof(barriers_in), barriers_in);
    D3D12_TEXTURE_COPY_LOCATION from = {}, into = {};
    from.pResource = pipe.picture.gpu_tex;
    from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; from.SubresourceIndex = 0;
    into.pResource = pipe.result;
    into.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; into.SubresourceIndex = 0;
    D3D12_BOX box = { 0, 0, 0, split_x, pic_h, 1 };
    lfCore.cmd->CopyTextureRegion(&into, 0, 0, 0, &from, &box);
    D3D12_RESOURCE_BARRIER barriers_out[] = {
        Barrier(pipe.picture.gpu_tex, D3D12_RESOURCE_STATE_COPY_SOURCE,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Barrier(pipe.result, D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
    };
    lfCore.cmd->ResourceBarrier(_countof(barriers_out), barriers_out);

    if (split_x < pic_w && CompareTargetReady(pipe.result))
    {
        const UINT lw = (pic_h >= 1400u) ? 3u : 2u;
        LONG left = static_cast<LONG>(split_x) - static_cast<LONG>(lw / 2);
        if (left < 0) left = 0;
        LONG right = left + static_cast<LONG>(lw);
        if (right > static_cast<LONG>(pic_w)) { right = static_cast<LONG>(pic_w); left = right - static_cast<LONG>(lw); }

        const FLOAT accent[4] = { 118.0f / 255.0f, 185.0f / 255.0f, 0.0f / 255.0f, 1.0f };
        const D3D12_RECT rect = { left, 0, right, static_cast<LONG>(pic_h) };
        ID3D12DescriptorHeap *heap_list[] = { lfCompare.heap_gpu };
        lfCore.cmd->SetDescriptorHeaps(1, heap_list);
        lfCore.cmd->ClearUnorderedAccessViewFloat(
            lfCompare.heap_gpu->GetGPUDescriptorHandleForHeapStart(),
            lfCompare.heap_cpu->GetCPUDescriptorHandleForHeapStart(),
            pipe.result, accent, 1, &rect);
    }
}

// One network pass on the picture, with the passes around it.
static bool PipelineRun(Pipeline &pipe, int reset, UINT64 *sent_mark)
{
#ifndef LF_RADEON
    if (!EnsureMultiPass(pipe)) return false;
#endif
    if (sent_mark) *sent_mark = 0;
    if (reset)
    {
        // The network's history thrown away: the first ten, then every hundredth.
        static unsigned resets = 0;
        if (++resets <= 10 || resets % 100 == 0) Trace("[net] history starts over (%u so far)", resets);
    }
    if (!CmdOpen()) return false;
    const bool ts = GpuStageOpen(GS_NETWORK);
    const UINT pic_w = pipe.stretched ? pipe.screen_w : pipe.work_w;
    const UINT pic_h = pipe.stretched ? pipe.screen_h : pipe.work_h;

    const UINT nw = pipe.net_small ? pipe.net_w : pic_w;
    const UINT nh = pipe.net_small ? pipe.net_h : pic_h;
    if (pipe.net_small)
    {
        D3D12_RESOURCE_BARRIER to_uav = Barrier(
            pipe.net_in, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        lfCore.cmd->ResourceBarrier(1, &to_uav);
        ResizePicture(pipe.picture.gpu_tex, pic_w, pic_h, pipe.net_in, nw, nh, 0);
        D3D12_RESOURCE_BARRIER to_srv = Barrier(
            pipe.net_in, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        lfCore.cmd->ResourceBarrier(1, &to_srv);
    }

    if (ts) lfCore.cmd->EndQuery(lfProf.query_heap, D3D12_QUERY_TYPE_TIMESTAMP, lfCore.ring_slot * 4 + 2);
    ID3D12Resource *nr_color = pipe.net_small ? pipe.net_in : pipe.picture.gpu_tex;

    // The Denoiser slider: its rim-removal share (halo_reduction.inl).
    const float halo = CurrentHalo() * kHaloShare;
    const bool halo_full = !pipe.net_small && halo > 0.0f && EnsureHaloPipeline() &&
                           EnsureHaloTargets(pic_w, pic_h, true);
    ID3D12Resource *nr_result = pipe.net_small ? pipe.net_out : (halo_full ? g_halo_t.raw : pipe.result);
#ifdef LF_RADEON
    DWORD blob = 0;
    NVSDK_NGX_Result outcome_code = RadeonEvaluate(nr_color, nr_result, nw, nh, pipe, reset)
                                  ? NVSDK_NGX_Result_Success : NVSDK_NGX_Result_Fail;
#else
    lfCore.ngx_params->Reset();
    lfCore.ngx_params->Set("DLSSNR.Color", nr_color); lfCore.ngx_params->Set("DLSSNR.Output", nr_result);
    lfCore.ngx_params->Set("DLSSNR.MVec", pipe.motion.gpu_tex);
    lfCore.ngx_params->Set("DLSSNR.ColorSubrectBaseX", 0u); lfCore.ngx_params->Set("DLSSNR.ColorSubrectBaseY", 0u);
    lfCore.ngx_params->Set("DLSSNR.ColorSubrectWidth", nw); lfCore.ngx_params->Set("DLSSNR.ColorSubrectHeight", nh);
    lfCore.ngx_params->Set("DLSSNR.MVecSubrectBaseX", 0u); lfCore.ngx_params->Set("DLSSNR.MVecSubrectBaseY", 0u);
    lfCore.ngx_params->Set("DLSSNR.MVecSubrectWidth", pipe.work_w); lfCore.ngx_params->Set("DLSSNR.MVecSubrectHeight", pipe.work_h);
    lfCore.ngx_params->Set("DLSSNR.OutputSubrectBaseX", 0u); lfCore.ngx_params->Set("DLSSNR.OutputSubrectBaseY", 0u);
    lfCore.ngx_params->Set("DLSSNR.OutputSubrectWidth", nw); lfCore.ngx_params->Set("DLSSNR.OutputSubrectHeight", nh);
    lfCore.ngx_params->Set("DLSSNR.MVecScaleX", pipe.net_small ? float(nw)/pipe.work_w : 1.0f);
    lfCore.ngx_params->Set("DLSSNR.MVecScaleY", pipe.net_small ? float(nh)/pipe.work_h : 1.0f);
    bool verified = true;
    verified &= ParamSetU(lfCore.ngx_params, "DLSSNR.Enabled", 1u);
    verified &= ParamSetU(lfCore.ngx_params, "DLSSNR.Reset", (unsigned int)reset);
    verified &= ParamSetF(lfCore.ngx_params, "DLSSNR.Intensity", NetworkIntensity());
    verified &= ParamSetF(lfCore.ngx_params, "DLSSNR.LocalToneStrength", lfLink.hello.local_tone);
    verified &= ParamSetF(lfCore.ngx_params, "DLSSNR.LocalStructureStrength", lfLink.hello.local_structure);
    verified &= ParamSetF(lfCore.ngx_params, "DLSSNR.SkinStructureStrength", lfLink.hello.skin_structure);
    verified &= ParamSetU(lfCore.ngx_params, "DLSSNR.UseAutoMask", lfLink.hello.auto_mask);
    verified &= ParamSetU(lfCore.ngx_params, "DLSSNR.Style", lfLink.hello.style);
    verified &= ParamSetU(lfCore.ngx_params, "DLSSNR.UICorrection", lfLink.hello.ui_correction);
    if (!verified)
        Trace("[card] an NGX setting read back differently - it did not take");
    if (NrExposureGiven())
    {
        lfCore.ngx_params->Set("DLSS.Pre.Exposure", 1.0f);
        lfCore.ngx_params->Set("DLSS.Exposure.Scale", lfExposure.gain);
    }
    DWORD blob = 0;
    NVSDK_NGX_Result outcome_code = static_cast<NVSDK_NGX_Result>(0x7FFFFFFF);
    __try { outcome_code = lfNr.evaluate(lfCore.cmd, lfCore.ngx_feature, lfCore.ngx_params, nullptr); }
    __except (EXCEPTION_EXECUTE_HANDLER) { blob = GetExceptionCode(); }
    if (blob == 0 && !NVSDK_NGX_FAILED(outcome_code) && g_pass_count > 1)
        outcome_code = EvaluateExtraLayers(nr_result, reset);
#endif
    lfNr.last_code = static_cast<uint32_t>(outcome_code);
    if (blob != 0) { DropCommands(); Trace("[net] a network run threw 0x%08X", blob); return false; }
    if (ts) lfCore.cmd->EndQuery(lfProf.query_heap, D3D12_QUERY_TYPE_TIMESTAMP, lfCore.ring_slot * 4 + 3);
    if (pipe.net_small)
    {
        D3D12_RESOURCE_BARRIER to_srv = Barrier(
            pipe.net_out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        lfCore.cmd->ResourceBarrier(1, &to_srv);
        const bool halo_small = pipe.add_detail && halo > 0.0f && EnsureHaloPipeline() &&
                                EnsureHaloTargets(nw, nh, false);
        if (halo_small)
        {
            HaloFilter(pipe.net_in, pipe.net_out, nw, nh, halo, pipe.motion.gpu_tex, pipe.work_w, pipe.work_h);
            DetailCompose(pipe.picture.gpu_tex, pipe.net_in, pipe.net_out, pic_w, pic_h, pipe.result,
                            pipe.detail_gain, g_halo_t.dc, halo);
            HaloDone();
        }
        else if (pipe.add_detail)
            DetailCompose(pipe.picture.gpu_tex, pipe.net_in, pipe.net_out, pic_w, pic_h, pipe.result,
                            pipe.detail_gain);
        else
            ResizePicture(pipe.net_out, nw, nh, pipe.result, pic_w, pic_h, 1);
        D3D12_RESOURCE_BARRIER back = Barrier(
            pipe.net_out, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        lfCore.cmd->ResourceBarrier(1, &back);
    }
    else if (halo_full)
    {
        D3D12_RESOURCE_BARRIER to_srv = Barrier(
            g_halo_t.raw, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        lfCore.cmd->ResourceBarrier(1, &to_srv);
        HaloFilter(pipe.picture.gpu_tex, g_halo_t.raw, pic_w, pic_h, halo, pipe.motion.gpu_tex, pipe.work_w, pipe.work_h);
        DetailCompose(pipe.picture.gpu_tex, pipe.picture.gpu_tex, g_halo_t.raw, pic_w, pic_h, pipe.result,
                        1.0f, g_halo_t.dc, halo);
        HaloDone();
        D3D12_RESOURCE_BARRIER back = Barrier(
            g_halo_t.raw, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        lfCore.cmd->ResourceBarrier(1, &back);
    }

    // And its noise-cleaning half (denoise.inl), on the finished composite
    // and before the sharpening, so what gets sharpened is already clean.
    const float denoise = CurrentHalo();
    if (denoise > 0.0f && EnsureDenoisePipeline() && EnsureDenoiseTargets(pic_w, pic_h))
        DenoiseOutput(pipe.result, pic_w, pic_h, denoise, pipe.motion.gpu_tex, pipe.work_w, pipe.work_h, reset != 0);
    else
        DenoiseSkipped();

    const float sharp = SharpnessNow(), edge = EdgesNow();
    if ((sharp > 0.0f || edge > 0.0f || ColorsActive()) && EnsureFinishPipeline() &&
        EnsureFinishTargets(pic_w, pic_h))
        FinishOutput(pipe.result, pic_w, pic_h, sharp, edge, pipe.motion.gpu_tex, pipe.work_w, pipe.work_h, reset != 0);
    else
        FinishSkipped();

    if (lfOut.compare_x != 0)
    {
        CompareRecord(pipe, lfOut.compare_x);
        lfOut.compare_x = 0;
    }
    if (ts) GpuStageClose(GS_NETWORK, 4);
    const UINT64 fence = CmdSubmit();
    if (sent_mark) *sent_mark = fence;
    if (fence == 0) return false;
    if (NVSDK_NGX_FAILED(outcome_code)) { Trace("[net] a network run failed 0x%08X (%s)", outcome_code, NgxCodeText(outcome_code)); return false; }
    if (sent_mark) return true;
    if (!GpuStageWait(GS_NETWORK, fence, 60000)) return false;
    ++lfNr.evaluations;
    return true;
}

// Copy the result into memory for the engine.
static bool PipelineReadback(Pipeline &pipe, std::vector<BYTE> &tight,
                               ID3D12Resource *from,
                               D3D12_RESOURCE_STATES src_before,
                               D3D12_RESOURCE_STATES src_after)
{
    if (from == nullptr) from = pipe.result;
    if (!CmdOpen()) return false;
    D3D12_RESOURCE_BARRIER a = Barrier(from, src_before,
                                           D3D12_RESOURCE_STATE_COPY_SOURCE);
    lfCore.cmd->ResourceBarrier(1, &a);
    // Flow Super Resolution: the picture scaled to the display first.
    ID3D12Resource *copy_from = pipe.super_res
        ? SuperResExport(lfCore.cmd, from, pipe.display_w, pipe.display_h, false) : from;
    if (copy_from == nullptr) { DropCommands(); return false; }
    D3D12_TEXTURE_COPY_LOCATION s = {}, d = {};
    s.pResource = copy_from; s.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    d.pResource = pipe.result_cpu; d.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    d.PlacedFootprint = pipe.result_layout;
    lfCore.cmd->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    if (pipe.super_res) SuperResExportDone(lfCore.cmd, false);
    D3D12_RESOURCE_BARRIER b = Barrier(from, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                           src_after);
    lfCore.cmd->ResourceBarrier(1, &b);
    const UINT64 fence = CmdSubmit();
    if (!AwaitMark(lfCore.timeline, fence, 60000)) return false;

    BYTE *view_bytes = nullptr;

    const UINT rb_rows = pipe.result_rows != 0 ? pipe.result_rows : pipe.work_h;
    const SIZE_T rb_bytes =
        static_cast<SIZE_T>(pipe.result_layout.Footprint.RowPitch) * (rb_rows - 1) +
        static_cast<SIZE_T>(pipe.result_row_bytes);
    D3D12_RANGE read_range = { 0, rb_bytes };
    if (FAILED(pipe.result_cpu->Map(0, &read_range, reinterpret_cast<void **>(&view_bytes)))) return false;
    const UINT ow = pipe.super_res ? pipe.display_w : (pipe.stretched ? pipe.screen_w : pipe.work_w);
    const UINT oh = pipe.super_res ? pipe.display_h : (pipe.stretched ? pipe.screen_h : pipe.work_h);
    tight.resize(static_cast<size_t>(ow) * oh * 4);
    for (UINT y = 0; y < oh; ++y)
        memcpy(tight.data() + static_cast<size_t>(y) * ow * 4,
               view_bytes + static_cast<size_t>(y) * pipe.result_layout.Footprint.RowPitch,
               static_cast<size_t>(ow) * 4);
    D3D12_RANGE written = { 0, 0 };
    pipe.result_cpu->Unmap(0, &written);
    return true;
}
