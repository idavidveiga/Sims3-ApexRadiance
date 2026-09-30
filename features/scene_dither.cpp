// Banding Fix (scene dither)
// The game draws its 3D scene straight into an 8-bit back buffer, so a smooth light gradient (a lamp's pool of light on
// a wall, the fall-off of a room light map) is rounded to 256 steps per channel, and in the dark range each step is a
// visible ring. This adds a fixed, invisible grain to every scene pixel shader's colour before the rounding (triangular
// noise of +-1 step at the default Strength, from interleaved gradient noise of the pixel position), so the rings become
// a fine grain. The alpha (the bloom mask) is never touched.
//
// How:
//  - ps_3_0 (ShaderPatches::AddDither): the pixel position is vPos. ps_2_0 / ps_2_x (AddDither2, 30/09 evening: in game
//    more than half of the scene draws, the walls among them, were ps_2_x): no vPos, so the position comes from a free
//    texture coordinate k that a copy of the paired vertex shader fills with the clip position (AddScreenPosVs), and
//    the pixel copy is ps_2_x (the grain does not fit in the slots of some ps_2_0).
//  - At creation (Create*Shader, last in the chain) the game's shader is created first, then its copy (vertex shaders:
//    the copy for TEXCOORD7, the usual k); pairs are kept by the game shader's pointer. Older shaders get their copies at
//    their first draw.
//  - At each draw (last in the chain, after every observer): when render target 0 is the back buffer and the depth
//    test is on (the 3D scene; the UI draws with it off and reads the back buffer back many times per frame, where a
//    dither would feed on itself), the copies are bound for the draw with the amount constant, then the game's shaders
//    and the constant's previous value are put back.
//  - The mouse-pick pass draws into its own 16x16 target, shadows and reflections into their own targets: untouched.
// Offline checks (30/09, Shaders_Win32.precomp): ps_3_0 4907 / 4907 and ps_2_0 2996 / 3104 patched, vertex shaders
// vs_2_0 4035 / 4338, every copy accepted by native D3D9; a grey of 20.40 / 100.70 levels comes out as 20.355 / 100.657
// on average with the grain (20 / 101 without), for ps_3_0 and for a ps_2_0 + vs_2_0 pair alike.

#include "patch_base.h"
#include "apex_version.h"
#include "apex_log.h"
#include "d3d9_bootstrap.h"
#include "d3d9_hooks.h"
#include "render_callbacks.h"
#include "shader_patches.h"
#include "scene_dither.h"
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
constexpr int kUsualTexcoord = 7;                                       // the texture coordinate most ps_2_x copies use

std::atomic<bool> g_on{false};
thread_local bool t_own = false; // our own creations pass through
std::mutex g_lock;
struct Copy {
    IDirect3DPixelShader9* ps = nullptr; // the dithered copy, null = none
    int amountReg = -1;                 // its constant holding the amount (set around the draw)
    int texcoord = -1;                  // ps_2_x copies: the texture coordinate with the clip position (-1: ps_3_0, vPos)
    DWORD version = 0;                  // the game shader's version token
};
std::unordered_map<IDirect3DPixelShader9*, Copy> g_copies; // game pixel shader -> its copy
// game vertex shader -> its copies writing the clip position to TEXCOORDk (null = cannot), key = pointer * 8 + k
std::unordered_map<uint64_t, IDirect3DVertexShader9*> g_vsCopies;
float g_strength = 1.0f;                  // the grain in 8-bit steps (triangular, peak): 1 = +-1 step
bool g_showCovered = false;               // Developer: a coarse grain where the fix applies (not saved)
constexpr float kShowCoveredSteps = 24.0f;
IDirect3DSurface9* g_backBuffer = nullptr; // identity only (render thread)

// statistics (Developer page, dev log)
std::atomic<unsigned> g_made{0}, g_madeVs{0}, g_refused[7] = {};
struct FrameCount {
    unsigned dithered3 = 0, dithered2 = 0; // 3D scene draws with the grain: ps_3_0, ps_2_x
    unsigned ps2NoPair = 0;                // ps_2_x with a copy whose vertex shader could not carry the position
    unsigned ps2Refused = 0, ps3Refused = 0, other = 0;
};
FrameCount g_frame, g_last;
unsigned long long g_lastLog = 0;
int g_logs = 0;

uint64_t VsKey(IDirect3DVertexShader9* vs, int k) { return (reinterpret_cast<uint64_t>(vs) << 3) | static_cast<uint64_t>(k & 7); }

// Tokens up to and with the end token (no length is given to Create*Shader), 0 when unreadable
size_t CodeLength(const DWORD* fn) {
    __try {
        if ((fn[0] >> 17) != 0x7FFFu) return 0; // 0xFFFF.... pixel, 0xFFFE.... vertex
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

template <typename Shader> std::vector<DWORD> ReadCode(Shader* s) {
    UINT size = 0;
    if (FAILED(s->GetFunction(nullptr, &size)) || size < 8 || size % 4) return {};
    std::vector<DWORD> t(size / 4);
    if (FAILED(s->GetFunction(t.data(), &size))) return {};
    return t;
}

// The dithered copy of a game pixel shader (Copy::ps null when it cannot have one)
Copy MakeCopy(IDirect3DDevice9* dev, std::vector<DWORD> t) {
    Copy c;
    if (t.empty()) {
        g_refused[static_cast<int>(ShaderPatches::DitherResult::Unreadable)]++;
        return c;
    }
    c.version = t[0];
    const bool ps2 = t[0] == 0xFFFF0200u || t[0] == 0xFFFF0201u;
    const ShaderPatches::DitherResult r = ps2 ? ShaderPatches::AddDither2(t, &c.amountReg, &c.texcoord) : ShaderPatches::AddDither(t, &c.amountReg);
    if (r != ShaderPatches::DitherResult::Ok) {
        g_refused[static_cast<int>(r)]++;
        c.texcoord = -1;
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

// The copy of a game vertex shader that also writes the clip position to TEXCOORDk, or null
IDirect3DVertexShader9* MakeVsCopy(IDirect3DDevice9* dev, std::vector<DWORD> t, int k) {
    if (t.empty() || !ShaderPatches::AddScreenPosVs(t, k)) return nullptr;
    IDirect3DVertexShader9* copy = nullptr;
    t_own = true;
    const HRESULT hr = D3D9Hooks::CallOriginalCreateVertexShader(dev, t.data(), &copy);
    t_own = false;
    if (FAILED(hr)) return nullptr;
    g_madeVs++;
    return copy;
}

void Remember(IDirect3DPixelShader9* game, const Copy& copy) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto [it, fresh] = g_copies.try_emplace(game, copy);
    if (!fresh) { // an address the game reused for a new shader
        if (it->second.ps) it->second.ps->Release();
        it->second = copy;
    }
}

void RememberVs(uint64_t key, IDirect3DVertexShader9* copy) {
    std::lock_guard<std::mutex> lock(g_lock);
    auto [it, fresh] = g_vsCopies.try_emplace(key, copy);
    if (!fresh) {
        if (it->second) it->second->Release();
        it->second = copy;
    }
}

// A new game vertex shader at an address: its old copies (every k) go
void ForgetVs(IDirect3DVertexShader9* vs) {
    std::lock_guard<std::mutex> lock(g_lock);
    for (int k = 0; k < 8; k++) {
        const auto it = g_vsCopies.find(VsKey(vs, k));
        if (it == g_vsCopies.end()) continue;
        if (it->second) it->second->Release();
        g_vsCopies.erase(it);
    }
}

void ReleaseCopies() {
    std::lock_guard<std::mutex> lock(g_lock);
    for (auto& [game, copy] : g_copies)
        if (copy.ps) copy.ps->Release();
    g_copies.clear();
    for (auto& [key, copy] : g_vsCopies)
        if (copy) copy->Release();
    g_vsCopies.clear();
}

// Render thread: the copy for the bound pixel shader (made now when the shader is older than the feature)
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

// Render thread: the vertex copy writing TEXCOORDk (made now when missing)
IDirect3DVertexShader9* VsCopyOf(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs, int k) {
    const uint64_t key = VsKey(vs, k);
    {
        std::lock_guard<std::mutex> lock(g_lock);
        const auto it = g_vsCopies.find(key);
        if (it != g_vsCopies.end()) return it->second;
    }
    IDirect3DVertexShader9* copy = MakeVsCopy(dev, ReadCode(vs), k);
    RememberVs(key, copy);
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
    const Copy copy = CopyOf(dev, ps);
    const DWORD major = (copy.version >> 8) & 0xFF;
    if (!copy.ps) {
        (major == 2 ? g_frame.ps2Refused : major == 3 ? g_frame.ps3Refused : g_frame.other)++;
        return D3D9Hooks::HookAction::Continue;
    }
    IDirect3DVertexShader9 *vs = nullptr, *vsCopy = nullptr;
    if (copy.texcoord >= 0) { // ps_2_x: the vertex shader must carry the position
        dev->GetVertexShader(&vs);
        if (vs) {
            vs->Release();
            vsCopy = VsCopyOf(dev, vs, copy.texcoord);
        }
        if (!vsCopy) {
            g_frame.ps2NoPair++;
            return D3D9Hooks::HookAction::Continue;
        }
    }
    D3DVIEWPORT9 vp{};
    dev->GetViewport(&vp);
    float before[4] = {};
    dev->GetPixelShaderConstantF(static_cast<UINT>(copy.amountReg), before, 1);
    // Developer "Show covered surfaces": a coarse grain only where the fix applies
    const float amount[4] = {(g_showCovered ? kShowCoveredSteps : g_strength) / 255.0f, 0.5f * static_cast<float>(vp.Width), 0.5f * static_cast<float>(vp.Height), 0};
    D3D9Hooks::CallOriginalSetPixelShaderConstantF(dev, static_cast<UINT>(copy.amountReg), amount, 1);
    D3D9Hooks::CallOriginalSetPixelShader(dev, copy.ps);
    if (vsCopy) D3D9Hooks::CallOriginalSetVertexShader(dev, vsCopy);
    draw();
    if (vsCopy) D3D9Hooks::CallOriginalSetVertexShader(dev, vs);
    D3D9Hooks::CallOriginalSetPixelShader(dev, ps);
    D3D9Hooks::CallOriginalSetPixelShaderConstantF(dev, static_cast<UINT>(copy.amountReg), before, 1); // the game may cache it
    (vsCopy ? g_frame.dithered2 : g_frame.dithered3)++;
    return D3D9Hooks::HookAction::Skip;
}

void OnFrameBoundary(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        g_backBuffer = bb;
        bb->Release();
    }
    g_last = g_frame;
    g_frame = {};
    // Development build: the coverage in the log now and then (the first 12 times, every 20 s of scene)
    if constexpr (!kPublicBuild) {
        const unsigned long long now = GetTickCount64();
        const unsigned total = g_last.dithered3 + g_last.dithered2 + g_last.ps2NoPair + g_last.ps2Refused + g_last.ps3Refused + g_last.other;
        if (total > 50 && g_logs < 12 && now - g_lastLog >= 20000) {
            g_lastLog = now;
            g_logs++;
            LOG_INFO(std::format("[SceneDither] Last frame, 3D scene draws: {} dithered (ps_3_0), {} dithered (ps_2_x), {} ps_2_x whose vertex shader "
                                 "could not carry the position, {} ps_2_x refused, {} ps_3_0 refused, {} other | copies made: pixel {}, vertex {}; "
                                 "refused: no colour write {}, subroutines {}, relative constants {}, no free register {}, unreadable {}",
                                 g_last.dithered3, g_last.dithered2, g_last.ps2NoPair, g_last.ps2Refused, g_last.ps3Refused, g_last.other, g_made.load(),
                                 g_madeVs.load(), g_refused[2].load(), g_refused[3].load(), g_refused[4].load(), g_refused[5].load(), g_refused[6].load()));
        }
    }
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
    RegisterCreateVertexShader(kHookName, [](DeviceContext& ctx, const DWORD* fn, IDirect3DVertexShader9** out) {
        if (t_own || !fn || !out || !g_on.load()) return HookAction::Continue;
        if (FAILED(CallOriginalCreateVertexShader(ctx.device, fn, out)) || !*out) return HookAction::Block;
        ForgetVs(*out); // a reused address: the old shader's copies must go
        std::vector<DWORD> t = ReadCode(fn);
        if (!t.empty() && t[0] != 0xFFFE0300u) RememberVs(VsKey(*out, kUsualTexcoord), MakeVsCopy(ctx.device, std::move(t), kUsualTexcoord));
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
        LOG_INFO(std::format("[SceneDither] Uninstalled (copies made this session: pixel {}, vertex {})", g_made.load(), g_madeVs.load()));
        return true;
    }

    // Read live every draw
    void Update() override { pendingReinstall = false; }

    void RenderCustomUI() override {
        SAFE_IMGUI_BEGIN();
        if (ApexUi::SliderPercent("Strength", &g_strength, 0.5f, 3.0f, "More hides stronger steps, with a slightly more visible grain; 100% is the default", 1.0f))
            NotifySettingChanged();
    }

    // Developer page > Debug views
    void RenderDeveloperUI() override {
        SAFE_IMGUI_BEGIN();
        size_t pairs = 0, vsPairs = 0;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            pairs = g_copies.size();
            vsPairs = g_vsCopies.size();
        }
        ImGui::TextDisabled("Pixel shaders seen: %zu (copies %u)  |  vertex copies asked: %zu (made %u)", pairs, g_made.load(), vsPairs, g_madeVs.load());
        ImGui::TextDisabled("Refused: no colour write %u, subroutines %u, relative constants %u, no free register %u, unreadable %u", g_refused[2].load(),
                            g_refused[3].load(), g_refused[4].load(), g_refused[5].load(), g_refused[6].load());
        const unsigned dithered = g_last.dithered3 + g_last.dithered2;
        const unsigned total = dithered + g_last.ps2NoPair + g_last.ps2Refused + g_last.ps3Refused + g_last.other;
        ImGui::TextDisabled("Last frame, 3D scene draws: %u with the grain (%.0f%%: ps_3_0 %u, ps_2_x %u); without: %u ps_2_x (vertex shader), %u ps_2_x, "
                            "%u ps_3_0 refused, %u other",
                            dithered, total ? 100.0 * dithered / total : 0.0, g_last.dithered3, g_last.dithered2, g_last.ps2NoPair, g_last.ps2Refused,
                            g_last.ps3Refused, g_last.other);
        ImGui::Checkbox("Show covered surfaces", &g_showCovered);
        ApexUi::Tooltip("A coarse grain on every surface the fix covers; smooth surfaces are not covered (not saved)");
    }
};

APEX_REGISTER_FEATURE(SceneDitherPatch, {.displayName = "Banding Fix",
                                         .description = "Removes the color steps (banding) in lamp light, shadows and other smooth gradients of the 3D world: "
                                                        "an invisible, fixed grain where the game rounds its colors, so light fades smoothly. Menus are "
                                                        "untouched. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
                                         .category = "Graphics",
                                         .enabledByDefault = true,
                                         .supportedVersions = VERSION_ALL,
                                         .technicalDetails = {"A dithered copy of every pixel shader (triangular noise from interleaved gradient noise of the pixel "
                                                              "position, +-1/255 at Strength 100% on RGB; alpha untouched), made when the game creates the shader.",
                                                              "ps_2_x copies take the pixel position from a texture coordinate that a copy of the vertex shader fills "
                                                              "with the clip position.",
                                                              "Bound only for draws into the back buffer with the depth test on (the 3D scene), last in the draw chain; "
                                                              "the amount constant's previous value is put back after the draw."}})

namespace SceneDither {
bool On() { return g_on.load(std::memory_order_relaxed); }
} // namespace SceneDither
