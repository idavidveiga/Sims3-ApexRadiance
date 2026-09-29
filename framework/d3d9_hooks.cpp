#include "d3d9_hooks.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "hook_chain.h"
#include "frame_profiler.h"
#include <detours/detours.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <vector>

namespace D3D9Hooks {
namespace {

// ---- callback chains ----
// g_lock: held while callbacks are (un)registered, while a locked chain runs (Present, BeginScene, Create*) and while any
// chain runs on a thread other than the render thread. The draw and state chains run on the render thread without it
// (see d3d9_hooks.h and docs/architecture.md section 3.6).
std::recursive_mutex g_lock;
uint64_t g_sequence = 0; // registration order (ties between equal priorities)

template <typename Fn> struct Entry {
    std::string name;
    std::string timingName; // the name the per-name timing reports ("<name> (Present)" on the Present chain)
    Fn fn;
    int priority;
    uint64_t sequence;
};

// Per-name timing of a chain's callbacks (Frame Profiler, development build)
enum class Timing : uint8_t {
    None,   // not timed per name
    Option, // while Advanced > "Per-hook registry timing" is checked (draw chains)
    Always, // while the profiler is on (Present chain: a few callbacks once per frame)
};

// A chain's list is immutable once published: registering builds a new list and publishes it through an atomic pointer.
// Every list ever published is kept until Uninstall, so a dispatch that read an older one (possibly the caller of
// Register, or the render thread's lock-free dispatch) keeps iterating valid memory.
template <typename Fn> struct Chain {
    using List = std::vector<Entry<Fn>>;
    const char* method;
    bool lockFree;            // dispatched without g_lock on the render thread (draw and state chains)
    bool modTimed;            // its outermost dispatch is booked as "D3D hooks (mod)" (every chain but Present)
    Timing timing;
    const char* timingSuffix; // appended to the name for the per-name timing
    std::atomic<const List*> list{nullptr};
    std::atomic<size_t> count{0};             // lock-free "nothing registered" test for the hot paths
    std::vector<std::unique_ptr<List>> lists; // every list published, the last one current (guarded by g_lock)
    std::atomic<bool> offThreadLogged{false};

    Chain(const char* m, bool lf, bool mt, Timing t, const char* suffix) : method(m), lockFree(lf), modTimed(mt), timing(t), timingSuffix(suffix) {
        lists.push_back(std::make_unique<List>());
        list.store(lists.back().get());
    }
};

// Caller holds g_lock
template <typename Fn> void Publish(Chain<Fn>& chain, std::unique_ptr<typename Chain<Fn>::List> next) {
    chain.count.store(next->size(), std::memory_order_relaxed);
    chain.list.store(next.get(), std::memory_order_seq_cst); // xchg: globally visible before WaitForRenderThread's barrier
    chain.lists.push_back(std::move(next));
}

template <typename Fn> bool Add(Chain<Fn>& chain, const std::string& name, Fn fn, Priority priority) {
    if (!fn) return false;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto next = std::make_unique<typename Chain<Fn>::List>(*chain.list.load(std::memory_order_relaxed));
    next->push_back({name, chain.timingSuffix ? name + chain.timingSuffix : name, std::move(fn), static_cast<int>(priority), ++g_sequence});
    std::stable_sort(next->begin(), next->end(), [](const Entry<Fn>& a, const Entry<Fn>& b) {
        return a.priority != b.priority ? a.priority < b.priority : a.sequence < b.sequence;
    });
    Publish(chain, std::move(next));
    return true;
}

// Caller holds g_lock. True when a callback was removed from a lock-free chain.
template <typename Fn> bool RemoveName(Chain<Fn>& chain, const std::string& name) {
    const auto* cur = chain.list.load(std::memory_order_relaxed);
    auto next = std::make_unique<typename Chain<Fn>::List>();
    for (const auto& e : *cur)
        if (e.name != name) next->push_back(e);
    if (next->size() == cur->size()) return false;
    Publish(chain, std::move(next));
    return chain.lockFree;
}

// Caller holds g_lock (Uninstall): frees every list but the current one
template <typename Fn> void FreeRetired(Chain<Fn>& chain) {
    if (chain.lists.size() > 1) chain.lists.erase(chain.lists.begin(), chain.lists.end() - 1);
}

// ---- render thread and lock-free dispatch ----
inline DWORD ThreadId() { return __readfsdword(0x24); } // TEB ClientId.UniqueThread, what GetCurrentThreadId returns

std::atomic<DWORD> g_renderTid{0};          // the thread of the first EndScene (Install)
int g_renderDepth = 0;                      // render thread only: nesting of its lock-free dispatches
std::atomic<int> g_renderInside{0};         // written by the render thread only: inside a lock-free dispatch
std::atomic<uint32_t> g_renderExits{0};     // written by the render thread only: outermost lock-free dispatches finished
std::atomic<uint32_t> g_offThread{0};       // lock-free chains dispatched from another thread (under the lock)
std::mutex g_timingLock;                    // FrameProfiler::AddRegistryHookTime is not thread-safe on its own
thread_local int t_dispatchDepth = 0;       // development build: nesting of dispatches on this thread (the mod time)

// Render thread, around a lock-free dispatch. The "inside" store must precede the list load: the compiler barrier keeps
// that order in the code, the other thread's FlushProcessWriteBuffers covers the processor's store buffer.
struct RenderInside {
    RenderInside() {
        if (g_renderDepth++ == 0) {
            g_renderInside.store(1, std::memory_order_relaxed);
            std::atomic_signal_fence(std::memory_order_seq_cst);
        }
    }
    ~RenderInside() {
        if (--g_renderDepth == 0) {
            g_renderExits.store(g_renderExits.load(std::memory_order_relaxed) + 1, std::memory_order_release);
            g_renderInside.store(0, std::memory_order_release);
        }
    }
    RenderInside(const RenderInside&) = delete;
    RenderInside& operator=(const RenderInside&) = delete;
};

// A thread other than the render thread just published new lists (UnregisterAll): returns once the render thread can no
// longer be running a removed callback. After the barrier, a lock-free dispatch that starts later loads the new list;
// one that had started is visible through g_renderInside, and it is waited for (it ends when g_renderExits moves or the
// flag drops). Bounded: a render thread blocked on a lock the caller holds would otherwise never return.
void WaitForRenderThread(const std::string& name) {
    if (!g_renderTid.load(std::memory_order_relaxed)) return; // not installed: no dispatch has ever run
    FlushProcessWriteBuffers();
    if (g_renderInside.load(std::memory_order_acquire) == 0) return;
    const uint32_t exits = g_renderExits.load(std::memory_order_acquire);
    const ULONGLONG start = GetTickCount64();
    while (g_renderInside.load(std::memory_order_acquire) != 0 && g_renderExits.load(std::memory_order_acquire) == exits) {
        if (GetTickCount64() - start > 1000) {
            LOG_WARNING(std::format("[D3D9Hooks] UnregisterAll(\"{}\") from thread {}: the render thread did not leave its draw hook within 1 s; continuing", name, ThreadId()));
            return;
        }
        SwitchToThread();
    }
}

template <typename Fn> void NoteOffThread(Chain<Fn>& chain) {
    g_offThread.fetch_add(1, std::memory_order_relaxed);
    if constexpr (!kPublicBuild)
        if (!chain.offThreadLogged.exchange(true))
            LOG_WARNING(std::format("[D3D9Hooks] {} called from thread {} (render thread {}): dispatched under the lock (first time only)", chain.method, ThreadId(),
                                    g_renderTid.load(std::memory_order_relaxed)));
}

// Development build: the outermost dispatch on a thread is booked as mod time ("D3D hooks (mod)") by the Frame Profiler,
// from before the first callback to after the last one, also when a callback returns Skip / Block. Nested dispatches (a
// callback calling the device, e.g. a replaced draw re-issued by Night Lighting) are part of it.
struct ModTimeGuard {
    int token = -1;
    const void* key;
    ModTimeGuard(bool modTimed, const void* k) : key(k) {
        if constexpr (!kPublicBuild) {
            if (t_dispatchDepth++ == 0 && modTimed && FrameProfiler::ModTimeActive()) token = FrameProfiler::BeginModTime(FrameProfiler::ModTime::D3DDispatch, key);
        }
    }
    ~ModTimeGuard() {
        if constexpr (!kPublicBuild) {
            --t_dispatchDepth;
            if (token >= 0) FrameProfiler::EndModTime(token, key);
        }
    }
    ModTimeGuard(const ModTimeGuard&) = delete;
    ModTimeGuard& operator=(const ModTimeGuard&) = delete;
};

// Runs one list. false = a callback asked to skip the device call; result then holds what the game gets back.
template <typename Fn, typename... Args> bool RunList(const Chain<Fn>& chain, const typename Chain<Fn>::List& list, DeviceContext& ctx, HRESULT& result, Args... args) {
    bool timed = false;
    if constexpr (!kPublicBuild)
        timed = chain.timing == Timing::Always ? FrameProfiler::PresentHookTimingActive() : (chain.timing == Timing::Option && FrameProfiler::RegistryHookTimingActive());
    for (const auto& e : list) {
        const uint64_t t0 = timed ? FrameProfiler::Ticks() : 0;
        const HookAction r = e.fn(ctx, args...);
        if (timed) {
            const uint64_t dt = FrameProfiler::Ticks() - t0;
            std::lock_guard<std::mutex> tl(g_timingLock);
            FrameProfiler::AddRegistryHookTime(e.timingName, dt);
        }
        if (r == HookAction::Skip) {
            result = S_OK;
            return false;
        }
        if (r == HookAction::Block) {
            result = E_FAIL;
            return false;
        }
    }
    return true;
}

// Runs the chain. false = a callback asked to skip the device call; result then holds what the game gets back.
template <typename Fn, typename... Args> bool Run(Chain<Fn>& chain, IDirect3DDevice9* device, HRESULT& result, Args... args) {
    if (chain.count.load(std::memory_order_relaxed) == 0) return true;
    DeviceContext ctx{device};
    ModTimeGuard mod(chain.modTimed, &ctx);
    if (chain.lockFree) {
        if (ThreadId() == g_renderTid.load(std::memory_order_relaxed)) {
            RenderInside inside;
            const auto* list = chain.list.load(std::memory_order_acquire);
            return RunList(chain, *list, ctx, result, args...);
        }
        NoteOffThread(chain);
    }
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const auto* list = chain.list.load(std::memory_order_relaxed);
    return RunList(chain, *list, ctx, result, args...);
}

Chain<DrawIndexedPrimitiveHook> g_dip("DrawIndexedPrimitive", true, true, Timing::Option, nullptr);
Chain<DrawPrimitiveHook> g_dp("DrawPrimitive", true, true, Timing::Option, nullptr);
Chain<SetRenderTargetHook> g_srt("SetRenderTarget", true, true, Timing::None, nullptr);
Chain<SetPixelShaderHook> g_sps("SetPixelShader", true, true, Timing::None, nullptr);
Chain<SetVertexShaderHook> g_svs("SetVertexShader", true, true, Timing::None, nullptr);
Chain<SetTextureHook> g_stex("SetTexture", true, true, Timing::None, nullptr);
Chain<PresentHook> g_present("Present", false, false, Timing::Always, " (Present)");
Chain<BeginSceneHook> g_begin("BeginScene", false, true, Timing::None, nullptr);
Chain<CreateTextureHook> g_ctex("CreateTexture", false, true, Timing::None, nullptr);
Chain<CreateRenderTargetHook> g_crt("CreateRenderTarget", false, true, Timing::None, nullptr);
Chain<SetViewportHook> g_svp("SetViewport", true, true, Timing::None, nullptr);
Chain<CreatePixelShaderHook> g_cps("CreatePixelShader", false, true, Timing::None, nullptr);
Chain<CreateVertexShaderHook> g_cvs("CreateVertexShader", false, true, Timing::None, nullptr);
Chain<SetPixelShaderConstantFHook> g_psc("SetPixelShaderConstantF", true, true, Timing::None, nullptr);
Chain<SetVertexShaderConstantFHook> g_vsc("SetVertexShaderConstantF", true, true, Timing::None, nullptr);

// ---- development build: state-call counters for the Frame Profiler (replaces six registered counting callbacks) ----
enum CallSlot : int { kCallSetTexture, kCallSetVS, kCallSetPS, kCallSetVSC, kCallSetPSC, kCallSetRT, kCallSlots };
std::atomic<uint32_t> g_calls[kCallSlots]{};
inline void CountCall(CallSlot s) {
    if constexpr (!kPublicBuild) g_calls[s].store(g_calls[s].load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); // no locked instruction
}

// ---- detours (IDirect3DDevice9 vtable slots) ----
using DrawIndexedPrimitive_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT);
using DrawPrimitive_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT);
using SetRenderTarget_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
using SetPixelShader_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DPixelShader9*);
using SetVertexShader_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, IDirect3DVertexShader9*);
using SetTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*);
using Present_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);
using BeginScene_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*);
using CreateTexture_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
using CreateRenderTarget_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*);
using SetViewport_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const D3DVIEWPORT9*);
using CreatePixelShader_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DPixelShader9**);
using CreateVertexShader_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, const DWORD*, IDirect3DVertexShader9**);
using SetShaderConstantF_t = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, UINT, const float*, UINT);

enum Slot : int {
    kCreateTexture = 23,
    kCreateRenderTarget = 28,
    kSetRenderTarget = 37,
    kBeginScene = 41,
    kSetViewport = 47,
    kSetTexture = 65,
    kDrawPrimitive = 81,
    kDrawIndexedPrimitive = 82,
    kCreateVertexShader = 91,
    kSetVertexShader = 92,
    kSetVertexShaderConstantF = 94,
    kCreatePixelShader = 106,
    kSetPixelShader = 107,
    kSetPixelShaderConstantF = 109,
    kPresent = 17,
};

DrawIndexedPrimitive_t o_dip = nullptr;
DrawPrimitive_t o_dp = nullptr;
SetRenderTarget_t o_srt = nullptr;
SetPixelShader_t o_sps = nullptr;
SetVertexShader_t o_svs = nullptr;
SetTexture_t o_stex = nullptr;
Present_t o_present = nullptr;
BeginScene_t o_begin = nullptr;
CreateTexture_t o_ctex = nullptr;
CreateRenderTarget_t o_crt = nullptr;
SetViewport_t o_svp = nullptr;
CreatePixelShader_t o_cps = nullptr;
CreateVertexShader_t o_cvs = nullptr;
SetShaderConstantF_t o_psc = nullptr;
SetShaderConstantF_t o_vsc = nullptr;

std::atomic<bool> g_installed{false};

HRESULT STDMETHODCALLTYPE H_DrawIndexedPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT bv, UINT mv, UINT nv, UINT si, UINT pc) {
    HRESULT hr = S_OK;
    if (!Run(g_dip, d, hr, t, bv, mv, nv, si, pc)) return hr;
    return o_dip(d, t, bv, mv, nv, si, pc);
}
HRESULT STDMETHODCALLTYPE H_DrawPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT sv, UINT pc) {
    HRESULT hr = S_OK;
    if (!Run(g_dp, d, hr, t, sv, pc)) return hr;
    return o_dp(d, t, sv, pc);
}
HRESULT STDMETHODCALLTYPE H_SetRenderTarget(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s) {
    CountCall(kCallSetRT);
    HRESULT hr = S_OK;
    if (!Run(g_srt, d, hr, i, s)) return hr;
    return o_srt(d, i, s);
}
HRESULT STDMETHODCALLTYPE H_SetPixelShader(IDirect3DDevice9* d, IDirect3DPixelShader9* s) {
    CountCall(kCallSetPS);
    HRESULT hr = S_OK;
    if (!Run(g_sps, d, hr, s)) return hr;
    return o_sps(d, s);
}
HRESULT STDMETHODCALLTYPE H_SetVertexShader(IDirect3DDevice9* d, IDirect3DVertexShader9* s) {
    CountCall(kCallSetVS);
    HRESULT hr = S_OK;
    if (!Run(g_svs, d, hr, s)) return hr;
    return o_svs(d, s);
}
HRESULT STDMETHODCALLTYPE H_SetTexture(IDirect3DDevice9* d, DWORD st, IDirect3DBaseTexture9* t) {
    CountCall(kCallSetTexture);
    HRESULT hr = S_OK;
    if (!Run(g_stex, d, hr, st, t)) return hr;
    return o_stex(d, st, t);
}
HRESULT STDMETHODCALLTYPE H_Present(IDirect3DDevice9* d, const RECT* sr, const RECT* dr, HWND w, const RGNDATA* rg) {
    HRESULT hr = S_OK;
    if (!Run(g_present, d, hr, sr, dr, w, rg)) return hr;
    return o_present(d, sr, dr, w, rg);
}
HRESULT STDMETHODCALLTYPE H_BeginScene(IDirect3DDevice9* d) {
    HRESULT hr = S_OK;
    if (!Run(g_begin, d, hr)) return hr;
    return o_begin(d);
}
HRESULT STDMETHODCALLTYPE H_CreateTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9** t, HANDLE* sh) {
    HRESULT hr = S_OK;
    if (!Run(g_ctex, d, hr, w, h, l, u, f, p, t, sh)) return hr;
    return o_ctex(d, w, h, l, u, f, p, t, sh);
}
HRESULT STDMETHODCALLTYPE H_CreateRenderTarget(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE m, DWORD q, BOOL lk, IDirect3DSurface9** s, HANDLE* sh) {
    HRESULT hr = S_OK;
    if (!Run(g_crt, d, hr, w, h, f, m, q, lk, s, sh)) return hr;
    return o_crt(d, w, h, f, m, q, lk, s, sh);
}
HRESULT STDMETHODCALLTYPE H_SetViewport(IDirect3DDevice9* d, const D3DVIEWPORT9* v) {
    HRESULT hr = S_OK;
    if (!Run(g_svp, d, hr, v)) return hr;
    return o_svp(d, v);
}
HRESULT STDMETHODCALLTYPE H_CreatePixelShader(IDirect3DDevice9* d, const DWORD* fn, IDirect3DPixelShader9** s) {
    HRESULT hr = S_OK;
    if (!Run(g_cps, d, hr, fn, s)) return hr;
    return o_cps(d, fn, s);
}
HRESULT STDMETHODCALLTYPE H_CreateVertexShader(IDirect3DDevice9* d, const DWORD* fn, IDirect3DVertexShader9** s) {
    HRESULT hr = S_OK;
    if (!Run(g_cvs, d, hr, fn, s)) return hr;
    return o_cvs(d, fn, s);
}
HRESULT STDMETHODCALLTYPE H_SetPixelShaderConstantF(IDirect3DDevice9* d, UINT r, const float* c, UINT n) {
    CountCall(kCallSetPSC);
    HRESULT hr = S_OK;
    if (!Run(g_psc, d, hr, r, c, n)) return hr;
    return o_psc(d, r, c, n);
}
HRESULT STDMETHODCALLTYPE H_SetVertexShaderConstantF(IDirect3DDevice9* d, UINT r, const float* c, UINT n) {
    CountCall(kCallSetVSC);
    HRESULT hr = S_OK;
    if (!Run(g_vsc, d, hr, r, c, n)) return hr;
    return o_vsc(d, r, c, n);
}

struct Target {
    const char* name;
    int slot;
    void** original;
    void* detour;
};

// The table the detours are attached from (and detached in the same order)
std::vector<Target> Targets() {
    return {
        {"DrawIndexedPrimitive", kDrawIndexedPrimitive, reinterpret_cast<void**>(&o_dip), reinterpret_cast<void*>(&H_DrawIndexedPrimitive)},
        {"DrawPrimitive", kDrawPrimitive, reinterpret_cast<void**>(&o_dp), reinterpret_cast<void*>(&H_DrawPrimitive)},
        {"SetRenderTarget", kSetRenderTarget, reinterpret_cast<void**>(&o_srt), reinterpret_cast<void*>(&H_SetRenderTarget)},
        {"SetPixelShader", kSetPixelShader, reinterpret_cast<void**>(&o_sps), reinterpret_cast<void*>(&H_SetPixelShader)},
        {"SetVertexShader", kSetVertexShader, reinterpret_cast<void**>(&o_svs), reinterpret_cast<void*>(&H_SetVertexShader)},
        {"SetTexture", kSetTexture, reinterpret_cast<void**>(&o_stex), reinterpret_cast<void*>(&H_SetTexture)},
        {"Present", kPresent, reinterpret_cast<void**>(&o_present), reinterpret_cast<void*>(&H_Present)},
        {"BeginScene", kBeginScene, reinterpret_cast<void**>(&o_begin), reinterpret_cast<void*>(&H_BeginScene)},
        {"CreateTexture", kCreateTexture, reinterpret_cast<void**>(&o_ctex), reinterpret_cast<void*>(&H_CreateTexture)},
        {"CreateRenderTarget", kCreateRenderTarget, reinterpret_cast<void**>(&o_crt), reinterpret_cast<void*>(&H_CreateRenderTarget)},
        {"SetViewport", kSetViewport, reinterpret_cast<void**>(&o_svp), reinterpret_cast<void*>(&H_SetViewport)},
        {"CreatePixelShader", kCreatePixelShader, reinterpret_cast<void**>(&o_cps), reinterpret_cast<void*>(&H_CreatePixelShader)},
        {"CreateVertexShader", kCreateVertexShader, reinterpret_cast<void**>(&o_cvs), reinterpret_cast<void*>(&H_CreateVertexShader)},
        {"SetPixelShaderConstantF", kSetPixelShaderConstantF, reinterpret_cast<void**>(&o_psc), reinterpret_cast<void*>(&H_SetPixelShaderConstantF)},
        {"SetVertexShaderConstantF", kSetVertexShaderConstantF, reinterpret_cast<void**>(&o_vsc), reinterpret_cast<void*>(&H_SetVertexShaderConstantF)},
    };
}

void** VTable(IDirect3DDevice9* device) { return *reinterpret_cast<void***>(device); }

} // namespace

// ---- registration ----
bool RegisterDrawIndexedPrimitive(const std::string& n, DrawIndexedPrimitiveHook h, Priority p) { return Add(g_dip, n, std::move(h), p); }
bool RegisterDrawPrimitive(const std::string& n, DrawPrimitiveHook h, Priority p) { return Add(g_dp, n, std::move(h), p); }
bool RegisterSetRenderTarget(const std::string& n, SetRenderTargetHook h, Priority p) { return Add(g_srt, n, std::move(h), p); }
bool RegisterSetPixelShader(const std::string& n, SetPixelShaderHook h, Priority p) { return Add(g_sps, n, std::move(h), p); }
bool RegisterSetVertexShader(const std::string& n, SetVertexShaderHook h, Priority p) { return Add(g_svs, n, std::move(h), p); }
bool RegisterSetTexture(const std::string& n, SetTextureHook h, Priority p) { return Add(g_stex, n, std::move(h), p); }
bool RegisterPresent(const std::string& n, PresentHook h, Priority p) { return Add(g_present, n, std::move(h), p); }
bool RegisterBeginScene(const std::string& n, BeginSceneHook h, Priority p) { return Add(g_begin, n, std::move(h), p); }
bool RegisterCreateTexture(const std::string& n, CreateTextureHook h, Priority p) { return Add(g_ctex, n, std::move(h), p); }
bool RegisterCreateRenderTarget(const std::string& n, CreateRenderTargetHook h, Priority p) { return Add(g_crt, n, std::move(h), p); }
bool RegisterSetViewport(const std::string& n, SetViewportHook h, Priority p) { return Add(g_svp, n, std::move(h), p); }
bool RegisterCreatePixelShader(const std::string& n, CreatePixelShaderHook h, Priority p) { return Add(g_cps, n, std::move(h), p); }
bool RegisterCreateVertexShader(const std::string& n, CreateVertexShaderHook h, Priority p) { return Add(g_cvs, n, std::move(h), p); }
bool RegisterSetPixelShaderConstantF(const std::string& n, SetPixelShaderConstantFHook h, Priority p) { return Add(g_psc, n, std::move(h), p); }
bool RegisterSetVertexShaderConstantF(const std::string& n, SetVertexShaderConstantFHook h, Priority p) { return Add(g_vsc, n, std::move(h), p); }

void UnregisterAll(const std::string& name) {
    bool lockFreeChanged = false;
    {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        lockFreeChanged |= RemoveName(g_dip, name);
        lockFreeChanged |= RemoveName(g_dp, name);
        lockFreeChanged |= RemoveName(g_srt, name);
        lockFreeChanged |= RemoveName(g_sps, name);
        lockFreeChanged |= RemoveName(g_svs, name);
        lockFreeChanged |= RemoveName(g_stex, name);
        lockFreeChanged |= RemoveName(g_present, name);
        lockFreeChanged |= RemoveName(g_begin, name);
        lockFreeChanged |= RemoveName(g_ctex, name);
        lockFreeChanged |= RemoveName(g_crt, name);
        lockFreeChanged |= RemoveName(g_svp, name);
        lockFreeChanged |= RemoveName(g_cps, name);
        lockFreeChanged |= RemoveName(g_cvs, name);
        lockFreeChanged |= RemoveName(g_psc, name);
        lockFreeChanged |= RemoveName(g_vsc, name);
    }
    // The render thread itself (inside or outside a dispatch) needs no wait: as before, a chain it is running keeps its
    // list until it returns. Any other thread waits, outside the lock, for the render thread to leave the dispatch that
    // may still run a removed callback (the locked dispatches of other threads were already excluded by the lock).
    if (lockFreeChanged && ThreadId() != g_renderTid.load(std::memory_order_relaxed)) WaitForRenderThread(name);
}

// ---- originals ----
HRESULT CallOriginalCreateRenderTarget(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE m, DWORD q, BOOL lk, IDirect3DSurface9** s, HANDLE* sh) {
    return o_crt ? o_crt(d, w, h, f, m, q, lk, s, sh) : d->CreateRenderTarget(w, h, f, m, q, lk, s, sh);
}
HRESULT CallOriginalSetRenderTarget(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s) { return o_srt ? o_srt(d, i, s) : d->SetRenderTarget(i, s); }
HRESULT CallOriginalSetViewport(IDirect3DDevice9* d, const D3DVIEWPORT9* v) { return o_svp ? o_svp(d, v) : d->SetViewport(v); }
HRESULT CallOriginalDrawIndexedPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, INT bv, UINT mv, UINT nv, UINT si, UINT pc) {
    return o_dip ? o_dip(d, t, bv, mv, nv, si, pc) : d->DrawIndexedPrimitive(t, bv, mv, nv, si, pc);
}
HRESULT CallOriginalDrawPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT sv, UINT pc) { return o_dp ? o_dp(d, t, sv, pc) : d->DrawPrimitive(t, sv, pc); }
HRESULT CallOriginalSetVertexShaderConstantF(IDirect3DDevice9* d, UINT r, const float* c, UINT n) {
    return o_vsc ? o_vsc(d, r, c, n) : d->SetVertexShaderConstantF(r, c, n);
}
HRESULT CallOriginalSetPixelShaderConstantF(IDirect3DDevice9* d, UINT r, const float* c, UINT n) {
    return o_psc ? o_psc(d, r, c, n) : d->SetPixelShaderConstantF(r, c, n);
}
HRESULT CallOriginalSetPixelShader(IDirect3DDevice9* d, IDirect3DPixelShader9* s) { return o_sps ? o_sps(d, s) : d->SetPixelShader(s); }
HRESULT CallOriginalSetVertexShader(IDirect3DDevice9* d, IDirect3DVertexShader9* s) { return o_svs ? o_svs(d, s) : d->SetVertexShader(s); }
HRESULT CallOriginalSetTexture(IDirect3DDevice9* d, DWORD st, IDirect3DBaseTexture9* t) { return o_stex ? o_stex(d, st, t) : d->SetTexture(st, t); }

// ---- development build counters ----
StateCallCounts ReadStateCallCounts() {
    StateCallCounts c;
    c.setTexture = g_calls[kCallSetTexture].load(std::memory_order_relaxed);
    c.setVertexShader = g_calls[kCallSetVS].load(std::memory_order_relaxed);
    c.setPixelShader = g_calls[kCallSetPS].load(std::memory_order_relaxed);
    c.setVertexConstants = g_calls[kCallSetVSC].load(std::memory_order_relaxed);
    c.setPixelConstants = g_calls[kCallSetPSC].load(std::memory_order_relaxed);
    c.setRenderTarget = g_calls[kCallSetRT].load(std::memory_order_relaxed);
    return c;
}

uint32_t OffThreadDispatches() { return g_offThread.load(std::memory_order_relaxed); }

// ---- install ----
bool Install(IDirect3DDevice9* device) {
    if (g_installed.load() || !device) return g_installed.load();
    void** vt = VTable(device);
    const std::vector<Target> targets = Targets();
    for (const Target& t : targets) {
        *t.original = vt[t.slot];
        LOG_INFO(std::format("[D3D9Hooks] {} (slot {}) at {}: {}", t.name, t.slot, HookChain::AddressText(vt[t.slot]), HookChain::DescribePrologue(vt[t.slot])));
    }
    // Two slots sharing one function (a wrapper folding methods) would be detoured twice: refuse the whole set.
    for (size_t i = 0; i < targets.size(); i++)
        for (size_t j = i + 1; j < targets.size(); j++)
            if (*targets[i].original == *targets[j].original) {
                LOG_ERROR(std::format("[D3D9Hooks] {} and {} share one function: device hooks not installed", targets[i].name, targets[j].name));
                for (const Target& t : targets) *t.original = nullptr;
                return false;
            }
    // Install runs on the render thread (the game's first EndScene): its draw and state dispatches go lock-free
    g_renderTid.store(ThreadId());
    if (DetourTransactionBegin() != NO_ERROR) return false;
    DetourUpdateThread(GetCurrentThread());
    for (const Target& t : targets) {
        const LONG r = DetourAttach(t.original, t.detour);
        if (r != NO_ERROR) {
            LOG_ERROR(std::format("[D3D9Hooks] DetourAttach({}) failed: {}", t.name, r));
            DetourTransactionAbort();
            for (const Target& u : targets) *u.original = nullptr;
            return false;
        }
    }
    const LONG r = DetourTransactionCommit();
    if (r != NO_ERROR) {
        LOG_ERROR(std::format("[D3D9Hooks] Commit failed: {}", r));
        for (const Target& u : targets) *u.original = nullptr;
        return false;
    }
    g_installed.store(true);
    LOG_INFO(std::format("[D3D9Hooks] Device hooks installed (render thread {}: draw and state hooks dispatched without a lock)", ThreadId()));
    return true;
}

bool IsInstalled() { return g_installed.load(); }

void Uninstall() {
    if (!g_installed.exchange(false)) return;
    if (DetourTransactionBegin() != NO_ERROR) return;
    DetourUpdateThread(GetCurrentThread());
    for (const Target& t : Targets())
        if (*t.original) DetourDetach(t.original, t.detour);
    DetourTransactionCommit();
    // No new dispatch can start; wait for one the render thread may still be running, then free the retired lists
    if (ThreadId() != g_renderTid.load(std::memory_order_relaxed)) WaitForRenderThread("(uninstall)");
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    FreeRetired(g_dip);
    FreeRetired(g_dp);
    FreeRetired(g_srt);
    FreeRetired(g_sps);
    FreeRetired(g_svs);
    FreeRetired(g_stex);
    FreeRetired(g_present);
    FreeRetired(g_begin);
    FreeRetired(g_ctex);
    FreeRetired(g_crt);
    FreeRetired(g_svp);
    FreeRetired(g_cps);
    FreeRetired(g_cvs);
    FreeRetired(g_psc);
    FreeRetired(g_vsc);
}

} // namespace D3D9Hooks
