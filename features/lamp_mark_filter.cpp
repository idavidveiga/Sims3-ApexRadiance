// Rooms keep their light when their lamps did not change (part of Night Lighting, 2026-09-30).
//
// User: "the indoor light breaks for a moment at every floor switch"; "keep the light information cached and change it only
// when something changes". F6 092629: at every floor switch the game restarted room 0 of every story and every room holding
// a lamp (return address 0x006C7451 in FUN_006c7250), twice within 16 ms, and solved them again in 110-140 ms, with the same
// ambient, light counts and normalisation as before. Two studies (30/09):
// - The mark comes from the lamp entry update FUN_006c7ba0 (the entry's vtable slot +8, run by FUN_006c7250 for the
//   entries flagged dirty): for a lamp (light vfunc+0x18 = 0x00620D60, false, in 7 of the 9 light classes) it finds the
//   lamp's room (FUN_006c7b20), rewrites its lit bit and lit colour (FUN_006bdca0) and then marks the room changed
//   (FUN_006c7160: 0x006C7CCA for the room it left when it moved, 0x006C7CD6 always) without comparing anything. The
//   entries are flagged by FUN_006c4cf0 from the light manager's messages (transform 0x3361F6C9 and colour 0x966EC80A
//   always, intensity, enable and alpha only on a change); which one a floor switch sends is not known yet (the counters
//   below tell).
// - Nothing of the room solve reads the shown story or a light's visibility: the gather (FUN_006c7820) takes the lamp by
//   its room, flags, lit bit, lit colour sum and type; the point solve by its values. So a lamp whose values are all as they
//   were gives its rooms exactly the light they already have.
// So the call at 0x006C7CD6 goes through MarkThunk: the lamp's values (Signature) are compared with the last time it marked
// its rooms (per story tree level and light); the same values keep the rooms' solve (the mark is dropped), anything else
// marks as before. First sight, window lights (types 7 and 8: the long path, they follow the sky) and anything unreadable
// always mark. Left as they are: the mark of the room a lamp left (0x006C7CCA), the occluder entries (0x006C7939: objects
// that fade or hide block light differently, their rooms must be solved again), room creation and object removal marks.
#include "lamp_mark_filter.h"
#include "lot_light_bridge.h"
#include "game_addresses.h"
#include "memory_patch.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "level_light_share.h"
#include "object_light_bridge.h"
#include "unlit_rooms.h"
#include "room_ambient_policy.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

// Steam 1.67.2 values; Install takes them from GameAddr on every build (every site still checked byte by byte)
uintptr_t kMarkCall = 0x006C7CD6;             // in FUN_006c7ba0: CALL FUN_006c7160, ecx = entry+0x14 (tree level), push entry+0x1C
uintptr_t kMark = 0x006C7160;                 // FUN_006c7160: thiscall(treeLevel, int room) ret 4: the room goes into tl+8
uintptr_t kLitCall = 0x006C7CB6;              // "mov ecx, edi; call FUN_006bdca0": edi = the light, esi = the entry
constexpr uintptr_t kEntryFlag = 0x006C4CF0;  // FUN_006c4cf0(objId): flags every light and occluder entry of an object
// The light manager's message handlers that flag entries (development build: counted, then the game's call)
constexpr uintptr_t kFlagSites[5] = {0x006B0A8D, 0x006B0BFA, 0x006B0B33, 0x006B0C9A, 0x006B0D26};
constexpr const char* kFlagNames[5] = {"transform", "colour", "intensity", "enable", "alpha"};

std::vector<MemPatch::PatchLocation> g_patches;
bool g_installed = false, g_flagsCounted = false;
std::atomic<bool> g_on{true};
uintptr_t g_markTarget = kMark;
uintptr_t g_flagTarget = kEntryFlag;
std::atomic<long> g_first{0}, g_changed{0}, g_kept{0}, g_windows{0}, g_unread{0}, g_off{0};
volatile long g_flagCount[5] = {};

struct Key {
    uintptr_t tl, light;
    bool operator==(const Key&) const = default;
};
struct KeyHash {
    size_t operator()(const Key& k) const { return std::hash<uintptr_t>()(k.tl) ^ (std::hash<uintptr_t>()(k.light) * 31u); }
};
std::mutex g_mx;
// A lamp at its last mark: its values, and what a player sees change (on or off, where it is, its room)
struct LampState {
    uint64_t sig;
    bool on;
    float pos[3];
    int room;
    DWORD eventsFrom = 0; // switches on or off seen since this tick (a light that keeps switching itself is left out)
    int events = 0;
    float editable[5] = {}; // base RGB, intensity, enabled; excludes animated fade
};
std::unordered_map<Key, LampState, KeyHash> g_sig; // (tree level, light) -> the lamp at its last mark

// A lamp switched on or off, or moved (30/09, user: "also refresh the lighting whenever a lamp is moved, switched off or on,
// if it costs no performance"): a switch waits 120 ms; a move still waits 700 ms after its last change (a drag:
// once when it stops). At most once per kLotGap per lot; never while dusk/dawn switches every lamp at once.
// Not the whole world as the shortcut: only the rooms of that lot (every story), on the light tree thread, and the rigs.
constexpr DWORD kLotGap = 2000;
constexpr float kMoveMin = 0.10f; // m: animated lamps wobble less
constexpr DWORD kSelfWindow = 10000; // ms
constexpr int kSelfMax = 3;          // more switches on or off than this within kSelfWindow: a light switching itself, left out
struct LotDue {
    DWORD due = 0, last = 0, changed = 0;
    bool switchOnly = false;
};
std::unordered_map<uintptr_t, LotDue> g_lotDue; // tracker -> when its refresh is due (under g_mx); due 0 = none pending
std::atomic<long> g_lampEvents{0}, g_lotRefreshes{0}, g_lotSkippedDusk{0};
std::atomic<bool> g_editRefresh{false};

inline void Mix(uint64_t& h, uint32_t v) { h = (h ^ v) * 1099511628211ull; }
inline void MixDwords(uint64_t& h, uintptr_t at, int n) {
    for (int k = 0; k < n; k++) Mix(h, *reinterpret_cast<const uint32_t*>(at + k * 4));
}
// The lamp as the room solve takes it: its room, the lit bit (+0x100 & 0x20) and the object's flags (entry+0x20 record,
// +0x90), the values +0x10 (4), the type +0xB0, +0xC0..+0xDC, the lit colour +0xE0..+0xEC (FUN_006bdca0 has just written
// it), the first base-colour component +0xF0, the position +0x120 (3) and range +0x130, the cone of spot lights
// (types 4 and 5: +0x170, 13 dwords). The separate editable sample wakes terrain reconciliation without changing
// this room-mark signature; animated fade +0x20 does not wake it. False when it cannot be read.
bool Signature(uintptr_t entry, uintptr_t light, int room, uint64_t& h, int& type, bool& on, float* pos, float* editable) {
    __try {
        h = 1469598103934665603ull;
        const float* lit = reinterpret_cast<const float*>(light + 0xE0); // the lit colour: 0 when the game switched it off
        on = (*reinterpret_cast<const BYTE*>(light + 0x100) & 0x20) && lit[0] + lit[1] + lit[2] > 1e-3f;
        std::memcpy(pos, reinterpret_cast<const void*>(light + 0x120), 3 * sizeof(float));
        Mix(h, static_cast<uint32_t>(room));
        Mix(h, *reinterpret_cast<const BYTE*>(light + 0x100) & 0x20);
        if (const uintptr_t info = *reinterpret_cast<const uintptr_t*>(entry + 0x20)) Mix(h, *reinterpret_cast<const BYTE*>(info + 0x90) & 0x6);
        type = *reinterpret_cast<const int*>(light + 0xB0);
        if ((type >= 3 && type <= 6) || type == 11) {
            std::memcpy(editable, reinterpret_cast<const void*>(light + 0xF0), 3 * sizeof(float));
            editable[3] = *reinterpret_cast<const float*>(light + 0x10);
            editable[4] = (*reinterpret_cast<const BYTE*>(light + 0x100) & 0x40) ? 1.0f : 0.0f;
        }
        Mix(h, static_cast<uint32_t>(type));
        MixDwords(h, light + 0x10, 4);
        MixDwords(h, light + 0xC0, 8);
        MixDwords(h, light + 0xE0, 5);
        MixDwords(h, light + 0x120, 3);
        MixDwords(h, light + 0x130, 1);
        if (type == 4 || type == 5) MixDwords(h, light + 0x170, 13);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The lot of a story tree level (tl+4: its tracker, as level_light_share reads it); 0 when unreadable
uintptr_t TrackerOf(uintptr_t tl) {
    __try {
        return *reinterpret_cast<const uintptr_t*>(tl + 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// true = mark the room (the game's call), false = keep its solve
bool __cdecl MarkDecide(uintptr_t tl, int room, uintptr_t entry, uintptr_t light) {
    uint64_t h = 0;
    int type = 0;
    bool on = false;
    float pos[3] = {};
    float editable[5] = {};
    if (!entry || !light || !Signature(entry, light, room, h, type, on, pos, editable)) {
        g_unread.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (type == 7 || type == 8) { // window lights: the long path, they follow the sky
        g_windows.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_sig.size() > 65536) g_sig.clear();
    const LampState now{h, on, {pos[0], pos[1], pos[2]}, room, 0, 0};
    const auto [it, fresh] = g_sig.try_emplace(Key{tl, light}, now);
    if (fresh) {
        std::memcpy(it->second.editable, editable, sizeof editable);
        if ((type >= 3 && type <= 6) || type == 11) g_editRefresh.store(true, std::memory_order_relaxed);
        g_first.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    LampState& was = it->second;
    if (std::memcmp(was.editable, editable, sizeof editable) != 0) {
        std::memcpy(was.editable, editable, sizeof editable);
        g_editRefresh.store(true, std::memory_order_relaxed);
    }
    const bool same = was.sig == h;
    const float dx = pos[0] - was.pos[0], dy = pos[1] - was.pos[1], dz = pos[2] - was.pos[2];
    const bool moved = dx * dx + dy * dy + dz * dz > kMoveMin * kMoveMin; // NaN: not a move
    if (was.on != on || moved || was.room != room) { // the player sees this: its lot lights again (OnPresent)
        const DWORD tick = GetTickCount();
        if (tick - was.eventsFrom > kSelfWindow) {
            was.eventsFrom = tick;
            was.events = 0;
        }
        // a light that switches itself on and off (a flickering TV or effect light): not the player (a drag only moves: never left out)
        const bool selfSwitching = was.on != on && ++was.events > kSelfMax;
        if (!selfSwitching) g_lampEvents.fetch_add(1, std::memory_order_relaxed);
        if (const uintptr_t tracker = selfSwitching ? 0 : TrackerOf(tl)) {
            if (g_lotDue.size() > 1024) g_lotDue.clear();
            LotDue& pending = g_lotDue[tracker];
            const bool switchOnly = was.on != on && !moved && was.room == room;
            pending.switchOnly = pending.due ? pending.switchOnly && switchOnly : switchOnly;
            pending.changed = tick;
            pending.due = (tick + RoomAmbientPolicy::LampRefreshDelay(pending.switchOnly)) | 1;
        }
        was.on = on;
        std::memcpy(was.pos, pos, sizeof was.pos); // a slow drag: each 10 cm step counts, the refresh waits for the last
        was.room = room;
    }
    was.sig = h;
    if (!g_on.load(std::memory_order_relaxed)) { // off: every mark goes through (the values are still followed)
        g_off.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (!same) {
        g_changed.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    g_kept.fetch_add(1, std::memory_order_relaxed);
    return false;
}

// At the call: ecx = the tree level, [esp+4] = the room, esi = the entry, edi = the light (the game's registers, kept
// across the calls since "mov ecx, edi; call FUN_006bdca0")
__declspec(naked) void MarkThunk() {
    __asm {
        push ecx
        push edi
        push esi
        push dword ptr [esp + 16]
        push ecx
        call MarkDecide
        add esp, 16
        pop ecx
        test al, al
        jz keep
        jmp dword ptr [g_markTarget]
    keep:
        ret 4
    }
}

// The light manager's messages that flag light entries: counted, then the game's call
__declspec(naked) void FlagThunk0() { __asm { lock inc dword ptr [g_flagCount + 0] } __asm { jmp dword ptr [g_flagTarget] } }
__declspec(naked) void FlagThunk1() { __asm { lock inc dword ptr [g_flagCount + 4] } __asm { jmp dword ptr [g_flagTarget] } }
__declspec(naked) void FlagThunk2() { __asm { lock inc dword ptr [g_flagCount + 8] } __asm { jmp dword ptr [g_flagTarget] } }
__declspec(naked) void FlagThunk3() { __asm { lock inc dword ptr [g_flagCount + 12] } __asm { jmp dword ptr [g_flagTarget] } }
__declspec(naked) void FlagThunk4() { __asm { lock inc dword ptr [g_flagCount + 16] } __asm { jmp dword ptr [g_flagTarget] } }
void (*const kFlagThunks[5])() = {&FlagThunk0, &FlagThunk1, &FlagThunk2, &FlagThunk3, &FlagThunk4};

bool CallsTarget(uintptr_t site, uintptr_t target) {
    return *reinterpret_cast<const BYTE*>(site) == 0xE8 && site + 5 + *reinterpret_cast<const int32_t*>(site + 1) == target;
}
bool Redirect(uintptr_t site, uintptr_t target, const void* to) {
    const DWORD orig = static_cast<DWORD>(target - (site + 5));
    return MemPatch::WriteDWORD(site + 1, static_cast<DWORD>(reinterpret_cast<uintptr_t>(to) - (site + 5)), &g_patches, &orig);
}

} // namespace

namespace LampMarkFilter {

bool Install(std::string& why) {
    if (g_installed) return true;
    {
        using GameAddr::Id;
        std::string missing;
        if (!GameAddr::Have({Id::LampLitCall, Id::LampMarkCall, Id::LampMark}, &missing)) {
            why = "Rooms keep their light: " + GameAddr::NotAvailable(missing);
            return false;
        }
        kLitCall = GameAddr::Get(Id::LampLitCall);
        kMarkCall = GameAddr::Get(Id::LampMarkCall);
        kMark = GameAddr::Get(Id::LampMark);
        g_markTarget = kMark;
    }
    // 0x6C7CB6: 8B CF E8 (mov ecx, edi; call FUN_006bdca0); 0x6C7CCF: 8B 4E 1C 51 8B 4E 14 (mov ecx,[esi+1C]; push ecx; mov ecx,[esi+14])
    const BYTE lit[] = {0x8B, 0xCF, 0xE8}, arg[] = {0x8B, 0x4E, 0x1C, 0x51, 0x8B, 0x4E, 0x14};
    const bool ok = std::memcmp(reinterpret_cast<const void*>(kLitCall), lit, sizeof lit) == 0 &&
                    std::memcmp(reinterpret_cast<const void*>(kMarkCall - sizeof arg), arg, sizeof arg) == 0 && CallsTarget(kMarkCall, kMark);
    if (!ok) {
        why = "Rooms keep their light: the game code differs";
        return false;
    }
    if (!Redirect(kMarkCall, kMark, reinterpret_cast<const void*>(&MarkThunk))) {
        MemPatch::RestoreAll(g_patches);
        g_patches.clear();
        why = "Rooms keep their light: could not patch the game";
        return false;
    }
    g_flagsCounted = false;
    if (!kPublicBuild && GameAddr::IsFixed()) { // (Steam addresses only) which messages flag the entries (the floor switch's trigger): all five or none
        bool all = true;
        for (uintptr_t s : kFlagSites) all = all && CallsTarget(s, kEntryFlag);
        std::vector<MemPatch::PatchLocation> before = g_patches;
        for (int i = 0; i < 5 && all; i++) all = Redirect(kFlagSites[i], kEntryFlag, reinterpret_cast<const void*>(kFlagThunks[i]));
        if (!all) {
            std::vector<MemPatch::PatchLocation> added(g_patches.begin() + static_cast<std::ptrdiff_t>(before.size()), g_patches.end());
            MemPatch::RestoreAll(added);
            g_patches = before;
            LOG_WARNING("[LampMarkFilter] The light-entry message counters could not be installed (the game code differs)");
        }
        g_flagsCounted = all;
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_installed = true;
    LOG_INFO(std::format("[LampMarkFilter] Installed: a lamp marks its rooms changed only when it changed{}", g_flagsCounted ? " (message counters on)" : ""));
    return true;
}

void Uninstall() {
    if (!g_installed) return;
    MemPatch::RestoreAll(g_patches);
    g_patches.clear();
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_installed = false;
    std::lock_guard<std::mutex> lk(g_mx);
    g_sig.clear(); // lamps freed while it was out would leave stale keys
    g_lotDue.clear();
    g_editRefresh.store(false, std::memory_order_relaxed);
}

bool IsInstalled() { return g_installed; }

void OnPresent(float nightLevel) {
    if (!g_installed) return;
    if (g_editRefresh.exchange(false, std::memory_order_relaxed)) LotLightBridge::RequestLampEditRefresh();
    const DWORD now = GetTickCount();
    std::vector<std::pair<uintptr_t, DWORD>> run;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        for (auto& [tracker, d] : g_lotDue) {
            if (!d.due || static_cast<int32_t>(now - d.due) < 0) continue;
            if (d.last && now - d.last < kLotGap) { // refreshed a moment ago: once more when the gap is over
                d.due = (d.last + kLotGap) | 1;
                continue;
            }
            d.due = 0;
            if (nightLevel < 0.0f || (nightLevel > 0.02f && nightLevel < 0.98f)) { // no world, or dusk / dawn switching every lamp
                g_lotSkippedDusk.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            d.last = now | 1;
            run.emplace_back(tracker, d.switchOnly ? d.changed : 0);
        }
    }
    for (const auto& [t, changed] : run)
        if (LevelLightShare::RelightLot(t, "a lamp switched or moved", changed) >= 0) g_lotRefreshes.fetch_add(1, std::memory_order_relaxed);
    if (!run.empty()) {
        ObjectLightBridge::RequestRigRefresh();
        // RelightLot watches completion and retains its own bounded fallback.
    }
}
void SetEnabled(bool on) { g_on.store(on, std::memory_order_relaxed); }
bool Enabled() { return g_on.load(std::memory_order_relaxed); }

std::string Status() {
    if (!g_installed) return "not installed";
    std::string s = std::format("{} | lamp marks kept (the lamp as it was) {}, let through: first sight {}, changed {}, window lights {}, unreadable {}, "
                                "while off {}",
                                g_on.load() ? "on" : "off", g_kept.load(), g_first.load(), g_changed.load(), g_windows.load(), g_unread.load(), g_off.load());
    s += std::format(" | lamps switched or moved {}, lots lit again {} (skipped at dusk or dawn {})", g_lampEvents.load(), g_lotRefreshes.load(), g_lotSkippedDusk.load());
    if (g_flagsCounted) {
        s += " | light entries flagged by message:";
        for (int i = 0; i < 5; i++) s += std::format(" {} {}", kFlagNames[i], static_cast<long>(g_flagCount[i]));
    }
    return s;
}

} // namespace LampMarkFilter
