// LIVE FLOW on a Radeon (ALPHA) - the neural runtime, driven from outside.
//
// DLSS 5's network runs on a Radeon through a third-party runtime (the
// DLSS-NR-on-AMD project) that was written to sit inside a game. It exports
// nothing: it is driven by writing to fixed places inside its image and by
// calling a few internal addresses, and it finds the frame by following the
// game's FidelityFX upscaler dispatch. This file loads it, proves which build
// it is, prepares it, and reports - in our own log - what it did.
//
// Every address below belongs to ONE build of that runtime. A wrong address is
// not an error code, it is a jump into whatever is there, so the build is
// identified by its SHA-256 before anything is written, and an unknown file
// is refused with the hash in the log. Two builds are known:
//
//   v0.3.1   the maintainer's current release, and the default here: its own
//            notes fix the stalls and crashes the older one had, and it is the
//            build with a working picture on RDNA3 in the reports we know of.
//   v0.2.17  the previous release, kept as the fallback (LF_RADEON_BUILD=0217
//            picks it first). Same design, different addresses.
//
// The weights, the upscaler and the runtime are third-party files; this file
// never ships or changes them - it checks them and drives them.

#pragma once

#include <cstdint>
#include <string>

#include <d3d12.h>

namespace lfr {

enum class Release { None, V0217, V0310 };

// Where things are inside one build's image, as offsets from its base.
// 0 = unknown for that build: never read, never written, never called.
struct Map {
    // Code.
    uintptr_t start;          // bool (void *context, const std::string *weights)
    uintptr_t stop;           // void (void) - stops the runtime's worker threads

    // Written once, before start.
    uintptr_t device;         // ID3D12Device *
    uintptr_t queue;          // ID3D12CommandQueue *
    uintptr_t context;        // the runtime's own context block; its ADDRESS is start's argument
    uintptr_t hip_device;     // int, -1 = let the runtime match the adapter itself
    uintptr_t inline_mode;    // uint8, 1 = the job finishes inside the frame's own list
    uintptr_t interop;        // uint8, 1 = zero-copy shared textures
    uintptr_t enabled;        // uint8
    uintptr_t started;        // uint8, set once start returned
    uintptr_t fsr_inputs;     // uint8, 1 = follow the FidelityFX dispatch
    uintptr_t use_depth;      // uint8

    // Read by the runtime on every job.
    uintptr_t temporal;       // uint8
    uintptr_t depth_inverted; // uint32
    uintptr_t depth_explicit; // uint8
    uintptr_t tone;           // float, 0..2
    uintptr_t structure;      // float, 0..2
    uintptr_t skin;           // float, 0..2
    uintptr_t character_mask; // uint32
    uintptr_t tone_channels;  // uint32 bitfield
    uintptr_t strength;       // float - the network's Scale

    // History.
    uintptr_t history;        // void *, null forgets it
    uintptr_t history_wanted; // uint8

    // Read-only for us: what the runtime says about itself.
    uintptr_t jobs;           // uint32, jobs recorded
    uintptr_t gave_up;        // uint8, non-zero once the runtime latched a failure
    uintptr_t timeouts;       // uint32, GPU waits that ran out
    uintptr_t rebuilding;     // uint8, set while it re-creates its staging
};

struct Knobs {
    float tone = 0.5f;
    float structure = 1.0f;
    float skin = -1.0f;          // -1 in the app means "off": written as 0
    bool character_mask = true;
    uint32_t tone_channels = 0;
    float intensity = 1.0f;      // the app's 0..1 slider
    bool enabled = true;
};

// The runtime's own verdict on whether its engine started.
enum class Verdict { Pending, Up, Down };

using SayFn = void (*)(const char *line);

class NeuralRuntime {
public:
    // Opens `file` in `dir`: size and SHA-256 first, then the ini (keys we
    // need, written only where absent), HIP (the device matching our D3D12
    // adapter by LUID), the module itself, its bindings and flags, a wait for
    // its own hooks, and the start call. Never throws; on false, Why() says
    // what stopped it in one line.
    bool Open(const std::wstring &dir, const wchar_t *file, ID3D12Device *device,
              ID3D12CommandQueue *queue, SayFn say);

    bool Ready() const { return ready_; }
    // The image was loaded (its hooks are in the process, and it is pinned):
    // no second runtime may follow it, and the process must not unload it.
    bool Loaded() const { return image_ != nullptr; }
    Release Which() const { return release_; }
    const char *WhichName() const;
    const std::string &Why() const { return why_; }
    const std::string &Sha256() const { return sha_; }
    bool HooksSeen() const { return hooks_seen_; }
    unsigned HooksMs() const { return hooks_ms_; }
    int HipIndex() const { return hip_index_; }
    int HipCount() const { return hip_count_; }
    int Interop() const { return interop_; }

    // Asked from the frame loop: the runtime starts its engine only after
    // our swapchain exists, which is after Open returned. Pending until its
    // own log says "engine init ok" (Up) or has grown for this run without
    // it for `budget_ms` (Down).
    Verdict Poll(unsigned budget_ms);
    // One more start, this time with the HIP index we matched ourselves
    // instead of the runtime's own match. Once per run; Poll decides again.
    bool RetryWithOurHip();
    bool Retried() const { return retried_; }

    void Apply(const Knobs &k);
    float StrengthNow() const { return strength_written_; }
    void ForgetHistory();

    // What the runtime says about itself. The *Known() pair tells a real 0
    // from "this build has no such counter".
    uint32_t Jobs() const;
    uint32_t Timeouts() const;
    bool TimeoutsKnown() const;
    bool GaveUp() const;
    bool Rebuilding() const;   // unknown = true: assume the worst

    // The last line of the runtime's own log (this run only) containing
    // `needle`, for quoting into ours. False when there is none.
    bool LastLogLine(const char *needle, std::string &line) const;

    // Stops the runtime's threads. Only once all our work has drained.
    void Stop();

    // A loaded runtime that did not come up is told to stand still: its
    // hooks stay in the process (it cannot be unloaded), its work does not.
    void Disable();

private:
    void Say(const char *fmt, ...) const;

    bool ready_ = false;
    Release release_ = Release::None;
    const Map *map_ = nullptr;
    void *image_ = nullptr;
    void *hip_ = nullptr;
    int (*hip_select_)(int) = nullptr;
    int hip_index_ = -1;
    int hip_count_ = 0;
    int interop_ = -1;
    bool hooks_seen_ = false;
    unsigned hooks_ms_ = 0;
    bool retried_ = false;
    Verdict verdict_ = Verdict::Pending;
    unsigned long long grew_at_ = 0;     // when this run's log first grew without the line
    unsigned long long log_start_ = 0;   // the runtime's log size before we loaded it
    std::wstring log_path_, ini_path_, weights_path_;
    float strength_written_ = -1.0f;
    std::string why_, sha_;
    SayFn say_ = nullptr;
};

// Intensity 1.0 = this much network strength. 0.03 is where detail appears
// without ringing on real content; the runtime's own ceiling (0.125) halos.
// LF_RADEON_STRENGTH_MAX moves it without a rebuild.
float StrengthMax();

}  // namespace lfr
