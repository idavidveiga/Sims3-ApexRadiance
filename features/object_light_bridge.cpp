// Object light bridge (part of Night Lighting)
//
// Outdoor objects (fences, bushes, Sims, props) are lit by per-object light rigs: slot 0 = sun, slots 1..3 = the three
// strongest point lights. The rig gather (FUN_006bbba0 -> FUN_006b5af0 -> FUN_006bb270) asks each light for its colour at
// the object centre through light vfunc+0x10. For street lamps (class vtable 0xFF42F8, slot 0xFF4308 = FUN_006c02a0) that
// colour falls off with the squared 3D distance from the lamp head, without the x3.33 the lot ground gets, so fences and
// bushes next to a lamp get ~30% of the light and dimmer lamps fall under the 0.1 luminance cut. A per-rig brightness cap
// (FUN_006b92a0, cap read at 0x6B9418 from 0x011D0BA8) then scales everything down again.
//
// Fix: replace that vtable slot with a wrapper that, only when called by the rig gather (return address 0x006BB2B3), uses
// the lamp's ground footprint instead: horizontal distance with the radius the game stores in the light bounds
// (+0x134..+0x140), like the terrain light stamp. The rig cap is raised so the result is not scaled back.
//
// A rig only gathers point lights when rig+0x224 bit 0x10 is set (FUN_006bbba0; the binder FUN_006b8b30 otherwise sends
// zero lamp constants). The rig constructor FUN_006bb8f0 copies that bit from the scene model (model+0x29C bit 4). The
// SceneModel constructor (FUN_006f5b40) sets it; the game clears it (FUN_006f4840(0)) for terrain, roads, roofs, the sea
// and for objects whose script asks so (message 0x827917ca). RigCtorForce sets it for every rig built by FUN_006f7880.
// Note: fences, railings and stairs already have it; their missing lamp light has another cause (they read only the
// rig's overflow "vertex light" slots) and is fixed in the pixel shader (lot_light_bridge.cpp DrawInstanced).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "object_light_bridge.h"
#include "level_light_share.h"
#include "game_addresses.h"
#include "memory_patch.h"
#include "apex_log.h"
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

// Addresses: the fixed Steam 1.67.2 ones (in the comments), or found by signature on other builds (game_addresses.h).
// Set by LoadAddresses before anything uses them; 0 = not found on this build.
uintptr_t kVtableSlot = 0;       // 0x00FF4308: class 0xFF42F8 (type 11, street lamp) vfunc+0x10
uintptr_t kOriginalFn = 0;       // 0x006C02A0: FUN_006c02a0
uintptr_t kRigGatherReturn = 0;  // 0x006BB2B3: after "call edx" in FUN_006bb270
uintptr_t kCapOperandSite = 0;   // 0x006B9418: mov ecx, offset 0x011D0BA8
std::vector<BYTE> kCapOrig;      // B9 <kCapGlobal>
uintptr_t kCapGlobal = 0;        // 0x011D0BA8
uintptr_t kLumaWeights = 0;      // 0x011D1140
uintptr_t kDirtyAllRigs = 0;     // 0x006B58F0: __fastcall(cells)
const BYTE kDirtyAllBytes[] = {0x83, 0xEC, 0x10, 0x55, 0x8B, 0xE9, 0x33, 0xC9, 0x33, 0xC0, 0x39, 0x4D, 0x30}; // Steam's prologue (checked on Steam)
uintptr_t kRootPtr = 0;          // 0x011D1860

using LightColour_t = void(__fastcall*)(void* light, void* edx, const float* pos, float* rec);
using DirtyAll_t = void(__fastcall*)(void* cells);

std::vector<MemPatch::PatchLocation> g_patches;
std::atomic<bool> g_installed{false};
float g_strength = 1.0f;
float g_capScaled = 1.0f;
std::atomic<bool> g_refreshRequested{false};
std::atomic<DWORD> g_renderThread{0}; // the game's light system may only be touched from the render thread
std::atomic<int> g_boosted{0};

uintptr_t kRigCtor = 0;            // 0x006BB8F0: FUN_006bb8f0 (thiscall rig, flag, model, kind)
uintptr_t kRigCtorCalls[3] = {};   // 0x006F7905, 0x006F795C, 0x006F799D: call sites in FUN_006f7880
std::vector<MemPatch::PatchLocation> g_rigPatches;
std::atomic<bool> g_forceAll{true};
std::atomic<int> g_forcedRigs{0};

using RigCtor_t = void*(__thiscall*)(void* rig, unsigned flag, void* model, int kind);

void* __fastcall RigCtorForce(void* rig, void*, unsigned flag, void* model, int kind) {
    if (!(flag & 1) && g_forceAll.load(std::memory_order_relaxed)) {
        flag |= 1;
        g_forcedRigs.fetch_add(1, std::memory_order_relaxed);
    }
    return reinterpret_cast<RigCtor_t>(kRigCtor)(rig, flag, model, kind);
}

// Fences and railings that close an area turn it into a roofless "room" (room+0x18 = 1). Objects inside get room mode 1
// (FUN_006c7cf0 -> FUN_006baa70) and their lamps come only from that room's light list (FUN_006bbde0 -> FUN_006bb2f0);
// the per-light test FUN_006bb270 only takes lights of the object's own room (light+8 == rig+0x1e0). Street lamps belong
// to no room, so a fenced yard never gets their light. Fix: after the room gather, for mode 1 rigs also run the world
// cell gather (FUN_006b5af0, the one outdoor objects use) with rig+0x1e0 briefly set to 0. The game then keeps the three
// strongest of both lists.
uintptr_t kRoomGatherCall = 0; // 0x006BBE70: CALL FUN_006bb2f0 in FUN_006bbde0
uintptr_t kRoomGather = 0;     // 0x006BB2F0
uintptr_t kCellGather = 0;     // 0x006B5AF0
std::vector<MemPatch::PatchLocation> g_roomPatches;
std::atomic<int> g_roomRigs{0};

using RoomGather_t = void(__thiscall*)(void* rig, void* a, void* b, void* c, void* d);
using CellGather_t = void(__thiscall*)(void* cells, void* rig);

void CellGatherForRoomRig(BYTE* rig) {
    __try {
        const uintptr_t lightMgr = *reinterpret_cast<const uintptr_t*>(rig + 0x1B4);
        if (!lightMgr || !(rig[0x224] & 0x10)) return;
        const uintptr_t cells = *reinterpret_cast<const uintptr_t*>(lightMgr + 0x104);
        if (!cells) return;
        uint32_t& room = *reinterpret_cast<uint32_t*>(rig + 0x1E0);
        const uint32_t saved = room;
        room = 0;
        __try {
            reinterpret_cast<CellGather_t>(kCellGather)(reinterpret_cast<void*>(cells), rig);
        } __finally {
            room = saved;
        }
        g_roomRigs.fetch_add(1, std::memory_order_relaxed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Room-mode rigs are taken out of the world light cells (FUN_006baa70), and the game re-gathers only in-cell rigs when
// lamps switch on at dusk (FUN_006b58f0 walks the cells). So the extra cell gather above would keep the lamps of the
// moment the rig was placed (none, for a save loaded by day). The rigs it served are remembered and re-gathered on the
// render thread whenever the night level moves by 0.1 (FUN_006bbf90 = unconditional rig update, which comes back here).
uintptr_t kRigUpdate = 0; // 0x006BBF90: __fastcall(rig)
uint32_t kRigVtable = 0;  // 0x00FF4218: set by the rig constructor FUN_006bb8f0
std::mutex g_roomRigMx;
std::unordered_set<BYTE*> g_roomRigSet;
float g_regatherLevel = -1.0f;
uintptr_t g_regatherCells = 0;

// The lamps of another story for room-mode rigs (06/10, user: "the light between stories is not perfect"; Light capture
// 13:20: the frame of an atrium's upper window was black, 0.02, right over the lower window's frame lit red by a sconce
// of the story below). FUN_006bbde0 gathers a roofed room's rig from that room's light list (room+0xC8), which holds the
// lamps of the stories next to it that the light between stories took near an opening (level_light_share part 4: the
// room's walls and floors are lit by them), but FUN_006bb2f0 keeps only lights whose room id is the object's (light+8 ==
// rig+0x1E0 at 0x6BB333, and again in FUN_006bb270 at 0x6BB283). So such a lamp lit the walls and the directional maps of
// the room but never its objects' rigs: the upper frame's rig held only the three [NoLight] lights and the fill light,
// and its directional map towards the room was dark there. Now, after the game's gather, the lamps of the list that
// belong to another room and reach the rig's centre (LevelLightShare::CrossLampReach: through an opening, past the walls
// on the way; at least kCrossReachMin) go through the game's own gather once more, per room id, with rig+0x1E0 set to
// that id for the call: the same colour at the centre, the same 0.1 luminance cut, the same wall test against the
// object's room walls (arguments a..d are passed through), into the same candidate list the game then sorts.
constexpr float kCrossReachMin = 0.25f;
constexpr float kCrossRangeM = 15.0f; // farther lamps are not tested (the reach test is the costly part)
constexpr int kCrossMax = 32; // lamps of another story looked at per rig
std::atomic<long> g_crossRigs{0}, g_crossLamps{0};
struct LightVector {
    uintptr_t* begin;
    uintptr_t* end;
    uintptr_t* cap;
};
// The rigs with a lamp of another story within kCrossRangeM: the game marks a room-mode rig for a new gather only when a
// lamp of its own room changes (0x006B9230: light+8 == rig+0x1E0), and FUN_006b58f0 (all rigs) walks only the world cells,
// which room-mode rigs leave (FUN_006baa70). They are gathered again with the other rigs after a lamp edit
// (RequestRigRefresh), so the lamp switching on or off below reaches them too.
std::mutex g_crossRigMx;
std::unordered_set<BYTE*> g_crossRigSet;
// The list's lamps of another room that reach the rig's centre: how many were written to lamps / ids; nearby: one of them
// lies within kCrossRangeM, reached or not
int CrossCandidates(BYTE* rig, BYTE* room, const LightVector* list, uintptr_t* lamps, int* ids, bool& nearby) {
    int n = 0;
    nearby = false;
    __try {
        const int own = *reinterpret_cast<const int*>(rig + 0x1E0);
        const float* centre = reinterpret_cast<const float*>(rig + 0x140);
        if (!list->begin || list->end < list->begin || list->end - list->begin > 4096) return 0;
        for (const uintptr_t* p = list->begin; p < list->end && n < kCrossMax; p++) {
            const uintptr_t light = *p;
            if (!light) continue;
            const int id = *reinterpret_cast<const int*>(light + 8);
            if (id == own || id <= 0) continue;
            const float* head = reinterpret_cast<const float*>(light + 0x120); // the lamp's position
            const float dx = head[0] - centre[0], dy = head[1] - centre[1], dz = head[2] - centre[2];
            if (!(dx * dx + dy * dy + dz * dz < kCrossRangeM * kCrossRangeM)) continue; // the game's 0.1 cut drops it anyway
            const float reach = LevelLightShare::CrossLampReach(room, reinterpret_cast<const void*>(light), centre);
            if (reach >= 0.0f) nearby = true; // a lamp of another story this room takes
            if (reach < kCrossReachMin) continue;
            lamps[n] = light;
            ids[n] = id;
            n++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return n;
}
// The game's gather for one room id's lamps, rig+0x1E0 set to that id for the call
void GatherAsRoom(BYTE* rig, int id, void* a, LightVector* list, void* c, void* d) {
    __try {
        int& room = *reinterpret_cast<int*>(rig + 0x1E0);
        const int saved = room;
        room = id;
        __try {
            reinterpret_cast<RoomGather_t>(kRoomGather)(rig, a, list, c, d);
        } __finally {
            room = saved;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
void CrossStoryLamps(BYTE* rig, void* a, void* b, void* c, void* d) {
    uintptr_t lamps[kCrossMax];
    int ids[kCrossMax];
    BYTE* room = static_cast<BYTE*>(a) - 0x30; // a = room+0x30, the room's walls (FUN_006bbde0 at 0x6BBE6A)
    bool nearby = false;
    const int n = CrossCandidates(rig, room, static_cast<const LightVector*>(b), lamps, ids, nearby);
    if (nearby) {
        std::lock_guard<std::mutex> lk(g_crossRigMx);
        if (g_crossRigSet.size() < 8192) g_crossRigSet.insert(rig);
    }
    if (n <= 0) return;
    bool done[kCrossMax] = {};
    for (int i = 0; i < n; i++) {
        if (done[i]) continue;
        uintptr_t group[kCrossMax];
        int count = 0;
        for (int j = i; j < n; j++)
            if (!done[j] && ids[j] == ids[i]) {
                group[count++] = lamps[j];
                done[j] = true;
            }
        LightVector one{group, group + count, group + count};
        GatherAsRoom(rig, ids[i], a, &one, c, d);
    }
    g_crossRigs.fetch_add(1, std::memory_order_relaxed);
    g_crossLamps.fetch_add(n, std::memory_order_relaxed);
}

void __fastcall RoomGatherThunk(BYTE* rig, void*, void* a, void* b, void* c, void* d) {
    reinterpret_cast<RoomGather_t>(kRoomGather)(rig, a, b, c, d);
    const int mode = *reinterpret_cast<const int*>(rig + 0x1D4);
    if (mode == 0 && a && b) CrossStoryLamps(rig, a, b, c, d);
    if (!g_forceAll.load(std::memory_order_relaxed) || mode != 1) return;
    CellGatherForRoomRig(rig);
    std::lock_guard<std::mutex> lk(g_roomRigMx);
    if (g_roomRigSet.size() < 8192) g_roomRigSet.insert(rig);
}

// True when the rig is still a live room-mode rig and was updated.
bool RegatherRoomRig(BYTE* rig, int mode = 1) {
    if (!kRigUpdate || !kRigVtable) return false;
    __try {
        if (*reinterpret_cast<const uint32_t*>(rig) != kRigVtable || *reinterpret_cast<const int*>(rig + 0x1D4) != mode) return false;
        reinterpret_cast<void(__fastcall*)(void*)>(kRigUpdate)(rig);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RegatherRoomRigs() {
    std::vector<BYTE*> list;
    {
        std::lock_guard<std::mutex> lk(g_roomRigMx);
        list.assign(g_roomRigSet.begin(), g_roomRigSet.end());
    }
    std::vector<BYTE*> gone;
    for (BYTE* rig : list)
        if (!RegatherRoomRig(rig)) gone.push_back(rig);
    std::lock_guard<std::mutex> lk(g_roomRigMx);
    for (BYTE* rig : gone) g_roomRigSet.erase(rig);
}
// The room-mode rigs near a lamp of another story gather again (after a lamp edit, see g_crossRigSet)
void RegatherCrossRigs() {
    std::vector<BYTE*> list;
    {
        std::lock_guard<std::mutex> lk(g_crossRigMx);
        list.assign(g_crossRigSet.begin(), g_crossRigSet.end());
    }
    std::vector<BYTE*> gone;
    for (BYTE* rig : list)
        if (!RegatherRoomRig(rig, 0)) gone.push_back(rig);
    std::lock_guard<std::mutex> lk(g_crossRigMx);
    for (BYTE* rig : gone) g_crossRigSet.erase(rig);
}

// Render thread, every frame: current night level and light cells (0 when no world).
bool ReadNight(float& level, uintptr_t& cells) {
    if (!kRootPtr) return false;
    __try {
        const uintptr_t root = *reinterpret_cast<const uintptr_t*>(kRootPtr);
        const uintptr_t lightMgr = root ? *reinterpret_cast<const uintptr_t*>(root + 0x1C0) : 0;
        if (!lightMgr) return false;
        level = *reinterpret_cast<const float*>(lightMgr + 0xF0);
        cells = *reinterpret_cast<const uintptr_t*>(lightMgr + 0x104);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void UpdateRoomRigs() {
    float level = 0.0f;
    uintptr_t cells = 0;
    if (!ReadNight(level, cells)) return;
    if (cells != g_regatherCells) { // another world: the remembered rigs are gone
        g_regatherCells = cells;
        g_regatherLevel = level;
        {
            std::lock_guard<std::mutex> lk(g_crossRigMx);
            g_crossRigSet.clear();
        }
        std::lock_guard<std::mutex> lk(g_roomRigMx);
        g_roomRigSet.clear();
        return;
    }
    if (std::fabs(level - g_regatherLevel) < 0.1f) return;
    g_regatherLevel = level;
    RegatherRoomRigs();
}

// FUN_006b5af0 returns with "ret 4" (thiscall(cells, rig)): at +0x145 on Steam; on other builds within its first 0x400
// bytes after the stack cleanup "add esp,1Ch" (its frame, same signature)
bool CellGatherReturns4() {
    if (GameAddr::IsFixed()) return std::memcmp(reinterpret_cast<const void*>(kCellGather + 0x145), "\xC2\x04\x00", 3) == 0;
    BYTE f[0x400];
    if (!MemPatch::ReadBytes(kCellGather, f, sizeof f)) return false;
    for (size_t i = 0; i + 6 <= sizeof f; i++)
        if (std::memcmp(f + i, "\x83\xC4\x1C\xC2\x04\x00", 6) == 0) return true;
    return false;
}

bool InstallRoomGatherPatch() {
    std::string missing;
    if (!GameAddr::Have({GameAddr::Id::RoomGatherCall, GameAddr::Id::RoomGather, GameAddr::Id::CellGather, GameAddr::Id::RigUpdate, GameAddr::Id::RigVtable}, &missing)) {
        LOG_WARNING("[ObjectLightBridge] Fenced areas: " + GameAddr::NotAvailable(missing));
        return false;
    }
    if (*reinterpret_cast<const BYTE*>(kRoomGatherCall) != 0xE8 || kRoomGatherCall + 5 + *reinterpret_cast<const int32_t*>(kRoomGatherCall + 1) != kRoomGather ||
        !CellGatherReturns4())
        return false;
    DWORD orig = static_cast<DWORD>(kRoomGather - (kRoomGatherCall + 5));
    const DWORD rel = static_cast<DWORD>(reinterpret_cast<uintptr_t>(&RoomGatherThunk) - (kRoomGatherCall + 5));
    return MemPatch::WriteDWORD(kRoomGatherCall + 1, rel, &g_roomPatches, &orig);
}

bool InstallRigCtorPatch() {
    std::string missing;
    if (!GameAddr::Have({GameAddr::Id::RigCtor, GameAddr::Id::RigCtorCall0, GameAddr::Id::RigCtorCall1, GameAddr::Id::RigCtorCall2}, &missing)) {
        LOG_WARNING("[ObjectLightBridge] Object light creation: " + GameAddr::NotAvailable(missing));
        return false;
    }
    for (uintptr_t site : kRigCtorCalls) {
        if (*reinterpret_cast<const BYTE*>(site) != 0xE8 ||
            site + 5 + *reinterpret_cast<const int32_t*>(site + 1) != kRigCtor) return false;
    }
    for (uintptr_t site : kRigCtorCalls) {
        DWORD orig = static_cast<DWORD>(kRigCtor - (site + 5));
        const DWORD rel = static_cast<DWORD>(reinterpret_cast<uintptr_t>(&RigCtorForce) - (site + 5));
        if (!MemPatch::WriteDWORD(site + 1, rel, &g_rigPatches, &orig)) {
            MemPatch::RestoreAll(g_rigPatches);
            g_rigPatches.clear();
            return false;
        }
    }
    return true;
}

// All 9 light classes (light_vtables.txt listed 7; 0xFF4408 CircleWindowLight and 0xFF4468 TubeLight, both made by the
// factory FUN_006ac590, were missing) answer "colour of this light at a point" with vfunc+0x10, all in
// the same record format (FUN_006bdb00). The street lamp class got the ground-footprint boost first; lot lamps (the
// other classes) kept the 1/d2 falloff, so objects on lots (fences, plants, mailboxes) got ~0.1 or fell under the 0.1
// luminance cut (captures m32-m41). Every class goes through the boost now, but only lit, outdoor (room 0) lamps of
// lamp types (3..6; street lamps 0xB), so interiors are unchanged.
struct LightClass {
    uintptr_t slot; // vtable + 0x10
    uintptr_t orig;
};
// Steam: {0x00FF42B0, 0x006C02A0}, {0x00FF4308, 0x006C02A0}, {0x00FF4360, 0x006C0690}, {0x00FF43B8, 0x006C0AF0},
// {0x00FF44D0, 0x006C16D0}, {0x00FF4528, 0x006C1980}, {0x00FF4580, 0x006C1BC0}, {0x00FF4418, 0x006C0FE0}, {0x00FF4478, 0x006C1320}.
// Light types of those classes (the light factory FUN_006ac590 makes type N with the vtable GameAddr::LightVtableN):
constexpr int kClassTypes[] = {3, 11, 5, 7, 9, 10, 4, 8, 6};
LightClass kClasses[std::size(kClassTypes)] = {}; // set by LoadAddresses; {0, 0} = class not found on this build
constexpr int kStreetClass = 1; // vtable 0xFF42F8 (type 11)
std::atomic<int> g_classesPatched{0};

void BoostRec(BYTE* L, const float* pos, float* rec, bool street) {
    __try {
        const BYTE f = L[0x100];
        if (!(f & 0x20)) return; // lamp off
        if (!street) {
            if (!(f & 0x04) || *reinterpret_cast<const int*>(L + 0x08) != 0) return; // outdoor lamps only
            const int type = *reinterpret_cast<const int*>(L + 0xB0);
            if (type < 3 || type > 6) return;
        }
        const float* head = reinterpret_cast<const float*>(L + 0x120);
        const float* bounds = reinterpret_cast<const float*>(L + 0x134); // minX, minZ, maxX, maxZ
        const float radius = 0.5f * (bounds[2] - bounds[0]);
        if (!(radius > 0.5f && radius < 100.0f)) return;
        const float dx = head[0] - pos[0], dz = head[2] - pos[2];
        float w = 1.0f - (dx * dx + dz * dz) / (radius * radius);
        if (w <= 0.0f) return;
        w *= w;
        const float intensity = *reinterpret_cast<const float*>(L + 0x10);
        const float fade = *reinterpret_cast<const float*>(L + 0x20);
        const float s = g_strength * intensity * fade * w;
        const float* base = reinterpret_cast<const float*>(L + 0xF0);
        bool changed = false;
        for (int c = 0; c < 3; c++) {
            const float v = base[c] * s;
            if (v > rec[4 + c]) {
                rec[4 + c] = v;
                changed = true;
            }
        }
        if (changed) {
            const float* luma = reinterpret_cast<const float*>(kLumaWeights);
            rec[10] = rec[4] * luma[0] + rec[5] * luma[1] + rec[6] * luma[2];
            g_boosted.fetch_add(1, std::memory_order_relaxed);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

template <int I> void __fastcall ClassColour(void* L, void* edx, const float* pos, float* rec) {
    reinterpret_cast<LightColour_t>(kClasses[I].orig)(L, edx, pos, rec);
    if (_ReturnAddress() != reinterpret_cast<void*>(kRigGatherReturn)) return;
    BoostRec(static_cast<BYTE*>(L), pos, rec, I == kStreetClass);
}

const LightColour_t kClassThunks[] = {&ClassColour<0>, &ClassColour<1>, &ClassColour<2>, &ClassColour<3>, &ClassColour<4>, &ClassColour<5>, &ClassColour<6>,
                                     &ClassColour<7>, &ClassColour<8>};
static_assert(std::size(kClassThunks) == std::size(kClasses));

void DirtyAllRigs() {
    if (!kRootPtr || !kDirtyAllRigs) return; // not found on this build
    __try {
        const uintptr_t root = *reinterpret_cast<const uintptr_t*>(kRootPtr);
        if (!root) return;
        const uintptr_t lightMgr = *reinterpret_cast<const uintptr_t*>(root + 0x1C0);
        if (!lightMgr) return;
        const uintptr_t cells = *reinterpret_cast<const uintptr_t*>(lightMgr + 0x104);
        if (!cells) return;
        reinterpret_cast<DirtyAll_t>(kDirtyAllRigs)(reinterpret_cast<void*>(cells));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ---- Lamp colour. The game's stock lamps (street lamps and lot lamps) are pink: base colour (1, 0.75, 0.79), measured
// in the light objects and in the lot light maps (G/R 0.754, B/R 0.793), with or without snow. On green grass it barely
// shows, on white snow it does. The colour reaches a light two ways:
//  - at creation, from its definition: FUN_006bda90(light, def+0x10), 7 callers (one per light type);
//  - from the lamp object's script (stock colour and colours picked in build mode): FUN_006b0b50(objId, r, g, b) ->
//    FUN_006bc3e0(light, rgba) for every light of the object (call at 0x6B0BDE).
// Both write +0xF0 (base colour) and +0xE0 (intensity * colour when lit). Both go through TintStockColour, which turns
// the stock pink into warm white (tungsten-like) with the same brightness. Other colours are left alone.
uintptr_t kSetLightColour = 0;      // 0x006BDA90
uintptr_t kSetColourCalls[7] = {};  // 0x006C047D, 0x006C051D, 0x006C05C1, 0x006C1251, 0x006C15D1, 0x006C1891, 0x006C1B11
uintptr_t kScriptSetColour = 0;     // 0x006BC3E0
uintptr_t kScriptSetColourCall = 0; // 0x006B0BDE
std::vector<MemPatch::PatchLocation> g_colourPatches;
std::atomic<float> g_lampTint{1.0f};    // street lamps (and lot lamps unless they have their own)
std::atomic<float> g_lotLampTint{1.0f}; // lot lamps (= g_lampTint when "own colour for lot lamps" is off)
std::atomic<int> g_tinted{0};

using SetColour_t = void(__thiscall*)(void* light, const float* rgb);

// Every light that got the stock pink: its pink and what was written, so a tint change can re-colour it live
// (RetintLamps). Written by the two colour thunks (game thread), read by RetintLamps (render thread = the same thread in
// this game; the mutex keeps it safe either way). Entries of freed lights are dropped by the next RetintLamps (they are
// not in the light enumeration); a light created again at the same address replaces its entry.
struct TintedLight {
    float pink[3];
    float written[3];
    bool street;
};
std::mutex g_tintedMutex;
std::unordered_map<uintptr_t, TintedLight> g_tintedLights;

bool IsStockPink(const float c[3]) {
    if (c[0] <= 0.05f) return false;
    const float g = c[1] / c[0], b = c[2] / c[0];
    return std::fabs(g - 0.75f) < 0.03f && std::fabs(b - 0.79f) < 0.03f;
}

// The stock pink turned towards warm white (1, 0.80, 0.62) by t, with the luminance of the original
void TintPink(const float pink[3], float t, float out[3]) {
    const float g = pink[1] / pink[0], b = pink[2] / pink[0];
    const float lumPink = 0.2126f + 0.7152f * g + 0.0722f * b;
    const float lumWarm = 0.2126f + 0.7152f * 0.80f + 0.0722f * 0.62f;
    const float k = pink[0] * lumPink / lumWarm;
    const float warm[3] = {k, 0.80f * k, 0.62f * k};
    for (int i = 0; i < 3; i++) out[i] = pink[i] + (warm[i] - pink[i]) * t;
}

float TintFor(bool street) { return (street ? g_lampTint : g_lotLampTint).load(std::memory_order_relaxed); }

// A street lamp: the street-lamp class (0xB) outside any lot (lot id +0xC0/+0xC4 = 0)
bool IsStreetLight(const BYTE* light) {
    __try {
        return *reinterpret_cast<const int*>(light + 0xB0) == 0xB && *reinterpret_cast<const uint32_t*>(light + 0xC0) == 0 &&
               *reinterpret_cast<const uint32_t*>(light + 0xC4) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Tints c in place when it is the stock pink and records the light; any other colour forgets the light
void TintStockColour(void* light, float c[3], bool street) {
    const uintptr_t key = reinterpret_cast<uintptr_t>(light);
    if (!IsStockPink(c)) {
        std::lock_guard lock(g_tintedMutex);
        g_tintedLights.erase(key);
        return;
    }
    TintedLight rec{{c[0], c[1], c[2]}, {}, street};
    const float t = TintFor(street);
    if (t > 0.0f) {
        TintPink(rec.pink, t, c);
        g_tinted.fetch_add(1, std::memory_order_relaxed);
    }
    std::memcpy(rec.written, c, sizeof(rec.written));
    std::lock_guard lock(g_tintedMutex);
    g_tintedLights[key] = rec;
}

// Creation (one call site per light class): the class decides street or lot (site 1 = the street-lamp class 0xB; its
// lot id may not be set yet, the lamp's script colour, which comes later, decides with the lot id)
template <bool Street> void __fastcall LampColourSet(void* light, void*, const float* rgb) {
    float c[4] = {rgb[0], rgb[1], rgb[2], 0.0f}; // this setter reads only rgb
    TintStockColour(light, c, Street);
    reinterpret_cast<SetColour_t>(kSetLightColour)(light, c);
}

void __fastcall LampColourSetScript(void* light, void*, const float* rgba) {
    // FUN_006bc3e0 reads its argument with MOVAPS: the buffer must be 16-byte aligned (the game's caller aligns its frame).
    alignas(16) float c[4] = {rgba[0], rgba[1], rgba[2], rgba[3]}; // w = r (FUN_006b0b50 passes r, g, b, r)
    TintStockColour(light, c, IsStreetLight(static_cast<const BYTE*>(light)));
    c[3] = c[0];
    reinterpret_cast<SetColour_t>(kScriptSetColour)(light, c);
}

bool RedirectCall(uintptr_t site, uintptr_t target, const void* thunk) {
    if (*reinterpret_cast<const BYTE*>(site) != 0xE8 || site + 5 + *reinterpret_cast<const int32_t*>(site + 1) != target) return false;
    DWORD orig = static_cast<DWORD>(target - (site + 5));
    const DWORD rel = static_cast<DWORD>(reinterpret_cast<uintptr_t>(thunk) - (site + 5));
    return MemPatch::WriteDWORD(site + 1, rel, &g_colourPatches, &orig);
}

// Fills the addresses above from GameAddr once it resolved (Steam: the fixed values in the comments).
void LoadAddresses() {
    static bool loaded = false;
    if (loaded || !GameAddr::Resolved()) return;
    loaded = true;
    using GameAddr::Get;
    using GameAddr::Id;
    const uintptr_t streetVtable = Get(Id::LightVtable11);
    kVtableSlot = streetVtable ? streetVtable + 0x10 : 0;
    kOriginalFn = Get(Id::LightColour11);
    kRigGatherReturn = Get(Id::RigGatherReturn);
    kCapOperandSite = Get(Id::CapOperandSite);
    kCapGlobal = Get(Id::CapGlobal);
    const uint32_t cap = static_cast<uint32_t>(kCapGlobal);
    kCapOrig = {0xB9, static_cast<BYTE>(cap), static_cast<BYTE>(cap >> 8), static_cast<BYTE>(cap >> 16), static_cast<BYTE>(cap >> 24)};
    kLumaWeights = Get(Id::LumaWeights);
    kDirtyAllRigs = Get(Id::DirtyAllRigs);
    kRootPtr = Get(Id::RootPtr);
    kRigCtor = Get(Id::RigCtor);
    for (int i = 0; i < 3; i++) kRigCtorCalls[i] = Get(static_cast<Id>(static_cast<int>(Id::RigCtorCall0) + i));
    kRoomGatherCall = Get(Id::RoomGatherCall);
    kRoomGather = Get(Id::RoomGather);
    kCellGather = Get(Id::CellGather);
    kRigUpdate = Get(Id::RigUpdate);
    kRigVtable = static_cast<uint32_t>(Get(Id::RigVtable));
    for (size_t i = 0; i < std::size(kClassTypes); i++) {
        const int t = kClassTypes[i] - 3;
        const uintptr_t vt = Get(static_cast<Id>(static_cast<int>(Id::LightVtable3) + t));
        const uintptr_t fn = Get(static_cast<Id>(static_cast<int>(Id::LightColour3) + t));
        kClasses[i] = vt && fn ? LightClass{vt + 0x10, fn} : LightClass{0, 0};
    }
    kSetLightColour = Get(Id::SetLightColour);
    for (int i = 0; i < 7; i++) kSetColourCalls[i] = Get(static_cast<Id>(static_cast<int>(Id::SetColourCall0) + i));
    kScriptSetColour = Get(Id::ScriptSetColour);
    kScriptSetColourCall = Get(Id::ScriptSetColourCall);
}

// The return address follows "call reg" (FF D0..FF D7): FF D2 (call edx) on Steam
bool AfterCallReg(uintptr_t ret) {
    const BYTE* p = reinterpret_cast<const BYTE*>(ret - 2);
    return GameAddr::IsFixed() ? std::memcmp(p, "\xFF\xD2", 2) == 0 : (p[0] == 0xFF && (p[1] & 0xF8) == 0xD0);
}

} // namespace

namespace ObjectLightBridge {

bool InstallLampColour() {
    if (!g_colourPatches.empty()) return true;
    LoadAddresses();
    std::string missing;
    if (!GameAddr::Have({GameAddr::Id::SetLightColour, GameAddr::Id::SetColourCall0, GameAddr::Id::SetColourCall1, GameAddr::Id::SetColourCall2, GameAddr::Id::SetColourCall3,
                         GameAddr::Id::SetColourCall4, GameAddr::Id::SetColourCall5, GameAddr::Id::SetColourCall6, GameAddr::Id::ScriptSetColour, GameAddr::Id::ScriptSetColourCall},
                        &missing)) {
        LOG_WARNING("[ObjectLightBridge] Lamp colour: " + GameAddr::NotAvailable(missing));
        return false;
    }
    bool ok = true;
    for (int i = 0; i < 7; i++) // site 1 (0x006C051D) is the street-lamp class's constructor
        ok = ok && RedirectCall(kSetColourCalls[i], kSetLightColour, i == 1 ? reinterpret_cast<const void*>(&LampColourSet<true>) : reinterpret_cast<const void*>(&LampColourSet<false>));
    ok = ok && RedirectCall(kScriptSetColourCall, kScriptSetColour, reinterpret_cast<const void*>(&LampColourSetScript));
    if (!ok) {
        MemPatch::RestoreAll(g_colourPatches);
        g_colourPatches.clear();
        LOG_WARNING("[ObjectLightBridge] Lamp colour: call site differs");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    LOG_INFO("[ObjectLightBridge] Lamp colour: installed (creation + script)");
    return true;
}

void UninstallLampColour() {
    if (g_colourPatches.empty()) return;
    MemPatch::RestoreAll(g_colourPatches);
    g_colourPatches.clear();
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
}

void SetLampTint(float street, float lot) {
    g_lampTint = street < 0 ? 0.0f : (street > 1 ? 1.0f : street);
    g_lotLampTint = lot < 0 ? 0.0f : (lot > 1 ? 1.0f : lot);
}

} // namespace ObjectLightBridge

namespace {
// One tracked light with its current tint: -1 = not ours any more (another colour, or a light at a reused address),
// 0 = already right, 1 = re-coloured (rec.written updated). No C++ objects here (SEH).
int RetintLight(uintptr_t L, TintedLight& rec) {
    __try {
        if (std::memcmp(reinterpret_cast<const void*>(L + 0xF0), rec.written, sizeof(rec.written)) != 0) return -1;
        alignas(16) float c[4];
        const float t = TintFor(rec.street);
        if (t > 0.0f) TintPink(rec.pink, t, c);
        else std::memcpy(c, rec.pink, sizeof(rec.pink));
        c[3] = c[0];
        if (std::memcmp(c, rec.written, sizeof(rec.written)) == 0) return 0;
        reinterpret_cast<SetColour_t>(kScriptSetColour)(reinterpret_cast<void*>(L), c); // +0xF0, and +0xE0 when lit
        std::memcpy(rec.written, c, sizeof(rec.written));
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
} // namespace

namespace ObjectLightBridge {

int RetintLamps(const std::vector<uintptr_t>& lights) {
    if (g_colourPatches.empty() || !kScriptSetColour) return 0;
    std::lock_guard lock(g_tintedMutex);
    std::unordered_map<uintptr_t, TintedLight> alive;
    alive.reserve(g_tintedLights.size());
    int changed = 0;
    for (uintptr_t L : lights) {
        auto it = g_tintedLights.find(L);
        if (it == g_tintedLights.end()) continue;
        TintedLight rec = it->second;
        const int r = RetintLight(L, rec);
        if (r < 0) continue;
        changed += r;
        alive.emplace(L, rec);
    }
    g_tintedLights.swap(alive);
    if (changed > 0) {
        g_refreshRequested = true; // the rigs gather the new colour (next OnPresent)
        LOG_INFO(std::format("[ObjectLightBridge] Lamp colour changed live: {} lights re-coloured (street {:.2f}, lot {:.2f})", changed, g_lampTint.load(),
                             g_lotLampTint.load()));
    }
    return changed;
}

std::string LampColourStatus() {
    std::lock_guard lock(g_tintedMutex);
    return std::format("{} | lights with corrected colour: {} | stock lamps tracked: {}", g_colourPatches.empty() ? "off" : "active", g_tinted.load(),
                       g_tintedLights.size());
}

bool Install(std::string& error) {
    if (g_installed) return true;
    LoadAddresses();
    std::string missing;
    if (!GameAddr::Have({GameAddr::Id::LightVtable11, GameAddr::Id::LightColour11, GameAddr::Id::RigGatherReturn, GameAddr::Id::CapOperandSite, GameAddr::Id::CapGlobal,
                         GameAddr::Id::LumaWeights, GameAddr::Id::DirtyAllRigs, GameAddr::Id::RootPtr},
                        &missing)) {
        error = "Objects: " + GameAddr::NotAvailable(missing);
        return false;
    }
    if (*reinterpret_cast<const uint32_t*>(kVtableSlot) != kOriginalFn) {
        error = std::format("Street lamp light function differs at 0x{:X}", kVtableSlot);
        return false;
    }
    if (!MemPatch::ValidateBytes(reinterpret_cast<LPVOID>(kCapOperandSite), kCapOrig.data(), kCapOrig.size()) ||
        (GameAddr::IsFixed() && std::memcmp(reinterpret_cast<const void*>(kDirtyAllRigs), kDirtyAllBytes, sizeof(kDirtyAllBytes)) != 0) ||
        !AfterCallReg(kRigGatherReturn)) {
        error = "Object light code differs (different game version?)";
        return false;
    }
    g_capScaled = *reinterpret_cast<const float*>(kCapGlobal);
    const DWORD capAddr = static_cast<DWORD>(reinterpret_cast<uintptr_t>(&g_capScaled));
    std::vector<BYTE> capBytes = {0xB9, 0, 0, 0, 0};
    std::memcpy(capBytes.data() + 1, &capAddr, 4);
    if (!MemPatch::WriteBytes(kCapOperandSite, capBytes, &g_patches, &kCapOrig)) {
        MemPatch::RestoreAll(g_patches);
        error = "Could not patch the object light";
        return false;
    }
    int classes = 0;
    for (int i = 0; i < static_cast<int>(std::size(kClasses)); i++) {
        if (!kClasses[i].slot) continue; // class not found on this build
        if (*reinterpret_cast<const uint32_t*>(kClasses[i].slot) != kClasses[i].orig) continue; // not the expected function: leave it
        DWORD orig = static_cast<DWORD>(kClasses[i].orig);
        if (MemPatch::WriteDWORD(kClasses[i].slot, static_cast<DWORD>(reinterpret_cast<uintptr_t>(kClassThunks[i])), &g_patches, &orig)) classes++;
    }
    g_classesPatched = classes;
    LOG_INFO(std::format("[ObjectLightBridge] Light classes boosted: {} of {}", classes, std::size(kClasses)));
    if (!InstallRigCtorPatch()) LOG_WARNING("[ObjectLightBridge] Object light creation differs; stairs and railings stay without lamps");
    if (!InstallRoomGatherPatch()) LOG_WARNING("[ObjectLightBridge] Light gathering in fenced areas differs; fences of enclosed areas stay without street lamps");
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_installed = true;
    g_refreshRequested = true;
    LOG_INFO("[ObjectLightBridge] Installed");
    return true;
}

void Uninstall() {
    if (!g_installed) return;
    MemPatch::RestoreAll(g_patches);
    g_patches.clear();
    MemPatch::RestoreAll(g_rigPatches);
    g_rigPatches.clear();
    MemPatch::RestoreAll(g_roomPatches);
    g_roomPatches.clear();
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_installed = false;
    if (GetCurrentThreadId() == g_renderThread.load()) DirtyAllRigs();
    else g_refreshRequested = true; // done by the next OnPresent on the render thread
    LOG_INFO("[ObjectLightBridge] Uninstalled");
}

void SetStrength(float s) {
    if (s != g_strength) {
        g_strength = s;
        g_refreshRequested = true;
    }
}

void OnPresent() {
    g_renderThread = GetCurrentThreadId();
    LoadAddresses();
    if (!g_installed) {
        if (g_refreshRequested.exchange(false)) DirtyAllRigs();
        return;
    }
    __try {
        g_capScaled = *reinterpret_cast<const float*>(kCapGlobal) * std::max(1.0f, g_strength); // original cap unless the user raises the strength
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (g_refreshRequested.exchange(false)) {
        DirtyAllRigs();
        g_regatherLevel = -1.0f; // also the room-mode rigs
        RegatherCrossRigs();     // and those near a lamp of another story (the game never marks them for it)
    }
    UpdateRoomRigs();
}

void SetAllObjects(bool on) { g_forceAll = on; }

void RequestRigRefresh() { g_refreshRequested = true; }

std::string Status() {
    return std::format("{} | light classes: {}/{} | lights boosted on objects: {} | objects opened to lamps (stairs, railings...): {}{} | in fenced areas: {} | "
                       "indoor objects given lamps of another story: {} ({} lamps offered)",
                       g_installed ? "Active" : "Off", g_classesPatched.load(), std::size(kClasses), g_boosted.load(), g_forcedRigs.load(),
                       g_rigPatches.empty() && g_installed ? " (not installed)" : "", g_roomRigs.load(), g_crossRigs.load(), g_crossLamps.load());
}

} // namespace ObjectLightBridge
