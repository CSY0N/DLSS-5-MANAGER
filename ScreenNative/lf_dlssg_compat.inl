// Desktop FG compatibility routing. Loaded only on the selected NVIDIA adapter,
// only when DLSS FG is requested; never installed in the game or manager process.
#include <bcrypt.h>
#pragma comment(lib, "bcrypt.lib")

static int FgComputeCapability()
{
    static int cached = -1;
    if (cached >= 0) return cached;
    cached = 0;
    DXGI_ADAPTER_DESC1 desc = {};
    if (!lfCard.adapter || FAILED(lfCard.adapter->GetDesc1(&desc)) || desc.VendorId != 0x10de) return 0;
    const HMODULE cuda = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!cuda) return 0;
    auto init = reinterpret_cast<int(__stdcall *)(unsigned)>(GetProcAddress(cuda, "cuInit"));
    auto count = reinterpret_cast<int(__stdcall *)(int *)>(GetProcAddress(cuda, "cuDeviceGetCount"));
    auto device = reinterpret_cast<int(__stdcall *)(int *, int)>(GetProcAddress(cuda, "cuDeviceGet"));
    auto luid = reinterpret_cast<int(__stdcall *)(char *, unsigned *, int)>(GetProcAddress(cuda, "cuDeviceGetLuid"));
    auto attribute = reinterpret_cast<int(__stdcall *)(int *, int, int)>(GetProcAddress(cuda, "cuDeviceGetAttribute"));
    int n = 0;
    const LUID selected = lfCore.device->GetAdapterLuid();
    if (init && count && device && luid && attribute && init(0) == 0 && count(&n) == 0) {
        for (int i = 0; i < n; ++i) {
            int dev = 0, major = 0, minor = 0; unsigned mask = 0; LUID found = {};
            if (device(&dev, i) == 0 && luid(reinterpret_cast<char *>(&found), &mask, dev) == 0 &&
                memcmp(&found, &selected, sizeof(LUID)) == 0 &&
                attribute(&major, 75, dev) == 0 && attribute(&minor, 76, dev) == 0) {
                cached = major * 10 + minor; break;
            }
        }
    }
    // CUDA may have installed process state; retain its module for the worker lifetime.
    Trace("[dlssg] selected adapter %ls, CUDA SM%d", desc.Description, cached);
    return cached;
}

static bool FgFileHash(const std::wstring &path, const char *expected)
{
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
              BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    BYTE buffer[65536], digest[32] = {}; DWORD got = 0;
    while (ok) {
        if (!ReadFile(file, buffer, sizeof(buffer), &got, nullptr)) { ok = false; break; }
        if (!got) break;
        ok = BCryptHashData(hash, buffer, got, 0) >= 0;
    }
    if (ok) ok = BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(file);
    char hex[65] = {};
    for (unsigned i = 0; i < 32; ++i) sprintf_s(hex + i * 2, 3, "%02X", digest[i]);
    return ok && _stricmp(hex, expected) == 0;
}

static bool FgWriteUtf8(const std::wstring &path, const std::wstring &text)
{
    const int length = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) return false;
    std::vector<char> bytes(length);
    WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), bytes.data(), length, nullptr, nullptr);
    HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr) && written == bytes.size();
    CloseHandle(file); return ok;
}

static bool FgPrepareCompatibility(const wchar_t *workerDirectory, bool startup = false)
{
    const int sm = FgComputeCapability();
    if (sm != 75 && sm != 86) return true; // Ada has its own retarget; Blackwell stays native.
    static HMODULE proxy = nullptr;
    if (proxy) return true;
    if (!startup) {
        Trace("[dlssg] unavailable: RTX 20/30 compatibility requires a worker restart before NGX initialization"); return false;
    }
    // Pin the reviewed upstream 0.3.5 build, including its feature-recreation fix.
    constexpr const char *sha = "C3934A09399F022504227C72DF0BF8C0DE55F9A08880DDDDE898C5262CEFA838";
    const std::wstring shipped = std::wstring(workerDirectory) + L"dlssg_compat\\version.dll";
    if (!FgFileHash(shipped, sha)) {
        Trace("[dlssg] unavailable: the RTX 20/30 compatibility package is missing or has the wrong hash"); return false;
    }
    wchar_t root[MAX_PATH] = {}; LiveFlowCachePath(root);
    const std::wstring parent = std::wstring(root) + L"\\dlssg-compat";
    const std::wstring cache = parent + (sm == 75 ? L"\\0.3.5-sm75" : L"\\0.3.5-sm86");
    if (cache.size() + 40 >= MAX_PATH) { Trace("[dlssg] unavailable: compatibility cache path is too long"); return false; }
    if ((!CreateDirectoryW(parent.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) ||
        (!CreateDirectoryW(cache.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)) return false;
    const std::wstring local = cache + L"\\version.dll";
    if (!FgFileHash(local, sha)) {
        const std::wstring temporary = local + L"." + std::to_wstring(GetCurrentProcessId()) + L".tmp";
        if (!CopyFileW(shipped.c_str(), temporary.c_str(), FALSE) || !FgFileHash(temporary, sha) ||
            !MoveFileExW(temporary.c_str(), local.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return false;
    }
    const std::wstring ini = L"[General]\nEnabled=1\n[Compatibility]\nRouter=" + std::wstring(sm == 75 ? L"SM75" : L"SM86") +
        L"\nKernelImage=Auto\n[FrameGeneration]\nOptimized=1\nMaxGeneratedFrames=5\nSkipRepeatedRealCopy=0\n"
        L"[Logging]\nLevel=2\nDirectory=" + cache + L"\\logs\n[Runtime]\nMode=Bundled\nCacheDirectory=" + cache + L"\\bundles\n";
    if (!FgWriteUtf8(cache + L"\\dlssg_sm86.ini", ini)) return false;
    proxy = LoadLibraryW(local.c_str());
    if (!proxy) { Trace("[dlssg] unavailable: compatibility loader Windows error %lu", GetLastError()); return false; }
    Trace("[dlssg] RTX compatibility 0.3.5 loaded for SM%d; exact kernels, up to 6x", sm);
    return true;
}
