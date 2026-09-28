// Rig tracker (part of Night Lighting)
//
// A SceneModel part is drawn by FUN_006f6250 (thiscall(model, part, ctx) ret 8; vtable 0xFF97F8 +0x18, also called
// directly from 0x6F83B0 / 0x6FA0B0). It binds the part's light rig with FUN_006b8b30 (__fastcall rig) at 0x6F68C5,
// which only stores pointers to rig memory in the game's shader parameter table; the effect pass reads them and the
// DrawIndexedPrimitive follows in the same call tree, on the Present thread (workflow object-perpixel-lamp-research).
// So: the call at 0x6F68C5 remembers the rig, and a detour of FUN_006f6250 limits it to that part's draw (cleared on
// entry, restored on exit: binds without a draw, pass flag 8 and nested draws do not leak a rig into other draws).
// The Swarm effects path (FUN_0071cfb0, 0x71D314) is left alone: its draws never see a rig here.
// Instanced batches (FUN_006cf920, thiscall with 5 stack arguments, ret 0x14) can be flushed from inside FUN_006f6250
// and draw other objects' instances: it is detoured too and clears the rig while it runs.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "rig_tracker.h"
#include "memory_patch.h"
#include "apex_log.h"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <vector>

namespace {

constexpr uintptr_t kModelDraw = 0x006F6250;
const BYTE kModelDrawProlog[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x94, 0x01, 0x00, 0x00};
constexpr uintptr_t kBinderCall = 0x006F68C5; // CALL FUN_006b8b30, ECX = rig
constexpr uintptr_t kBinder = 0x006B8B30;
constexpr uint32_t kRigVtable = 0x00FF4218;
constexpr uintptr_t kInstanceFlush = 0x006CF920;
const BYTE kInstanceFlushProlog[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0xA4, 0x0B, 0x00, 0x00};

using ModelDraw_t = void(__thiscall*)(void* model, void* part, void* ctx);
using Binder_t = void(__fastcall*)(void* rig);
using InstanceFlush_t = void(__thiscall*)(void* self, void* a, void* b, void* c, void* d, void* e);

ModelDraw_t oModelDraw = reinterpret_cast<ModelDraw_t>(kModelDraw);
InstanceFlush_t oInstanceFlush = reinterpret_cast<InstanceFlush_t>(kInstanceFlush);
std::vector<MemPatch::PatchLocation> g_patches;
std::vector<DetourBatch::Hook> g_hooks;
bool g_installed = false;
uintptr_t g_rig = 0;
int g_depth = 0;
std::atomic<DWORD> g_drawThread{0};

void __fastcall BinderThunk(void* rig) {
    g_rig = reinterpret_cast<uintptr_t>(rig);
    reinterpret_cast<Binder_t>(kBinder)(rig);
}

void __fastcall ModelDrawHook(void* model, void*, void* part, void* ctx) {
    const uintptr_t saved = g_rig;
    g_rig = 0;
    g_depth++;
    g_drawThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    oModelDraw(model, part, ctx);
    g_depth--;
    g_rig = saved;
}

void __fastcall InstanceFlushHook(void* self, void*, void* a, void* b, void* c, void* d, void* e) {
    const uintptr_t saved = g_rig;
    g_rig = 0; // the batch holds other objects' instances: no rig of ours applies
    oInstanceFlush(self, a, b, c, d, e);
    g_rig = saved;
}

int ReadMode(uintptr_t rig) {
    __try {
        if (*reinterpret_cast<const uint32_t*>(rig) != kRigVtable) return -1;
        return *reinterpret_cast<const int*>(rig + 0x1D4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

} // namespace

namespace RigTracker {

bool Install() {
    if (g_installed) return true;
    if (std::memcmp(reinterpret_cast<const void*>(kModelDraw), kModelDrawProlog, sizeof(kModelDrawProlog)) != 0 ||
        std::memcmp(reinterpret_cast<const void*>(kInstanceFlush), kInstanceFlushProlog, sizeof(kInstanceFlushProlog)) != 0 ||
        *reinterpret_cast<const BYTE*>(kBinderCall) != 0xE8 || kBinderCall + 5 + *reinterpret_cast<const int32_t*>(kBinderCall + 1) != kBinder) {
        LOG_WARNING("[RigTracker] Object draw code differs (different game version?)");
        return false;
    }
    DWORD orig = static_cast<DWORD>(kBinder - (kBinderCall + 5));
    const DWORD rel = static_cast<DWORD>(reinterpret_cast<uintptr_t>(&BinderThunk) - (kBinderCall + 5));
    if (!MemPatch::WriteDWORD(kBinderCall + 1, rel, &g_patches, &orig)) {
        MemPatch::RestoreAll(g_patches);
        g_patches.clear();
        return false;
    }
    oModelDraw = reinterpret_cast<ModelDraw_t>(kModelDraw);
    oInstanceFlush = reinterpret_cast<InstanceFlush_t>(kInstanceFlush);
    g_hooks = {{reinterpret_cast<void**>(&oModelDraw), reinterpret_cast<void*>(&ModelDrawHook)},
               {reinterpret_cast<void**>(&oInstanceFlush), reinterpret_cast<void*>(&InstanceFlushHook)}};
    if (!DetourBatch::InstallHooks(g_hooks)) {
        g_hooks.clear();
        MemPatch::RestoreAll(g_patches);
        g_patches.clear();
        LOG_WARNING("[RigTracker] Could not hook the object draw");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_installed = true;
    LOG_INFO("[RigTracker] Installed");
    return true;
}

void Uninstall() {
    if (!g_installed) return;
    DetourBatch::RemoveHooks(g_hooks);
    g_hooks.clear();
    MemPatch::RestoreAll(g_patches);
    g_patches.clear();
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_installed = false;
    g_rig = 0;
}

bool IsInstalled() { return g_installed; }

int CurrentMode() {
    if (!g_installed || g_depth <= 0 || !g_rig || GetCurrentThreadId() != g_drawThread.load(std::memory_order_relaxed)) return -1;
    return ReadMode(g_rig);
}

} // namespace RigTracker
