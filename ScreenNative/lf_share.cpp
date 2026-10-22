// lf_share.cpp - optional Spout output (LF_SPOUT=1): the finished picture is
// copied into a D3D11 texture that OBS and other Spout receivers can read.
#include "lf_share.h"
#include "spout/SpoutDX.h"

#include <dxgi1_2.h>

#include <cstdio>
#include <cstdlib>

static ID3D11Device *d11 = nullptr;
static ID3D11DeviceContext *d11_ctx = nullptr;
static ID3D11Texture2D *tex11 = nullptr;
static HANDLE nt_handle = nullptr;
static ID3D12Resource *tex12 = nullptr;
static ID3D12Device *dev12 = nullptr;
static spoutDX *sender = nullptr;
static bool sending = false;
bool ShareActive() { return sending; }
static UINT size_w = 0, size_h = 0;

bool ShareStart(ID3D12Device *dev)
{
    if (sending) return true;
    char v[8] = {};
    const DWORD len = GetEnvironmentVariableA("LF_SPOUT", v, sizeof(v));
    if (len == 0 || len >= sizeof(v) || v[0] != '1')
        return false;

    dev12 = dev;

    const LUID wanted = dev->GetAdapterLuid();
    IDXGIFactory1 *dxgi_factory = nullptr;
    IDXGIAdapter1 *card = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),
                                     reinterpret_cast<void **>(&dxgi_factory))))
    {
        IDXGIAdapter1 *option = nullptr;
        for (UINT i = 0; dxgi_factory->EnumAdapters1(i, &option) != DXGI_ERROR_NOT_FOUND; ++i)
        {
            DXGI_ADAPTER_DESC1 ad = {};
            if (option == nullptr) continue;
            if (SUCCEEDED(option->GetDesc1(&ad)) &&
                ad.AdapterLuid.LowPart == wanted.LowPart &&
                ad.AdapterLuid.HighPart == wanted.HighPart)
            { card = option; break; }
            option->Release();
            option = nullptr;
        }
    }
    if (card == nullptr)
        fprintf(stderr, "[share] the worker's card was not found by LUID - "
                        "falling back to the default one\n");
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;

    HRESULT status = D3D11CreateDevice(card,
                                   card != nullptr ? D3D_DRIVER_TYPE_UNKNOWN
                                                      : D3D_DRIVER_TYPE_HARDWARE,
                                   nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, &fl, 1,
                                   D3D11_SDK_VERSION, &d11, nullptr, &d11_ctx);
    if (card != nullptr) card->Release();
    if (dxgi_factory != nullptr) dxgi_factory->Release();
    if (FAILED(status)) { fprintf(stderr, "[spout] D3D11 device failed 0x%08X\n", (unsigned)status); return false; }
    {
        IDXGIDevice *dxgi_dev = nullptr;
        IDXGIAdapter *len = nullptr;
        DXGI_ADAPTER_DESC gd = {};
        if (SUCCEEDED(d11->QueryInterface(__uuidof(IDXGIDevice),
                                            reinterpret_cast<void **>(&dxgi_dev))) &&
            SUCCEEDED(dxgi_dev->GetAdapter(&len)) && SUCCEEDED(len->GetDesc(&gd)))
            fprintf(stderr, "[share] D3D11 on %ls (luid %08lX:%08lX; the worker is on "
                            "%08lX:%08lX)\n", gd.Description,
                    (unsigned long)gd.AdapterLuid.HighPart, (unsigned long)gd.AdapterLuid.LowPart,
                    (unsigned long)wanted.HighPart, (unsigned long)wanted.LowPart);
        if (len != nullptr) len->Release();
        if (dxgi_dev != nullptr) dxgi_dev->Release();
    }

    sender = new spoutDX();
    if (!sender->OpenDirectX11(d11))
    { fprintf(stderr, "[spout] OpenDirectX11 failed\n"); ShareStop(); return false; }
    sender->SetSenderName("DLSS 5 MANAGER Screen");
    sender->SetSenderFormat(DXGI_FORMAT_R8G8B8A8_UNORM);

    sending = true;
    fprintf(stderr, "[spout] bridge enabled (LF_SPOUT=1)\n");
    return true;
}

void ShareCopy(ID3D12GraphicsCommandList *cmdl, ID3D12Resource *from,
                     UINT w, UINT h)
{
    if (!sending || from == nullptr) return;
    if (from->GetDesc().Format != DXGI_FORMAT_R8G8B8A8_UNORM) return;
    if (tex11 == nullptr || w != size_w || h != size_h)
    {
        if (tex11) { tex11->Release(); tex11 = nullptr; }
        if (tex12) { tex12->Release(); tex12 = nullptr; }
        if (nt_handle) { CloseHandle(nt_handle); nt_handle = nullptr; }

        D3D11_TEXTURE2D_DESC view_desc = {};
        view_desc.Width = w; view_desc.Height = h; view_desc.MipLevels = 1; view_desc.ArraySize = 1;
        view_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; view_desc.SampleDesc.Count = 1;
        view_desc.Usage = D3D11_USAGE_DEFAULT;
        view_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET;
        view_desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
        if (FAILED(d11->CreateTexture2D(&view_desc, nullptr, &tex11)))
        { fprintf(stderr, "[spout] shared texture failed\n"); return; }
        IDXGIResource1 *r1 = nullptr;
        if (FAILED(tex11->QueryInterface(__uuidof(IDXGIResource1), (void **)&r1)) ||
            FAILED(r1->CreateSharedHandle(nullptr,
                                          DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                          nullptr, &nt_handle)))
        { fprintf(stderr, "[spout] NT handle failed\n"); if (r1) r1->Release(); return; }
        r1->Release();

        if (FAILED(dev12->OpenSharedHandle(nt_handle, __uuidof(ID3D12Resource),
                                             (void **)&tex12)))
        { fprintf(stderr, "[spout] OpenSharedHandle failed\n"); return; }
        size_w = w; size_h = h;
        fprintf(stderr, "[spout] shared texture %ux%u created\n", w, h);
    }
    if (tex12 == nullptr) { fprintf(stderr, "[spout] g_d12 null - skipping copy\n"); return; }

    D3D12_RESOURCE_BARRIER barriers_in = {};
    barriers_in.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers_in.Transition.pResource = tex12;
    barriers_in.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    barriers_in.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
    cmdl->ResourceBarrier(1, &barriers_in);
    cmdl->CopyResource(tex12, from);
    D3D12_RESOURCE_BARRIER barriers_out = {};
    barriers_out.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barriers_out.Transition.pResource = tex12;
    barriers_out.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    barriers_out.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
    cmdl->ResourceBarrier(1, &barriers_out);
}

void ShareSend()
{
    if (!sending || tex11 == nullptr) return;
    if (!sender->SendTexture(tex11))
        fprintf(stderr, "[spout] SendTexture failed\n");
}

void ShareStop()
{
    if (sender) { sender->ReleaseSender(); delete sender; sender = nullptr; }
    if (tex11) { tex11->Release(); tex11 = nullptr; }
    if (tex12) { tex12->Release(); tex12 = nullptr; }
    if (nt_handle) { CloseHandle(nt_handle); nt_handle = nullptr; }
    if (d11_ctx) { d11_ctx->Release(); d11_ctx = nullptr; }
    if (d11) { d11->Release(); d11 = nullptr; }
    sending = false;
}
