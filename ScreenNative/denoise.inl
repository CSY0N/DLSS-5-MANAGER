// The noise cleaner - the other half of the LIVE FLOW slider called Denoiser
// (the first half is the rim removal in halo_reduction.inl, which the same
// number drives).
//
// Round 23: the first version (round 22) pulled last frame's cleaned picture
// in through the motion vectors and fed its own output back as history. Two
// faults came of it on real screens: the vectors are noisy on flat and still
// places, so the history arrived a pixel or two off - the whole picture
// wavered like air over a hot road - and the feedback kept that error and the
// clipping's own flicker going frame after frame ("strange noise"). This one
// can do neither:
//
//   * No motion vectors. A pixel is averaged with the SAME pixel of the two
//     frames before it, and only as far as that pixel did not change there -
//     judged on its 3x3 neighbourhood against the noise level the slider
//     sets. Noise changes every frame and a still surface does not, so still
//     places lose their grain without being moved or softened; anything that
//     moved or changed simply keeps its own pixel. Nothing can be dragged.
//   * No feedback. The two earlier frames are the frames as they came in, not
//     what this pass made of them, so nothing it does is carried on.
//   * Then an edge-aware 5x5 filter on that average, over pixels that look
//     like this one (within 2.5x the noise level): it takes the grain out of
//     what moved, where the frames before could not help, and does less where
//     they already did. Edges and texture above the noise level stay.
//
// The slider is the noise level: at 5% (the default) about 2 of 255 levels -
// barely there - and at 100% about 16.
//
// One pass on the finished composite, before the sharpening and the colours
// (post_finish.inl), so the sharpening never digs grain back out. v.output is
// copied into a ring of three frames (this one and the two before) and the
// pass writes the output. At 0 it is not recorded at all.
//
// Cost: one pass at the output size; each 8x8 group loads a 14x14
// neighbourhood of the three frames into shared memory once.

static const char kDenoiseHlsl[] = R"hlsl(
Texture2D<float4>   gCur   : register(t0);   // this frame's composite, copied aside
Texture2D<float4>   gPrev1 : register(t1);   // the frame before, as it came in
Texture2D<float4>   gPrev2 : register(t2);   // the one before that
RWTexture2D<float4> gDst   : register(u0);   // the output
cbuffer PC : register(b0) { uint gW; uint gH; float gAmount; uint gFlags; };
// gFlags: 1 = gPrev1 holds the frame before, at this size; 2 = gPrev2 too.
groupshared float3 gC[14][14];
groupshared float3 gP1[14][14];
groupshared float3 gP2[14][14];
groupshared float3 gT[12][12];      // the average over time, for the 5x5 filter

float Gap(float3 a, float3 b)
{
    float3 d = abs(a - b);
    return (d.r + d.g + d.b) * (1.0f / 3.0f);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint3 id : SV_DispatchThreadID)
{
    int2 last = int2(int(gW) - 1, int(gH) - 1);
    int2 origin = int2(gid.xy) * 8 - 3;
    uint self = tid.y * 8 + tid.x;
    bool have1 = (gFlags & 1u) != 0u, have2 = (gFlags & 2u) != 0u;
    for (uint i = self; i < 196; i += 64)
    {
        int2 q = clamp(origin + int2(i % 14, i / 14), int2(0, 0), last);
        float3 c = gCur[q].rgb;
        gC[i / 14][i % 14] = c;
        gP1[i / 14][i % 14] = have1 ? gPrev1[q].rgb : c;
        gP2[i / 14][i % 14] = have2 ? gPrev2[q].rgb : c;
    }
    GroupMemoryBarrierWithGroupSync();

    // The noise level the slider asks for, in 0..1 colour units.
    float sigma = 0.004f + 0.06f * gAmount;
    float inv = 1.0f / (1.2f * sigma);

    // Over time, for the inner 12x12: each earlier frame counts as far as the
    // pixel's 3x3 did not change there.
    for (uint j = self; j < 144; j += 64)
    {
        int ty = int(j / 12) + 1, tx = int(j % 12) + 1;
        float d1 = 0.0f, d2 = 0.0f;
        [unroll] for (int y = -1; y <= 1; ++y)
        [unroll] for (int x = -1; x <= 1; ++x)
        {
            float3 c = gC[ty + y][tx + x];
            d1 += Gap(gP1[ty + y][tx + x], c);
            d2 += Gap(gP2[ty + y][tx + x], c);
        }
        d1 *= (1.0f / 9.0f);
        d2 *= (1.0f / 9.0f);
        float w1 = have1 ? exp(-(d1 * inv) * (d1 * inv)) : 0.0f;
        // The frame before that only where the frame before already agreed.
        float w2 = have2 ? exp(-(d2 * inv) * (d2 * inv)) * w1 : 0.0f;
        float3 c0 = gC[ty][tx];
        gT[ty - 1][tx - 1] = (c0 + w1 * gP1[ty][tx] + w2 * gP2[ty][tx]) / (1.0f + w1 + w2);
    }
    GroupMemoryBarrierWithGroupSync();
    if (id.x >= gW || id.y >= gH) return;

    int2 t = int2(tid.xy) + 2;           // this pixel in gT
    float3 avg = gT[t.y][t.x];
    float3 cur = gC[t.y + 1][t.x + 1];
    // How much the time average already did here: 0 moved, 1 still.
    float still = saturate(1.0f - Gap(avg, cur) / max(sigma, 1e-4f));

    // In space: the 5x5 over pixels that look like this one.
    float range = 2.5f * sigma;
    float invRange = 1.0f / (2.0f * range * range);
    const float invSpace = 1.0f / (2.0f * 1.3f * 1.3f);
    float3 acc = 0.0f;
    float wsum = 0.0f;
    [unroll] for (int v = -2; v <= 2; ++v)
    [unroll] for (int u = -2; u <= 2; ++u)
    {
        float3 s = gT[t.y + v][t.x + u];
        float3 d = s - avg;
        float w = exp(-float(u * u + v * v) * invSpace - dot(d, d) * invRange);
        acc += s * w;
        wsum += w;
    }
    float3 spatial = acc / max(wsum, 1e-6f);
    float mix = saturate(gAmount * 2.0f) * lerp(1.0f, 0.5f, still);
    gDst[id.xy] = float4(lerp(avg, spatial, mix), 1.0f);
}
)hlsl";

static ID3D12RootSignature  *g_dn_rs;
static ID3D12PipelineState  *g_dn_pso;
static ID3D12DescriptorHeap *g_dn_heap;          // three sets of 3 SRV + 1 UAV, one per ring position
static ID3D12Resource       *g_dn_bound[3][4];
static ID3D12Resource       *g_dn_ring[3];      // the inputs, NPSR at rest
static UINT                  g_dn_w, g_dn_h;
static UINT                  g_dn_head;         // where this frame's input goes
static UINT                  g_dn_frames;       // how many of the ring hold frames of this run (0..3)
static bool                  g_dn_failed;
static bool                  g_dn_logged;

static void CloseDenoiseTargets()
{
    for (auto &r : g_dn_ring)
        if (r != nullptr) { r->Release(); r = nullptr; }
    g_dn_w = g_dn_h = 0;
    g_dn_head = 0;
    g_dn_frames = 0;
    for (auto &set : g_dn_bound)
        for (auto &slot : set) slot = nullptr;
}

// A frame that went out without the pass. The ring keeps what it has: a
// frame from before the gap counts only where the pixel did not change.
static void DenoiseSkipped()
{
}

static bool EnsureDenoisePipeline()
{
    if (g_dn_pso != nullptr) return true;
    if (g_dn_failed) return false;
    g_dn_failed = true;
    HMODULE compiler = LoadLibraryW(L"d3dcompiler_47.dll");
    auto compile = compiler ? static_cast<CompileFn>(&CachedCompile) : nullptr;
    HMODULE d3d12 = GetModuleHandleW(L"d3d12.dll");
    auto serialize = d3d12 ? reinterpret_cast<SerializeRootFn>(
                                 GetProcAddress(d3d12, "D3D12SerializeRootSignature")) : nullptr;
    if (compile == nullptr || serialize == nullptr) { Trace("[denoise] compiler unavailable"); return false; }
    ID3DBlob *code = nullptr, *errors = nullptr;
    HRESULT hr = compile(kDenoiseHlsl, sizeof(kDenoiseHlsl) - 1, "denoise.hlsl", nullptr, nullptr,
                         "CSMain", "cs_5_0", 0, 0, &code, &errors);
    if (FAILED(hr) || code == nullptr)
    {
        Trace("[denoise] shader failed 0x%08X: %s", hr,
            errors ? static_cast<const char *>(errors->GetBufferPointer()) : "(no log)");
        if (errors) errors->Release();
        return false;
    }
    if (errors) { errors->Release(); errors = nullptr; }
    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 3;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 3;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.Num32BitValues = 4;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 2;
    params[1].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = _countof(params);
    rsd.pParameters = params;
    ID3DBlob *rs_blob = nullptr;
    hr = serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs_blob, &errors);
    if (errors) errors->Release();
    if (FAILED(hr) || rs_blob == nullptr) { Trace("[denoise] root signature failed 0x%08X", hr); code->Release(); return false; }
    hr = lfCore.device->CreateRootSignature(0, rs_blob->GetBufferPointer(), rs_blob->GetBufferSize(),
                                    __uuidof(ID3D12RootSignature), reinterpret_cast<void **>(&g_dn_rs));
    rs_blob->Release();
    if (FAILED(hr)) { code->Release(); return false; }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = g_dn_rs;
    pd.CS.pShaderBytecode = code->GetBufferPointer();
    pd.CS.BytecodeLength = code->GetBufferSize();
    hr = lfCore.device->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), reinterpret_cast<void **>(&g_dn_pso));
    code->Release();
    if (FAILED(hr)) { Trace("[denoise] pipeline failed 0x%08X", hr); return false; }
    D3D12_DESCRIPTOR_HEAP_DESC hd = {};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 12;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(lfCore.device->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void **>(&g_dn_heap))))
        return false;
    g_dn_failed = false;
    Trace("[denoise] pipeline ready (still pixels over three frames, no motion vectors, edge-aware 5x5)");
    return true;
}

static bool EnsureDenoiseTargets(UINT w, UINT hgt)
{
    if (g_dn_ring[0] != nullptr && g_dn_ring[1] != nullptr && g_dn_ring[2] != nullptr &&
        g_dn_w == w && g_dn_h == hgt)
        return true;
    CloseDenoiseTargets();
    D3D12_HEAP_PROPERTIES def = {};
    def.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td = {};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w; td.Height = hgt; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    for (auto &r : g_dn_ring)
        if (FAILED(lfCore.device->CreateCommittedResource(&def, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr, __uuidof(ID3D12Resource),
                reinterpret_cast<void **>(&r))))
        { Trace("[denoise] %ux%u targets failed", w, hgt); CloseDenoiseTargets(); return false; }
    g_dn_w = w;
    g_dn_h = hgt;
    return true;
}

// Set `set`: this frame's input at ring[set], the two before it, the output.
static void BindDenoiseSet(UINT set, ID3D12Resource *output)
{
    ID3D12Resource *want[4] = { g_dn_ring[set], g_dn_ring[(set + 2) % 3], g_dn_ring[(set + 1) % 3], output };
    if (memcmp(g_dn_bound[set], want, sizeof(want)) == 0) return;
    const UINT stride = lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_dn_heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(set) * 4 * stride;
    D3D12_SHADER_RESOURCE_VIEW_DESC sd = {};
    sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sd.Texture2D.MipLevels = 1;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    for (int i = 0; i < 3; ++i)
    {
        lfCore.device->CreateShaderResourceView(want[i], &sd, cpu);
        cpu.ptr += stride;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
    ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    lfCore.device->CreateUnorderedAccessView(output, nullptr, &ud, cpu);
    memcpy(g_dn_bound[set], want, sizeof(want));
}

// Records the pass into the open evaluate list. output is in
// UNORDERED_ACCESS on entry and on exit; reset says the frames before belong
// to another scene. (The motion arguments are kept for the call site; this
// pass does not use motion.)
static void DenoiseOutput(ID3D12Resource *output, UINT w, UINT hgt, float amount,
                          ID3D12Resource *, UINT, UINT, bool reset)
{
    if (!g_dn_logged)
    {
        g_dn_logged = true;
        Trace("[denoise] cleaning at %.2f (%ux%u)", amount, w, hgt);
    }
    if (reset) g_dn_frames = 0;
    const UINT set = g_dn_head;
    ID3D12Resource *slot = g_dn_ring[set];
    D3D12_RESOURCE_BARRIER pre[] = {
        Barrier(output, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
        Barrier(slot, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST),
    };
    lfCore.cmd->ResourceBarrier(_countof(pre), pre);
    lfCore.cmd->CopyResource(slot, output);
    D3D12_RESOURCE_BARRIER mid[] = {
        Barrier(output, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS),
        Barrier(slot, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
    };
    lfCore.cmd->ResourceBarrier(_countof(mid), mid);
    BindDenoiseSet(set, output);
    ID3D12DescriptorHeap *heaps[] = { g_dn_heap };
    lfCore.cmd->SetDescriptorHeaps(1, heaps);
    lfCore.cmd->SetComputeRootSignature(g_dn_rs);
    lfCore.cmd->SetPipelineState(g_dn_pso);
    const UINT flags = (g_dn_frames >= 1 ? 1u : 0u) | (g_dn_frames >= 2 ? 2u : 0u);
    UINT32 c[4] = { w, hgt, 0, flags };
    memcpy(&c[2], &amount, sizeof(float));
    lfCore.cmd->SetComputeRoot32BitConstants(0, 4, c, 0);
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = g_dn_heap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(set) * 4 *
               lfCore.device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    lfCore.cmd->SetComputeRootDescriptorTable(1, gpu);
    lfCore.cmd->Dispatch((w + 7) / 8, (hgt + 7) / 8, 1);
    g_dn_head = (set + 1) % 3;
    if (g_dn_frames < 2) ++g_dn_frames;
}
