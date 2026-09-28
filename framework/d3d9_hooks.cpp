#include "d3d9_hooks.h"
#include "apex_log.h"
#include "hook_chain.h"
#include "frame_profiler.h"
#include <detours/detours.h>
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
std::recursive_mutex g_lock; // held while a chain runs and while chains change
uint64_t g_sequence = 0;     // registration order (ties between equal priorities)

template <typename Fn> struct Entry {
    std::string name;
    Fn fn;
    int priority;
    uint64_t sequence;
};

// A chain's list is immutable once published: registering builds a new list, so a chain that is running (possibly the
// caller of Register) keeps iterating over its own copy.
template <typename Fn> struct Chain {
    std::shared_ptr<const std::vector<Entry<Fn>>> list = std::make_shared<std::vector<Entry<Fn>>>();
    std::atomic<size_t> count{0}; // lock-free "nothing registered" test for the hot paths
};

template <typename Fn> bool Add(Chain<Fn>& chain, const std::string& name, Fn fn, Priority priority) {
    if (!fn) return false;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto next = std::make_shared<std::vector<Entry<Fn>>>(*chain.list);
    next->push_back({name, std::move(fn), static_cast<int>(priority), ++g_sequence});
    std::stable_sort(next->begin(), next->end(), [](const Entry<Fn>& a, const Entry<Fn>& b) {
        return a.priority != b.priority ? a.priority < b.priority : a.sequence < b.sequence;
    });
    chain.count.store(next->size());
    chain.list = std::move(next);
    return true;
}

template <typename Fn> void RemoveName(Chain<Fn>& chain, const std::string& name) {
    auto next = std::make_shared<std::vector<Entry<Fn>>>();
    for (const auto& e : *chain.list)
        if (e.name != name) next->push_back(e);
    if (next->size() == chain.list->size()) return;
    chain.count.store(next->size());
    chain.list = std::move(next);
}

std::mutex g_timingLock; // FrameProfiler::AddRegistryHookTime is not thread-safe on its own

// Runs the chain. false = a callback asked to skip the device call; result then holds what the game gets back.
template <typename Fn, typename... Args> bool Run(Chain<Fn>& chain, bool timed, IDirect3DDevice9* device, HRESULT& result, Args... args) {
    if (chain.count.load(std::memory_order_relaxed) == 0) return true;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const std::shared_ptr<const std::vector<Entry<Fn>>> list = chain.list;
    DeviceContext ctx{device};
    const bool prof = timed && FrameProfiler::RegistryHookTimingActive();
    for (const auto& e : *list) {
        const uint64_t t0 = prof ? FrameProfiler::Ticks() : 0;
        const HookAction r = e.fn(ctx, args...);
        if (prof) {
            const uint64_t dt = FrameProfiler::Ticks() - t0;
            std::lock_guard<std::mutex> tl(g_timingLock);
            FrameProfiler::AddRegistryHookTime(e.name, dt);
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

Chain<DrawIndexedPrimitiveHook> g_dip;
Chain<DrawPrimitiveHook> g_dp;
Chain<SetRenderTargetHook> g_srt;
Chain<SetPixelShaderHook> g_sps;
Chain<SetVertexShaderHook> g_svs;
Chain<SetTextureHook> g_stex;
Chain<PresentHook> g_present;
Chain<BeginSceneHook> g_begin;
Chain<CreateTextureHook> g_ctex;
Chain<CreateRenderTargetHook> g_crt;
Chain<SetViewportHook> g_svp;
Chain<CreatePixelShaderHook> g_cps;
Chain<CreateVertexShaderHook> g_cvs;
Chain<SetPixelShaderConstantFHook> g_psc;
Chain<SetVertexShaderConstantFHook> g_vsc;

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
    if (!Run(g_dip, true, d, hr, t, bv, mv, nv, si, pc)) return hr;
    return o_dip(d, t, bv, mv, nv, si, pc);
}
HRESULT STDMETHODCALLTYPE H_DrawPrimitive(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT sv, UINT pc) {
    HRESULT hr = S_OK;
    if (!Run(g_dp, true, d, hr, t, sv, pc)) return hr;
    return o_dp(d, t, sv, pc);
}
HRESULT STDMETHODCALLTYPE H_SetRenderTarget(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s) {
    HRESULT hr = S_OK;
    if (!Run(g_srt, false, d, hr, i, s)) return hr;
    return o_srt(d, i, s);
}
HRESULT STDMETHODCALLTYPE H_SetPixelShader(IDirect3DDevice9* d, IDirect3DPixelShader9* s) {
    HRESULT hr = S_OK;
    if (!Run(g_sps, false, d, hr, s)) return hr;
    return o_sps(d, s);
}
HRESULT STDMETHODCALLTYPE H_SetVertexShader(IDirect3DDevice9* d, IDirect3DVertexShader9* s) {
    HRESULT hr = S_OK;
    if (!Run(g_svs, false, d, hr, s)) return hr;
    return o_svs(d, s);
}
HRESULT STDMETHODCALLTYPE H_SetTexture(IDirect3DDevice9* d, DWORD st, IDirect3DBaseTexture9* t) {
    HRESULT hr = S_OK;
    if (!Run(g_stex, false, d, hr, st, t)) return hr;
    return o_stex(d, st, t);
}
HRESULT STDMETHODCALLTYPE H_Present(IDirect3DDevice9* d, const RECT* sr, const RECT* dr, HWND w, const RGNDATA* rg) {
    HRESULT hr = S_OK;
    if (!Run(g_present, false, d, hr, sr, dr, w, rg)) return hr;
    return o_present(d, sr, dr, w, rg);
}
HRESULT STDMETHODCALLTYPE H_BeginScene(IDirect3DDevice9* d) {
    HRESULT hr = S_OK;
    if (!Run(g_begin, false, d, hr)) return hr;
    return o_begin(d);
}
HRESULT STDMETHODCALLTYPE H_CreateTexture(IDirect3DDevice9* d, UINT w, UINT h, UINT l, DWORD u, D3DFORMAT f, D3DPOOL p, IDirect3DTexture9** t, HANDLE* sh) {
    HRESULT hr = S_OK;
    if (!Run(g_ctex, false, d, hr, w, h, l, u, f, p, t, sh)) return hr;
    return o_ctex(d, w, h, l, u, f, p, t, sh);
}
HRESULT STDMETHODCALLTYPE H_CreateRenderTarget(IDirect3DDevice9* d, UINT w, UINT h, D3DFORMAT f, D3DMULTISAMPLE_TYPE m, DWORD q, BOOL lk, IDirect3DSurface9** s, HANDLE* sh) {
    HRESULT hr = S_OK;
    if (!Run(g_crt, false, d, hr, w, h, f, m, q, lk, s, sh)) return hr;
    return o_crt(d, w, h, f, m, q, lk, s, sh);
}
HRESULT STDMETHODCALLTYPE H_SetViewport(IDirect3DDevice9* d, const D3DVIEWPORT9* v) {
    HRESULT hr = S_OK;
    if (!Run(g_svp, false, d, hr, v)) return hr;
    return o_svp(d, v);
}
HRESULT STDMETHODCALLTYPE H_CreatePixelShader(IDirect3DDevice9* d, const DWORD* fn, IDirect3DPixelShader9** s) {
    HRESULT hr = S_OK;
    if (!Run(g_cps, false, d, hr, fn, s)) return hr;
    return o_cps(d, fn, s);
}
HRESULT STDMETHODCALLTYPE H_CreateVertexShader(IDirect3DDevice9* d, const DWORD* fn, IDirect3DVertexShader9** s) {
    HRESULT hr = S_OK;
    if (!Run(g_cvs, false, d, hr, fn, s)) return hr;
    return o_cvs(d, fn, s);
}
HRESULT STDMETHODCALLTYPE H_SetPixelShaderConstantF(IDirect3DDevice9* d, UINT r, const float* c, UINT n) {
    HRESULT hr = S_OK;
    if (!Run(g_psc, false, d, hr, r, c, n)) return hr;
    return o_psc(d, r, c, n);
}
HRESULT STDMETHODCALLTYPE H_SetVertexShaderConstantF(IDirect3DDevice9* d, UINT r, const float* c, UINT n) {
    HRESULT hr = S_OK;
    if (!Run(g_vsc, false, d, hr, r, c, n)) return hr;
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
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    RemoveName(g_dip, name);
    RemoveName(g_dp, name);
    RemoveName(g_srt, name);
    RemoveName(g_sps, name);
    RemoveName(g_svs, name);
    RemoveName(g_stex, name);
    RemoveName(g_present, name);
    RemoveName(g_begin, name);
    RemoveName(g_ctex, name);
    RemoveName(g_crt, name);
    RemoveName(g_svp, name);
    RemoveName(g_cps, name);
    RemoveName(g_cvs, name);
    RemoveName(g_psc, name);
    RemoveName(g_vsc, name);
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
    LOG_INFO("[D3D9Hooks] Device hooks installed");
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
}

} // namespace D3D9Hooks
