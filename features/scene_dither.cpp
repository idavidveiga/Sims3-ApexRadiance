// Banding Fix (scene dither)
// The game draws its 3D scene straight into an 8-bit back buffer, so a smooth light gradient (a lamp's pool of light on
// a wall, the fall-off of a room light map) is rounded to 256 steps per channel, and in the dark range each step is a
// visible ring. This adds a fixed, invisible grain to every scene pixel shader's colour before the rounding
// (ShaderPatches::AddDither: triangular noise of +-1 step at the default Strength, from interleaved gradient noise of the
// pixel position), so the rings become a fine grain. The alpha (the bloom mask) is never touched.
//
// How:
//  - At creation (CreatePixelShader, last in the chain) the game's shader is created first, then a dithered copy of it
//    (ps_3_0 only: vPos); the pair is kept by the game shader's pointer. Shaders created before the feature was on get
//    their copy at their first draw.
//  - At each draw (last in the chain, after every observer): when render target 0 is the back buffer and the depth
//    test is on (the 3D scene; the UI draws with it off and reads the back buffer back many times per frame, where a
//    dither would feed on itself), the copy is bound for the draw with its amount constant, then the game's shader and
//    the constant's previous value are put back.
//  - The mouse-pick pass draws into its own 16x16 target, shadows and reflections into their own targets: untouched.
// Offline checks (30/09): all 4907 ps_3_0 shaders of Shaders_Win32.precomp patched and accepted by native D3D9; a grey
// of 20.40 / 100.70 levels comes out as 20.355 / 100.657 on average with the grain (20 / 101 without: the step error).

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
#include <cstring>
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
struct Copy {
    IDirect3DPixelShader9* ps = nullptr; // the dithered copy, null = none
    int amountReg = -1;                 // its constant holding the amount (set around the draw)
    DWORD version = 0;                  // the game shader's version token (Developer view of uncovered surfaces)
};
std::unordered_map<IDirect3DPixelShader9*, Copy> g_copies; // game shader -> its copy
float g_strength = 1.0f;                  // the grain in 8-bit steps (triangular, peak): 1 = +-1 step
bool g_showUncovered = false;             // Developer: draws without a copy in magenta (not saved)
IDirect3DPixelShader9* g_magenta[2] = {}; // ps_2_0, ps_3_0 (made on first use)
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

// The dithered copy of a game shader (Copy::ps null when it cannot have one)
Copy MakeCopy(IDirect3DDevice9* dev, std::vector<DWORD> t) {
    Copy c;
    if (t.empty()) {
        g_refused[static_cast<int>(ShaderPatches::DitherResult::Unreadable)]++;
        return c;
    }
    c.version = t[0];
    const ShaderPatches::DitherResult r = ShaderPatches::AddDither(t, &c.amountReg);
    if (r != ShaderPatches::DitherResult::Ok) {
        g_refused[static_cast<int>(r)]++;
        return c;
    }
    t_own = true;
    const HRESULT hr = D3D9Hooks::CallOriginalCreatePixelShader(dev, t.data(), &c.ps);
    t_own = false;
    if (FAILED(hr) || !c.ps) {
        c.ps = nullptr;
        g_refused[static_cast<int>(ShaderPatches::DitherResult::Unreadable)]++;
        return c;
    }
    g_made++;
    return c;
}

void Remember(IDirect3DPixelShader9* game, const Copy& copy) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto [it, fresh] = g_copies.try_emplace(game, copy);
    if (!fresh) { // an address the game reused for a new shader
        if (it->second.ps) it->second.ps->Release();
        it->second = copy;
    }
}

void ReleaseCopies() {
    std::lock_guard<std::mutex> lock(g_lock);
    for (auto& [game, copy] : g_copies)
        if (copy.ps) copy.ps->Release();
    g_copies.clear();
    for (auto& m : g_magenta)
        if (m) {
            m->Release();
            m = nullptr;
        }
}

// Render thread: the copy for the bound shader (made now when the shader is older than the feature)
Copy CopyOf(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps) {
    {
        std::lock_guard<std::mutex> lock(g_lock);
        const auto it = g_copies.find(ps);
        if (it != g_copies.end()) return it->second;
    }
    const Copy copy = MakeCopy(dev, ReadCode(ps));
    Remember(ps, copy);
    return copy;
}

// Developer view: a flat magenta shader of the game shader's version (a ps_3_0 needs a vs_3_0 and the reverse)
IDirect3DPixelShader9* Magenta(IDirect3DDevice9* dev, DWORD version) {
    const int i = version == 0xFFFF0300u ? 1 : ((version & 0xFFFFFF00u) == 0xFFFF0200u ? 0 : -1);
    if (i < 0) return nullptr;
    if (!g_magenta[i]) {
        const DWORD code[] = {i ? 0xFFFF0300u : 0xFFFF0200u, 0x05000051u, 0xA00F0000u, 0x3F800000u, 0u, 0x3F800000u, 0x3F800000u, // def c0, 1, 0, 1, 1
                              0x02000001u, 0x800F0800u, 0xA0E40000u, 0x0000FFFFu};                                             // mov oC0, c0
        t_own = true;
        if (FAILED(D3D9Hooks::CallOriginalCreatePixelShader(dev, code, &g_magenta[i]))) g_magenta[i] = nullptr;
        t_own = false;
    }
    return g_magenta[i];
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
    const Copy copy = CopyOf(dev, ps);
    if (!copy.ps) {
        g_frameUndithered++;
        if constexpr (!kPublicBuild) {
            if (g_showUncovered)
                if (IDirect3DPixelShader9* m = Magenta(dev, copy.version)) {
                    D3D9Hooks::CallOriginalSetPixelShader(dev, m);
                    draw();
                    D3D9Hooks::CallOriginalSetPixelShader(dev, ps);
                    return D3D9Hooks::HookAction::Skip;
                }
        }
        return D3D9Hooks::HookAction::Continue;
    }
    float before[4] = {};
    dev->GetPixelShaderConstantF(static_cast<UINT>(copy.amountReg), before, 1);
    const float amount[4] = {g_strength / 255.0f, 0, 0, 0};
    D3D9Hooks::CallOriginalSetPixelShaderConstantF(dev, static_cast<UINT>(copy.amountReg), amount, 1);
    D3D9Hooks::CallOriginalSetPixelShader(dev, copy.ps);
    draw();
    D3D9Hooks::CallOriginalSetPixelShader(dev, ps);
    D3D9Hooks::CallOriginalSetPixelShaderConstantF(dev, static_cast<UINT>(copy.amountReg), before, 1); // the game may cache it
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
    SceneDitherPatch() : ApexPatch("SceneDither", nullptr) {
        RegisterFloatSetting(&g_strength, "forca", SettingWidget::Slider, 1.0f, 0.5f, 3.0f, "Grain in 8-bit steps (triangular peak); 1 = +-1 step");
    }

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

    // Read live every draw
    void Update() override { pendingReinstall = false; }

    void RenderCustomUI() override {
        SAFE_IMGUI_BEGIN();
        bool changed = ApexUi::SliderPercent("Strength", &g_strength, 0.5f, 3.0f, "More hides stronger steps, with a slightly more visible grain; 100% is the default",
                                             1.0f);
        ApexUi::IconNote(ApexUi::IconId::Info, "Covers walls, floors, ground, objects and Sims; the sky keeps its own look");
        if (changed) NotifySettingChanged();
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
        ImGui::Checkbox("Show surfaces without the fix in magenta", &g_showUncovered);
        ApexUi::Tooltip("3D scene draws that have no dithered copy (ps_2_0 shaders and refused ones) are drawn in flat magenta (not saved)");
    }
};

APEX_REGISTER_FEATURE(SceneDitherPatch, {.displayName = "Banding Fix",
                                         .description = "Removes the color steps (banding) in lamp light, shadows and other smooth gradients of the 3D world: "
                                                        "an invisible, fixed grain where the game rounds its colors, so light fades smoothly. Menus are "
                                                        "untouched. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
                                         .category = "Graphics",
                                         .enabledByDefault = true,
                                         .supportedVersions = VERSION_ALL,
                                         .technicalDetails = {"A dithered copy of every ps_3_0 pixel shader (triangular noise from interleaved gradient noise of vPos, "
                                                              "+-1/255 at Strength 100% on RGB; alpha untouched), made when the game creates the shader.",
                                                              "Bound only for draws into the back buffer with the depth test on (the 3D scene), last in the draw chain; "
                                                              "the amount constant's previous value is put back after the draw.",
                                                              "ps_2_x shaders (the sky, rugs, foliage, pool water and a few others) have no pixel position and stay as they are."}})
