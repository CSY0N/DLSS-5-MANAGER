// LIVE FLOW on a Radeon (ALPHA) - see radeon_engine.h for what this drives
// and why every address is tied to one build's hash.

#include "radeon_engine.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace lfr {

// The runtime's start call takes a std::string by pointer, and it was built
// with the same compiler family: 32 bytes, small-buffer first. A debug STL
// (a different layout) must never reach it.
static_assert(sizeof(std::string) == 32, "the runtime expects the release std::string layout");

namespace {

// ---- the two known builds ---------------------------------------------------

constexpr uint64_t kSize0217 = 7248384;
constexpr uint64_t kSize0310 = 7304192;

constexpr uint8_t kHash0217[32] = {
    0xbc, 0x97, 0xf3, 0xb0, 0x67, 0x18, 0xe1, 0x90, 0x42, 0xac, 0xaf, 0x22, 0x7b, 0xfe, 0x15, 0xd1,
    0xe4, 0x3d, 0x49, 0x77, 0xf9, 0xdc, 0x2e, 0x39, 0x99, 0x4f, 0xcc, 0x51, 0x14, 0x45, 0xff, 0x4e,
};
constexpr uint8_t kHash0310[32] = {
    0xb1, 0x08, 0xd6, 0x40, 0x7e, 0xb7, 0xf0, 0x94, 0xa4, 0xf9, 0x11, 0x1e, 0xdd, 0x77, 0x8e, 0xee,
    0x7b, 0x97, 0x8b, 0x64, 0x8d, 0x41, 0x3a, 0x9f, 0xc7, 0xae, 0xed, 0xfd, 0xd9, 0x14, 0xc1, 0x54,
};

//                              start    stop     device   queue    context  hip      inline   interop  enabled  started  fsr_in   depth
constexpr Map kMap0217 = {      0x19240, 0x12690, 0x8cee8, 0x8cef0, 0x8cef8, 0x8dad0, 0x8d6c0, 0x8d82c, 0x8d9bc, 0x8d218, 0x8d9be, 0x8d9bf,
    // temporal depth_inv depth_exp tone     struct   skin     mask     channels strength
       0x8d9bd, 0x8d9b0,  0x8d9b4,  0x8d9d0, 0x8d9d4, 0x8d9d8, 0x8d9e0, 0x8d9e4, 0x8d9dc,
    // history  wanted   jobs     gave_up  timeouts rebuilding
       0x8d010, 0x8d018, 0x8d914, 0x8d21a, 0x8d6f8, 0x8daa8 };

// v0.3.1: the context block sits 0x18 past the device here (0x10 in
// v0.2.17), and the staging-rebuild byte is not mapped - Rebuilding() says
// "unknown" for it rather than reading a guess.
constexpr Map kMap0310 = {      0x21720, 0x17150, 0x9a0e8, 0x9a0f0, 0x9a100, 0x9ae08, 0x9a928, 0x9ab58, 0x9acf4, 0x9a420, 0x9acf6, 0x9acf7,
       0x9acf5, 0x9ace8,  0x9acec,  0x9ad08, 0x9ad0c, 0x9ad10, 0x9ad18, 0x9ad1c, 0x9ad14,
       0x9a218, 0x9a220, 0x9a95c, 0x9a422, 0x9a960, 0 };

// Addresses NOT written, on purpose: the watchdog's job pair (the runtime
// records which job timed out there), the wait budget (it shortens it itself
// after a timeout), and Tonemap (its ini value, -1 = by input format, stands).

template <class T>
T &At(void *image, uintptr_t offset)
{
    return *reinterpret_cast<T *>(reinterpret_cast<uintptr_t>(image) + offset);
}

std::string Narrow(const std::wstring &w)
{
    std::string s;
    s.reserve(w.size());
    for (wchar_t c : w) s.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    return s;
}

std::string Hex(const uint8_t *p, size_t n)
{
    static const char digits[] = "0123456789abcdef";
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) { s[2 * i] = digits[p[i] >> 4]; s[2 * i + 1] = digits[p[i] & 15]; }
    return s;
}

bool FileSize(const std::wstring &path, unsigned long long &size)
{
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return false;
    size = (static_cast<unsigned long long>(a.nFileSizeHigh) << 32) | a.nFileSizeLow;
    return true;
}

bool Sha256File(const std::wstring &path, uint8_t out[32])
{
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    bool ok = BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
              BCryptCreateHash(alg, &hash, nullptr, 0, nullptr, 0, 0) >= 0;
    std::vector<uint8_t> chunk(1 << 20);
    while (ok)
    {
        DWORD got = 0;
        if (!ReadFile(f, chunk.data(), static_cast<DWORD>(chunk.size()), &got, nullptr)) { ok = false; break; }
        if (got == 0) break;
        ok = BCryptHashData(hash, chunk.data(), got, 0) >= 0;
    }
    if (ok) ok = BCryptFinishHash(hash, out, 32, 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return ok;
}

// What the runtime wrote to its log after `from` (it appends across runs, so
// anything before that belongs to an earlier launch). Bounded: one run's
// start-up is a few kilobytes.
std::string LogSince(const std::wstring &path, unsigned long long from)
{
    std::string text;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return text;
    LARGE_INTEGER size{}, at{};
    if (GetFileSizeEx(f, &size) && static_cast<unsigned long long>(size.QuadPart) > from)
    {
        at.QuadPart = static_cast<LONGLONG>(from);
        if (SetFilePointerEx(f, at, nullptr, FILE_BEGIN))
        {
            char buf[8192];
            DWORD got = 0;
            while (text.size() < (1u << 20) && ReadFile(f, buf, sizeof(buf), &got, nullptr) && got > 0)
                text.append(buf, got);
        }
    }
    CloseHandle(f);
    return text;
}

// The runtime compiles its shaders with whatever D3DCompile its import slot
// points at. If an old d3dcompiler_47.dll was already in the process, that
// would be the one. Point this image's slot at the System32 build. Found by
// walking the image's own import table - never a fixed address - and only if
// the image imports it at all (v0.3.1 does not).
const char *RebindCompiler(void *image)
{
    auto *base = static_cast<uint8_t *>(image);
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return "not a PE image";
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS64 *>(base + dos->e_lfanew);
    const IMAGE_DATA_DIRECTORY &dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress == 0) return "no import table";
    for (auto *imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR *>(base + dir.VirtualAddress); imp->Name; ++imp)
    {
        if (_stricmp(reinterpret_cast<const char *>(base + imp->Name), "d3dcompiler_47.dll") != 0) continue;
        auto *names = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + (imp->OriginalFirstThunk ? imp->OriginalFirstThunk
                                                                                             : imp->FirstThunk));
        auto *slots = reinterpret_cast<IMAGE_THUNK_DATA64 *>(base + imp->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots)
        {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            auto *by_name = reinterpret_cast<IMAGE_IMPORT_BY_NAME *>(base + names->u1.AddressOfData);
            if (strcmp(reinterpret_cast<const char *>(by_name->Name), "D3DCompile") != 0) continue;
            HMODULE system = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            FARPROC fn = system ? GetProcAddress(system, "D3DCompile") : nullptr;
            if (fn == nullptr) return "System32 d3dcompiler_47.dll has no D3DCompile";
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof(void *), PAGE_READWRITE, &old))
                return "the import slot could not be unprotected";
            InterlockedExchangePointer(reinterpret_cast<void **>(&slots->u1.Function), reinterpret_cast<void *>(fn));
            VirtualProtect(&slots->u1.Function, sizeof(void *), old, &old);
            return "pointed at System32";
        }
        return "imports d3dcompiler_47.dll without D3DCompile";
    }
    return "not imported (loaded at run time by the runtime itself)";
}

// Calls into the image, outside any function that owns C++ objects (C2712).
// > 0: the call returned false; 0: true; < 0: the exception code, negated.
int CallStart(void *image, uintptr_t offset, void *context, const std::string *weights)
{
    using Fn = bool(__fastcall *)(void *, const std::string *);
    const Fn fn = reinterpret_cast<Fn>(reinterpret_cast<uintptr_t>(image) + offset);
    __try { return fn(context, weights) ? 0 : 1; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -static_cast<int>(GetExceptionCode()); }
}

int CallStop(void *image, uintptr_t offset)
{
    using Fn = void (*)();
    const Fn fn = reinterpret_cast<Fn>(reinterpret_cast<uintptr_t>(image) + offset);
    __try { fn(); return 0; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return -static_cast<int>(GetExceptionCode()); }
}

// hipDeviceProp_t (the R0600 ABI) is about a kilobyte: name[256], uuid[16],
// then the adapter LUID. The buffer is generous because a short one is a
// stack overrun, not an error return.
struct alignas(16) HipProperties { unsigned char bytes[8192]; };
constexpr size_t kHipLuidAt = 272;

bool EnvIs(const char *name, char value)
{
    char v[8] = {};
    const DWORD got = GetEnvironmentVariableA(name, v, sizeof(v));
    return got > 0 && got < sizeof(v) && v[0] == value;
}

}  // namespace

float StrengthMax()
{
    static const float value = [] {
        char v[32] = {};
        const DWORD got = GetEnvironmentVariableA("LF_RADEON_STRENGTH_MAX", v, sizeof(v));
        const float f = (got > 0 && got < sizeof(v)) ? static_cast<float>(atof(v)) : 0.0f;
        return f > 0.0f && f <= 0.125f ? f : 0.03f;
    }();
    return value;
}

const char *NeuralRuntime::WhichName() const
{
    switch (release_)
    {
    case Release::V0310: return "v0.3.1";
    case Release::V0217: return "v0.2.17";
    default:             return "none";
    }
}

void NeuralRuntime::Say(const char *fmt, ...) const
{
    if (say_ == nullptr) return;
    char line[1024];
    va_list args;
    va_start(args, fmt);
    _vsnprintf_s(line, sizeof(line), _TRUNCATE, fmt, args);
    va_end(args);
    say_(line);
}

bool NeuralRuntime::Open(const std::wstring &dir, const wchar_t *file, ID3D12Device *device,
                         ID3D12CommandQueue *queue, SayFn say)
{
    say_ = say;
    ready_ = false;
    why_.clear();
    sha_.clear();
    release_ = Release::None;
    map_ = nullptr;
    hip_index_ = -1;
    hooks_seen_ = false;
    verdict_ = Verdict::Pending;
    grew_at_ = 0;
    const std::wstring path = dir + L"\\" + file;
    weights_path_ = dir + L"\\dlssnr_on_amd_weights.bin";   // the runtime's own name for it
    ini_path_ = dir + L"\\dlssnr_on_amd.ini";               // read by the runtime on load
    log_path_ = dir + L"\\dlssnr_on_amd.log";               // written by the runtime

    // 1. Which build is this? Size, then SHA-256 - the hash picks the map.
    unsigned long long size = 0;
    if (!FileSize(path, size)) { why_ = Narrow(file) + " is not in " + Narrow(dir); return false; }
    if (size != kSize0217 && size != kSize0310)
    {
        why_ = Narrow(file) + " is " + std::to_string(size) + " bytes - not a build we know (v0.3.1 is " +
               std::to_string(kSize0310) + ", v0.2.17 is " + std::to_string(kSize0217) + ")";
        return false;
    }
    uint8_t hash[32] = {};
    if (!Sha256File(path, hash)) { why_ = "could not read " + Narrow(file) + " to hash it"; return false; }
    sha_ = Hex(hash, 32);
    if (memcmp(hash, kHash0310, 32) == 0) { release_ = Release::V0310; map_ = &kMap0310; }
    else if (memcmp(hash, kHash0217, 32) == 0) { release_ = Release::V0217; map_ = &kMap0217; }
    else
    {
        why_ = Narrow(file) + " has an unknown SHA-256 " + sha_ + " - refused rather than driven with a wrong map";
        return false;
    }
    if (GetFileAttributesW(weights_path_.c_str()) == INVALID_FILE_ATTRIBUTES)
    {
        why_ = "dlssnr_on_amd_weights.bin (the network) is not beside the runtime";
        return false;
    }

    // 2. The ini, read by the runtime while it loads - so before LoadLibrary.
    // Each key only where it is missing: a value someone tuned by hand stays.
    {
        const wchar_t *keys[][2] = {
            { L"Enabled", L"1" },
            { L"UseFsrInputs", L"1" },   // follow the FidelityFX dispatch - without it no frame is processed
            { L"UseDepth", L"0" },       // a desktop has no depth
            { L"Interop", L"1" },
            { L"Inline", L"1" },         // the job finishes inside our frame's list
            { L"InlineWaitMs", L"200" },
            { L"Temporal", L"1" },
            { L"Tonemap", L"-1" },       // by input format
            { L"HipDevice", L"-1" },     // the runtime matches our adapter itself
            { L"Scale", L"0.03000" },
            { L"UseAutoMask", L"1" },
            { L"ToneChannels", L"0" },
            { L"PreUpscale", L"1" },     // v0.3.x: the network on the dispatch's input, FSR gets the corrected copy
        };
        int written = 0;
        for (const auto &kv : keys)
        {
            // A sentinel no real value can be: "1" and "0" are one character
            // long, so the length alone cannot tell a set key from a missing one.
            wchar_t have[64] = {};
            GetPrivateProfileStringW(L"DlssNrOnAmd", kv[0], L"\x1\x2", have, 64, ini_path_.c_str());
            if (wcscmp(have, L"\x1\x2") != 0) continue;
            if (WritePrivateProfileStringW(L"DlssNrOnAmd", kv[0], kv[1], ini_path_.c_str())) ++written;
        }
        Say("settings file: %d key(s) added, the rest kept as they were", written);
    }

    // 3. HIP, and the HIP device that IS our D3D12 adapter. The two APIs list
    // devices in their own orders and one card can appear more than once, so
    // the match is by LUID, never by position.
    hip_ = LoadLibraryExW(L"amdhip64_7.dll", nullptr, LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (hip_ == nullptr)
    {
        why_ = "amdhip64_7.dll was not found - it comes with the AMD Adrenalin driver 26.1.1 or newer";
        return false;
    }
    auto hip_count = reinterpret_cast<int (*)(int *)>(GetProcAddress(static_cast<HMODULE>(hip_), "hipGetDeviceCount"));
    auto hip_props = reinterpret_cast<int (*)(void *, int)>(
        GetProcAddress(static_cast<HMODULE>(hip_), "hipGetDevicePropertiesR0600"));
    hip_select_ = reinterpret_cast<int (*)(int)>(GetProcAddress(static_cast<HMODULE>(hip_), "hipSetDevice"));
    if (hip_count == nullptr || hip_props == nullptr || hip_select_ == nullptr)
    {
        why_ = "amdhip64_7.dll lacks hipGetDeviceCount/hipGetDevicePropertiesR0600/hipSetDevice - driver too old?";
        return false;
    }
    int count = 0;
    if (hip_count(&count) != 0 || count <= 0) { why_ = "HIP sees no device"; return false; }
    hip_count_ = count;
    const LUID want = device->GetAdapterLuid();
    for (int i = 0; i < count && hip_index_ < 0; ++i)
    {
        static HipProperties props;   // static: 8 KB is not for the stack
        memset(&props, 0, sizeof(props));
        if (hip_props(&props, i) == 0 && memcmp(props.bytes + kHipLuidAt, &want, sizeof(want)) == 0)
            hip_index_ = i;
    }
    if (hip_index_ < 0)
    {
        why_ = "none of the " + std::to_string(count) + " HIP device(s) is the adapter LIVE FLOW runs on";
        return false;
    }
    hip_select_(hip_index_);
    Say("HIP: %d device(s), ours is #%d (matched by adapter LUID)", count, hip_index_);

    // 4. The module. Its log is appended to across runs: remember where it
    // ends now, so everything read later belongs to this run.
    if (!FileSize(log_path_, log_start_)) log_start_ = 0;
    image_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (image_ == nullptr)
    {
        why_ = "LoadLibrary of " + Narrow(file) + " failed, error " + std::to_string(GetLastError());
        return false;
    }
    // Pinned: its threads keep pointers into it, and an unload under them
    // cannot be made safe.
    HMODULE pinned = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       static_cast<LPCWSTR>(image_), &pinned);
    Say("shader compiler import: %s", RebindCompiler(image_));

    // 5. Bindings and flags. The runtime has already read its ini; what is
    // written here wins, and Inline goes in twice - before and after Enabled -
    // because the runtime re-derives it when Enabled changes.
    At<ID3D12Device *>(image_, map_->device) = device;
    device->AddRef();
    At<ID3D12CommandQueue *>(image_, map_->queue) = queue;
    queue->AddRef();
    At<int>(image_, map_->hip_device) = -1;
    At<uint8_t>(image_, map_->inline_mode) = 1;
    At<uint8_t>(image_, map_->enabled) = 1;
    At<uint8_t>(image_, map_->inline_mode) = 1;
    interop_ = EnvIs("LF_RADEON_INTEROP", '0') ? 0 : 1;
    At<uint8_t>(image_, map_->interop) = static_cast<uint8_t>(interop_);
    At<uint8_t>(image_, map_->fsr_inputs) = 1;
    At<uint8_t>(image_, map_->use_depth) = 0;

    // 6. The runtime installs its D3D12/DXGI hooks from a thread of its own.
    // A swapchain made before they land is invisible to it, so wait for its
    // own word (this run's lines only) - up to five seconds, then go on and
    // say so.
    {
        const char *markers[] = { "hooked IDXGISwapChain1::Present1", "env: d3d12 device yes" };
        const ULONGLONG t0 = GetTickCount64();
        while (!hooks_seen_ && GetTickCount64() - t0 < 5000)
        {
            const std::string text = LogSince(log_path_, log_start_);
            for (const char *m : markers)
                if (text.find(m) != std::string::npos) hooks_seen_ = true;
            if (!hooks_seen_) Sleep(25);
        }
        hooks_ms_ = static_cast<unsigned>(GetTickCount64() - t0);
    }

    // 7. Start. The argument is the ADDRESS of the runtime's own context block.
    hip_select_(hip_index_);
    const std::string weights = Narrow(weights_path_);
    const int rc = CallStart(image_, map_->start, reinterpret_cast<void *>(
                                 reinterpret_cast<uintptr_t>(image_) + map_->context), &weights);
    if (rc < 0)
    {
        char code[16];
        sprintf_s(code, "0x%08X", static_cast<unsigned>(-rc));
        why_ = std::string("the runtime faulted in its start call (") + code + ")";
        Disable();
        return false;
    }
    if (rc > 0)
    {
        why_ = "the runtime refused to start - its own log (dlssnr_on_amd.log) says why";
        Disable();
        return false;
    }
    At<uint8_t>(image_, map_->started) = 1;
    ready_ = true;
    return true;
}

Verdict NeuralRuntime::Poll(unsigned budget_ms)
{
    if (!ready_ || verdict_ != Verdict::Pending) return verdict_;
    const std::string text = LogSince(log_path_, log_start_);
    // Its own line: "engine init <result>". Anything but "ok" is its no.
    const size_t at = text.rfind("engine init ");
    if (at != std::string::npos)
        return verdict_ = text.compare(at + 12, 2, "ok") == 0 ? Verdict::Up : Verdict::Down;
    // Nothing yet for this run: still waiting for our swapchain. Something,
    // but not the line: start the clock, and give it the budget.
    if (text.empty()) return verdict_;
    if (grew_at_ == 0) grew_at_ = GetTickCount64();
    if (GetTickCount64() - grew_at_ >= budget_ms) verdict_ = Verdict::Down;
    return verdict_;
}

bool NeuralRuntime::RetryWithOurHip()
{
    if (!ready_ || retried_ || hip_index_ < 0) return false;
    retried_ = true;
    At<int>(image_, map_->hip_device) = hip_index_;
    hip_select_(hip_index_);
    const std::string weights = Narrow(weights_path_);
    const int rc = CallStart(image_, map_->start, reinterpret_cast<void *>(
                                 reinterpret_cast<uintptr_t>(image_) + map_->context), &weights);
    if (rc != 0) return false;
    At<uint8_t>(image_, map_->started) = 1;
    // Only the runtime's own line decides, so the question is open again -
    // and the clock only starts once the log moves past this point.
    FileSize(log_path_, log_start_);
    verdict_ = Verdict::Pending;
    grew_at_ = 0;
    return true;
}

void NeuralRuntime::Apply(const Knobs &k)
{
    if (!ready_) return;
    auto unit2 = [](float v) { return v < 0.0f ? 0.0f : (v > 2.0f ? 2.0f : v); };
    At<uint8_t>(image_, map_->temporal) = 1;
    At<uint32_t>(image_, map_->depth_inverted) = 0;
    At<uint8_t>(image_, map_->depth_explicit) = 1;
    At<float>(image_, map_->tone) = unit2(k.tone);
    At<float>(image_, map_->structure) = unit2(k.structure);
    At<float>(image_, map_->skin) = unit2(k.skin);
    At<uint32_t>(image_, map_->character_mask) = k.character_mask ? 1u : 0u;
    // Bit 4 on, bit 2 off: when a job runs late the runtime falls back to the
    // frame it was given instead of waiting on.
    At<uint32_t>(image_, map_->tone_channels) = (k.tone_channels & ~2u) | 4u;
    At<uint8_t>(image_, map_->enabled) = k.enabled ? 1 : 0;
    // The strength is read on every job from here. The ini's Scale is NOT
    // rewritten: the runtime reads its file once, at load, and treats a
    // changed file as a reason to rebuild its staging - a slider would turn
    // into a rebuild per step for nothing, since this field wins anyway.
    const float intensity = k.intensity < 0.0f ? 0.0f : (k.intensity > 1.0f ? 1.0f : k.intensity);
    const float strength = intensity * StrengthMax();
    At<float>(image_, map_->strength) = strength;
    strength_written_ = strength;
}

void NeuralRuntime::ForgetHistory()
{
    if (!ready_) return;
    At<uint8_t>(image_, map_->history_wanted) = 0;
    At<void *>(image_, map_->history) = nullptr;
}

uint32_t NeuralRuntime::Jobs() const
{
    return ready_ ? At<volatile uint32_t>(image_, map_->jobs) : 0u;
}

uint32_t NeuralRuntime::Timeouts() const
{
    return ready_ && map_->timeouts ? At<volatile uint32_t>(image_, map_->timeouts) : 0u;
}

bool NeuralRuntime::TimeoutsKnown() const { return ready_ && map_->timeouts != 0; }

bool NeuralRuntime::GaveUp() const
{
    return ready_ && At<volatile uint8_t>(image_, map_->gave_up) != 0;
}

bool NeuralRuntime::Rebuilding() const
{
    if (!ready_ || map_->rebuilding == 0) return true;
    return At<volatile uint8_t>(image_, map_->rebuilding) != 0;
}

bool NeuralRuntime::LastLogLine(const char *needle, std::string &line) const
{
    const std::string text = LogSince(log_path_, log_start_);
    const size_t at = text.rfind(needle);
    if (at == std::string::npos) return false;
    size_t begin = text.rfind('\n', at);
    begin = begin == std::string::npos ? 0 : begin + 1;
    size_t end = text.find('\n', at);
    if (end == std::string::npos) end = text.size();
    line = text.substr(begin, end - begin);
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    if (line.size() > 300) line.resize(300);
    return true;
}

void NeuralRuntime::Disable()
{
    if (image_ != nullptr && map_ != nullptr) At<uint8_t>(image_, map_->enabled) = 0;
}

void NeuralRuntime::Stop()
{
    if (image_ == nullptr || map_ == nullptr || !ready_) return;
    ready_ = false;
    const int rc = CallStop(image_, map_->stop);
    if (rc != 0) Say("the runtime faulted while stopping (0x%08X)", static_cast<unsigned>(-rc));
}

}  // namespace lfr
