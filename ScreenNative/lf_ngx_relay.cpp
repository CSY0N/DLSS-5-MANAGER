// lf_ngx_relay.cpp - the optional relay DLL (LF_FORWARDER=1): loads the
// network runtime from its own module and passes the calls through, for
// setups where the runtime must not see nvngx.dll as its caller.
#include <windows.h>
#include <d3d12.h>

#include "nvsdk_ngx.h"

using NrInitFn = NVSDK_NGX_Result(NVSDK_CONV *)(unsigned long long, const wchar_t *,
                                                      ID3D12Device *, NVSDK_NGX_Version,
                                                      const NVSDK_NGX_Parameter *);
using NrCreateFn = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D12GraphicsCommandList *, NVSDK_NGX_Feature,
                                                     const NVSDK_NGX_Parameter *, NVSDK_NGX_Handle **);
using NrEvaluateFn = NVSDK_NGX_Result(NVSDK_CONV *)(ID3D12GraphicsCommandList *, const NVSDK_NGX_Handle *,
                                                       const NVSDK_NGX_Parameter *, PFN_NVSDK_NGX_ProgressCallback);
using NrReleaseFn = NVSDK_NGX_Result(NVSDK_CONV *)(NVSDK_NGX_Handle *);

static HMODULE relay_module;
static NrInitFn relay_init;
static NrCreateFn relay_create;
static NrEvaluateFn relay_evaluate;
static NrReleaseFn relay_release;

static volatile LONG relay_last;
#define RELAY_CALL(expr) do { const NVSDK_NGX_Result r_ = (expr); \
                           InterlockedExchange(&relay_last, static_cast<LONG>(r_)); \
                           return r_; } while (0)

extern "C" __declspec(dllexport)
int LfRelayLoad(const wchar_t *dlssnr_path)
{
    if (relay_module != nullptr) return 0;
    relay_module = LoadLibraryW(dlssnr_path);
    if (relay_module == nullptr) { const DWORD e = GetLastError(); return e != 0 ? static_cast<int>(e) : -2; }
    relay_init = reinterpret_cast<NrInitFn>(GetProcAddress(relay_module, "NVSDK_NGX_D3D12_Init_Ext"));
    relay_create = reinterpret_cast<NrCreateFn>(GetProcAddress(relay_module, "NVSDK_NGX_D3D12_CreateFeature"));
    relay_evaluate = reinterpret_cast<NrEvaluateFn>(GetProcAddress(relay_module, "NVSDK_NGX_D3D12_EvaluateFeature"));
    relay_release = reinterpret_cast<NrReleaseFn>(GetProcAddress(relay_module, "NVSDK_NGX_D3D12_ReleaseFeature"));
    if (relay_init == nullptr || relay_create == nullptr || relay_evaluate == nullptr || relay_release == nullptr)
        return -1;
    return 0;
}

extern "C" __declspec(dllexport)
void LfRelayPath(wchar_t *out, unsigned int cch)
{
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                       | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&LfRelayPath), &self);
    if (out != nullptr && cch > 0) { out[0] = L'\0'; GetModuleFileNameW(self, out, cch); }
}

extern "C" __declspec(dllexport)
NVSDK_NGX_Result NVSDK_CONV LfRelayStart(unsigned long long app_id, const wchar_t *data_dir,
                                         ID3D12Device *dev, NVSDK_NGX_Version version,
                                         const NVSDK_NGX_Parameter *params)
{
    if (relay_init == nullptr) return NVSDK_NGX_Result_FAIL_NotInitialized;
    RELAY_CALL(relay_init(app_id, data_dir, dev, version, params));
}

extern "C" __declspec(dllexport)
NVSDK_NGX_Result NVSDK_CONV LfRelayCreate(ID3D12GraphicsCommandList *cmdl, NVSDK_NGX_Feature feature,
                                        const NVSDK_NGX_Parameter *params, NVSDK_NGX_Handle **out)
{
    if (relay_create == nullptr) return NVSDK_NGX_Result_FAIL_NotInitialized;
    RELAY_CALL(relay_create(cmdl, feature, params, out));
}

extern "C" __declspec(dllexport)
NVSDK_NGX_Result NVSDK_CONV LfRelayRun(ID3D12GraphicsCommandList *cmdl, const NVSDK_NGX_Handle *feature,
                                          const NVSDK_NGX_Parameter *params,
                                          PFN_NVSDK_NGX_ProgressCallback cb)
{
    if (relay_evaluate == nullptr) return NVSDK_NGX_Result_FAIL_NotInitialized;
    RELAY_CALL(relay_evaluate(cmdl, feature, params, cb));
}

extern "C" __declspec(dllexport)
NVSDK_NGX_Result NVSDK_CONV LfRelayRelease(NVSDK_NGX_Handle *feature)
{
    if (relay_release == nullptr) return NVSDK_NGX_Result_FAIL_NotInitialized;
    RELAY_CALL(relay_release(feature));
}

BOOL APIENTRY DllMain(HMODULE, DWORD, LPVOID) { return TRUE; }
