// Shared trigger for the post-scene effects (see post_scene.h).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "post_scene.h"
#include "d3d9_hooks.h"
#include "depth_share.h"
#include <algorithm>
#include <mutex>
#include <utility>
#include <vector>

namespace {

constexpr const char* kHookName = "PostScene";
constexpr int kMinSceneDraws = 20; // backbuffer draws with depth test before the UI can start

std::mutex g_mutex;
std::vector<std::pair<int, PostScene::Effect>> g_effects; // sorted by order
bool g_hooks = false;
IDirect3DSurface9* g_curRT0 = nullptr;     // identity only
IDirect3DSurface9* g_backBuffer = nullptr; // identity only
int g_sceneDraws = 0;
bool g_done = false;

void OnFrameBoundary(IDirect3DDevice9* dev) {
    IDirect3DSurface9* s = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &s)) && s) {
        g_backBuffer = s;
        s->Release();
    }
    if (!g_curRT0 && SUCCEEDED(dev->GetRenderTarget(0, &s)) && s) {
        g_curRT0 = s;
        s->Release();
    }
    g_sceneDraws = 0;
    g_done = false;
}

void OnGameDraw(IDirect3DDevice9* dev) {
    if (g_done || DepthShare::InternalPass()) return;
    if (!g_curRT0 || g_curRT0 != g_backBuffer) return;
    DWORD z = D3DZB_TRUE;
    dev->GetRenderState(D3DRS_ZENABLE, &z);
    if (z != D3DZB_FALSE) {
        g_sceneDraws++;
        return;
    }
    if (g_sceneDraws < kMinSceneDraws) return;
    g_done = true; // set first so a failure never retries within the frame
    std::vector<std::pair<int, PostScene::Effect>> run;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        run = g_effects;
    }
    for (const auto& e : run) e.second(dev);
}

void RegisterHooks() {
    using namespace D3D9Hooks;
    RegisterPresent(kHookName, [](DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
        OnFrameBoundary(ctx.device);
        return HookAction::Continue;
    }, Priority::First);
    RegisterSetRenderTarget(kHookName, [](DeviceContext&, DWORD index, IDirect3DSurface9* rt) {
        if (index == 0) g_curRT0 = rt;
        return HookAction::Continue;
    }, Priority::First);
    RegisterDrawIndexedPrimitive(kHookName, [](DeviceContext& ctx, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) {
        OnGameDraw(ctx.device);
        return HookAction::Continue;
    }, Priority::First);
    RegisterDrawPrimitive(kHookName, [](DeviceContext& ctx, D3DPRIMITIVETYPE, UINT, UINT) {
        OnGameDraw(ctx.device);
        return HookAction::Continue;
    }, Priority::First);
}

} // namespace

namespace PostScene {

void Add(int order, Effect fn) {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (const auto& e : g_effects)
        if (e.second == fn) return;
    g_effects.push_back({order, fn});
    std::stable_sort(g_effects.begin(), g_effects.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (!g_hooks) {
        g_hooks = true;
        g_curRT0 = nullptr;
        g_backBuffer = nullptr;
        g_sceneDraws = 0;
        g_done = true; // start at the next frame boundary
        RegisterHooks();
    }
}

void Remove(Effect fn) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_effects.erase(std::remove_if(g_effects.begin(), g_effects.end(), [&](const auto& e) { return e.second == fn; }), g_effects.end());
    if (g_effects.empty() && g_hooks) {
        g_hooks = false;
        D3D9Hooks::UnregisterAll(kHookName);
    }
}

} // namespace PostScene
