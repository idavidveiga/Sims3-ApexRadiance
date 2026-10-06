// Room to save: a reserve of address space and the game's idle resource cache, given back when the game needs a large
// free block (see memory_guard.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "memory_guard.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "game_addresses.h"
#include "memory_patch.h"
#include "imgui.h"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <vector>

namespace MemoryGuard {
namespace {

// Steam 1.67.2 (research\engine_map\full.asm, 05/10)
// 0x007377F0: mov eax,[ecx+14h]; push eax; add ecx,18h; call 0x00737560; ret 8 (the ResourceSystem's per-frame update,
// which trims both caches with Trim(0, 0, 0) at 0x007375A3 / 0x007375B4; thiscall, 1 argument, ret 4)
uintptr_t kUpdateCall = 0x007377F7, kUpdate = 0x00737560;
const BYTE kUpdateThunkBytes[] = {0x8B, 0x41, 0x14, 0x50, 0x83, 0xC1, 0x18, 0xE8};
// 0x00733E70: ResourceSystem vtable 0x00FFE2F0 slot +0x50: Trim(budget, 0, force) on both caches (+0x1E0, +0x1E4), which
// frees every idle entry (the "Shrink cache" of the game's live settings, 0x00733120, for each)
uintptr_t kShrinkBoth = 0x00733E70;
const BYTE kShrinkBytes[] = {0x56, 0x8B, 0xF1, 0x8B, 0x8E, 0xE0, 0x01, 0x00, 0x00, 0x85, 0xC9};
// 0x00AAC31C: mov ecx,[esp+30h]; call 0x00C6D460 (the world save, thiscall, 2 arguments, ret 8, al = saved); its false
// makes 0x00AAC110 return 12 (Error 12)
uintptr_t kSaveCall = 0x00AAC320, kWorldSave = 0x00C6D460;
const BYTE kSaveCallBytes[] = {0x8B, 0x4C, 0x24, 0x30, 0xE8};
const BYTE kWorldSaveBytes[] = {0x83, 0xEC, 0x24, 0x53, 0x55, 0x56, 0x57, 0x8B, 0xF9};

constexpr SIZE_T kReserveBytes = 128u << 20;  // the reserve
constexpr uint64_t kLowBytes = 320ull << 20;    // the largest free block under this: the game's idle resource cache emptied
constexpr uint64_t kLetGoBytes = 160ull << 20;  // under this: the reserve let go too (it is then the largest block left)
constexpr uint64_t kRoomBytes = 512ull << 20; // free left beside the reserve before it is taken again
constexpr DWORD kPeriodMs = 4000, kShrinkGapMs = 60000, kRetakeAfterMs = 30000;

std::vector<MemPatch::PatchLocation> g_patches;
std::atomic<bool> g_running{false};
HANDLE g_thread = nullptr, g_stop = nullptr;
std::mutex g_mx;           // the reserve
void* g_reserve = nullptr; // held reserve (MEM_RESERVE only)
DWORD g_releasedAt = 0;
std::atomic<void*> g_rs{nullptr};      // the ResourceSystem, seen by its update
std::atomic<DWORD> g_rsThread{0};       // the thread that runs its update
std::atomic<bool> g_shrinkWanted{false};
std::atomic<uint64_t> g_largest{0}, g_lowest{~0ull};
std::atomic<long> g_saves{0}, g_saveFails{0}, g_shrinks{0}, g_lets{0}, g_takes{0};
std::atomic<uint64_t> g_freedBytes{0};

// The largest free block of the process's address space (bytes)
uint64_t LargestFree() {
    uint64_t largest = 0;
    uintptr_t at = 0x10000;
    MEMORY_BASIC_INFORMATION mbi{};
    while (at < 0xFFFF0000u && VirtualQuery(reinterpret_cast<LPCVOID>(at), &mbi, sizeof mbi) == sizeof mbi) {
        if (mbi.State == MEM_FREE && mbi.RegionSize > largest) largest = mbi.RegionSize;
        const uintptr_t next = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        if (next <= at) break;
        at = next;
    }
    return largest;
}

bool TakeReserve() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_reserve) return true;
    g_reserve = VirtualAlloc(nullptr, kReserveBytes, MEM_RESERVE | MEM_TOP_DOWN, PAGE_NOACCESS);
    if (g_reserve) g_takes.fetch_add(1, std::memory_order_relaxed);
    return g_reserve != nullptr;
}
bool LetReserveGo() {
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_reserve) return false;
    VirtualFree(g_reserve, 0, MEM_RELEASE);
    g_reserve = nullptr;
    g_releasedAt = GetTickCount() | 1;
    g_lets.fetch_add(1, std::memory_order_relaxed);
    return true;
}
bool Holding() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_reserve != nullptr;
}

// The idle bytes of both caches (+0x98: bytes of the entries on the idle list; reading of the trim at 0x00732E90, unverified
// in game), 0 when unreadable
uint64_t IdleBytes(void* rs) {
    uint64_t sum = 0;
    __try {
        for (int k = 0; k < 2; k++)
            if (const uintptr_t cache = *reinterpret_cast<const uintptr_t*>(reinterpret_cast<uintptr_t>(rs) + 0x1E0 + k * 4))
                sum += *reinterpret_cast<const uint32_t*>(cache + 0x98);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return sum;
}

// The game's own shrink of both caches (on the thread that runs the ResourceSystem's update)
bool ShrinkNow(void* rs, uint64_t& freed) {
    freed = 0;
    if (!rs) return false;
    const uint64_t before = IdleBytes(rs);
    __try {
        reinterpret_cast<void(__thiscall*)(void*)>(kShrinkBoth)(rs);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    const uint64_t after = IdleBytes(rs);
    freed = before > after ? before - after : 0;
    g_shrinks.fetch_add(1, std::memory_order_relaxed);
    g_freedBytes.fetch_add(freed, std::memory_order_relaxed);
    return true;
}

using Update_t = void(__thiscall*)(void* rs, int arg);
void __fastcall UpdateHook(void* rs, void*, int arg) {
    reinterpret_cast<Update_t>(kUpdate)(rs, arg);
    g_rs.store(rs, std::memory_order_relaxed);
    g_rsThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (g_shrinkWanted.exchange(false, std::memory_order_relaxed)) {
        uint64_t freed = 0;
        if (ShrinkNow(rs, freed)) LOG_INFO(std::format("[MemoryGuard] Largest free block low: the game's resource cache emptied ({} MB of idle files)", freed >> 20));
    }
}

using WorldSave_t = bool(__thiscall*)(void* wm, void* a, void* b);
bool __fastcall WorldSaveHook(void* wm, void*, void* a, void* b) {
    const uint64_t before = LargestFree();
    uint64_t freed = 0;
    void* rs = g_rs.load(std::memory_order_relaxed);
    const bool shrunk = rs && g_rsThread.load(std::memory_order_relaxed) == GetCurrentThreadId() && ShrinkNow(rs, freed);
    const bool let = LetReserveGo();
    const uint64_t room = LargestFree();
    const bool ok = reinterpret_cast<WorldSave_t>(kWorldSave)(wm, a, b);
    g_saves.fetch_add(1, std::memory_order_relaxed);
    if (!ok) g_saveFails.fetch_add(1, std::memory_order_relaxed);
    LOG_INFO(std::format("[MemoryGuard] World save {}: largest free block {} MB, {} MB for the save ({}{})", ok ? "done" : "FAILED (Error 12)", before >> 20,
                         room >> 20, let ? "reserve let go" : "no reserve held",
                         shrunk ? std::format(", resource cache emptied: {} MB of idle files", freed >> 20) : std::string()));
    return ok;
}

DWORD WINAPI Watch(LPVOID) {
    DWORD lastShrink = 0;
    for (;;) {
        if (WaitForSingleObject(g_stop, kPeriodMs) != WAIT_TIMEOUT) break;
        const uint64_t largest = LargestFree();
        g_largest.store(largest, std::memory_order_relaxed);
        if (largest < g_lowest.load(std::memory_order_relaxed)) g_lowest.store(largest, std::memory_order_relaxed);
        const DWORD now = GetTickCount();
        if (largest < kLowBytes) {
            if (largest < kLetGoBytes && LetReserveGo()) LOG_WARNING(std::format("[MemoryGuard] Largest free block {} MB: the reserve of {} MB let go", largest >> 20, kReserveBytes >> 20));
            if (!lastShrink || now - lastShrink >= kShrinkGapMs) {
                lastShrink = now | 1;
                g_shrinkWanted.store(true, std::memory_order_relaxed);
            }
        } else if (!Holding() && largest >= kReserveBytes + kRoomBytes) {
            DWORD released = 0;
            {
                std::lock_guard<std::mutex> lk(g_mx);
                released = g_releasedAt;
            }
            if (!released || now - released >= kRetakeAfterMs) TakeReserve();
        }
    }
    return 0;
}

bool CallsTo(uintptr_t site, uintptr_t target) {
    BYTE op = 0;
    int32_t rel = 0;
    return MemPatch::ReadBytes(site, &op, 1) && op == 0xE8 && MemPatch::ReadBytes(site + 1, &rel, 4) && site + 5 + rel == target;
}
bool Redirect(uintptr_t site, uintptr_t target, const void* to) {
    const DWORD orig = static_cast<DWORD>(target - (site + 5));
    return MemPatch::WriteDWORD(site + 1, static_cast<DWORD>(reinterpret_cast<uintptr_t>(to) - (site + 5)), &g_patches, &orig);
}

} // namespace

bool Start(std::string* error) {
    if (g_running.load()) return true;
    const auto fail = [&](const char* why) {
        if (error) *error = std::string("Room to save: ") + why;
        return false;
    };
    using GameAddr::Id;
    std::string missing;
    if (!GameAddr::Have({Id::ResUpdateCall, Id::ResUpdate, Id::ResShrinkBoth, Id::WorldSaveCall, Id::WorldSave}, &missing))
        return fail(GameAddr::NotAvailable(missing).c_str());
    kUpdateCall = GameAddr::Get(Id::ResUpdateCall);
    kUpdate = GameAddr::Get(Id::ResUpdate);
    kShrinkBoth = GameAddr::Get(Id::ResShrinkBoth);
    kSaveCall = GameAddr::Get(Id::WorldSaveCall);
    kWorldSave = GameAddr::Get(Id::WorldSave);
    // the calls land where they should on every build; Steam: every byte around them as well (other builds: the signatures
    // already matched the bytes around the calls; the called functions' own prologues are not assumed)
    bool same = CallsTo(kUpdateCall, kUpdate) && CallsTo(kSaveCall, kWorldSave);
    if (GameAddr::IsFixed())
        same = same && MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(kUpdateCall - 7), kUpdateThunkBytes, sizeof kUpdateThunkBytes) &&
               MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(kShrinkBoth), kShrinkBytes, sizeof kShrinkBytes) &&
               MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(kSaveCall - 4), kSaveCallBytes, sizeof kSaveCallBytes) &&
               MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(kWorldSave), kWorldSaveBytes, sizeof kWorldSaveBytes);
    if (!same) return fail("the game code differs");
    if (!Redirect(kUpdateCall, kUpdate, reinterpret_cast<const void*>(&UpdateHook)) || !Redirect(kSaveCall, kWorldSave, reinterpret_cast<const void*>(&WorldSaveHook))) {
        MemPatch::RestoreAll(g_patches);
        return fail("could not patch the game");
    }
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_thread = g_stop ? CreateThread(nullptr, 64 * 1024, Watch, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr) : nullptr;
    if (!g_thread) {
        MemPatch::RestoreAll(g_patches);
        if (g_stop) CloseHandle(g_stop);
        g_stop = nullptr;
        return fail("could not start its thread");
    }
    TakeReserve();
    g_running = true;
    LOG_INFO(std::format("[MemoryGuard] On: a reserve of {} MB of address space for world saves{}", kReserveBytes >> 20, Holding() ? "" : " (not taken yet)"));
    return true;
}

void Stop() {
    if (!g_running.exchange(false)) return;
    if (g_stop) SetEvent(g_stop);
    if (g_thread) {
        WaitForSingleObject(g_thread, 5000);
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
    if (g_stop) {
        CloseHandle(g_stop);
        g_stop = nullptr;
    }
    MemPatch::RestoreAll(g_patches);
    LetReserveGo();
    g_shrinkWanted = false;
}

bool Running() { return g_running.load(); }

std::string StatusText() {
    if (!g_running.load()) return "off";
    const uint64_t largest = g_largest.load(), lowest = g_lowest.load();
    return std::format("on | largest free block {} MB (lowest {} MB) | reserve {} | world saves {} (failed {}) | resource cache emptied {} times ({} MB of idle files)",
                       largest >> 20, lowest == ~0ull ? 0 : lowest >> 20, Holding() ? std::format("{} MB held", kReserveBytes >> 20) : std::string("let go"),
                       g_saves.load(), g_saveFails.load(), g_shrinks.load(), g_freedBytes.load() >> 20);
}

void RenderDeveloperUI() {
    ImGui::TextUnformatted(("Room to save: " + StatusText()).c_str());
    void* rs = g_rs.load();
    ImGui::TextDisabled("Resource cache: idle files %llu MB", static_cast<unsigned long long>(rs ? IdleBytes(rs) >> 20 : 0));
    if (ImGui::Button("Empty the game's resource cache now")) g_shrinkWanted = true;
}

} // namespace MemoryGuard
