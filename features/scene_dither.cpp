// Banding Fix (scene dither)
// The game draws its 3D scene straight into an 8-bit back buffer, so a smooth light gradient (a lamp's pool of light on
// a wall, the fall-off of a room light map) is rounded to 256 steps per channel, and in the dark range each step is a
// visible ring. This adds a fixed, invisible grain of +-0.5 of a step to every scene pixel shader's colour before the
// rounding (ShaderPatches::AddDither: interleaved gradient noise of the pixel position), so the rings become a fine
// grain. The alpha (the bloom mask) is never touched.
//
// How:
//  - At creation (CreatePixelShader, last in the chain) the game's shader is created first, then a dithered copy of it
//    (ps_3_0 only: vPos); the pair is kept by the game shader's pointer. Shaders created before the feature was on get
//    their copy at their first draw.
//  - At each draw (last in the chain, after every observer): when render target 0 is the back buffer and the depth
//    test is on (the 3D scene; the UI draws with it off and reads the back buffer back many times per frame, where a
//    dither would feed on itself), the copy is bound for the draw and the game's shader put back right after.
//  - The mouse-pick pass draws into its own 16x16 target, shadows and reflections into their own targets: untouched.
// Offline check (30/09): all 4907 ps_3_0 shaders of Shaders_Win32.precomp patched and accepted by native D3D9.

#include "patch_base.h"
#include "apex_version.h"
#include "apex_log.h"
#include "d3d9_bootstrap.h"
#include "d3d9_hooks.h"
#include "render_callbacks.h"
#include "shader_patches.h"
#include "imgui.h"
#include "ui/violet_theme.h"
#include "ui/widgets.h"
#include "build_flavor.h"
#include <d3d9.h>
#include <atomic>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char* kHookName = "SceneDither";
constexpr auto kAfterEveryone = static_cast<D3D9Hooks::Priority>(1000); // after Last: every observer has seen the draw
constexpr size_t kMaxShaderTokens = 16384;                              // 64 KB

std::atomic<bool> g_on{false};
thread_local bool t_own = false; // our own creations pass through
std::mutex g_lock;
std::unordered_map<IDirect3DPixelShader9*, IDirect3DPixelShader9*> g_copies; // game shader -> dithered copy (null = none)
IDirect3DSurface9* g_backBuffer = nullptr; // identity only (render thread)

// statistics (Developer page)
std::atomic<unsigned> g_made{0}, g_refused[7] = {};
unsigned g_frameDithered = 0, g_frameUndithered = 0, g_lastDithered = 0, g_lastUndithered = 0;

// Tokens up to and with the end token (no length is given to CreatePixelShader), 0 when unreadable
size_t CodeLength(const DWORD* fn) {
    __try {
        if ((fn[0] & 0xFFFF0000u) != 0xFFFF0000u) return 0;
        for (size_t i = 1; i < kMaxShaderTokens; i++)
            if (fn[i] == 0x0000FFFFu) return i + 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

std::vector<DWORD> ReadCode(const DWORD* fn) {
    const size_t n = CodeLength(fn);
    return n ? std::vector<DWORD>(fn, fn + n) : std::vector<DWORD>{};
}

std::vector<DWORD> ReadCode(IDirect3DPixelShader9* ps) {
    UINT size = 0;
    if (FAILED(ps->GetFunction(nullptr, &size)) || size < 8 || size % 4) return {};
    std::vector<DWORD> t(size / 4);
    if (FAILED(ps->GetFunction(t.data(), &size))) return {};
    return t;
}

// The dithered copy of a game shader, or null when it cannot have one
IDirect3DPixelShader9* MakeCopy(IDirect3DDevice9* dev, std::vector<DWORD> t) {
    if (t.empty()) {
        g_refused[static_cast<int>(ShaderPatches::DitherResult::Unreadable)]++;
        return nullptr;
    }
    const ShaderPatches::DitherResult r = ShaderPatches::AddDither(t);
    if (r != ShaderPatches::DitherResult::Ok) {
        g_refused[static_cast<int>(r)]++;
        return nullptr;
    }
    IDirect3DPixelShader9* copy = nullptr;
    t_own = true;
    const HRESULT hr = D3D9Hooks::CallOriginalCreatePixelShader(dev, t.data(), &copy);
    t_own = false;
    if (FAILED(hr) || !copy) {
        g_refused[static_cast<int>(ShaderPatches::DitherResult::Unreadable)]++;
        return nullptr;
    }
    g_made++;
    return copy;
}

void Remember(IDirect3DPixelShader9* game, IDirect3DPixelShader9* copy) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto [it, fresh] = g_copies.try_emplace(game, copy);
    if (!fresh) { // an address the game reused for a new shader
        if (it->second) it->second->Release();
        it->second = copy;
    }
}

void ReleaseCopies() {
    std::lock_guard<std::mutex> lock(g_lock);
    for (auto& [game, copy] : g_copies)
        if (copy) copy->Release();
    g_copies.clear();
}

// Render thread: the copy for the bound shader (made now when the shader is older than the feature)
IDirect3DPixelShader9* CopyOf(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps) {
    {
        std::lock_guard<std::mutex> lock(g_lock);
        const auto it = g_copies.find(ps);
        if (it != g_copies.end()) return it->second;
    }
    IDirect3DPixelShader9* copy = MakeCopy(dev, ReadCode(ps));
    Remember(ps, copy);
    return copy;
}

template <typename DrawFn> D3D9Hooks::HookAction OnDraw(IDirect3DDevice9* dev, DrawFn draw) {
    if (!g_on.load(std::memory_order_relaxed) || !g_backBuffer) return D3D9Hooks::HookAction::Continue;
    IDirect3DSurface9* rt = nullptr;
    dev->GetRenderTarget(0, &rt);
    const bool toScreen = rt && rt == g_backBuffer;
    if (rt) rt->Release();
    if (!toScreen) return D3D9Hooks::HookAction::Continue;
    DWORD z = D3DZB_FALSE;
    dev->GetRenderState(D3DRS_ZENABLE, &z);
    if (z == D3DZB_FALSE) return D3D9Hooks::HookAction::Continue; // UI and 2D passes
    IDirect3DPixelShader9* ps = nullptr;
    dev->GetPixelShader(&ps);
    if (!ps) return D3D9Hooks::HookAction::Continue;
    ps->Release(); // the device keeps it alive
    IDirect3DPixelShader9* copy = CopyOf(dev, ps);
    if (!copy) {
        g_frameUndithered++;
        return D3D9Hooks::HookAction::Continue;
    }
    D3D9Hooks::CallOriginalSetPixelShader(dev, copy);
    draw();
    D3D9Hooks::CallOriginalSetPixelShader(dev, ps);
    g_frameDithered++;
    return D3D9Hooks::HookAction::Skip;
}

void OnFrameBoundary(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        g_backBuffer = bb;
        bb->Release();
    }
    g_lastDithered = g_frameDithered;
    g_lastUndithered = g_frameUndithered;
    g_frameDithered = g_frameUndithered = 0;
}

void OnPreReset(IDirect3DDevice9*) { g_backBuffer = nullptr; } // read again at the next frame boundary

void RegisterHooks() {
    using namespace D3D9Hooks;
    RegisterPresent(kHookName, [](DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
        OnFrameBoundary(ctx.device);
        return HookAction::Continue;
    }, Priority::First);
    RegisterCreatePixelShader(kHookName, [](DeviceContext& ctx, const DWORD* fn, IDirect3DPixelShader9** out) {
        if (t_own || !fn || !out || !g_on.load()) return HookAction::Continue;
        // create the game's shader here (every earlier callback already ran) to learn its pointer, then its copy
        if (FAILED(CallOriginalCreatePixelShader(ctx.device, fn, out)) || !*out) return HookAction::Block;
        Remember(*out, MakeCopy(ctx.device, ReadCode(fn)));
        return HookAction::Skip;
    }, kAfterEveryone);
    RegisterDrawIndexedPrimitive(kHookName, [](DeviceContext& ctx, D3DPRIMITIVETYPE type, INT bvi, UINT minV, UINT numV, UINT start, UINT prims) {
        return OnDraw(ctx.device, [&] { CallOriginalDrawIndexedPrimitive(ctx.device, type, bvi, minV, numV, start, prims); });
    }, kAfterEveryone);
    RegisterDrawPrimitive(kHookName, [](DeviceContext& ctx, D3DPRIMITIVETYPE type, UINT start, UINT prims) {
        return OnDraw(ctx.device, [&] { CallOriginalDrawPrimitive(ctx.device, type, start, prims); });
    }, kAfterEveryone);
    RenderCallbacks::Add(RenderCallbacks::preReset, OnPreReset);
}

} // namespace

class SceneDitherPatch : public ApexPatch {
  public:
    SceneDitherPatch() : ApexPatch("SceneDither", nullptr) {}

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        g_backBuffer = nullptr;
        g_on = true;
        RegisterHooks();
        isEnabled = true;
        LOG_INFO("[SceneDither] Installed");
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        lastError.clear();
        g_on = false;
        D3D9Hooks::UnregisterAll(kHookName);
        RenderCallbacks::Remove(RenderCallbacks::preReset, OnPreReset);
        ReleaseCopies(); // an address reused while off must never meet an old copy
        g_backBuffer = nullptr;
        isEnabled = false;
        LOG_INFO(std::format("[SceneDither] Uninstalled ({} copies made this session)", g_made.load()));
        return true;
    }

    void Update() override { pendingReinstall = false; }

    // No options: the switch in the card header is all; a line on what it covers
    void RenderCustomUI() override {
        SAFE_IMGUI_BEGIN();
        ApexUi::IconNote(ApexUi::IconId::Info, "Covers walls, floors, ground, objects and Sims; the sky keeps its own look");
    }

    // Developer page > Debug views
    void RenderDeveloperUI() override {
        SAFE_IMGUI_BEGIN();
        size_t pairs = 0;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            pairs = g_copies.size();
        }
        ImGui::TextDisabled("Shaders seen: %zu  |  dithered copies made: %u", pairs, g_made.load());
        ImGui::TextDisabled("Refused: not ps_3_0 %u, no colour write %u, subroutines %u, relative constants %u, no free register %u, unreadable %u",
                            g_refused[1].load(), g_refused[2].load(), g_refused[3].load(), g_refused[4].load(), g_refused[5].load(), g_refused[6].load());
        const unsigned total = g_lastDithered + g_lastUndithered;
        ImGui::TextDisabled("Last frame, 3D scene draws: %u dithered, %u without a copy (%.0f%% covered)", g_lastDithered, g_lastUndithered,
                            total ? 100.0 * g_lastDithered / total : 0.0);
    }
};

APEX_REGISTER_FEATURE(SceneDitherPatch, {.displayName = "Banding Fix",
                                         .description = "Removes the color steps (banding) in lamp light, shadows and other smooth gradients of the 3D world: "
                                                        "an invisible, fixed grain where the game rounds its colors, so light fades smoothly. Menus are "
                                                        "untouched. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
                                         .category = "Graphics",
                                         .enabledByDefault = true,
                                         .supportedVersions = VERSION_ALL,
                                         .technicalDetails = {"A dithered copy of every ps_3_0 pixel shader (interleaved gradient noise of vPos, +-0.5/255 on RGB; "
                                                              "alpha untouched), made when the game creates the shader.",
                                                              "Bound only for draws into the back buffer with the depth test on (the 3D scene), last in the draw chain.",
                                                              "ps_2_x shaders (the sky, rugs, foliage, pool water and a few others) have no pixel position and stay as they are."}})
