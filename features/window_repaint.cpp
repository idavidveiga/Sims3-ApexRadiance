// Lighter window updates: the game's per-frame InvalidateRect of its own window skipped (see window_repaint.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "window_repaint.h"
#include "apex_log.h"
#include "game_addresses.h"
#include "memory_patch.h"
#include <windows.h>
#include <atomic>
#include <format>
#include <vector>

namespace WindowRepaint {
namespace {
std::vector<MemPatch::PatchLocation> g_patches;
std::atomic<bool> g_running{false};
uintptr_t g_site = 0;
} // namespace

bool Start(std::string* error) {
    if (g_running.load()) return true;
    using GameAddr::Id;
    std::string missing;
    if (!GameAddr::Have({Id::WindowRepaintJump}, &missing)) {
        if (error) *error = "Lighter window updates: " + GameAddr::NotAvailable(missing);
        return false;
    }
    g_site = GameAddr::Get(Id::WindowRepaintJump);
    // jne short (75 xx) over "mov eax,[esi+70h]; push ebx; push ebx; push eax; call [InvalidateRect]" (8B 46 70 53 53 50 FF 15)
    BYTE b[2] = {};
    const BYTE tail[] = {0x8B, 0x46, 0x70, 0x53, 0x53, 0x50, 0xFF, 0x15};
    if (!MemPatch::ReadBytes(g_site, b, 2) || b[0] != 0x75 || !MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(g_site + 2), tail, sizeof tail) || b[1] != 0x0C) {
        if (error) *error = "Lighter window updates: the game code differs";
        return false;
    }
    const std::vector<BYTE> jmp = {0xEB}, jne = {0x75};
    if (!MemPatch::WriteBytes(g_site, jmp, &g_patches, &jne)) {
        if (error) *error = "Lighter window updates: could not patch the game";
        return false;
    }
    g_running = true;
    LOG_INFO(std::format("[WindowRepaint] On: the game no longer invalidates its window every frame ({:#010x})", g_site));
    return true;
}

void Stop() {
    if (!g_running.exchange(false)) return;
    MemPatch::RestoreAll(g_patches);
}

bool Running() { return g_running.load(); }

std::string StatusText() { return g_running.load() ? "on (no per-frame repaint of the game's window)" : "off"; }

} // namespace WindowRepaint
