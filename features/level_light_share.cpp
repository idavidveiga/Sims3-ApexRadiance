// Outdoor lot lamps on every floor (part of Night Lighting)
//
// Walls and floors of a lot are lit by a light map per room and per floor (one lot lighting manager per level; the wall
// shader PS_2669DA40 reads it in s2 through per-vertex UVs). The room's light list (room+0xC8..+0xCC) is gathered by
// FUN_006c7010 -> FUN_006c6ab0(treeLevel, room):
//  - FUN_006c6990(treeLevel, room, 1): the lot lights registered on that floor for that room;
//  - only when treeLevel is level 0 and the room is room 0: FUN_006c6990(level 0, room, 0) once more (0x6C6B25), so the
//    ground level's outdoor lamps count twice (measured in ApexRadiance_LightDiag.txt);
//  - world lights around the lot (light cells, FUN_006b66b0). With the Split-Level Lighting Fix, type 11 lot lights also
//    come in here, on every floor.
// Lot load (FUN_006c54e0, 0x6C5525) gathers room 0 of EVERY floor through level 0's treeLevel; later updates
// (FUN_006c7250) gather it through the room's own floor. Either way a lamp on an upper floor's outside wall never lit the
// floors below, and ground lamps lit the upper floors only until the first update: straight cut at the floor line
// (LightProbe-andar2-b / andar1-b).
//
// Fix:
//  1. Both calls of FUN_006c6ab0 (0x6C5816 room creation, 0x6C7094 room update) go through OutdoorGather. After the game's
//     gather, room 0 of floors 0..7 also takes the outdoor lights of the other floors 0..7, with the weight each lamp has
//     on its own floor (level 0 lamps twice, the others once). Each lamp then lights every floor alike. The room's real
//     floor comes from its manager (room[0] + 0x88); on the lot-load path the gather floor is level 0 and the sharing is
//     done relative to it, which gives the same weights. Basements (levels < 0) stay as the game has them.
//  2. FUN_006c7250 at 0x6C73B1: "room 0 of level 0 changed -> refresh room 0 of all levels 0..7" becomes "room 0 of any
//     level 0..7 changed" (JNZ -> JL on the level test), so turning on, recolouring or moving an upper-floor lamp also
//     refreshes the floors below.
//  3. Walls of the lamp's floor. The light map of a point sums every light of the room's list (LightPointWithAllLights
//     0x69FD60) and tests each one against the room's 2D wall occluders (FUN_0069fc40 -> FUN_0069d4c0, room+0x30). Those
//     are only the walls of the room's own floor, and a wall blocks a ray only below its top (FUN_0069aa90). So a sconce on
//     the upper floor, next to a corner, lit the lower floor's side wall around the corner: the ray passes above the lower
//     walls and the upper walls are not in the list (LightProbe-andar1-c: side wall lighter below the floor line). The
//     light's evaluation (class vfunc+0x4C) is wrapped: when called from that loop (return 0x69FE19) for a light that came
//     from another floor, it is also tested with the game's own FUN_0069fc40 against the walls of room 0 of the lamp's
//     floor and of the floors in between. The room's own lamps are tested only as the game does (a porch lamp under an
//     overhang is not blocked by the upper floor's walls).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "level_light_share.h"
#include "game_addresses.h"
#include "memory_patch.h"
#include "apex_log.h"
#include "build_flavor.h"
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#pragma intrinsic(_ReturnAddress)

namespace {

// Addresses: the fixed Steam 1.67.2 ones (in the comments), or found by signature on other builds (game_addresses.h).
// Set by LoadAddresses (Install); 0 = not found on this build.
uintptr_t kAddWorldLights = 0;         // 0x006C6AB0: FUN_006c6ab0 thiscall(treeLevel, room) ret 4
uintptr_t kGatherCalls[2] = {};        // 0x006C5816, 0x006C7094: its two callers
uintptr_t kLevelGather = 0;            // 0x006C6990: thiscall(treeLevel, room, char ownFloor) ret 8
uintptr_t kLevelGatherCalls[2] = {};   // 0x006C6B08, 0x006C6B2D: the game's two calls of it, inside FUN_006c6ab0
uintptr_t kRoomById = 0;               // 0x006A6550: thiscall(manager, id) ret 4
uintptr_t kInvalidateRoom = 0;         // 0x0069EED0: thiscall(room, char full, char keep) ret 8
uintptr_t kSetInsert = 0;              // 0x00B7AAD0: thiscall(set, out, const int* key, char) ret 0xC
uintptr_t kRoomByIdCall = 0, kInvalidateCall = 0, kSetInsertCall = 0; // 0x006C73F0, 0x006C73FF, 0x006C741B: same, in FUN_006c7250
uintptr_t kCascadeTest = 0;            // 0x006C73AA: cmp [esi+0x1a0], eax / jnz 0x6C7432
const BYTE kCascadeBytes[] = {0x39, 0x86, 0xA0, 0x01, 0x00, 0x00, 0x0F, 0x85, 0x7C, 0x00, 0x00, 0x00}; // Steam; other builds: the first 8 (the jnz distance may differ)
uintptr_t kCascadeJcc = 0;             // 0x006C73B1 (kCascadeTest + 7): 0x85 (jnz) -> 0x8C (jl)
uintptr_t kRootPtr = 0;                // 0x011D1860

uintptr_t kSolvePoint = 0;             // 0x0069FD60: LightPointWithAllLights thiscall(room, out, l2D, l3D, flags, sample) ret 0x14
uintptr_t kSolvePointCalls[3] = {};    // 0x006A1187, 0x006A126F, 0x006A3336: FUN_006a0f50 x2, FUN_006a31d0
uintptr_t kLightEvalReturn = 0;        // 0x0069FE19: after "call edx" (light vfunc+0x4C) in it
uintptr_t kWallTest = 0;               // 0x0069FC40: thiscall(room, int* indexVec, lightPos, sample, float* t) ret 0x10
uintptr_t kWallTestCall = 0;           // 0x0069FE93: the game's call of it, in LightPointWithAllLights
uintptr_t kLightPos = 0;               // 0x009691E0: vfunc+0x24 of the 9 classes: thiscall(light, float out[4]) ret 4
uintptr_t kBatchSolveCall = 0;         // 0x006A3336: the call inside FUN_006a31d0 (a batch of wall samples)
uintptr_t kBatchSamples = 0;           // 0x01158AC8: global vector {begin, end} of that batch, 0x30 per sample
// "push 0x1158AC8" in the 4 callers of FUN_006a31d0: checked on Steam (other builds found kBatchSamples in such a push)
constexpr uintptr_t kBatchPushesSteam[] = {0x006A3B03, 0x006A3687, 0x006A37CD, 0x006A3956};
uintptr_t kWallCull = 0;               // 0x0069DFF0: thiscall(walls, int-vector* out, const float* from, const float* lightPos) ret 0xC
uintptr_t kWallCullCall = 0;           // 0x006A311F: its call in FUN_006a30b0 (per-light wall lists of a batch)
struct LightClass {
    uintptr_t vtable, eval;
};
// Steam: {0xFF42A0, 0x6BDE90}, {0xFF42F8, 0x6BE020}, {0xFF4350, 0x6BE1C0}, {0xFF43A8, 0x6BEFD0}, {0xFF44C0, 0x6BFBA0}, {0xFF4518, 0x6BFDC0},
// {0xFF4570, 0x6BFFB0}, {0xFF4408, 0x6BF880}, {0xFF4468, 0x6BFA70} (+0x4C; the last two, CircleWindowLight and TubeLight, are missing
// from light_vtables.txt). Their light types (the light factory FUN_006ac590 makes type N with GameAddr::LightVtableN):
constexpr int kClassTypes[] = {3, 11, 5, 7, 9, 10, 4, 8, 6};
LightClass kClasses[std::size(kClassTypes)] = {}; // {0, 0} = class not found on this build

void LoadAddresses() {
    static bool loaded = false;
    if (loaded || !GameAddr::Resolved()) return;
    loaded = true;
    using GameAddr::Get;
    using GameAddr::Id;
    auto at = [](Id first, int k) { return Get(static_cast<Id>(static_cast<int>(first) + k)); };
    kAddWorldLights = Get(Id::AddWorldLights);
    for (int k = 0; k < 2; k++) kGatherCalls[k] = at(Id::AddWorldLightsCall0, k);
    kLevelGather = Get(Id::LevelGather);
    for (int k = 0; k < 2; k++) kLevelGatherCalls[k] = at(Id::LevelGatherCall0, k);
    kRoomById = Get(Id::RoomById);
    kInvalidateRoom = Get(Id::InvalidateRoom);
    kSetInsert = Get(Id::SetInsert);
    kRoomByIdCall = Get(Id::RoomByIdCall);
    kInvalidateCall = Get(Id::InvalidateCall);
    kSetInsertCall = Get(Id::SetInsertCall);
    kCascadeTest = Get(Id::CascadeTest);
    kCascadeJcc = kCascadeTest ? kCascadeTest + 7 : 0;
    kRootPtr = Get(Id::RootPtr);
    kSolvePoint = Get(Id::SolvePoint);
    for (int k = 0; k < 3; k++) kSolvePointCalls[k] = at(Id::SolvePointCall0, k);
    kLightEvalReturn = Get(Id::LightEvalReturn);
    kWallTest = Get(Id::WallTest);
    kWallTestCall = Get(Id::WallTestCall);
    kLightPos = Get(Id::LightPos);
    kBatchSolveCall = Get(Id::BatchSolveCall);
    kBatchSamples = Get(Id::BatchSamples);
    kWallCull = Get(Id::WallCull);
    kWallCullCall = Get(Id::WallCullCall);
    for (size_t i = 0; i < std::size(kClassTypes); i++) {
        const uintptr_t vt = at(Id::LightVtable3, kClassTypes[i] - 3), eval = at(Id::LightEval3, kClassTypes[i] - 3);
        kClasses[i] = vt && eval ? LightClass{vt, eval} : LightClass{0, 0};
    }
}

using AddWorldLights_t = void(__thiscall*)(void* treeLevel, void* room);
using LevelGather_t = void(__thiscall*)(void* treeLevel, void* room, int ownFloor);
using RoomById_t = void*(__thiscall*)(void* manager, int id);
using InvalidateRoom_t = void(__thiscall*)(void* room, int full, int keep);
using SetInsert_t = void*(__thiscall*)(void* set, void* out, const int* key, int hint);
using SolvePoint_t = float*(__thiscall*)(void* room, float* out, void* list2D, void* list3D, void* flags, void* sample);
using LightEval_t = void(__thiscall*)(void* light, const float* sample, const float* normal, float* colour);
using WallTest_t = bool(__thiscall*)(void* room, void* indexVec, const float* lightPos, const void* sample, float* transmission);
using LightPos_t = void(__thiscall*)(void* light, float* out);
using WallCull_t = void(__thiscall*)(void* walls, void* out, const float* from, const float* lightPos);
struct IntVec { // the game's vector layout {begin, end, capacity}, as FUN_0069dff0 fills it and FUN_0069d4c0 reads it
    int* b;
    int* e;
    int* c;
};

std::vector<MemPatch::PatchLocation> g_patches;
std::atomic<bool> g_installed{false};
std::atomic<bool> g_refreshRequested{false};
std::atomic<bool> g_clearRooms{false};
std::atomic<DWORD> g_renderThread{0};
std::atomic<DWORD> g_gatherThread{0};
std::atomic<int> g_shared{0};      // lights added to the other floors' outdoor lists
std::atomic<int> g_queued{0};      // floors sent to gather again by us
std::atomic<int> g_faults{0};
std::atomic<int> g_evalClasses{0}; // light classes whose evaluation is wrapped
std::atomic<long> g_wallTests{0};  // cross-floor light x point tested against the lamp floor's walls
std::atomic<long> g_wallBlocked{0};
std::atomic<long> g_otherThread{0};

// Per outdoor room (room 0 of floors 0..7): the lights of its list that belong to other floors, and their floor.
// Written by the gather and read by the point solve, both on the light tree thread (g_gatherThread).
struct RoomInfo {
    uintptr_t mgr = 0, tracker = 0;
    int level = 0;
    std::vector<std::pair<uintptr_t, int>> cross; // (light, home floor), sorted by light
};
std::unordered_map<uintptr_t, RoomInfo> g_rooms;
uintptr_t g_evalOrig[std::size(kClasses)] = {};

uintptr_t TreeLevel(uintptr_t tracker, int level) { return tracker + 0x6A0 + static_cast<intptr_t>(level) * 0x1A4; }

// The calling thread's id from the TEB (ClientId.UniqueThread, what GetCurrentThreadId returns) without the call: the
// point solve checks it for every point it lights (2026-09-29)
inline DWORD ThreadId() { return __readfsdword(0x24); }

size_t ListSize(const BYTE* room) {
    const uintptr_t b = *reinterpret_cast<const uintptr_t*>(room + 0xC8), e = *reinterpret_cast<const uintptr_t*>(room + 0xCC);
    return e >= b ? (e - b) / 4 : 0;
}

// The game's refresh from FUN_006c7250 (0x6C73B6..0x6C7426) for room 0 of floors 0..7. Rooms already waiting for their
// gather keep their countdown.
void QueueOutdoorRegather(uintptr_t tracker) {
    for (int level = 0; level <= 7; level++) {
        const uintptr_t tl = TreeLevel(tracker, level);
        void* mgr = *reinterpret_cast<void* const*>(tl);
        if (!mgr) continue;
        BYTE* room0 = static_cast<BYTE*>(reinterpret_cast<RoomById_t>(kRoomById)(mgr, 0));
        if (!room0) continue;
        if (*reinterpret_cast<const int*>(room0 + 0xF0) == 1 && *reinterpret_cast<const int*>(room0 + 0x168) != 0) continue;
        reinterpret_cast<InvalidateRoom_t>(kInvalidateRoom)(room0, 1, 0);
        alignas(16) BYTE out[16] = {};
        const int key = 0;
        reinterpret_cast<SetInsert_t>(kSetInsert)(reinterpret_cast<void*>(tl + 0x28), out, &key, 0);
        g_queued.fetch_add(1, std::memory_order_relaxed);
    }
}

// Lights registered on a floor for room 0: treeLevel+0x90 hash (buckets +0x98, count +0x9C; node +8 -> {begin, end} of
// entries, next +0x10; entry +0x1C room id, +0x24 light). Same walk as FUN_006c6990.
void FloorOutdoorLights(uintptr_t tl, int floor, std::vector<std::pair<uintptr_t, int>>& out) {
    const uintptr_t buckets = *reinterpret_cast<const uintptr_t*>(tl + 0x98);
    const uint32_t count = *reinterpret_cast<const uint32_t*>(tl + 0x9C);
    if (!buckets || !count || count >= (1u << 20)) return;
    const uintptr_t endNode = *reinterpret_cast<const uintptr_t*>(buckets + count * 4);
    uintptr_t slot = buckets;
    uintptr_t node = *reinterpret_cast<const uintptr_t*>(slot);
    int guard = 0;
    while (node == 0 && guard++ < (1 << 20)) node = *reinterpret_cast<const uintptr_t*>(slot += 4);
    guard = 0;
    while (node && node != endNode && guard++ < 100000) {
        const uintptr_t vec = *reinterpret_cast<const uintptr_t*>(node + 8);
        const uintptr_t b = *reinterpret_cast<const uintptr_t*>(vec), e = *reinterpret_cast<const uintptr_t*>(vec + 4);
        for (uintptr_t p = b; p < e && p - b < 0x40000; p += 4) {
            const uintptr_t entry = *reinterpret_cast<const uintptr_t*>(p);
            if (!entry || *reinterpret_cast<const int*>(entry + 0x1C) != 0) continue;
            const uintptr_t light = *reinterpret_cast<const uintptr_t*>(entry + 0x24);
            if (light && out.size() < 4096) out.emplace_back(light, floor);
        }
        node = *reinterpret_cast<const uintptr_t*>(node + 0x10);
        int g2 = 0;
        while (node == 0 && g2++ < (1 << 20)) node = *reinterpret_cast<const uintptr_t*>(slot += 4);
    }
}

void RecordRoom(BYTE* room, uintptr_t mgr, uintptr_t tracker, int roomLevel) {
    if (g_clearRooms.exchange(false) || g_rooms.size() > 8192) {
        g_rooms.clear(); // never during a point solve: the gather does not run inside it
    }
    RoomInfo& info = g_rooms[reinterpret_cast<uintptr_t>(room)];
    info.mgr = mgr;
    info.tracker = tracker;
    info.level = roomLevel;
    info.cross.clear();
    for (int floor = 0; floor <= 7; floor++) {
        if (floor == roomLevel) continue;
        const uintptr_t tl = TreeLevel(tracker, floor);
        if (*reinterpret_cast<const uintptr_t*>(tl)) FloorOutdoorLights(tl, floor, info.cross);
    }
    std::sort(info.cross.begin(), info.cross.end());
}

void ShareOutdoorLights(BYTE* treeLevel, BYTE* room) {
    if (*reinterpret_cast<const int*>(room + 0xC) != 0) return; // only room 0, the outside of a floor
    const uintptr_t rmgr = *reinterpret_cast<const uintptr_t*>(room); // room[0] = its manager
    if (!rmgr) return;
    const int roomLevel = *reinterpret_cast<const int*>(rmgr + 0x88);
    if (roomLevel < 0 || roomLevel > 7) return; // basements stay as the game has them
    const int level = *reinterpret_cast<const int*>(treeLevel + 0x1A0); // floor whose registry the game used
    const uintptr_t tracker = *reinterpret_cast<const uintptr_t*>(treeLevel + 4);
    if (level < 0 || level > 7 || !tracker || TreeLevel(tracker, level) != reinterpret_cast<uintptr_t>(treeLevel)) return;
    if (*reinterpret_cast<const uintptr_t*>(TreeLevel(tracker, roomLevel)) != rmgr) return;
    if (level != roomLevel && level != 0) return; // only the lot-load path (FUN_006c54e0) gathers through another floor, level 0
    g_gatherThread = ThreadId();

    int added = 0;
    for (int other = 0; other <= 7; other++) {
        if (other == level) continue;
        void* tl = reinterpret_cast<void*>(TreeLevel(tracker, other));
        if (!*reinterpret_cast<void* const*>(tl)) continue;
        const size_t before = ListSize(room);
        const int times = other == 0 ? 2 : 1; // the weight the lamp has on its own floor
        for (int t = 0; t < times; t++) reinterpret_cast<LevelGather_t>(kLevelGather)(tl, room, 0);
        added += static_cast<int>(ListSize(room) - before);
    }
    if (added) g_shared.fetch_add(added, std::memory_order_relaxed);
    RecordRoom(room, rmgr, tracker, roomLevel);
}

void __fastcall OutdoorGather(BYTE* treeLevel, void*, BYTE* room) {
    reinterpret_cast<AddWorldLights_t>(kAddWorldLights)(treeLevel, room);
    if (!g_installed.load(std::memory_order_relaxed) || !room) return;
    __try {
        ShareOutdoorLights(treeLevel, room);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
    }
}

// ---- walls of the lamp's floor ----
// The game tests a light against a room's walls (FUN_0069fc40 -> FUN_0069d4c0, 2D, room+0x30) only when the batch flags
// ask for it (flags[0]), and for a batch of more than 3 samples not against all walls: FUN_006a31d0 first builds, for each
// light, the walls whose culling edge (edge 2) crosses the segment from the batch centre (mean of the samples,
// FUN_0069f1e0) to the light (FUN_006a30b0 -> FUN_0069dff0), and each sample is tested only against those. A light from
// another floor is tested the same way against the walls of its own floor (and of the floors in between): same flags,
// same centre, same culling function, so the lamp's floor and the other floors see it alike.

struct SolveCtx {
    const RoomInfo* info = nullptr; // room 0 of a floor (g_rooms), else null
    void* list2D = nullptr;         // the game's per-light wall lists of this batch (null: all walls)
    const char* flags = nullptr;    // [0] = test 2D walls, [1] = 3D occluders
    bool batch = false;             // called from FUN_006a31d0: the batch is in kBatchSamples
    BYTE soft = 0;                  // the solving room's +0x639 (FUN_0069fc40 reads it: wall height test / soft shadows of this pass)
};
SolveCtx g_ctx;
struct BatchCentre {
    uintptr_t begin = 0, end = 0;
    alignas(16) float c[4] = {};
} g_batch;
struct Culled {
    uintptr_t light;
    int floor;
    std::vector<int> idx;
    size_t n;
};
std::vector<Culled> g_culled; // per batch: (light, floor) -> walls of that floor's room 0 between the batch centre and the light

// ---- diagnostics (F8): samples near each light of the active lot, with the game's wall test and ours ----
// Development build only, and only while armed (2026-09-29; before, every lit cross-floor evaluation of every solve paid
// the active-lot test and the distance test): the Developer checkbox "Record story light samples", or the first
// Ctrl+Shift+F8 / "Save light diagnostics" of a session, arms it; the records then cover the solves that follow.
std::atomic<bool> g_diagArmed{false};
struct DiagRec {
    int level, home; // home = -1: the room's own light (or a world light)
    uintptr_t light;
    float lpos[3], p[3], n[3], lum;
    float mine = -1.0f; // our share (cross-floor lights), -1 = not tested
    int game = -1;      // the game's wall test for this sample: 1 passed, 0 blocked, -1 not run
    float gameT = 1.0f;
    bool batch = false, culledList = false;
    int type = -1; // light+0xB0 when recorded
};
std::mutex g_diagMx;
std::vector<DiagRec> g_diag;
std::atomic<long> g_diagSeen{0};
int g_lastRec = -1; // record of the light evaluated last (the game's wall test for it comes right after)

bool RoomStillSame(const RoomInfo& info, const BYTE* room) {
    __try {
        return *reinterpret_cast<const uintptr_t*>(room) == info.mgr && *reinterpret_cast<const uintptr_t*>(TreeLevel(info.tracker, info.level)) == info.mgr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const RoomInfo* SolveInfo(BYTE* room) {
    if (!g_installed.load(std::memory_order_relaxed) || g_rooms.empty()) return nullptr;
    if (ThreadId() != g_gatherThread.load(std::memory_order_relaxed)) {
        g_otherThread.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    auto it = g_rooms.find(reinterpret_cast<uintptr_t>(room));
    if (it == g_rooms.end() || !RoomStillSame(it->second, room)) return nullptr;
    return &it->second;
}

bool BatchCentreFor(const void* sample) {
    __try {
        const uintptr_t b = *reinterpret_cast<const uintptr_t*>(kBatchSamples), e = *reinterpret_cast<const uintptr_t*>(kBatchSamples + 4);
        const uintptr_t s = reinterpret_cast<uintptr_t>(sample);
        if (!b || e <= b || (e - b) % 0x30 || s < b || s >= e) return false; // not a sample of that batch
        if (s == b || b != g_batch.begin || e != g_batch.end) {
            float sum[4] = {};
            for (uintptr_t p = b; p < e; p += 0x30)
                for (int k = 0; k < 4; k++) sum[k] += reinterpret_cast<const float*>(p)[k];
            const float cnt = static_cast<float>((e - b) / 0x30);
            for (int k = 0; k < 4; k++) g_batch.c[k] = sum[k] / cnt;
            g_batch.begin = b;
            g_batch.end = e;
            g_culled.clear();
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

float* SolvePoint(BYTE* room, float* out, void* list2D, void* list3D, void* flags, void* sample, bool batch) {
    const SolveCtx prev = g_ctx;
    g_ctx.info = SolveInfo(room);
    g_ctx.list2D = list2D;
    g_ctx.flags = static_cast<const char*>(flags);
    g_ctx.batch = batch && g_ctx.info && BatchCentreFor(sample);
    g_ctx.soft = room[0x639];
    g_lastRec = -1;
    float* r = reinterpret_cast<SolvePoint_t>(kSolvePoint)(room, out, list2D, list3D, flags, sample);
    g_ctx = prev;
    return r;
}
float* __fastcall SolvePointBatch(BYTE* room, void*, float* out, void* list2D, void* list3D, void* flags, void* sample) {
    return SolvePoint(room, out, list2D, list3D, flags, sample, true);
}
float* __fastcall SolvePointSingle(BYTE* room, void*, float* out, void* list2D, void* list3D, void* flags, void* sample) {
    return SolvePoint(room, out, list2D, list3D, flags, sample, false);
}

// Walls of room 0 of `floor` that the game would test for this light in this batch (its own culling), or null = all.
const Culled* CulledWalls(uintptr_t light, int floor, BYTE* room0, const float* pos) {
    for (const Culled& c : g_culled)
        if (c.light == light && c.floor == floor) return &c;
    if (g_culled.size() > 256) g_culled.clear();
    const uintptr_t wb = *reinterpret_cast<const uintptr_t*>(room0 + 0x30), we = *reinterpret_cast<const uintptr_t*>(room0 + 0x34);
    const size_t walls = we > wb ? (we - wb) / 4 : 0;
    if (walls > 65536) return nullptr;
    Culled c{light, floor, std::vector<int>(walls + 1), 0};
    IntVec v{c.idx.data(), c.idx.data(), c.idx.data() + c.idx.size()}; // room for every wall: FUN_0069dff0 never grows it
    reinterpret_cast<WallCull_t>(kWallCull)(room0 + 0x30, &v, g_batch.c, pos);
    if (v.b != c.idx.data()) return nullptr; // cannot happen with that capacity; if it did, the game owns the memory now
    c.n = static_cast<size_t>(v.e - v.b);
    g_culled.push_back(std::move(c));
    return &g_culled.back();
}

// Share of the light that passes the walls of the floors from the lamp's floor to the room's floor (room's floor
// excluded: the game tests it right after with its own lists).
BYTE* g_swapAt = nullptr; // room+0x639 byte changed around the game's wall test (restored on a fault too)
BYTE g_swapSaved = 0;

float WallPassImpl(uintptr_t tracker, int roomLevel, int home, void* light, const void* sample, bool& culledList) {
    alignas(16) float pos[4];
    reinterpret_cast<LightPos_t>(kLightPos)(light, pos);
    float keep = 1.0f;
    const int lo = std::min(home, roomLevel), hi = std::max(home, roomLevel);
    for (int floor = lo; floor <= hi; floor++) {
        if (floor == roomLevel) continue;
        void* mgr = *reinterpret_cast<void* const*>(TreeLevel(tracker, floor));
        if (!mgr) continue;
        BYTE* room0 = static_cast<BYTE*>(reinterpret_cast<RoomById_t>(kRoomById)(mgr, 0));
        if (!room0) continue;
        IntVec list{};
        void* idx = nullptr; // null = all walls, as the game does when the batch has no per-light lists
        if (g_ctx.list2D && g_ctx.batch) {
            const Culled* c = CulledWalls(reinterpret_cast<uintptr_t>(light), floor, room0, pos);
            if (c) {
                int* d = const_cast<int*>(c->idx.data());
                list = {d, d + c->n, d + c->n};
                idx = &list;
                culledList = true;
            }
        }
        float t = 1.0f;
        // FUN_0069fc40 takes the wall mode from its room (+0x639); use the one of the pass being solved
        g_swapAt = room0 + 0x639;
        g_swapSaved = *g_swapAt;
        *g_swapAt = g_ctx.soft;
        const bool passed = reinterpret_cast<WallTest_t>(kWallTest)(room0, idx, pos, sample, &t);
        *g_swapAt = g_swapSaved;
        g_swapAt = nullptr;
        if (!passed) return 0.0f;
        keep *= t;
    }
    return keep;
}
float WallPass(uintptr_t tracker, int roomLevel, int home, void* light, const void* sample, bool& culledList) {
    __try {
        return WallPassImpl(tracker, roomLevel, home, light, sample, culledList);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (g_swapAt) {
            *g_swapAt = g_swapSaved;
            g_swapAt = nullptr;
        }
        g_faults.fetch_add(1, std::memory_order_relaxed);
        return 1.0f;
    }
}

int HomeFloor(const RoomInfo& info, uintptr_t light) {
    const auto key = std::make_pair(light, -1000);
    auto it = std::lower_bound(info.cross.begin(), info.cross.end(), key);
    return it == info.cross.end() || it->first != light ? -1 : it->second;
}

bool ActiveLot(uintptr_t mgr) {
    __try {
        return *reinterpret_cast<const BYTE*>(mgr + 0x288) != 0; // the active lot uses the high lighting quality
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Diag(const RoomInfo& info, void* light, const float* sample, const float* colour, int home, float mine, bool culledList) {
    g_lastRec = -1;
    if (!ActiveLot(info.mgr)) return;
    const float* lp = reinterpret_cast<const float*>(static_cast<const BYTE*>(light) + 0x120);
    const float dx = sample[0] - lp[0], dy = sample[1] - lp[1], dz = sample[2] - lp[2];
    if (dx * dx + dy * dy + dz * dz > 3.5f * 3.5f) return;
    g_diagSeen.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_diagMx);
    if (g_diag.size() >= 4000) return;
    DiagRec r{info.level, home, reinterpret_cast<uintptr_t>(light), {lp[0], lp[1], lp[2]}, {sample[0], sample[1], sample[2]},
              {sample[4], sample[5], sample[6]}, colour[0] + colour[1] + colour[2], mine, -1, 1.0f, g_ctx.batch, culledList};
    r.type = *reinterpret_cast<const int*>(static_cast<const BYTE*>(light) + 0xB0);
    g_diag.push_back(r);
    g_lastRec = static_cast<int>(g_diag.size()) - 1;
}

void CrossFloorShadow(const RoomInfo& info, void* light, const float* sample, float* colour) {
    const int home = HomeFloor(info, reinterpret_cast<uintptr_t>(light));
    float mine = -1.0f;
    bool culledList = false;
    const bool lit = colour[0] > 0.0f || colour[1] > 0.0f || colour[2] > 0.0f;
    const float before[3] = {colour[0], colour[1], colour[2]};
    if (home >= 0 && lit && g_ctx.flags && g_ctx.flags[0]) { // the game tests 2D walls in this batch: so do we, on the lamp's floor
        g_wallTests.fetch_add(1, std::memory_order_relaxed);
        mine = WallPass(info.tracker, info.level, home, light, sample, culledList);
        if (mine <= 0.0f) g_wallBlocked.fetch_add(1, std::memory_order_relaxed);
        if (mine < 1.0f)
            for (int i = 0; i < 4; i++) colour[i] *= std::max(0.0f, mine);
    }
    if constexpr (!kPublicBuild) {
        if (lit) {
            if (g_diagArmed.load(std::memory_order_relaxed)) Diag(info, light, sample, before, home, mine, culledList);
            else g_lastRec = -1; // as Diag does first: no record for the game's wall test that follows
        }
    }
}

template <int I> void __fastcall LightEvalHook(void* light, void*, const float* sample, const float* normal, float* colour) {
    reinterpret_cast<LightEval_t>(g_evalOrig[I])(light, sample, normal, colour);
    if (g_ctx.info && _ReturnAddress() == reinterpret_cast<void*>(kLightEvalReturn)) CrossFloorShadow(*g_ctx.info, light, sample, colour);
}
using LightEvalHook_t = void(__fastcall*)(void* light, void* edx, const float* sample, const float* normal, float* colour); // = thiscall ret 0xC
const LightEvalHook_t kEvalHooks[] = {&LightEvalHook<0>, &LightEvalHook<1>, &LightEvalHook<2>, &LightEvalHook<3>,
                                  &LightEvalHook<4>, &LightEvalHook<5>, &LightEvalHook<6>, &LightEvalHook<7>, &LightEvalHook<8>};
static_assert(std::size(kEvalHooks) == std::size(kClasses));

// The game's own wall test in LightPointWithAllLights (0x69FE93): result recorded for the diagnostics.
bool __fastcall GameWallTest(BYTE* room, void*, void* idx, const float* lightPos, const void* sample, float* t) {
    const bool ok = reinterpret_cast<WallTest_t>(kWallTest)(room, idx, lightPos, sample, t);
    if (g_lastRec >= 0) {
        std::lock_guard<std::mutex> lk(g_diagMx);
        if (g_lastRec < static_cast<int>(g_diag.size())) {
            g_diag[g_lastRec].game = ok ? 1 : 0;
            g_diag[g_lastRec].gameT = *t;
        }
        g_lastRec = -1;
    }
    return ok;
}

// Every loaded lot: room 0 of floors 0..7 gathers again (after turning the option on or off). Render thread only.
void ClearDiag() {
    std::lock_guard<std::mutex> lk(g_diagMx);
    g_diag.clear();
}

void RefreshAllLots() {
    ClearDiag(); // the records show the solve that follows
    if (!kRootPtr) return;
    __try {
        const uintptr_t root = *reinterpret_cast<const uintptr_t*>(kRootPtr);
        if (!root) return;
        const uintptr_t lightMgr = *reinterpret_cast<const uintptr_t*>(root + 0x1C0);
        if (!lightMgr) return;
        const uintptr_t tree = *reinterpret_cast<const uintptr_t*>(lightMgr + 0xD4);
        if (!tree) return;
        const uintptr_t buckets = *reinterpret_cast<const uintptr_t*>(tree + 0x58);
        const uint32_t bucketCount = *reinterpret_cast<const uint32_t*>(tree + 0x5C);
        if (!buckets || !bucketCount || bucketCount >= (1u << 20)) return;
        const uintptr_t endNode = *reinterpret_cast<const uintptr_t*>(buckets + bucketCount * 4);
        uintptr_t slot = buckets;
        uintptr_t node = *reinterpret_cast<const uintptr_t*>(slot);
        int guard = 0;
        while (node == 0 && guard++ < (1 << 20)) node = *reinterpret_cast<const uintptr_t*>(slot += 4);
        guard = 0;
        while (node && node != endNode && guard++ < 100000) {
            const uintptr_t tracker = *reinterpret_cast<const uintptr_t*>(node + 8);
            if (tracker) QueueOutdoorRegather(tracker);
            node = *reinterpret_cast<const uintptr_t*>(node + 0x10);
            int g2 = 0;
            while (node == 0 && g2++ < (1 << 20)) node = *reinterpret_cast<const uintptr_t*>(slot += 4);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
    }
}

// The return address follows "call reg" (FF D0..FF D7): FF D2 (call edx) on Steam
bool AfterCallReg(uintptr_t ret) {
    const BYTE* p = reinterpret_cast<const BYTE*>(ret - 2);
    return GameAddr::IsFixed() ? std::memcmp(p, "\xFF\xD2", 2) == 0 : (p[0] == 0xFF && (p[1] & 0xF8) == 0xD0);
}

bool CallsTarget(uintptr_t site, uintptr_t target) {
    return *reinterpret_cast<const BYTE*>(site) == 0xE8 && site + 5 + *reinterpret_cast<const int32_t*>(site + 1) == target;
}

bool Redirect(uintptr_t site, uintptr_t target, const void* thunk) {
    DWORD orig = static_cast<DWORD>(target - (site + 5));
    const DWORD rel = static_cast<DWORD>(reinterpret_cast<uintptr_t>(thunk) - (site + 5));
    return MemPatch::WriteDWORD(site + 1, rel, &g_patches, &orig);
}

void RefreshSoon() {
    if (ThreadId() == g_renderThread.load()) RefreshAllLots();
    else g_refreshRequested = true; // done by the next OnPresent on the render thread
}

} // namespace

namespace LevelLightShare {

bool Install(std::string& error) {
    if (g_installed) return true;
    LoadAddresses();
    using GameAddr::Id;
    std::string missing;
    if (!GameAddr::Have({Id::AddWorldLights, Id::AddWorldLightsCall0, Id::AddWorldLightsCall1, Id::LevelGather, Id::LevelGatherCall0, Id::LevelGatherCall1, Id::CascadeTest,
                         Id::RoomByIdCall, Id::RoomById, Id::InvalidateCall, Id::InvalidateRoom, Id::SetInsertCall, Id::SetInsert, Id::RootPtr, Id::SolvePoint,
                         Id::SolvePointCall0, Id::SolvePointCall1, Id::SolvePointCall2, Id::LightEvalReturn, Id::WallTestCall, Id::WallTest},
                        &missing)) {
        error = "Light between stories: " + GameAddr::NotAvailable(missing);
        return false;
    }
    const bool fixed = GameAddr::IsFixed();
    bool ok = MemPatch::ValidateBytes(reinterpret_cast<LPVOID>(kCascadeTest), kCascadeBytes, fixed ? sizeof(kCascadeBytes) : 8) &&
              CallsTarget(kRoomByIdCall, kRoomById) && CallsTarget(kInvalidateCall, kInvalidateRoom) && CallsTarget(kSetInsertCall, kSetInsert) &&
              CallsTarget(kWallTestCall, kWallTest) && AfterCallReg(kLightEvalReturn);
    for (uintptr_t site : kGatherCalls) ok = ok && CallsTarget(site, kAddWorldLights);
    for (uintptr_t site : kLevelGatherCalls) ok = ok && CallsTarget(site, kLevelGather);
    for (uintptr_t site : kSolvePointCalls) ok = ok && CallsTarget(site, kSolvePoint);
    if (!ok) {
        error = "Light between stories differs (different game version?)";
        return false;
    }
    for (uintptr_t site : kGatherCalls) ok = ok && Redirect(site, kAddWorldLights, reinterpret_cast<const void*>(&OutdoorGather));
    const std::vector<BYTE> jl = {0x8C}, jnz = {0x85};
    ok = ok && MemPatch::WriteBytes(kCascadeJcc, jl, &g_patches, &jnz);
    if (!ok) {
        MemPatch::RestoreAll(g_patches);
        g_patches.clear();
        error = "Could not patch the light between stories";
        return false;
    }
    // Walls of the lamp's floor: optional part, each class only if its slot is the expected function.
    int classes = 0;
    std::string wallsMissing;
    bool solveOk = GameAddr::Have({Id::WallCullCall, Id::WallCull, Id::BatchSolveCall, Id::BatchSamples, Id::LightPos}, &wallsMissing) && CallsTarget(kWallCullCall, kWallCull);
    if (fixed)
        for (uintptr_t push : kBatchPushesSteam) solveOk = solveOk && std::memcmp(reinterpret_cast<const void*>(push), "\x68\xC8\x8A\x15\x01", 5) == 0;
    for (uintptr_t site : kSolvePointCalls)
        solveOk = solveOk && Redirect(site, kSolvePoint, site == kBatchSolveCall ? reinterpret_cast<const void*>(&SolvePointBatch) : reinterpret_cast<const void*>(&SolvePointSingle));
    // The game's own wall test is wrapped only to record its result for the F8 diagnostics: development build only (the
    // public build leaves that CALL as it is; the wrapper only forwarded it).
    if constexpr (!kPublicBuild) solveOk = solveOk && Redirect(kWallTestCall, kWallTest, reinterpret_cast<const void*>(&GameWallTest));
    if (solveOk) {
        for (size_t i = 0; i < std::size(kClasses); i++) {
            if (!kClasses[i].vtable) continue; // class not found on this build
            const uintptr_t slot = kClasses[i].vtable + 0x4C;
            if (*reinterpret_cast<const uint32_t*>(slot) != kClasses[i].eval || *reinterpret_cast<const uint32_t*>(kClasses[i].vtable + 0x24) != kLightPos) continue;
            g_evalOrig[i] = kClasses[i].eval;
            DWORD orig = static_cast<DWORD>(kClasses[i].eval);
            if (MemPatch::WriteDWORD(slot, static_cast<DWORD>(reinterpret_cast<uintptr_t>(kEvalHooks[i])), &g_patches, &orig)) classes++;
        }
    } else
        LOG_WARNING(wallsMissing.empty() ? std::string("[LevelLightShare] Per-point evaluation differs; no shadow from the walls of other stories")
                                         : "[LevelLightShare] Walls of other stories: " + GameAddr::NotAvailable(wallsMissing));
    g_evalClasses = classes;
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_installed = true;
    RefreshSoon();
    LOG_INFO(std::format("[LevelLightShare] Installed (walls of the light's story: {} of {} classes)", classes, std::size(kClasses)));
    return true;
}

void Uninstall() {
    if (!g_installed) return;
    g_installed = false;
    g_ctx = {};
    MemPatch::RestoreAll(g_patches);
    g_patches.clear();
    g_evalClasses = 0;
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_clearRooms = true;
    RefreshSoon(); // the lists drop the other floors' lamps at their next gather
    LOG_INFO("[LevelLightShare] Uninstalled");
}

bool IsInstalled() { return g_installed.load(); }

void OnPresent() {
    g_renderThread = ThreadId();
    if (g_refreshRequested.exchange(false)) RefreshAllLots();
}

void OnWorldChanged() { g_clearRooms = true; }

void SetDiagArmed(bool on) { g_diagArmed = on; }
bool DiagArmed() { return g_diagArmed.load(); }

std::string DiagText() {
    const bool wasArmed = g_diagArmed.exchange(true); // a dump arms the recording for the next one
    std::vector<DiagRec> recs;
    {
        std::lock_guard<std::mutex> lk(g_diagMx);
        recs.swap(g_diag);
    }
    if (!wasArmed && recs.empty())
        return std::format("\n==== ANDARES (luz externa entre andares) ====\n{}\nNo samples: recording them was off (it costs time in every light solve). It is on "
                           "now: let the lot relight (or use \"Relight lots now\") and save the diagnostics again.\n",
                           Status());
    std::sort(recs.begin(), recs.end(), [](const DiagRec& a, const DiagRec& b) {
        if (a.light != b.light) return a.light < b.light;
        if (a.level != b.level) return a.level < b.level;
        if (a.p[1] != b.p[1]) return a.p[1] > b.p[1];
        return a.p[0] + a.p[2] < b.p[0] + b.p[2];
    });
    std::string s = std::format("\n==== ANDARES (luz externa entre andares) ====\n{}\nPontos a ate 3,5 m de cada luz do lote ativo, no ultimo calculo: {} registrados de {} vistos.\n",
                                Status(), recs.size(), g_diagSeen.load());
    s += "Colunas: andar do ponto | luz, tipo, andar da luz (-1 = do proprio andar ou do mundo) | ponto | normal | soma da cor | teste do jogo (1 passou, 0 bloqueou, "
         "-1 nao rodou) e fator | nosso fator (-1 = nao testado) | lote de pontos | lista filtrada\n";
    struct Sum {
        int n = 0, gamePass = 0, gameBlock = 0, gameNone = 0, mineTested = 0, mineBlock = 0;
    };
    std::vector<std::pair<std::pair<uintptr_t, int>, Sum>> sums;
    for (const DiagRec& r : recs) {
        const auto key = std::make_pair(r.light, r.level);
        if (sums.empty() || sums.back().first != key) sums.push_back({key, {}});
        Sum& m = sums.back().second;
        m.n++;
        if (r.game == 1) m.gamePass++;
        else if (r.game == 0) m.gameBlock++;
        else m.gameNone++;
        if (r.mine >= 0) {
            m.mineTested++;
            if (r.mine <= 0) m.mineBlock++;
        }
    }
    s += "\n-- resumo por luz e andar --\n";
    for (const auto& [k, m] : sums)
        s += std::format("L{:08X} andar {}: {} pontos | jogo: {} passou, {} bloqueou, {} sem teste | nosso: {} testados, {} bloqueados\n", k.first, k.second, m.n, m.gamePass,
                         m.gameBlock, m.gameNone, m.mineTested, m.mineBlock);
    s += "\n-- pontos --\n";
    for (const DiagRec& r : recs)
        s += std::format("A{} L{:08X} tipo{} casa{} luz({:.2f} {:.2f} {:.2f}) p({:.2f} {:.2f} {:.2f}) n({:.2f} {:.2f} {:.2f}) cor={:.3f} jogo={} t={:.2f} nosso={:.2f} lote={} filtro={}\n",
                         r.level, r.light, r.type, r.home, r.lpos[0], r.lpos[1], r.lpos[2], r.p[0], r.p[1], r.p[2], r.n[0], r.n[1], r.n[2], r.lum, r.game, r.gameT, r.mine,
                         r.batch ? 1 : 0, r.culledList ? 1 : 0);
    return s;
}

std::string Status() {
    return std::format("{} | outdoor lights carried to other stories: {} | stories updated: {} | walls of the light's story: {}/{} classes, {} tests, {} blocked{}{}",
                       g_installed ? "Active" : "Off", g_shared.load(), g_queued.load(), g_evalClasses.load(), std::size(kClasses), g_wallTests.load(), g_wallBlocked.load(),
                       g_otherThread.load() ? std::format(" | on another thread: {}", g_otherThread.load()) : "",
                       g_faults.load() ? std::format(" | failures: {}", g_faults.load()) : "");
}

} // namespace LevelLightShare
