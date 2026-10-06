#pragma once
// Read-only loaded-world gate shared by the menu notice and Depth Blur.
// Fields: docs/engine/lot-loading-and-streaming.md. Never latch across loads.
#include "game_addresses.h"
#include <Windows.h>

namespace WorldSession {
struct Settled {
    unsigned long long activeAt = 0;
    bool tracking = false;
    bool ready = false;
    void Update(bool active, unsigned long long now) {
        if (!active) { tracking = false; ready = false; return; }
        if (!tracking) { tracking = true; activeAt = now; }
        ready = now - activeAt >= 3000;
    }
};
constexpr bool Active(unsigned char active, int mode) {
    return active != 0 && mode >= 1 && mode <= 3;
}
// Startup/loading window id set by 0x00EC7DB9 and removed by 0x00EC7A60.
// UI service/root/child lookups are the same read-only path used by that callback.
inline bool LoaderDismissed() {
    const uintptr_t getter = GameAddr::Get(GameAddr::Id::UiServiceGetter);
    if (!getter) return false;
    __try {
        const auto* code = reinterpret_cast<const unsigned char*>(getter);
        if (code[0] != 0xA1 || code[5] != 0xC3) return false;
        const uintptr_t global = *reinterpret_cast<const uint32_t*>(code + 1);
        const uintptr_t service = global ? *reinterpret_cast<const uintptr_t*>(global) : 0;
        if (!service) return false;
        const uintptr_t serviceVtable = *reinterpret_cast<const uintptr_t*>(service);
        using RootFn = void*(__thiscall*)(void*);
        const uintptr_t root = reinterpret_cast<uintptr_t>(reinterpret_cast<RootFn>(*reinterpret_cast<const uintptr_t*>(serviceVtable + 4))(reinterpret_cast<void*>(service)));
        if (!root) return false;
        const uintptr_t rootVtable = *reinterpret_cast<const uintptr_t*>(root);
        using ChildFn = void*(__thiscall*)(void*,uint32_t,int);
        return !reinterpret_cast<ChildFn>(*reinterpret_cast<const uintptr_t*>(rootVtable + 0xF4))(reinterpret_cast<void*>(root), 0x95947678u, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline bool IsActive() {
    const uintptr_t global = GameAddr::Get(GameAddr::Id::WorldManagerPtr);
    if (!global) return false;
    __try {
        const uintptr_t world = *reinterpret_cast<const uintptr_t*>(global);
        return world && LoaderDismissed() && Active(*reinterpret_cast<const unsigned char*>(world + 0x41),
                              *reinterpret_cast<const int*>(world + 0x1B4));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The screen effects' gate (Picture, Ambient Occlusion, Edge Smoothing; user 06/10: nothing on the main menu or on a load
// screen). IsActive alone opens during a load from the main menu (the script-driven load screen is not the startup
// window; log 06/10 01:07: Bridgeport's load screen counted as playable), so the world must also have been drawn: at least
// kMinDepthWrites depth-writing draws per frame for kDrawnMs in a row (a load screen draws 0 .. 50, the world hundreds).
// Then it stays open while the world is active, so the save screen and the in-game menus keep the same look; it closes
// with the world (main menu) and waits for the next world to be drawn. Render thread only.
constexpr int kMinDepthWrites = 48;
constexpr unsigned long long kDrawnMs = 750;
}
namespace PostScene { int DepthWritesThisFrame(); }
namespace WorldSession {
inline bool InWorld() {
    static bool open = false;
    static unsigned long long drawnSince = 0;
    if (!IsActive()) {
        open = false;
        drawnSince = 0;
        return false;
    }
    if (open) return true;
    const unsigned long long now = GetTickCount64();
    if (PostScene::DepthWritesThisFrame() >= kMinDepthWrites) {
        if (!drawnSince) drawnSince = now;
        open = now - drawnSince >= kDrawnMs;
    } else drawnSince = 0;
    return open;
}
}
