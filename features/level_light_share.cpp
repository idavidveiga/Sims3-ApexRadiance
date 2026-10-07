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
//  2. FUN_006c7250 at 0x6C73B0: "room 0 of level 0 changed -> refresh room 0 of all levels 0..7" (the cascade). The
//     game marks room 0 changed whenever one of its lamp entries is updated, changed or not (an animated lamp: every few
//     frames), and each cascade restarts the solve of room 0 of every floor. First Apex widened it to any level (JNZ ->
//     JL): the F8 of 29/09 (invalidate callers) showed room 0 of stories 1-3 of one lot restarted ~140 times each in 70 s
//     from 0x6C7404, never finishing (shown class 0: coarse, dim outdoor light on every floor until something else
//     restarted it). Now the cascade never runs ("jne" -> "nop; jmp" to the plain path: each floor refreshes its own room
//     0, as for any room) and AfterChangedWalk sends room 0 of the other floors 0..7 only when that floor's outdoor lamps
//     or outside walls really changed (RoomLampSignature of room 0): a lamp added, deleted or moved at once, a switch or
//     a flicker once the burst calms. The lights are the same as with the cascade once it settles.
//  3. Walls of the lamp's floor. The light map of a point sums every light of the room's list (LightPointWithAllLights
//     0x69FD60) and tests each one against the room's 2D wall occluders (FUN_0069fc40 -> FUN_0069d4c0, room+0x30). Those
//     are only the walls of the room's own floor, and a wall blocks a ray only below its top (FUN_0069aa90). So a sconce on
//     the upper floor, next to a corner, lit the lower floor's side wall around the corner: the ray passes above the lower
//     walls and the upper walls are not in the list (LightProbe-andar1-c: side wall lighter below the floor line). The
//     light's evaluation (class vfunc+0x4C) is wrapped: when called from that loop (return 0x69FE19) for a light that came
//     from another floor, it is also tested with the game's own FUN_0069fc40 against the walls of room 0 of the lamp's
//     floor and of the floors in between. The room's own lamps are tested only as the game does (a porch lamp under an
//     overhang is not blocked by the upper floor's walls).
//  4. Indoor lamps through stair openings (option "luzInternaEntreAndares", 2026-09-29). An indoor room gathers only the
//     lamps registered for its own room id (FUN_006c7820 compares entry+0x1C with room+0xC, and room ids are unique in a
//     lot), so a lamp next to a stairwell on story 2 lit the story-2 wall and stopped at the moulding: the story-1 wall
//     below belongs to another room (LightProbe andares2: its wall atlas had only the room's ambient). Now:
//     - Openings: quadrants with no floor in story B's floor grid over an indoor room of story B-1 (the room below:
//       tile+0x7C+q*0x14 > 0; the landing around a stairwell is often room 0 on B, railings close no room). The lighting
//       side cannot tell a floor from a hole (it lights stair holes too); the floor grid lives in the world-side level
//       floor object (vtable 0x01062680; its story's lighting manager found through its owner lot like the game does,
//       LevelManager: the copy at +0x238 goes stale; +0x264 = FloorGrid*: +0 data, +0x10
//       width, +0x14 height, 40-byte tiles, quadrant key at +8+q*8). An opening = a removed floor: a key with bit
//       0x40000000 of its low dword and other low bits (RemovedFloorKey, measured in game; not the bare 40000000 along
//       walls, not the never-built empty key 0xFFFFFFF8/0xFFFFFFFF).
//       Nothing links the manager to that object, so the 5 calls that set or remove a floor
//       (FUN_00a89dd0 x4, FUN_00a893a0) go through FloorSetThunk / FloorRemoveThunk, which remember the object (ecx).
//     - Gather: an indoor room of story S takes, from stories S+1 and S-1, the lamps of the rooms near an opening of the
//       floor between them (kOpeningReach) when the room itself is near it, with the game's own checks of FUN_006c7820
//       and its adder FUN_006a2060. The object rigs' gather (FUN_006bb2f0) takes only lights whose room (light+8) is the
//       object's room; since 06/10 ObjectLightBridge also gives a room-mode rig the lamps of that list that reach its
//       centre (CrossLampReach, the same floor and wall tests).
//     - Per point (the light evaluation wrapped for 3.): the ray from the lamp to the point must cross the floor between
//       the stories through an opening (else no light), and then pass the walls of the lamp's own room, tested from the
//       lamp to the crossing point with the game's FUN_0069fc40 (the receiving room's walls are tested by the game).
//     - Updates: the per-story room update FUN_006c7250 (a function pointer pushed at 0x6C5E2A) is wrapped, and so is its
//       call that empties the "changed rooms" set (0x6C7497): the rooms it found changed (a lamp switched, moved or
//       recoloured) also send the rooms of the other stories that take their lamps to gather again, and a lot whose
//       floors changed (or all lots, when the option changes) sends its rooms near openings. Rooms are sent the way the
//       game's own update does (invalidate + pending set), never as "changed", so nothing loops.
//  5. Wall light lined up with the wall (option "paredesSemEmendaEntreAndares", 2026-09-29; every wall of every room). The
//     wall pass FUN_006a3a30 lights each wall piece with FUN_006ac070 -> FUN_006abdd0: row k of the wall's atlas block (N
//     rows: wall+class*0x10+0x2C; texel y0+N-1-k) at height k*3/N, so the rows cover [0, 3) and the top row sits 3/N
//     under the top (0.23 m at LOD class 2, 0.43 m at class 1, 0.75 m at class 0). But the wall is drawn (FUN_006ac200,
//     used by the wall meshes through FUN_006a5600) from the centre of the bottom row at its foot to the centre of the top
//     row at its top: row k shows at k*3/(N-1). Every row is drawn higher than where it was lit, and at a floor line the
//     wall below shows the light from 3/N under the line while the wall above shows the light at the line: a step
//     wherever the light changes with height (a sconce near the line: 1.9x at class 2). F8 of 29/09 (atrium: lower top
//     row at 46.44 / 46.24 / 45.92 for classes 2 / 1 / 0, upper bottom row at the line 46.67). WallSamplesHook moves
//     each sample to k*3/(N-1) (after checking it against the game's formula); both walls then end on the light at the
//     line. The class-2 blur FUN_0069f650 (2 passes of [1 2 1] per axis, clamped to the block) pulled those edge rows
//     back towards the inside of each wall. In the rooms of an atrium the rows beyond each edge are lit too
//     (WallSolveHook) and the blur runs across them (BlurWalls), so the walls above and below the line blur as one; other
//     walls keep their top and bottom rows with their own light, blurred along the wall only.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "level_light_share.h"
#include "wall_heights.h"
#include "apex_version.h"
#include "room_ambient_policy.h"
#include "unlit_rooms.h"
#include "room_light_queue.h"
#include "object_light_bridge.h"
#include "recorder.h"
#include "game_addresses.h"
#include "memory_patch.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "entry_chain.h"
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <iterator>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
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
uintptr_t kCascadeJcc = 0;             // 0x006C73B1 (kCascadeTest + 7): 0x85 (jnz) -> 0x8C (jl): the cascade from any level 0..7
// With the changed-set hook in (InstallIndoor): 0x006C73B0 "0F 8C" -> "90 E9" (nop; jmp rel32, same target): no cascade, the
// filtered one of AfterChangedWalk instead (part 2)
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

// Indoor lamps through stair openings (4.)
uintptr_t kLightBright = 0;              // 0x006BC520: fastcall(light), al = bright enough to count
uintptr_t kAddRoomLight = 0;             // 0x006A2060: thiscall(room, light) ret 4
uintptr_t kRoomUpdatePush = 0;           // 0x006C5E2A: push FUN_006c7250 (68 imm32) in FUN_006c5e20
uintptr_t kRoomUpdate = 0;               // 0x006C7250: fastcall(treeLevel)
uintptr_t kLightEntryUpdate = 0;
uintptr_t kChangedClearCall = 0;         // 0x006C7497: in it, the call that empties the "changed rooms" set (ecx = tl+8)
uintptr_t kChangedClear = 0;             // 0x007F3790: thiscall(set, buckets, count) ret 8
uintptr_t kFloorSet = 0, kFloorRemove = 0; // 0x00A89DD0, 0x00A893A0: thiscall(level floor object, ...)
uintptr_t kFloorSetCalls[4] = {};        // 0x00AA0ADB, 0x00AA0CCC, 0x00AA0E4A, 0x00AA0F72
uintptr_t kFloorRemoveCall = 0;          // 0x00AA05C7
uintptr_t kLevelVtable = 0;              // 0x01062680
uintptr_t kLevelCtor = 0, kLevelCtorCall = 0; // 0x00A88790, its only call 0x00AA179E (every level floor object is made there)
uintptr_t kLodChoice = 0;                // 0x0069E710: fastcall(room) -> the room's lighting LOD class
uintptr_t kLodChoiceCalls[4] = {};       // 0x0069E82E, 0x0069EA86, 0x0069EF46, 0x0069F1B3
uintptr_t kLodMax = 0;                   // 0x01158B00: the class it gives the rooms of the camera's story
uintptr_t kRoomSolveStart = 0;           // 0x006A18B0: thiscall(room), state 0 of the room solve (ambient colour, normalisation, ramp)
uintptr_t kRoomAmbientFn = 0;            // 0x006A0F50: thiscall(room), its ambient step (colour room+0x110 / +0x120, normalisation +0x160)
uintptr_t kRoomSolveStartCall = 0;       // 0x006A3D0B: its only call, in the budgeted solve FUN_006a3c90
uintptr_t kWallPass = 0;                 // 0x006A3A30: thiscall(room, int, float budget) ret 8 -> al done: the wall texel pass (state 2)
uintptr_t kWallPassCall = 0;             // 0x006A3D4C: its only call
uintptr_t kWallSamples = 0;              // 0x006AC070: thiscall(wall, piece, class, batch) ret 0xC: the samples of one piece of a wall (5.)
uintptr_t kWallSamplesCall = 0;          // 0x006A3AF5: its only call, in the wall pass
uintptr_t kWallBlur = 0;                 // 0x0069F650: fastcall(room): the wall atlas blur of LOD class 2
uintptr_t kWallBlurCall = 0;             // 0x006A3B62: its only call, at the end of the wall pass
uintptr_t kWallBlurPasses = 0;           // 0x01158B1C: dword, blur passes (2)
uintptr_t kWallBlurMode = 0;             // 0x011D02E4: byte, 0 = [1 2 1] along the rows, then down the columns
uintptr_t kWallSolve = 0;                // 0x006A31D0: thiscall(room, batch, {base, pitch}, char flags[2], sampler, char) ret 0x14
uintptr_t kWallSolveCall = 0;            // 0x006A3B0A: its call for each wall piece, in the wall pass
uintptr_t kMarkRoom = 0;                 // 0x006C7160: thiscall(treeLevel, int room) ret 4, the lamp entry update's mark (held marks)
uintptr_t kSomGet = 0, kPrioLot = 0;            // the priority lot test (RoomLightQueue's): FUN() -> object, thiscall(object, lo, hi)

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
    kLightBright = Get(Id::LightBright);
    kAddRoomLight = Get(Id::AddRoomLight);
    kRoomUpdatePush = Get(Id::RoomUpdatePush);
    kRoomUpdate = Get(Id::RoomUpdate);
    kLightEntryUpdate = Get(Id::LightEntryUpdate);
    kFloorSet = Get(Id::FloorSet);
    kFloorRemove = Get(Id::FloorRemove);
    for (int k = 0; k < 4; k++) kFloorSetCalls[k] = at(Id::FloorSetCall0, k);
    kFloorRemoveCall = Get(Id::FloorRemoveCall);
    kLevelVtable = Get(Id::LevelVtable);
    kLevelCtor = Get(Id::LevelCtor);
    kLevelCtorCall = Get(Id::LevelCtorCall);
    kChangedClearCall = Get(Id::ChangedClearCall);
    kChangedClear = Get(Id::ChangedClear);
    kLodChoice = Get(Id::LodChoice);
    for (int k = 0; k < 4; k++) kLodChoiceCalls[k] = at(Id::LodChoiceCall0, k);
    kLodMax = Get(Id::LodMax);
    kRoomSolveStart = Get(Id::RoomSolveStart);
    kRoomSolveStartCall = Get(Id::RoomSolveStartCall);
    kRoomAmbientFn = Get(Id::RoomAmbient);
    kWallPass = Get(Id::WallPass);
    kWallPassCall = Get(Id::WallPassCall);
    kWallSamples = Get(Id::WallSamples);
    kWallSamplesCall = Get(Id::WallSamplesCall);
    kWallBlur = Get(Id::WallBlur);
    kWallBlurCall = Get(Id::WallBlurCall);
    kWallBlurPasses = Get(Id::WallBlurPasses);
    kWallBlurMode = Get(Id::WallBlurMode);
    kWallSolve = Get(Id::WallSolve);
    kWallSolveCall = Get(Id::WallSolveCall);
    kMarkRoom = Get(Id::LampMark);
    // the priority lot test, only where its code is the expected one (AllFloorsLod with "High quality on every lot")
    kSomGet = kPrioLot = 0;
    if (GameAddr::Have({Id::PriorityLotObject, Id::PriorityLotTest})) {
        const uintptr_t som = Get(Id::PriorityLotObject), test = Get(Id::PriorityLotTest);
        if (*reinterpret_cast<const BYTE*>(som) == 0xA1 && std::memcmp(reinterpret_cast<const void*>(test), "\x8B\x44\x24\x04", 4) == 0) kSomGet = som, kPrioLot = test;
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
thread_local BYTE* t_wallRoom = nullptr; // the room whose wall pass runs on this thread (WallPassHook; the F7 wall notes)
using WallCull_t = void(__thiscall*)(void* walls, void* out, const float* from, const float* lightPos);
using LightBright_t = char(__fastcall*)(void* light);
using AddRoomLight_t = void(__thiscall*)(void* room, void* light);
using RoomUpdate_t = void(__fastcall*)(void* treeLevel);
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
std::atomic<long> g_settles{0}, g_settlesEarly{0}; // 4.: lots whose rooms near openings were sent once more after their first solves
std::atomic<long> g_lotRebuilds{0}; // 4.: lots whose story managers changed (lighting rebuilt): their state started again
std::atomic<long> g_levelsMade{0};  // 4.: level floor objects seen at their construction
std::atomic<bool> g_diagArmed{false}; // development build: record samples and gathers for F8 (see the diagnostics below)

// A light of a room's list that belongs to another floor
struct Cross {
    uintptr_t light = 0;
    int floor = 0;       // the light's story
    int room = 0;        // the light's room on that story (indoor rooms, 4.; outdoor: 0 or a roofless room)
    uintptr_t level = 0; // indoor rooms: highest boundary floor; other boundaries resolved by current manager
    bool outdoor = false; // taken by the light between stories of outdoor rooms (part 1): tested against walls, not floors
    mutable bool lit = false; // it lit a point of this gather's solve (noted in g_litBy once)
    bool operator<(const Cross& o) const { return light < o.light; }
};
// Per outdoor room (room 0 of floors 0..7), and per indoor room that takes lamps through a stair opening: the lights of
// its list that belong to other floors. Written by the gather and read by the point solve, both on the light tree thread
// (g_gatherThread).
struct RoomInfo {
    uintptr_t mgr = 0, tracker = 0;
    int level = 0;
    int id = 0;          // room id (room+0xC)
    bool indoor = false; // 4.: the lights of `cross` came through a stair opening
    std::vector<Cross> cross; // sorted by light
};
std::unordered_map<uintptr_t, RoomInfo> g_rooms;
uintptr_t g_evalOrig[std::size(kClasses)] = {};

uintptr_t TreeLevel(uintptr_t tracker, int level) { return tracker + 0x6A0 + static_cast<intptr_t>(level) * 0x1A4; }
uintptr_t StoryManager(uintptr_t tracker, int level) { return *reinterpret_cast<const uintptr_t*>(TreeLevel(tracker, level)); }

// The calling thread's id from the TEB (ClientId.UniqueThread, what GetCurrentThreadId returns) without the call: the
// point solve checks it for every point it lights (2026-09-29)
inline DWORD ThreadId() { return __readfsdword(0x24); }

size_t ListSize(const BYTE* room) {
    const uintptr_t b = *reinterpret_cast<const uintptr_t*>(room + 0xC8), e = *reinterpret_cast<const uintptr_t*>(room + 0xCC);
    return e >= b ? (e - b) / 4 : 0;
}

// Rooms sent while the game was solving them (state 3, the one current room): sent once that solve is over
struct DeferredRoom {
    uintptr_t tracker;
    int level, id;
    DWORD at; // GetTickCount when held back (dropped after 10 s: a lot unloaded meanwhile)
};
extern bool g_indoorReady;
std::mutex g_deferredMx;
std::vector<DeferredRoom> g_deferred;
std::atomic<long> g_deferredCount{0};
std::atomic<DWORD> g_relightAllAt{0}; // a whole-world relight is due at this tick (0 = none)
std::mutex g_relightWhyMx;
std::string g_relightWhy; // the reasons merged into it

void NoteSolve(BYTE* room, char event, uintptr_t caller = 0); // development build: the solve journal of the F8 diag

// ---- Lamp edits first (2026-10-05) ----
// User: lamps must update at once when anything changes (moved, switched, recoloured, removed), "practically instant",
// above all while a lamp is dragged in Build mode. Before, every step of a drag marked the lamp's room again, and each
// mark goes through FUN_0069eed0(room, 1, 0): state 1 with the gather countdown +0x168 = 5 (FUN_0069eb00 counts it down
// once per room update), and a solve in progress is thrown away (FUN_006c4870, nothing committed). So the light of a
// dragged lamp moved only once the lamp stopped, 5 updates later, after the solves of every story's room 0 in the game's
// order (F6 of 17:31: the lamp's own story last). Now the rooms a lamp edit sends are urgent for kUrgentMs:
//  - the scheduler solves them before any other room (RoomLightQueue's priority hook asks LampUrgency): the lamp's own
//    room first (tier 0), then the rooms of other stories that take its light (tier 1);
//  - for a lamp already registered in that room that moved, switched or changed a value (`soon`), they gather in the room
//    update that sends them (countdown 1: the pending walk of the same FUN_006c7250 call gathers them) instead of 5
//    updates later. Never for a lamp added, removed or moved into another room or story: the game registers its
//    new entries over the next updates, and a gather before that misses the lamp (F8 19:22, build f65626e: a sconce moved
//    up to story 3 was in the lists of stories 0-2 but not in its own story's, gathered at once, nor in story 4's);
//  - a room being solved whose lamp only moved or changed a value keeps that solve: LampMarkFilter holds the mark
//    (HoldLampMark) and the room update gives it back once the solve is over (FlushHeldMarks), so the light of a lamp
//    being dragged follows it solve after solve instead of restarting on every step.
struct UrgentRoom {
    uintptr_t room, mgr;
    int id, tier;
    DWORD until;
    bool soon; // the latest edit sending it was a registered lamp that moved or changed a value: gathered at once
};
std::mutex g_urgentMx;
std::vector<UrgentRoom> g_urgent; // the rooms of the lamps edited in the last kUrgentMs (a handful)
std::atomic<int> g_urgentSize{0};
std::atomic<long> g_urgentMarked{0}, g_heldMarks{0}, g_heldGiven{0}, g_gatherSoon{0};
constexpr DWORD kUrgentMs = 3000;
bool RoomKey(const void* room, uintptr_t& mgr, int& id) {
    __try {
        mgr = *reinterpret_cast<const uintptr_t*>(room);
        id = *reinterpret_cast<const int*>(static_cast<const BYTE*>(room) + 0xC);
        return mgr != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
int RoomState(const void* room) {
    __try {
        return *reinterpret_cast<const int*>(static_cast<const BYTE*>(room) + 0xF0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// A room waiting for its gather (state 1) gathers at the next pending walk instead of after the rest of the countdown
void GatherSoon(void* room) {
    __try {
        BYTE* r = static_cast<BYTE*>(room);
        if (*reinterpret_cast<const int*>(r + 0xF0) == 1 && *reinterpret_cast<const uint32_t*>(r + 0x168) > 1) {
            *reinterpret_cast<uint32_t*>(r + 0x168) = 1;
            g_gatherSoon.fetch_add(1, std::memory_order_relaxed);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
// soon: the edit is a lamp registered in that room that moved, switched or changed a value (see above); the latest decides
void MarkUrgent(void* room, int tier, bool soon) {
    uintptr_t mgr = 0;
    int id = 0;
    if (!room || !RoomKey(room, mgr, id)) return;
    const DWORD now = GetTickCount();
    std::lock_guard<std::mutex> lk(g_urgentMx);
    std::erase_if(g_urgent, [now](const UrgentRoom& u) { return static_cast<int32_t>(now - u.until) >= 0; });
    const auto it = std::find_if(g_urgent.begin(), g_urgent.end(), [room](const UrgentRoom& u) { return u.room == reinterpret_cast<uintptr_t>(room); });
    if (it == g_urgent.end()) {
        if (g_urgent.size() >= 64) g_urgent.erase(g_urgent.begin());
        g_urgent.push_back(UrgentRoom{reinterpret_cast<uintptr_t>(room), mgr, id, tier, now + kUrgentMs, soon});
    } else {
        it->tier = it->mgr == mgr && it->id == id ? std::min(it->tier, tier) : tier;
        it->mgr = mgr;
        it->id = id;
        it->until = now + kUrgentMs;
        it->soon = soon;
    }
    g_urgentSize.store(static_cast<int>(g_urgent.size()), std::memory_order_relaxed);
    g_urgentMarked.fetch_add(1, std::memory_order_relaxed);
}
// The urgent entry of a room: its tier (0 = a lamp's own room, 1 = a room taking its light; -1 = not urgent) and whether
// it may gather at once
int UrgentTier(const void* room, bool* soon = nullptr) {
    if (soon) *soon = false;
    if (!room || !g_urgentSize.load(std::memory_order_relaxed)) return -1;
    uintptr_t mgr = 0;
    int id = 0;
    if (!RoomKey(room, mgr, id)) return -1;
    const DWORD now = GetTickCount();
    std::lock_guard<std::mutex> lk(g_urgentMx);
    for (const UrgentRoom& u : g_urgent)
        if (u.room == reinterpret_cast<uintptr_t>(room) && u.mgr == mgr && u.id == id && static_cast<int32_t>(now - u.until) < 0) {
            if (soon) *soon = u.soon;
            return u.tier;
        }
    return -1;
}
// A room's story, the story its lot shows and its solve state; false when unreadable or no longer that room
bool RoomViewState(uintptr_t room, uintptr_t mgr, int id, int& level, int& cam, int& state) {
    __try {
        if (*reinterpret_cast<const uintptr_t*>(room) != mgr || *reinterpret_cast<const int*>(room + 0xC) != id) return false;
        level = *reinterpret_cast<const int*>(mgr + 0x88);
        cam = *reinterpret_cast<const int*>(mgr + 0x284);
        state = *reinterpret_cast<const int*>(room + 0xF0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Rooms the lamp entry update marked for a lamp edit (LampMarkFilter, light tree thread): AfterChangedWalk makes them
// urgent once the game's walk has sent them. user = a value a player edits (colour, intensity, on / off), not a flicker;
// pure = the lamp stayed in that room (it moved, switched or changed a value): it is registered there, its rooms may gather at once.
struct LampMarkNote {
    uintptr_t tl;
    int room;
    bool user, pure;
};
std::mutex g_lampMarkMx;
std::vector<LampMarkNote> g_lampMarks;
// Marks held while their room is being solved (light tree thread): given back by the room update once the solve is over
struct HeldMark {
    uintptr_t tl;
    int room;
    DWORD at; // the first hold (the room is marked after kHoldMaxMs at the latest)
    bool user;
};
std::mutex g_heldMx;
std::vector<HeldMark> g_held;
constexpr DWORD kHoldMaxMs = 1500, kHoldDropMs = 10000;
using MarkRoom_t = void(__thiscall*)(void* treeLevel, int room);

// The game's refresh from FUN_006c7250 (0x6C73B6..0x6C7426) for one room: it gathers and solves again. Rooms already
// waiting for their gather keep their countdown (false). The room being solved right now (state 3) is not invalidated:
// that would throw its solve away (0x69EED0 -> 0x6C4870, 0x69E950(0): nothing committed, 29/09 study); it is sent again
// once the solve is over (FlushDeferred). urgentTier >= 0: a lamp edit sends it (see "Lamp edits first"); soon: that edit
// is a lamp registered in that room that moved, switched or changed a value.
bool QueueRoom(uintptr_t tracker, int level, int id, bool defer = false, int urgentTier = -1, bool soon = false) {
    const uintptr_t tl = TreeLevel(tracker, level);
    void* mgr = *reinterpret_cast<void* const*>(tl);
    if (!mgr) return false;
    BYTE* room = static_cast<BYTE*>(reinterpret_cast<RoomById_t>(kRoomById)(mgr, id));
    if (!room) return false;
    if (urgentTier >= 0) MarkUrgent(room, urgentTier, soon);
    bool urgentSoon = false;
    UrgentTier(room, &urgentSoon); // also for a room held while solved and sent now (FlushDeferred)
    if (*reinterpret_cast<const int*>(room + 0xF0) == 1 && *reinterpret_cast<const int*>(room + 0x168) != 0) {
        if (urgentSoon) GatherSoon(room);
        return false;
    }
    // Only for requeues after a setting or ambient change (defer = true), and only while the room update runs FlushDeferred:
    // a room whose lamp list lost a lamp must stop its solve now (its list may point to the lamp being deleted).
    if (defer && g_installed.load(std::memory_order_relaxed) && g_indoorReady && *reinterpret_cast<const int*>(room + 0xF0) == 3) {
        std::lock_guard<std::mutex> lk(g_deferredMx);
        const bool known = std::any_of(g_deferred.begin(), g_deferred.end(), [&](const DeferredRoom& d) { return d.tracker == tracker && d.level == level && d.id == id; });
        if (!known && g_deferred.size() < 1024) {
            g_deferred.push_back(DeferredRoom{tracker, level, id, GetTickCount()});
            g_deferredCount.fetch_add(1, std::memory_order_relaxed);
        }
        if (Recorder::Verbose())
            if (!known) NoteSolve(room, 'H');
        return true;
    }
    reinterpret_cast<InvalidateRoom_t>(kInvalidateRoom)(room, 1, 0);
    alignas(16) BYTE out[16] = {};
    reinterpret_cast<SetInsert_t>(kSetInsert)(reinterpret_cast<void*>(tl + 0x28), out, &id, 0);
    if (urgentSoon) GatherSoon(room);
    if (Recorder::Verbose()) NoteSolve(room, 'Q');
    return true;
}

// The rooms of a lot that QueueRoom held back while they were being solved: sent now if that solve is over (a room gone
// or of another lot at the same address is dropped: RoomById finds nothing, or QueueRoom sends a fresh gather, harmless)
bool RoomSolving(uintptr_t tracker, int level, int id) {
    void* mgr = *reinterpret_cast<void* const*>(TreeLevel(tracker, level));
    const BYTE* room = mgr ? static_cast<const BYTE*>(reinterpret_cast<RoomById_t>(kRoomById)(mgr, id)) : nullptr;
    return room && *reinterpret_cast<const int*>(room + 0xF0) == 3;
}
void FlushDeferred(uintptr_t tracker) {
    std::vector<DeferredRoom> now;
    {
        std::lock_guard<std::mutex> lk(g_deferredMx);
        if (g_deferred.empty()) return;
        const DWORD tick = GetTickCount();
        for (auto it = g_deferred.begin(); it != g_deferred.end();)
            if (tick - it->at > 10000) // its lot is gone or never lit again
                it = g_deferred.erase(it);
            else if (it->tracker == tracker && !RoomSolving(tracker, it->level, it->id)) {
                now.push_back(*it);
                it = g_deferred.erase(it);
            } else
                ++it;
    }
    for (const DeferredRoom& d : now) QueueRoom(d.tracker, d.level, d.id);
}

int RooflessRoomIds(uintptr_t mgr, int* ids, int max); // below
// The outdoor rooms of floors 0..7: room 0, and the roofless rooms when the room update hooks send them (05/10)
void QueueOutdoorRegather(uintptr_t tracker) {
    for (int level = 0; level <= 7; level++) {
        if (QueueRoom(tracker, level, 0)) g_queued.fetch_add(1, std::memory_order_relaxed);
        int roofless[256];
        const int nr = g_indoorReady ? RooflessRoomIds(StoryManager(tracker, level), roofless, static_cast<int>(std::size(roofless))) : 0;
        for (int k = 0; k < nr; k++)
            if (QueueRoom(tracker, level, roofless[k])) g_queued.fetch_add(1, std::memory_order_relaxed);
    }
}

// Every light registered on a floor: treeLevel+0x90 hash (buckets +0x98, count +0x9C; node +8 -> {begin, end} of entries,
// next +0x10; entry +0x1C room id, +0x20 light info, +0x24 light). Same walk as FUN_006c6990.
template <class F> void WalkRegistry(uintptr_t tl, F&& visit) {
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
            if (entry) visit(entry);
        }
        node = *reinterpret_cast<const uintptr_t*>(node + 0x10);
        int g2 = 0;
        while (node == 0 && g2++ < (1 << 20)) node = *reinterpret_cast<const uintptr_t*>(slot += 4);
    }
}

// Roofless outdoor rooms (05/10, user, F8 19:11: a sconce moved into a light well left the wall of the story above dark).
// Walls, fences or railings that close an outdoor area make a room of its own with +0x18 set (a light well, a fenced yard,
// a deck behind a railing: room 17 of story 2 in the test house); the game solves it as outdoor, but its lamps are
// registered for its own id, so the light between stories, which carried only room 0's lamps, never took them, and those
// rooms never took the other stories' outdoor lamps either. Room 0 and these rooms are a story's outdoor rooms.
// The ids of a story's roofless rooms (room hash at mgr+0x230: buckets +0x234, count +0x238; node: id +0, room +0x10,
// next +0x80, as the F8 walks it)
int RooflessRoomIds(uintptr_t mgr, int* ids, int max) {
    int n = 0;
    if (!mgr) return 0;
    const uintptr_t rb = *reinterpret_cast<const uintptr_t*>(mgr + 0x234);
    const uint32_t rc = *reinterpret_cast<const uint32_t*>(mgr + 0x238);
    for (uint32_t b = 0; rb && rc < 100000 && b < rc && n < max; b++) {
        int guard = 0;
        for (uintptr_t rn = *reinterpret_cast<const uintptr_t*>(rb + b * 4); rn && guard++ < 10000 && n < max; rn = *reinterpret_cast<const uintptr_t*>(rn + 0x80)) {
            const int id = *reinterpret_cast<const int*>(rn);
            const BYTE* room = *reinterpret_cast<const BYTE* const*>(rn + 0x10);
            if (id > 0 && room && room[0x18] && std::find(ids, ids + n, id) == ids + n) ids[n++] = id;
        }
    }
    return n;
}
bool RooflessRoom(uintptr_t mgr, int id) {
    if (id <= 0 || !mgr) return false;
    const BYTE* room = static_cast<const BYTE*>(reinterpret_cast<RoomById_t>(kRoomById)(reinterpret_cast<void*>(mgr), id));
    return room && room[0x18];
}

bool GameTakesLight(uintptr_t entry, uintptr_t light); // below: FUN_006c7820's checks without its room match
bool ListHolds(const BYTE* room, uintptr_t light) {
    const uintptr_t* b = *reinterpret_cast<const uintptr_t* const*>(room + 0xC8);
    const uintptr_t* e = *reinterpret_cast<const uintptr_t* const*>(room + 0xCC);
    for (const uintptr_t* p = b; p && p < e && p - b < 4096; p++)
        if (*p == light) return true;
    return false;
}
// Adds to `room` (an outdoor room of story S) the outdoor lamps of the other stories, with the game's checks and adder
// FUN_006a2060: the lamps of their roofless rooms once, and unless rooflessOnly those of their room 0 (story 0's twice, their
// weight in story 0's own list). A lamp already in the list stays as it is.
int AddOutdoorLamps(uintptr_t tracker, int S, BYTE* room, bool rooflessOnly) {
    int added = 0;
    for (int other = 0; other <= 7; other++) {
        if (other == S) continue;
        const uintptr_t tl = TreeLevel(tracker, other);
        const uintptr_t mgr = *reinterpret_cast<const uintptr_t*>(tl);
        if (!mgr) continue;
        int roofless[256];
        const int nr = RooflessRoomIds(mgr, roofless, static_cast<int>(std::size(roofless)));
        if (rooflessOnly && !nr) continue;
        WalkRegistry(tl, [&](uintptr_t entry) {
            const int home = *reinterpret_cast<const int*>(entry + 0x1C);
            if (home == 0 ? rooflessOnly : std::find(roofless, roofless + nr, home) == roofless + nr) return;
            const uintptr_t light = *reinterpret_cast<const uintptr_t*>(entry + 0x24);
            if (!light || !GameTakesLight(entry, light) || ListHolds(room, light)) return;
            const int times = home == 0 && other == 0 ? 2 : 1;
            for (int t = 0; t < times; t++) reinterpret_cast<AddRoomLight_t>(kAddRoomLight)(room, reinterpret_cast<void*>(light));
            added += times;
        });
    }
    return added;
}

// Lights registered on a floor for its outdoor rooms: room 0 and the roofless rooms
void FloorOutdoorLights(uintptr_t tl, int floor, std::vector<Cross>& out) {
    int roofless[256];
    const int nr = RooflessRoomIds(*reinterpret_cast<const uintptr_t*>(tl), roofless, static_cast<int>(std::size(roofless)));
    WalkRegistry(tl, [&](uintptr_t entry) {
        const int home = *reinterpret_cast<const int*>(entry + 0x1C);
        if (home != 0 && std::find(roofless, roofless + nr, home) == roofless + nr) return;
        const uintptr_t light = *reinterpret_cast<const uintptr_t*>(entry + 0x24);
        if (!light || out.size() >= 4096) return;
        Cross c{light, floor};
        c.room = home;
        c.outdoor = true;
        out.push_back(c);
    });
}

std::atomic<long> g_indoorGen{1}; // bumped when every lot's rooms near openings must gather again (4.: option switched, install)

// What an indoor room saw, at its last gather, of the lamps of the other stories' rooms near the openings between them
// (ShareIndoorLights; light tree thread): each lamp with where it was and whether it was taken. AuditTakers compares it with
// the lamps as they are now and sends the room to gather again when they differ (see there).
struct SeenLamp {
    uintptr_t light = 0, entry = 0; // entry: for the game's checks at the audit (not compared)
    int home = 0;                   // the lamp's room on its story
    float x = 0.0f, z = 0.0f;       // where it is (lot space of the taking room's story)
    bool reach = false, takes = false;
};
struct SeenStory {
    int story = 0;
    std::vector<int> rooms;      // that story's rooms near the openings
    std::vector<SeenLamp> lamps; // their lamps, sorted by light then room
};
struct TakerSeen {
    uintptr_t mgr = 0, tracker = 0;
    int level = 0, id = 0;
    std::vector<SeenStory> stories;
    DWORD sentAt = 0, sendsFrom = 0, backoffUntil = 0; // the audit's sends of this room (at most kAuditSends per 10 s)
    int sends = 0;
};
std::unordered_map<uintptr_t, TakerSeen> g_seen; // room -> what it saw (light tree thread)
std::atomic<long> g_auditChecks{0}, g_auditSent{0}, g_auditGaveUp{0};
constexpr float kAuditMoveM = 0.25f; // m: a lamp moved less than this (animated lamps wobble less) needs no audit: its edit sent it
bool SeenBefore(const SeenLamp& a, const SeenLamp& b) { return a.light != b.light ? a.light < b.light : a.home < b.home; }

// Forget the rooms of a previous world (or when the map grew too big). Never during a point solve: the gather does not
// run inside it. Indoor entries (4.) stay: their rooms hold lamps of another story and the point solve must keep testing
// them (a stale entry is rejected by RoomStillSame and replaced at its room's next gather).
void MaybeClearRooms() {
    const bool world = g_clearRooms.exchange(false);
    if (world || g_seen.size() > 8192) g_seen.clear();
    if (!world && g_rooms.size() <= 8192) return;
    std::erase_if(g_rooms, [](const auto& kv) { return !kv.second.indoor; });
    if (g_rooms.size() > 8192) {
        g_rooms.clear();
        g_indoorGen.fetch_add(1); // the rooms holding lamps of another story gather again (QueueOpeningRooms)
    }
}

void RecordRoom(BYTE* room, uintptr_t mgr, uintptr_t tracker, int roomLevel) {
    MaybeClearRooms();
    RoomInfo& info = g_rooms[reinterpret_cast<uintptr_t>(room)];
    info.mgr = mgr;
    info.tracker = tracker;
    info.level = roomLevel;
    info.id = 0;
    info.indoor = false;
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
    // the lamps of the other stories' roofless rooms (05/10), with their own weight: once
    if (g_indoorReady) added += AddOutdoorLamps(tracker, roomLevel, room, true);
    if (added) g_shared.fetch_add(added, std::memory_order_relaxed);
    RecordRoom(room, rmgr, tracker, roomLevel);
}

// A roofless room (id > 0, +0x18 set) takes the outdoor lamps of the other stories: those of their room 0 (story 0's twice,
// their weight there) and of their roofless rooms (once), with the game's own checks of FUN_006c7820 and its adder
// FUN_006a2060, after the indoor share (which starts this room's record again). Recorded before the first lamp goes in, so
// a fault in the middle never leaves a lamp the point solve does not test. Needs the room update hooks (InstallIndoor): they
// send these rooms again when an outdoor lamp of another story changes.
void ShareRooflessLights(BYTE* treeLevel, BYTE* room) {
    const int id = *reinterpret_cast<const int*>(room + 0xC);
    if (id <= 0 || !room[0x18] || !g_indoorReady || !kAddRoomLight) return;
    const uintptr_t rmgr = *reinterpret_cast<const uintptr_t*>(room);
    if (!rmgr) return;
    const int S = *reinterpret_cast<const int*>(rmgr + 0x88);
    const int level = *reinterpret_cast<const int*>(treeLevel + 0x1A0);
    const uintptr_t tracker = *reinterpret_cast<const uintptr_t*>(treeLevel + 4);
    if (S < 0 || S > 7 || level < -4 || level > 7 || !tracker || TreeLevel(tracker, level) != reinterpret_cast<uintptr_t>(treeLevel) || StoryManager(tracker, S) != rmgr)
        return;
    g_gatherThread = ThreadId();
    std::vector<Cross> cross;
    for (int other = 0; other <= 7; other++)
        if (other != S && StoryManager(tracker, other)) FloorOutdoorLights(TreeLevel(tracker, other), other, cross);
    if (cross.empty()) return;
    MaybeClearRooms();
    RoomInfo& info = g_rooms[reinterpret_cast<uintptr_t>(room)];
    if (!info.indoor) { // a fresh record (the indoor share made none for this room)
        info.mgr = rmgr;
        info.tracker = tracker;
        info.level = S;
        info.id = id;
        info.cross.clear();
    }
    std::vector<Cross> fresh; // a lamp the indoor share took through an opening keeps that test
    for (const Cross& c : cross)
        if (!std::binary_search(info.cross.begin(), info.cross.end(), c)) fresh.push_back(c);
    info.cross.insert(info.cross.end(), fresh.begin(), fresh.end());
    std::sort(info.cross.begin(), info.cross.end());
    info.cross.erase(std::unique(info.cross.begin(), info.cross.end(), [](const Cross& a, const Cross& b) { return a.light == b.light; }), info.cross.end());
    const int added = AddOutdoorLamps(tracker, S, room, false);
    if (added) g_shared.fetch_add(added, std::memory_order_relaxed);
}

// ---- indoor lamps through stair openings (4.) ----
constexpr float kOpeningReach = 8.0f; // m, horizontally: rooms and lamps this close to a stair opening take part

std::atomic<bool> g_indoorOn{true}; // the option (SetIndoor)
bool g_indoorReady = false;         // its hooks are in (Install)
std::atomic<long> g_indoorAdded{0}, g_indoorTests{0}, g_indoorFloorBlocked{0}, g_indoorWallBlocked{0}, g_indoorQueued{0}, g_floorRefreshes{0};

// Level floor objects seen by the floor set / remove calls; their +0x238 is their story's lighting manager
std::mutex g_levelsMx;
std::vector<uintptr_t> g_levels;
uint32_t g_levelsGen = 0; // bumped when g_levels changes (under g_levelsMx)
// LevelFor's snapshot of (story manager found through the lot, floor object), newest floor first (under g_levelsMx)
struct LevelLink {
    uintptr_t mgr, level;
    bool own; // the story's own floor (byte +0x234 set), not the ceiling layer of the story below
};
std::vector<LevelLink> g_links;
uint32_t g_linksGen = ~0u;
DWORD g_linksAt = 0;
constexpr DWORD kLinksMaxAgeMs = 250;
std::vector<std::pair<uintptr_t, int>> g_dirtyLevels; // floors changed: (level floor object, tries left)
std::vector<uintptr_t> g_dirtyMgrs;                   // their lighting managers, once quiet (OnPresent)
std::atomic<bool> g_dirtyReady{false};
std::atomic<uintptr_t> g_lastLevel{0};
std::atomic<DWORD> g_floorTick{0}; // GetTickCount of the last floor change not handed on yet (0 = none)
std::atomic<long> g_floorEdits{0};
std::atomic<bool> g_roomRefsDirty{false};
std::mutex g_structureMx;
std::unordered_map<uintptr_t, uint64_t> g_roomStructures;
std::vector<uintptr_t> g_structureRooms;
std::atomic<bool> g_structurePending{false};
DWORD g_structureRefreshAt = 0;

// Native gathers already follow room creation, enclosure and roof changes.
// Observe only structure, never lamp RGB/animation or lighting LOD.
void NoteRoomStructure(BYTE* room) {
    const int id = *reinterpret_cast<const int*>(room + 0xC);
    if (id <= 0) return;
    uint64_t signature = 1469598103934665603ull;
    const auto mix = [&](uint64_t value) { signature = (signature ^ value) * 1099511628211ull; };
    mix(*reinterpret_cast<const uintptr_t*>(room));
    mix(static_cast<uint32_t>(id));
    mix(room[0x18]); // roofless/outdoor classification
    const uintptr_t begin = *reinterpret_cast<const uintptr_t*>(room + 0x30);
    const uintptr_t end = *reinterpret_cast<const uintptr_t*>(room + 0x34);
    if (end < begin || (end - begin) % sizeof(uintptr_t) || end - begin > 1024 * sizeof(uintptr_t)) return;
    mix(end - begin);
    for (uintptr_t at = begin; at < end; at += sizeof(uintptr_t)) mix(*reinterpret_cast<const uintptr_t*>(at));
    const uintptr_t key = reinterpret_cast<uintptr_t>(room);
    std::lock_guard<std::mutex> lock(g_structureMx);
    if (g_roomStructures.size() >= 8192 && !g_roomStructures.contains(key)) g_roomStructures.clear();
    const auto [it, fresh] = g_roomStructures.try_emplace(key, signature);
    if (!fresh && it->second == signature) return;
    it->second = signature;
    if (g_structureRooms.size() < 1024 && std::find(g_structureRooms.begin(), g_structureRooms.end(), key) == g_structureRooms.end())
        g_structureRooms.push_back(key);
    g_structurePending.store(true, std::memory_order_release);
}
uintptr_t g_floorSetTarget = 0, g_floorRemoveTarget = 0; // the thunks' jumps (the game's functions)

// A lot's lighting manager of one story: 0x00ADBCC0 translated (thiscall(lot lighting, level) ret 4, pure reads), the
// lookup the world code does every time it talks to the lighting. A deque of story managers: +0x24 first element, +0x28 /
// +0x2C its block, +0x30 / +0x40 the first and last block slots of the map, +0x34 / +0x38 the last element and its block,
// 64 per block, +0x48 the lowest level. 0 = no such story.
uintptr_t LotStoryManager(uintptr_t lot, int32_t level) {
    const auto rd = [lot](uint32_t off) { return *reinterpret_cast<const int32_t*>(lot + off); };
    int32_t count = ((rd(0x40) - rd(0x30)) >> 2) - 1;
    count = (count << 6) + ((rd(0x34) - rd(0x38)) >> 2) + ((rd(0x2C) - rd(0x24)) >> 2);
    const int32_t rel = level - rd(0x48);
    if (static_cast<uint32_t>(rel) >= static_cast<uint32_t>(count)) return 0;
    const int32_t idx = ((rd(0x24) - rd(0x28)) >> 2) + rel;
    const int32_t t = idx + 0x1000000;
    const int32_t block = ((t + ((t >> 31) & 0x3F)) >> 6) - 0x40000;
    const uintptr_t blockPtr = *reinterpret_cast<const uintptr_t*>(static_cast<uintptr_t>(rd(0x30)) + static_cast<uintptr_t>(block) * 4);
    return *reinterpret_cast<const uintptr_t*>(blockPtr + static_cast<uintptr_t>(idx - block * 64) * 4);
}

// The level floor object's lighting manager, or 0 when it is not (or no longer) one. Found the way the game finds it
// (0x00A89B60..0x00A89B89 at the floor's init; the world level code at 0x00A9D0DC does the same with its own fields): the
// floor's owner lot +0x214, the lot's lighting +0x23C, and 0x00ADBCC0 at world level +0x230, minus 1 when byte +0x234 is 0.
// The floor keeps a copy at +0x238, written only at that init: when a lot's lighting is built again the copy goes stale,
// and a new story manager made at the same address made an old floor match it (in-game 30/09: a double-height room whose
// story 2 was paired with a floor of another shape, so no opening was seen and the lamp below never lit the walls above
// until the lot was left and entered again). Released floors (0x00A88980) have no owner.
uintptr_t LevelManager(uintptr_t level) {
    __try {
        if (!level || *reinterpret_cast<const uintptr_t*>(level) != kLevelVtable) return 0;
        const uintptr_t owner = *reinterpret_cast<const uintptr_t*>(level + 0x214);
        const uintptr_t lot = owner ? *reinterpret_cast<const uintptr_t*>(owner + 0x23C) : 0;
        if (!lot) return 0;
        const int32_t worldLevel = *reinterpret_cast<const int32_t*>(level + 0x230);
        return LotStoryManager(lot, *reinterpret_cast<const uint8_t*>(level + 0x234) ? worldLevel : worldLevel - 1);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
// Whether the floor object is its story's own floor. The lot's floor renderer makes two per story (0x00AA1710, the only
// maker; its callers 0x00AA35F4 / 0x00AA3690 pass byte 1, 0x00AA4171 / 0x00AA4398 pass 0): a floor at world level L with
// byte +0x234 = 1, lit by story L, and a layer at world level L+1 with byte 0, lit by story L too (LevelManager: minus 1),
// the ceiling of story L, which has the shape of the floor above and none of its holes. Both name story L; only the first
// says where story L's floor is open (in-game 30/09: the double-height room of lot 8C41002E4010A180 was paired with its
// ceiling (keys 0000C007 / 00000006, the attic's outline) whenever that object was the newer one, so no opening was seen).
bool LevelOwnFloor(uintptr_t level) {
    __try {
        return level && *reinterpret_cast<const uint8_t*>(level + 0x234) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
int LevelWorld(uintptr_t level) {
    __try {
        return level ? *reinterpret_cast<const int32_t*>(level + 0x230) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
// The copy the floor keeps (+0x238), for the F8 notes only
uintptr_t LevelManagerCopy(uintptr_t level) {
    __try {
        if (!level || *reinterpret_cast<const uintptr_t*>(level) != kLevelVtable) return 0;
        return *reinterpret_cast<const uintptr_t*>(level + 0x238);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
bool LevelAlive(uintptr_t level) {
    __try {
        return *reinterpret_cast<const uintptr_t*>(level) == kLevelVtable;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Called by the thunks (any thread) before the game sets or removes a floor
void __cdecl NoteLevel(uintptr_t level) {
    g_floorEdits.fetch_add(1, std::memory_order_relaxed);
    const DWORD now = GetTickCount();
    g_floorTick.store(now ? now : 1, std::memory_order_relaxed);
    if (!level || g_lastLevel.exchange(level, std::memory_order_relaxed) == level) return; // a lot sets thousands in a row
    std::lock_guard<std::mutex> lk(g_levelsMx);
    if (std::find(g_levels.begin(), g_levels.end(), level) == g_levels.end()) {
        if (g_levels.size() >= 4096) std::erase_if(g_levels, [](uintptr_t l) { return !LevelAlive(l); });
        if (g_levels.size() >= 4096) g_levels.erase(g_levels.begin()); // the oldest
        g_levels.push_back(level);
        g_levelsGen++;
    }
    if (g_dirtyLevels.size() < 256 && std::none_of(g_dirtyLevels.begin(), g_dirtyLevels.end(), [&](const auto& d) { return d.first == level; }))
        g_dirtyLevels.emplace_back(level, 10);
}

// A level floor object just made (FUN_00a88790 at its only call): known from now on, whatever fills its floors. Its
// story's lighting manager (+0x238) is linked later; LevelFor reads it when asked.
using LevelCtor_t = void*(__thiscall*)(void* level);
void* __fastcall LevelCtorHook(void* level) {
    void* made = reinterpret_cast<LevelCtor_t>(kLevelCtor)(level);
    if (level) {
        g_levelsMade.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(g_levelsMx);
        const uintptr_t l = reinterpret_cast<uintptr_t>(level);
        if (std::find(g_levels.begin(), g_levels.end(), l) == g_levels.end()) {
            if (g_levels.size() >= 4096) std::erase_if(g_levels, [](uintptr_t x) { return !LevelAlive(x); });
            if (g_levels.size() >= 4096) g_levels.erase(g_levels.begin()); // the oldest
            g_levels.push_back(l);
            g_levelsGen++;
        }
    }
    return made;
}

// ecx = the level floor object; the game's arguments stay on the stack for the jump
__declspec(naked) void FloorSetThunk() {
    __asm {
        push ecx
        push ecx
        call NoteLevel
        add esp, 4
        pop ecx
        jmp dword ptr [g_floorSetTarget]
    }
}
__declspec(naked) void FloorRemoveThunk() {
    __asm {
        push ecx
        push ecx
        call NoteLevel
        add esp, 4
        pop ecx
        jmp dword ptr [g_floorRemoveTarget]
    }
}

// The floor object of a story (by its lighting manager): its own floor (LevelOwnFloor), never the ceiling layer that
// names the same story; 0 when none is known.
// Every floor's link is found through its lot (LevelManager, ~20 reads), so the links are kept in a snapshot rebuilt when
// the list of floors changes or after kLinksMaxAgeMs; a hit is checked again (a lot relit since then rebuilds it at once),
// a miss may be up to kLinksMaxAgeMs old (a lot that just loaded: its rooms gather again when their openings appear,
// LotState).
void RebuildLinksLocked(DWORD now) {
    g_links.clear();
    for (auto it = g_levels.rbegin(); it != g_levels.rend(); ++it)
        if (const uintptr_t m = LevelManager(*it)) g_links.push_back({m, *it, LevelOwnFloor(*it)});
    g_linksGen = g_levelsGen;
    g_linksAt = now;
}
uintptr_t LevelFor(uintptr_t mgr) {
    if (!mgr) return 0;
    std::lock_guard<std::mutex> lk(g_levelsMx);
    const DWORD now = GetTickCount();
    bool rebuilt = false;
    if (g_linksGen != g_levelsGen || now - g_linksAt > kLinksMaxAgeMs) {
        RebuildLinksLocked(now);
        rebuilt = true;
    }
    for (;;) {
        bool changed = false;
        for (const LevelLink& k : g_links)
            if (k.mgr == mgr && k.own) {
                if (LevelManager(k.level) == mgr) return k.level;
                changed = true;
                break;
            }
        if (!changed || rebuilt) return 0;
        RebuildLinksLocked(now);
        rebuilt = true;
    }
}

// World -> lot: rows at mgr+0xE0 / +0xF0 / +0x100 / +0x110 (the inverse of the lot matrix, the same on every story)
struct Xform {
    float r[4][4];
};
bool ReadXform(uintptr_t mgr, Xform& x) {
    __try {
        std::memcpy(&x, reinterpret_cast<const void*>(mgr + 0xE0), sizeof x);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void ToLocal(const Xform& x, const float* w, float* l) {
    for (int k = 0; k < 3; k++) l[k] = w[0] * x.r[0][k] + w[1] * x.r[1][k] + w[2] * x.r[2][k] + x.r[3][k];
}

// A story's lighting tile (1 m): mgr+0x260 pointers, width +0x264, height +0x268 (FUN_006a42d0); 0 = none
uintptr_t LightTile(uintptr_t mgr, int ix, int iz) {
    const uint32_t w = *reinterpret_cast<const uint32_t*>(mgr + 0x264), h = *reinterpret_cast<const uint32_t*>(mgr + 0x268);
    if (static_cast<uint32_t>(ix) >= w || static_cast<uint32_t>(iz) >= h || w > 1024 || h > 1024) return 0;
    return reinterpret_cast<const uintptr_t*>(*reinterpret_cast<const uintptr_t*>(mgr + 0x260))[static_cast<size_t>(iz) * w + ix];
}
// The room of a tile's quadrant (0 = outside; FUN_006a9760)
int TileRoom(uintptr_t tile, int q) { return *reinterpret_cast<const int*>(tile + 0x7C + q * 0x14); }
// Quadrants split the tile along its diagonals (FUN_006aa390): 0 toward -z, 1 +x, 2 +z, 3 -x
int Quadrant(float fx, float fz) {
    if (std::fabs(fx - 0.5f) <= std::fabs(fz - 0.5f)) return fz > 0.5f ? 2 : 0;
    return fx > 0.5f ? 1 : 3;
}
// A floor key with a floor in it. Measured in game (F8 maps, 2026-09-29): a quadrant whose floor was removed keeps a key
// with bit 0x40000000 of its low dword set (the test tower: 4001E000 after its floor was removed; the ground under a
// foundation 4001BFFE; the stairwell of the house 4000E000; the air next to walls 40000000), and a quadrant never built
// has the empty key 0xFFFFFFF8 / 0xFFFFFFFF. Floors players place have keys without that bit (0000A007, 00000014, ...).
// Only a removed floor (the bit AND other bits of the low dword: 4001E000, 4000E000) is an opening: the bare 40000000
// runs along walls and around the edges of a story's floor, over rooms that keep their ceiling (counting it made the
// lamps of the story below shine up along the walls, second in-game test), and the empty key is never-built space.
bool RemovedFloorKey(const uint32_t* key) {
    return !(key[0] == 0xFFFFFFF8u && key[1] == 0xFFFFFFFFu) && (key[0] & 0x40000000u) && (key[0] & 0x3FFFFFFFu);
}

// The story's floor grid: 1 = a floor (or anything that is not a removed floor), 0 = a removed floor. What cannot be read
// (no grid, outside it) counts as a floor: light never passes a floor that was not seen open.
int FloorAt(uintptr_t level, int ix, int iz, int q) {
    const uintptr_t grid = *reinterpret_cast<const uintptr_t*>(level + 0x264);
    if (!grid) return 1;
    const uintptr_t data = *reinterpret_cast<const uintptr_t*>(grid);
    const int w = *reinterpret_cast<const int*>(grid + 0x10), h = *reinterpret_cast<const int*>(grid + 0x14);
    if (!data || ix < 0 || iz < 0 || ix >= w || iz >= h || w > 1024 || h > 1024) return 1;
    return RemovedFloorKey(reinterpret_cast<const uint32_t*>(data + (static_cast<size_t>(iz) * w + ix) * 40 + 8 + q * 8)) ? 0 : 1;
}

// Openings of story B's floor: quadrants with a removed floor above an indoor room of the story below (mgrBelow). The
// room is read below, not on B: the landing around a stairwell is often not a room on B (railings do not close a room),
// and the air outside the house has no indoor room under it.
// OpeningMask: per tile of the lot, bit 1 = it holds an opening quadrant, bit 2 = its centre lies within kOpeningReach
// of the centre of such a tile. A mask instead of a list: an atrium can have well over a thousand opening quadrants
// (a player's house: 1356 on one story), and an earlier list capped at 256 dropped the openings near some lamps.
struct OpeningMask {
    int w = 0, h = 0, openings = 0; // openings: quadrants
    std::vector<uint8_t> bits;
    bool NearTile(int ix, int iz) const { return ix >= 0 && iz >= 0 && ix < w && iz < h && (bits[static_cast<size_t>(iz) * w + ix] & 2); }
    bool Near(float x, float z) const { return NearTile(static_cast<int>(std::floor(x)), static_cast<int>(std::floor(z))); }
};

bool ReadGridSize(uintptr_t mgr, int& w, int& h) {
    __try {
        w = *reinterpret_cast<const int*>(mgr + 0x264);
        h = *reinterpret_cast<const int*>(mgr + 0x268);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Marks the opening tiles (bit 1); the number of opening quadrants, -1 on a fault
int ReadOpenings(uintptr_t mgrBelow, uintptr_t levelB, int w, int h, uint8_t* bits) {
    int n = 0;
    __try {
        for (int iz = 0; iz < h; iz++)
            for (int ix = 0; ix < w; ix++) {
                const uintptr_t below = LightTile(mgrBelow, ix, iz);
                if (!below) continue;
                for (int q = 0; q < 4; q++)
                    if (TileRoom(below, q) > 0 && FloorAt(levelB, ix, iz, q) == 0) {
                        bits[static_cast<size_t>(iz) * w + ix] |= 1;
                        n++;
                    }
            }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
}

// False on a fault (the mask is then empty)
bool BuildOpeningMask(uintptr_t mgrBelow, uintptr_t mgrB, uintptr_t levelB, OpeningMask& m) {
    m.w = m.h = m.openings = 0;
    m.bits.clear();
    if (!mgrBelow || !mgrB || !levelB) return true;
    int w = 0, h = 0;
    if (!ReadGridSize(mgrB, w, h)) return false;
    if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return true;
    m.bits.assign(static_cast<size_t>(w) * h, 0);
    const int n = ReadOpenings(mgrBelow, levelB, w, h, m.bits.data());
    if (n < 0) {
        m.bits.clear();
        return false;
    }
    m.w = w, m.h = h, m.openings = n;
    if (!n) return true;
    const int r = static_cast<int>(kOpeningReach);
    const int r2 = r * r;
    for (int iz = 0; iz < h; iz++)
        for (int ix = 0; ix < w; ix++) {
            if (!(m.bits[static_cast<size_t>(iz) * w + ix] & 1)) continue;
            for (int dz = -r; dz <= r; dz++)
                for (int dx = -r; dx <= r; dx++) {
                    const int x = ix + dx, z = iz + dz;
                    if (dx * dx + dz * dz <= r2 && x >= 0 && z >= 0 && x < w && z < h) m.bits[static_cast<size_t>(z) * w + x] |= 2;
                }
        }
    return true;
}

// Where a room of a story lies (lot space, tile bounds) and whether one of its quadrants is near an opening
struct RoomSpan {
    bool any = false, nearOpening = false;
    float x0 = 0, x1 = 0, z0 = 0, z1 = 0;
};
bool ReadRoomSpan(uintptr_t mgr, int id, const OpeningMask& m, RoomSpan& s) {
    s = RoomSpan{};
    __try {
        const int w = *reinterpret_cast<const int*>(mgr + 0x264), h = *reinterpret_cast<const int*>(mgr + 0x268);
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return true;
        for (int iz = 0; iz < h; iz++)
            for (int ix = 0; ix < w; ix++) {
                const uintptr_t tile = LightTile(mgr, ix, iz);
                if (!tile) continue;
                bool mine = false;
                for (int q = 0; q < 4; q++) mine |= TileRoom(tile, q) == id;
                if (!mine) continue;
                if (!s.any) s.x0 = s.x1 = static_cast<float>(ix), s.z0 = s.z1 = static_cast<float>(iz);
                s.any = true;
                s.x0 = std::min(s.x0, static_cast<float>(ix)), s.x1 = std::max(s.x1, static_cast<float>(ix + 1));
                s.z0 = std::min(s.z0, static_cast<float>(iz)), s.z1 = std::max(s.z1, static_cast<float>(iz + 1));
                s.nearOpening |= m.NearTile(ix, iz);
            }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}

// The rooms (ids > 0) of a story with a quadrant near an opening; how many were written to out
int RoomsNearOpenings(uintptr_t mgr, const OpeningMask& m, int* out, int max) {
    int n = 0;
    __try {
        const int w = std::min(*reinterpret_cast<const int*>(mgr + 0x264), m.w), h = std::min(*reinterpret_cast<const int*>(mgr + 0x268), m.h);
        for (int iz = 0; iz < h; iz++)
            for (int ix = 0; ix < w; ix++) {
                if (!m.NearTile(ix, iz)) continue;
                const uintptr_t tile = LightTile(mgr, ix, iz);
                if (!tile) continue;
                for (int q = 0; q < 4; q++) {
                    const int id = TileRoom(tile, q);
                    if (id <= 0 || n >= max) continue;
                    int k = 0;
                    while (k < n && out[k] != id) k++;
                    if (k == n) out[n++] = id;
                }
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
    }
    return n;
}

// Which rooms take the lamps of which: (lot, lamp story, lamp room) -> (story, room) of the rooms that take them.
// Kept until the world changes (an extra entry only costs one more gather of that room).
struct DepKey {
    uintptr_t tracker;
    int level, room;
    bool operator==(const DepKey&) const = default;
};
struct DepHash {
    size_t operator()(const DepKey& k) const { return k.tracker ^ (static_cast<size_t>(k.level) * 0x9E3779B1u) ^ (static_cast<size_t>(k.room) * 0x85EBCA6Bu); }
};
std::mutex g_depsMx;
std::unordered_map<DepKey, std::vector<std::pair<int, int>>, DepHash> g_deps;

// The lamps of other stories that have lit a room, at any point of any of its solves since its story was built (05/10,
// recording 20:38:25, atrium house: every 10 cm of a dragged sconce re-solved 13 rooms of stories 0, 2 and 3, 5.9 s of
// solving in 6.5 s, ~9 frames a second; 9 of them never get any of its light, all of it stopped by the floors). A lamp
// edit sends at once only the rooms its lamps have lit; the others wait until the lamps are quiet (FlushEditWaits).
// Light tree thread (the point solve writes, AfterChangedWalk and AuditTakers read).
struct LitBy {
    uintptr_t mgr = 0;
    std::vector<uintptr_t> lights; // at most kLitMax, oldest first
};
constexpr size_t kLitMax = 64;
std::unordered_map<DepKey, LitBy, DepHash> g_litBy;
void NoteLit(const DepKey& key, uintptr_t mgr, uintptr_t light) {
    if (g_litBy.size() > 8192) g_litBy.clear();
    LitBy& l = g_litBy[key];
    if (l.mgr != mgr) l = LitBy{mgr, {}};
    if (std::find(l.lights.begin(), l.lights.end(), light) != l.lights.end()) return;
    if (l.lights.size() >= kLitMax) l.lights.erase(l.lights.begin());
    l.lights.push_back(light);
}
// Whether one of these lamps has lit the room (unknown story manager: false)
bool LitByAny(const DepKey& key, const uintptr_t* lights, size_t n) {
    const auto it = g_litBy.find(key);
    if (it == g_litBy.end() || it->second.mgr != StoryManager(key.tracker, key.level)) return false;
    for (size_t k = 0; k < n; k++)
        if (std::find(it->second.lights.begin(), it->second.lights.end(), lights[k]) != it->second.lights.end()) return true;
    return false;
}
// Rooms an edit left for later (not lit by its lamps), sent once the lamps are quiet (light tree thread)
struct EditWait {
    DepKey key;
    DWORD at;
};
std::vector<EditWait> g_editWaits;
std::atomic<long> g_editWaited{0};

void NoteDeps(uintptr_t tracker, int lampLevel, const std::vector<int>& lampRooms, int level, int room) {
    std::lock_guard<std::mutex> lk(g_depsMx);
    if (g_deps.size() > 8192) g_deps.clear();
    for (int x : lampRooms) {
        auto& v = g_deps[DepKey{tracker, lampLevel, x}];
        const auto p = std::make_pair(level, room);
        if (v.size() < 64 && std::find(v.begin(), v.end(), p) == v.end()) v.push_back(p);
    }
}

// The game's checks of FUN_006c7820 (without its room match) for a registry entry
bool GameTakesLight(uintptr_t entry, uintptr_t light) {
    const uintptr_t info = *reinterpret_cast<const uintptr_t*>(entry + 0x20);
    if (!info) return false;
    const BYTE flags = *reinterpret_cast<const BYTE*>(info + 0x90);
    if (!(flags & 2) || !(*reinterpret_cast<const BYTE*>(light + 0x100) & 0x20)) return false;
    if (!reinterpret_cast<LightBright_t>(kLightBright)(reinterpret_cast<void*>(light))) return false;
    const int type = *reinterpret_cast<const int*>(light + 0xB0);
    return type >= 3 && (!(flags & 4) || type == 0xB);
}
// The light's class has its evaluation wrapped (so the point solve tests the floor for it)
bool EvalWrapped(uintptr_t light) {
    const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(light);
    for (size_t i = 0; i < std::size(kClasses); i++)
        if (kClasses[i].vtable == vt && g_evalOrig[i]) return true;
    return false;
}
bool InList(const BYTE* room, uintptr_t light) {
    const uintptr_t* b = *reinterpret_cast<const uintptr_t* const*>(room + 0xC8);
    const uintptr_t* e = *reinterpret_cast<const uintptr_t* const*>(room + 0xCC);
    for (const uintptr_t* p = b; p < e && p - b < 4096; p++)
        if (*p == light) return true;
    return false;
}

// Development build: what the indoor gathers did, for F8 (always recorded: a gather only runs when a room changes)
std::unordered_set<uintptr_t> g_crossLamps; // development build: lamps some room of another story took (light tree thread)
std::mutex g_gatherLogMx;
std::vector<std::string> g_gatherLog;
void GatherLog(uintptr_t tracker, const std::string& line) {
    std::lock_guard<std::mutex> lk(g_gatherLogMx);
    if (g_gatherLog.size() >= 2000) g_gatherLog.erase(g_gatherLog.begin(), g_gatherLog.begin() + 500); // the latest ones
    g_gatherLog.push_back(std::format("tracker {:08X} ", tracker) + line);
}

// The rooms holding lamps of another story, for any thread (Uninstall sends them to gather again: without the wrapped
// evaluation those lamps would shine through the floor)
struct IndoorRoom {
    uintptr_t tracker;
    int level, id;
    bool operator==(const IndoorRoom&) const = default;
};
std::mutex g_indoorListMx;
std::vector<IndoorRoom> g_indoorList;
void NoteIndoorRoom(uintptr_t tracker, int level, int id, bool has) {
    const IndoorRoom r{tracker, level, id};
    std::lock_guard<std::mutex> lk(g_indoorListMx);
    auto it = std::find(g_indoorList.begin(), g_indoorList.end(), r);
    if (has && it == g_indoorList.end() && g_indoorList.size() < 8192) g_indoorList.push_back(r);
    else if (!has && it != g_indoorList.end()) g_indoorList.erase(it);
}

// Rooms whose lighting keeps the LOD of the camera's story although they lie below it. The game gives every room below
// the camera's story LOD 0 (FUN_0069e710): on walls a light sample every 0.75 m instead of about 0.25 m, fine while the
// floor above hides it. A room seen through an opening shows it: under a sconce the lower wall's last sample (0.47 m
// over the lamp, 5.65) was stretched up to the floor line while the finely sampled wall above started at 0.45 (pool
// room of an atrium house, F8 2026-09-29: rooms 19 "classe 0" and 20 "classe 2"). So the rooms that take lamps through an
// opening (bit 1) and those whose lamps another story takes (bit 2) get the camera story's class; the game then raises
// them one step per solve itself (FUN_0069ea70).
std::mutex g_boostMx;
struct Boost {
    int id = 0;
    uintptr_t mgr = 0, tracker = 0; // a room at a reused address (another id or story manager) is not boosted
    int level = 0;
    bool taker = false; // takes lamps through an opening
    int givers = 0;     // rooms of another story holding its lamps
};
std::unordered_map<uintptr_t, Boost> g_boostRooms;
std::unordered_map<uintptr_t, std::vector<uintptr_t>> g_takerGives; // taking room -> the rooms whose lamps it holds

// Under g_boostMx. The entry of a room, reset when the address now holds another room.
Boost& BoostEntry(uintptr_t room, int id, uintptr_t mgr) {
    Boost& b = g_boostRooms[room];
    if (b.id != id || b.mgr != mgr) b = Boost{id, mgr};
    return b;
}
// Under g_boostMx. What a room held as a taker is released (its gather starts again). Entries stay, dormant (no taker,
// no giver: not boosted), so a giver released and taken again within one gather is not sent to solve again (a loop).
void ReleaseTakerLocked(uintptr_t room) {
    if (auto t = g_takerGives.find(room); t != g_takerGives.end()) {
        for (uintptr_t giver : t->second)
            if (auto g = g_boostRooms.find(giver); g != g_boostRooms.end() && --g->second.givers < 0) g->second.givers = 0;
        g_takerGives.erase(t);
    }
    if (auto b = g_boostRooms.find(room); b != g_boostRooms.end()) b->second.taker = false;
}
// Every gather of an indoor room starts here, before any early return, so nothing stays boosted that no longer should
void BeginTaker(uintptr_t room) {
    std::lock_guard<std::mutex> lk(g_boostMx);
    if (g_boostRooms.size() > 16384 || g_takerGives.size() > 16384) {
        g_boostRooms.clear();
        g_takerGives.clear();
    }
    ReleaseTakerLocked(room);
}
void MarkTaker(uintptr_t room, int id, uintptr_t mgr, uintptr_t tracker, int level) {
    std::lock_guard<std::mutex> lk(g_boostMx);
    Boost& b = BoostEntry(room, id, mgr);
    b.tracker = tracker, b.level = level, b.taker = true;
}
// True when the giving room was not boosted before (it is then sent to solve again once)
bool AddGiver(uintptr_t taker, uintptr_t giver, int id, uintptr_t mgr, uintptr_t tracker, int level) {
    std::lock_guard<std::mutex> lk(g_boostMx);
    auto& held = g_takerGives[taker];
    if (std::find(held.begin(), held.end(), giver) != held.end()) return false;
    held.push_back(giver);
    const bool known = g_boostRooms.count(giver) && g_boostRooms[giver].id == id && g_boostRooms[giver].mgr == mgr;
    Boost& b = BoostEntry(giver, id, mgr);
    const bool fresh = !known; // first seen as a giver (a dormant entry was seen before)
    b.tracker = tracker, b.level = level, b.givers++;
    return fresh;
}
bool Boosted(uintptr_t room, int id, uintptr_t mgr) {
    std::lock_guard<std::mutex> lk(g_boostMx);
    const auto it = g_boostRooms.find(room);
    return it != g_boostRooms.end() && it->second.id == id && it->second.mgr == mgr && (it->second.taker || it->second.givers > 0);
}
// The boosted rooms of a lot, (story, id): sent to gather again when the option changes (with the option off the hook
// no longer boosts them, and a room in the middle of its raise would wait at a class the game no longer asks for)
void BoostedRoomsOf(uintptr_t tracker, std::vector<std::pair<int, int>>& out) {
    std::lock_guard<std::mutex> lk(g_boostMx);
    for (const auto& [room, b] : g_boostRooms)
        if (b.tracker == tracker && (b.taker || b.givers > 0)) out.emplace_back(b.level, b.id);
}
void ClearBoosts() {
    std::lock_guard<std::mutex> lk(g_boostMx);
    g_boostRooms.clear();
    g_takerGives.clear();
}
std::atomic<long> g_lodBoosts{0};
bool g_lodReady = false; // LodChoiceHook is in (InstallIndoor)

int BoostedLod(BYTE* room) {
    __try {
        const uintptr_t mgr = *reinterpret_cast<const uintptr_t*>(room);
        if (!mgr) return 0;
        const int level = *reinterpret_cast<const int*>(mgr + 0x88), cam = *reinterpret_cast<const int*>(mgr + 0x284);
        // the case FUN_0069e710 gives 0 to: an indoor room (+0x18 clear) below the camera's story, on the active lot (+0x288)
        if (level >= cam || room[0x18] || *reinterpret_cast<const BYTE*>(mgr + 0x288) != 1) return 0;
        if (!Boosted(reinterpret_cast<uintptr_t>(room), *reinterpret_cast<const int*>(room + 0xC), mgr)) return 0;
        g_lodBoosts.fetch_add(1, std::memory_order_relaxed);
        return *reinterpret_cast<const int*>(kLodMax);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Every floor in full detail (user 30/09: "switching floors resets the lights every time"): FUN_0069e710 gives the top LOD
// class only to the rooms on the camera's story of the active lot, so every floor change re-solved the rooms of the floors
// the camera left and reached. With this on, every room of the active lot (+0x288 == 1) gets the top class on every story:
// its light maps are solved once and stay when the camera changes floor (more solve work when entering a lot, none after).
std::atomic<bool> g_allFloors{true};
std::atomic<long> g_allFloorsRaised{0};
// "High quality on every lot" (night_terrain_relight) sets +0x288 on every detailed lot: then only the lot the game plays
// first (its priority lot) gets every story at the top class (05/10 memory study: every story of up to 8 lots held all
// three class sets, ~4 MB a story, ~16 MB with Light detail High)
std::atomic<bool> g_allLotsHigh{false};
bool PriorityLotMgr(uintptr_t mgr) {
    if (!kSomGet || !kPrioLot) return true; // cannot tell: as before
    void* som = reinterpret_cast<void*(*)()>(kSomGet)();
    return som && reinterpret_cast<bool(__thiscall*)(void*, uint32_t, uint32_t)>(kPrioLot)(som, *reinterpret_cast<const uint32_t*>(mgr + 0x90), *reinterpret_cast<const uint32_t*>(mgr + 0x94));
}
int AllFloorsLod(BYTE* room) {
    __try {
        const uintptr_t mgr = *reinterpret_cast<const uintptr_t*>(room);
        if (!mgr || *reinterpret_cast<const BYTE*>(mgr + 0x288) != 1) return 0;
        if (g_allLotsHigh.load(std::memory_order_relaxed) && !PriorityLotMgr(mgr)) return 0;
        g_allFloorsRaised.fetch_add(1, std::memory_order_relaxed);
        return *reinterpret_cast<const int*>(kLodMax);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
using LodChoice_t = int(__fastcall*)(void* room);
int __fastcall LodChoiceHook(BYTE* room) {
    const int lod = reinterpret_cast<LodChoice_t>(kLodChoice)(room);
    if (lod || !room || !g_installed.load(std::memory_order_relaxed)) return lod;
    if (g_allFloors.load(std::memory_order_relaxed)) {
        if (const int all = AllFloorsLod(room)) return all;
    }
    if (!g_indoorReady || !g_indoorOn.load(std::memory_order_relaxed)) return lod;
    return BoostedLod(room);
}

// ---- one ambient for the rooms stacked through an opening (4., 2026-09-29) ----
// A double-height room is two rooms, one per story, and its wall two pieces, each lit by its own room. At the start of
// every room solve (state 0, FUN_006a18b0, its only call at 0x6A3D0B in FUN_006a3c90) the game computes per indoor room:
//  - the ambient colour room+0x110 (the wall shader adds lightmap.a x c4; the parameter table binds a pointer to it, so a
//    change shows at once), FUN_006a0f50: the room's lights sampled on its floor tiles and 4 m above, over its area,
//    already multiplied by the room's normalisation; also copied into the sampler at room+0x640 (+0x650), which the
//    ceiling pass adds into its texels;
//  - the normalisation room+0x160 (FUN_006a0230: 1/m below 1, 3/m above 3, m = the strongest light), multiplied into every
//    texel of the room;
//  - the ambient weight ramp (1 - k) + 2k (y - base)/3 (FUN_006ab210), base = the story's lowest floor, in the sampler
//    room+0x640 that the wall pass and the floor, ceiling and object passes all use.
// So the wall of an atrium changed at the floor line: another ambient colour (Light Probe: c4 0.041 above, 0.058 below),
// and a weight ramp restarting at (1 - k). For the rooms joined by removed floors into a real atrium (ReadStacked: 16
// shared quadrants at least and 30% of the smaller room's floor, so a stairwell does not join two rooms), right after
// state 0: every member gets the same ambient colour (their colours brought to the group's normalisation, averaged by
// floor quadrants) and the smallest normalisation (what the game's rule gives the group's strongest light), and the WALL
// pass (0x6A3D4C, FUN_006a3a30) runs with the ramp of the group's lowest story, so the atrium walls read as one tall
// wall while the floors, ceilings and objects keep their own ramp (review 2026-09-29). A member whose values differ from
// the group's is sent to solve again from the next room update; its own state 0 then reaches the same values.
struct RoomAmbient {
    float c4[4];
    float norm;
    float base;
};
struct Applied { // what a room was last solved with (its own merge)
    float c4[4];
    float norm, wallBase;
};
std::mutex g_ambMx;
std::unordered_map<DepKey, RoomAmbient, DepHash> g_ambOrig;  // (lot, story, room) -> what the game computed last
std::unordered_map<DepKey, Applied, DepHash> g_ambApplied;   // (lot, story, room) -> what the merge gave it last
std::unordered_map<DepKey, DWORD, DepHash> g_ambQueuedAt;    // members sent to solve again, when
std::unordered_map<uintptr_t, uintptr_t> g_mgrTracker;       // story manager -> its lot (filled by the gathers)
std::unordered_map<DepKey, Applied, DepHash> g_ambToQueue; // latest group target; retained until the room's merge agrees
struct AmbientMember { DepKey key; int area; };
std::unordered_map<DepKey, std::vector<AmbientMember>, DepHash> g_ambGroups;
std::unordered_set<DepKey, DepHash> g_ambDirty;
std::unordered_map<DepKey, DWORD, DepHash> g_ambGroupAt; // stable colour group, first staging tick
std::atomic<long> g_groupCommits{0}, g_groupFallbacks{0};
std::atomic<bool> g_groupRigsPending{false};
DWORD g_groupBudgetAt = 0; unsigned g_groupBudget = 16; // under g_ambMx, shared by every lot
struct WallBase {
    int id;
    uintptr_t mgr;
    float base;
};
std::unordered_map<uintptr_t, WallBase> g_wallBase; // room -> the ramp base of its wall pass (light tree thread)
bool g_ambReady = false;
std::atomic<long> g_ambMerges{0};

void NoteLotManagers(uintptr_t tracker) {
    std::lock_guard<std::mutex> lk(g_ambMx);
    if (g_mgrTracker.size() > 4096) g_mgrTracker.clear();
    for (int s = -4; s <= 7; s++)
        if (const uintptr_t m = StoryManager(tracker, s)) g_mgrTracker[m] = tracker;
}
uintptr_t MgrTracker(uintptr_t mgr) {
    std::lock_guard<std::mutex> lk(g_ambMx);
    const auto it = g_mgrTracker.find(mgr);
    return it == g_mgrTracker.end() ? 0 : it->second;
}

// The rooms stacked on a room through removed floors (the story above over its quadrants, the story below under them)
// with the number of shared quadrants, and the room's own floor quadrants. False on a fault.
struct Stacked {
    int level, id, shared;
};
bool ReadStacked(uintptr_t tracker, int S, int id, uintptr_t levelS, uintptr_t levelU, Stacked* out, int max, int& n, int& area) {
    n = 0, area = 0;
    __try {
        const uintptr_t mgrS = StoryManager(tracker, S);
        const uintptr_t mgrU = S < 7 ? StoryManager(tracker, S + 1) : 0, mgrD = S > 0 ? StoryManager(tracker, S - 1) : 0;
        const int w = std::min(*reinterpret_cast<const int*>(mgrS + 0x264), 1024), h = std::min(*reinterpret_cast<const int*>(mgrS + 0x268), 1024);
        const auto add = [&](int level, int other) {
            if (other <= 0) return;
            for (int k = 0; k < n; k++)
                if (out[k].level == level && out[k].id == other) {
                    out[k].shared++;
                    return;
                }
            if (n < max) out[n++] = Stacked{level, other, 1};
        };
        for (int iz = 0; iz < h; iz++)
            for (int ix = 0; ix < w; ix++) {
                const uintptr_t tile = LightTile(mgrS, ix, iz);
                if (!tile) continue;
                for (int q = 0; q < 4; q++) {
                    if (TileRoom(tile, q) != id) continue;
                    area++;
                    if (mgrU && levelU && FloorAt(levelU, ix, iz, q) == 0)
                        if (const uintptr_t up = LightTile(mgrU, ix, iz)) add(S + 1, TileRoom(up, q));
                    if (mgrD && levelS && FloorAt(levelS, ix, iz, q) == 0)
                        if (const uintptr_t down = LightTile(mgrD, ix, iz)) add(S - 1, TileRoom(down, q));
                }
            }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}

bool ReadAmbient(const BYTE* room, RoomAmbient& a) {
    __try {
        std::memcpy(a.c4, room + 0x110, sizeof a.c4);
        a.norm = *reinterpret_cast<const float*>(room + 0x160);
        a.base = *reinterpret_cast<const float*>(room + 0x640);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The group's colour into a room (+0x110 and the copy in its sampler, +0x650), and when `norm` > 0 its normalisation
// (it takes effect in the texel passes that follow)
void WriteAmbient(BYTE* room, const float* c4, float norm) {
    __try {
        std::memcpy(room + 0x110, c4, 16);
        std::memcpy(room + 0x650, c4, 16);
        if (norm > 0.0f) *reinterpret_cast<float*>(room + 0x160) = norm;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
    }
}

bool Differs(const Applied& a, const float* c4, float norm, float wallBase) {
    for (int k = 0; k < 4; k++)
        if (std::fabs(a.c4[k] - c4[k]) > 2e-3f) return true;
    return std::fabs(a.norm - norm) > 1e-4f || std::fabs(a.wallBase - wallBase) > 1e-3f;
}

// ---- One round for an atrium after a lamp edit (06/10) ----
// User (recordings 11:01 and 11:17: "all the lights", then one lamp of the atrium): the light's story changed first and
// the atrium's other stories up to 4 s later. Each member's merge used the other members' values from their LAST solve,
// so the first member solved after the edit took a target made of its new values and the others' old ones, the next
// member another target, and every change of the normalisation (AmbientMapsCompatible is bit-exact) sent every member to
// solve again: two or three rounds of the atrium's biggest rooms, one story after the other. The ambient step also
// depends on the solve class: the light threshold room+0x63C (FUN_006a8e50, set in state 0 at 0x6A194F: 2 x 3/255 at
// classes 0 and 1, 3/255 at class 2; LightPointWithAllLights drops a light whose r+g+b is under it), so a member's quick
// pass (class 0) and its refinement gave the group different targets as well.
// Now every member's ambient is taken at the merge with the game's own step (FUN_006a0f50: the room's lights sampled over
// its floor tiles and 4 m above, and its normalisation), at the threshold of the highest class seen whatever class the
// room is solved at: the solving room right after its state 0, and every other member whose light list is gathered
// (states 2 to 5; state 1 may still list a lamp being removed), the room's fields put back after (+0x110..+0x14F,
// +0x160, +0x63C). A member not gathered yet keeps its last value, as before. The group target is then final at the
// first member's solve, and each member is solved once per pass, with it.
struct CanonValue {
    RoomAmbient a;
    uint32_t serial; // GatherSerial of the room when taken
    float thr;       // at that threshold
};
std::unordered_map<DepKey, CanonValue, DepHash> g_ambCanon; // under g_ambMx: each member's value taken this way, last
float g_thrByClass[4] = {-1.0f, -1.0f, -1.0f, -1.0f};       // light tree thread: room+0x63C after state 0, by class
std::atomic<long> g_canonTaken{0}, g_canonKept{0}, g_canonOld{0}, g_canonReused{0};
std::atomic<long long> g_canonTicks{0}; // QueryPerformanceCounter ticks spent taking them
double CanonMs() {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f.QuadPart ? static_cast<double>(g_canonTicks.load(std::memory_order_relaxed)) * 1000.0 / static_cast<double>(f.QuadPart) : 0.0;
}
void NoteClassThreshold(const BYTE* room) {
    __try {
        const int cls = *reinterpret_cast<const int*>(room + 0xF4);
        const float thr = *reinterpret_cast<const float*>(room + 0x63C);
        if (cls >= 0 && cls < 4 && std::isfinite(thr) && thr > 0.0f) g_thrByClass[cls] = thr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
// The top class's threshold from the game's own table (Steam 1.67.2: FUN_006a8e50 = 0xFF36F4[idx] x [0xFF3700], the code
// checked at install), so it is known from the first solve: with only the solves seen, the rooms solved at classes 0-1
// while a lot loads set a first value, and the first class-2 solve changed it, which changed every atrium's target and
// sent its rooms to solve once more (06/10, user: "when entering the lot it takes long to correct")
float g_thrTable = -1.0f;
void ReadThresholdTable() {
    const BYTE code[] = {0x8B, 0x41, 0x08, 0xD9, 0x04, 0x85, 0xF4, 0x36, 0xFF, 0x00, 0xD8, 0x0D, 0x00, 0x37, 0xFF, 0x00, 0xC3};
    BYTE got[sizeof code] = {};
    float factor[3] = {}, scale = 0.0f;
    if (!GameAddr::IsFixed() || !MemPatch::ReadBytes(0x006A8E50, got, sizeof got) || std::memcmp(got, code, sizeof code) != 0) return;
    if (!MemPatch::ReadBytes(0x00FF36F4, factor, sizeof factor) || !MemPatch::ReadBytes(0x00FF3700, &scale, sizeof scale)) return;
    const float thr = factor[2] * scale;
    if (std::isfinite(thr) && thr > 0.0f) g_thrTable = thr;
}
float TopThreshold() {
    if (g_thrTable > 0.0f) return g_thrTable;
    for (int cls = 3; cls >= 0; cls--)
        if (g_thrByClass[cls] > 0.0f) return g_thrByClass[cls];
    return -1.0f;
}
// The game's ambient step on a room at the threshold thr (<= 0: the room's own), its fields put back; false when the room
// has no light (the game's constant colour: its own solve gives it), is outdoor or could not be read
bool ProbeAmbient(BYTE* room, float thr, RoomAmbient& a) {
    alignas(16) BYTE saved[0x40];
    float norm = 0.0f, ownThr = 0.0f;
    bool taken = false, ok = false;
    UnlitRooms::SetAmbientProbe(true);
    __try {
        const uintptr_t mgr = *reinterpret_cast<const uintptr_t*>(room);
        const uintptr_t b = *reinterpret_cast<const uintptr_t*>(room + 0xC8), e = *reinterpret_cast<const uintptr_t*>(room + 0xCC);
        if (mgr && !room[0x18] && b && e > b && e - b <= 4 * 4096) {
            std::memcpy(saved, room + 0x110, sizeof saved);
            norm = *reinterpret_cast<const float*>(room + 0x160);
            ownThr = *reinterpret_cast<const float*>(room + 0x63C);
            taken = true;
            if (thr > 0.0f) *reinterpret_cast<float*>(room + 0x63C) = thr;
            reinterpret_cast<void(__fastcall*)(void*)>(kRoomAmbientFn)(room);
            std::memcpy(a.c4, room + 0x110, sizeof a.c4);
            a.norm = *reinterpret_cast<const float*>(room + 0x160);
            a.base = *reinterpret_cast<const float*>(mgr + 0x98); // what state 0 puts at +0x640 (FUN_006ab110)
            ok = true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        ok = false;
    }
    UnlitRooms::SetAmbientProbe(false);
    if (!taken) return false;
    __try {
        std::memcpy(room + 0x110, saved, sizeof saved);
        *reinterpret_cast<float*>(room + 0x160) = norm;
        *reinterpret_cast<float*>(room + 0x63C) = ownThr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (!ok) return false;
    for (float v : a.c4)
        if (!std::isfinite(v)) return false;
    return std::isfinite(a.norm) && a.norm > 0.0f && std::isfinite(a.base);
}
// A member whose light list is gathered: waiting for its solve, being solved or done (states 2 to 5)
bool AmbientGathered(const BYTE* room) {
    __try {
        const int state = *reinterpret_cast<const int*>(room + 0xF0);
        return state >= 2 && state <= 5;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
BYTE* SafeRoomById(uintptr_t tracker, int level, int id); // below
float RoomThreshold(const BYTE* room) {
    __try {
        return *reinterpret_cast<const float*>(room + 0x63C);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1.0f;
    }
}
uint32_t GatherSerial(const BYTE* room); // below: which gather the room's light list comes from (0 = unknown)
// A member's value for the group: taken now (the solving room, or a gathered member whose list was gathered again since it
// was last taken; the light of a lamp it holds cannot change without that), else the last one taken, else what its own
// last solve computed. self: the room whose state 0 just ran (own = that result). False: nothing known.
bool MemberAmbient(BYTE* room, bool self, const RoomAmbient& own, const DepKey& key, RoomAmbient& a) {
    const float top = kRoomAmbientFn ? TopThreshold() : -1.0f;
    if (room && (self || AmbientGathered(room)) && top > 0.0f) {
        const uint32_t serial = GatherSerial(room);
        if (!self && serial) {
            std::lock_guard<std::mutex> lk(g_ambMx);
            if (const auto it = g_ambCanon.find(key); it != g_ambCanon.end() && it->second.serial == serial && it->second.thr == top) {
                a = it->second.a;
                g_canonReused.fetch_add(1, std::memory_order_relaxed);
                return true;
            }
        }
        // the solving room at that threshold already: its own state 0 is the value
        const bool same = self && std::fabs(RoomThreshold(room) - top) <= 1e-7f;
        LARGE_INTEGER q0{}, q1{};
        QueryPerformanceCounter(&q0);
        const bool taken = same || ProbeAmbient(room, top, a);
        if (same) a = own;
        else {
            QueryPerformanceCounter(&q1);
            g_canonTicks.fetch_add(q1.QuadPart - q0.QuadPart, std::memory_order_relaxed);
        }
        if (taken) {
            (same ? g_canonKept : g_canonTaken).fetch_add(1, std::memory_order_relaxed);
            std::lock_guard<std::mutex> lk(g_ambMx);
            if (g_ambCanon.size() > 8192) g_ambCanon.clear();
            g_ambCanon[key] = CanonValue{a, serial, top};
            return true;
        }
    }
    if (self) { // its fresh result beats any older value
        a = own;
        return true;
    }
    std::lock_guard<std::mutex> lk(g_ambMx);
    if (const auto it = g_ambCanon.find(key); it != g_ambCanon.end()) {
        a = it->second.a;
        return true;
    }
    if (const auto it = g_ambOrig.find(key); it != g_ambOrig.end()) {
        a = it->second;
        g_canonOld.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    return false;
}

void MergeStackedAmbient(BYTE* room) {
    if (room[0x18]) return; // outdoor / roofless: the game computes no ambient for it
    const int id = *reinterpret_cast<const int*>(room + 0xC);
    const uintptr_t mgr = *reinterpret_cast<const uintptr_t*>(room);
    if (id <= 0 || !mgr) return;
    const int S = *reinterpret_cast<const int*>(mgr + 0x88);
    const uintptr_t tracker = MgrTracker(mgr);
    if (S < 0 || S > 7 || !tracker || StoryManager(tracker, S) != mgr) return;
    const uintptr_t roomKey = reinterpret_cast<uintptr_t>(room);
    g_wallBase.erase(roomKey);
    RoomAmbient own{};
    if (!ReadAmbient(room, own)) return;
    {
        std::lock_guard<std::mutex> lk(g_ambMx);
        if (g_ambOrig.size() > 8192) g_ambOrig.clear();
        g_ambOrig[DepKey{tracker, S, id}] = own; // what the game computed, before any merge
    }
    uintptr_t levels[8] = {};
    for (int s = 0; s <= 7; s++)
        if (const uintptr_t m = StoryManager(tracker, s)) levels[s] = LevelFor(m);
    // the rooms reachable through removed floors (breadth first, 12 at most), with the quadrants they share
    struct Node {
        int level, id, area;
    };
    struct Edge {
        int a, b, shared; // node indices
    };
    std::vector<Node> nodes{{S, id, 0}};
    std::vector<Edge> edges;
    Stacked st[32];
    for (size_t i = 0; i < nodes.size(); i++) {
        int n = 0, area = 0;
        if (!ReadStacked(tracker, nodes[i].level, nodes[i].id, levels[nodes[i].level], nodes[i].level < 7 ? levels[nodes[i].level + 1] : 0, st, 32, n, area)) return;
        nodes[i].area = area;
        for (int k = 0; k < n; k++) {
            if (st[k].shared < 16) continue;
            size_t j = 0;
            while (j < nodes.size() && !(nodes[j].level == st[k].level && nodes[j].id == st[k].id)) j++;
            if (j == nodes.size()) {
                if (nodes.size() >= 12) continue;
                nodes.push_back(Node{st[k].level, st[k].id, 0});
            }
            if (i < j) edges.push_back(Edge{static_cast<int>(i), static_cast<int>(j), st[k].shared});
        }
    }
    // a real atrium only: the opening is 30% of the smaller room's floor at least (a stairwell is much less)
    std::vector<int> group{0};
    for (size_t g = 0; g < group.size(); g++)
        for (const Edge& e : edges) {
            const int other = e.a == group[g] ? e.b : e.b == group[g] ? e.a : -1;
            if (other < 0 || std::find(group.begin(), group.end(), other) != group.end()) continue;
            const int smaller = std::max(1, std::min(nodes[e.a].area, nodes[e.b].area));
            if (e.shared * 10 >= smaller * 3) group.push_back(other);
        }
    if (group.size() < 2) {
        std::lock_guard<std::mutex> lk(g_ambMx);
        g_ambToQueue.erase(DepKey{tracker, S, id}); // no longer connected: its own solve is authoritative
        g_ambGroups.erase(DepKey{tracker, S, id});
        return;
    }
    std::sort(group.begin(), group.end(), [&](int x, int y) { return nodes[x].level != nodes[y].level ? nodes[x].level < nodes[y].level : nodes[x].id < nodes[y].id; });
    // the members the game has solved already
    struct Known {
        Node node;
        RoomAmbient a;
    };
    std::vector<Known> known;
    for (int g : group) { // every member as it is now (see "One round for an atrium"); node 0 is this room
        RoomAmbient a{};
        BYTE* member = g == 0 ? room : SafeRoomById(tracker, nodes[g].level, nodes[g].id);
        if (MemberAmbient(member, g == 0, own, DepKey{tracker, nodes[g].level, nodes[g].id}, a)) known.push_back(Known{nodes[g], a});
    }
    if (known.size() < 2) return; // the others merge when they are solved
    float norm = known[0].a.norm, wallBase = known[0].a.base;
    for (const Known& k : known) {
        norm = std::min(norm, k.a.norm);
        wallBase = std::min(wallBase, k.a.base);
    }
    float c4[4] = {}, weight = 0.0f;
    for (const Known& k : known) {
        // each colour already carries its room's normalisation: bring it to the group's
        const float wgt = static_cast<float>(std::max(k.node.area, 1));
        RoomAmbientPolicy::AccumulateAmbient(c4, k.a.c4, k.a.norm, norm, wgt);
        weight += wgt;
    }
    for (float& v : c4) v /= weight;
    std::lock_guard<std::mutex> lk(g_ambMx);
    std::vector<AmbientMember> members;
    if (g_ambGroups.size() > 8192) g_ambGroups.clear();
    for (int g : group) members.push_back({{tracker, nodes[g].level, nodes[g].id}, nodes[g].area});
    for (const auto& member : members) g_ambGroups[member.key] = members;
    const DepKey selfKey{tracker, S, id};
    const auto previous = g_ambApplied.find(selfKey);
    bool stableMaps = previous != g_ambApplied.end();
    bool changed = false;
    for (const auto& member : members) {
        const auto old = g_ambApplied.find(member.key);
        if (old == g_ambApplied.end()
            || !RoomAmbientPolicy::AmbientMapsCompatible(old->second.norm, norm, old->second.wallBase, wallBase)
            || (previous != g_ambApplied.end() && Differs(previous->second, old->second.c4, old->second.norm, old->second.wallBase))) {
            stableMaps = false;
            break;
        }
        changed |= Differs(old->second, c4, norm, wallBase);
    }
    // Never gate the native solve on unavailable sources. Only stage a colour change
    // when a complete, compatible previous presentation already exists for the group.
    if (stableMaps && changed) {
        WriteAmbient(room, previous->second.c4, norm);
        g_wallBase[roomKey] = WallBase{id, mgr, wallBase};
        for (const auto& member : members) {
            Applied& wanted = g_ambToQueue[member.key];
            std::memcpy(wanted.c4, c4, sizeof c4);
            wanted.norm = norm;
            wanted.wallBase = wallBase;
        }
        g_ambGroupAt.try_emplace(members.front().key, GetTickCount()); // newer gathers do not postpone the bound
    } else {
        g_ambGroupAt.erase(members.front().key);
        WriteAmbient(room, c4, norm);
        g_wallBase[roomKey] = WallBase{id, mgr, wallBase};
        Applied& self = g_ambApplied[selfKey];
        std::memcpy(self.c4, c4, sizeof c4);
        self.norm = norm; self.wallBase = wallBase;
        g_ambToQueue.erase(selfKey);
        for (const auto& member : members) {
            if (member.key == selfKey) continue;
            const auto applied = g_ambApplied.find(member.key);
            if (applied != g_ambApplied.end() && !Differs(applied->second, c4, norm, wallBase)) {
                g_ambToQueue.erase(member.key);
                continue;
            }
            Applied& wanted = g_ambToQueue[member.key];
            std::memcpy(wanted.c4, c4, sizeof c4);
            wanted.norm = norm; wanted.wallBase = wallBase;
        }
    }
    g_ambMerges.fetch_add(1, std::memory_order_relaxed);
    if (g_ambGroupAt.size() > 8192) g_ambGroupAt.clear();
    if (g_ambQueuedAt.size() > 4096) g_ambQueuedAt.clear();
    if (g_ambApplied.size() > 8192) g_ambApplied.clear();
}

// The quick pass lights a room the way its refinement will (06/10). Class 0 is not only coarser: the game's per-class
// switches turn every test off there. Steam 1.67.2, from the code that reads them (index idx = classEntry+8, classEntry =
// [room+4] + class * 64):
//  - FUN_006a8cb0: wall mode room+0x639 (wall height test / soft shadows in FUN_0069fc40), 0xFF36A4[idx] = 0 0 1;
//  - FUN_006a8ce0(pass): test the room's 2D walls, 0xFF36C4[3 pass + idx]; FUN_006a8d00(pass): test 3D occluders (objects),
//    0xFF36D0[3 pass + idx]. Passes 0 floor, 1 ceiling, 2 objects are copied by state 0 (0x6A1AA1..0x6A1B8C) to
//    +0x1B0/+0x1B1, +0x2A0/+0x2A1 and +0x390/+0x391; pass 3, the walls, is read by the wall pass FUN_006a3a30 at every
//    step (0x6A3A76). Class 0: 0 everywhere (no wall and no object stops a light); class 2: walls on every pass;
//  - the light threshold room+0x63C (FUN_006a8e50, see "One round for an atrium"): 2 x 3/255 at classes 0-1, 3/255 at 2.
// So with every lamp switched off, the window lights and the lamps of the room next door reached the walls straight
// through them in the quick pass (Wall seams of 11:50: room 19 five times brighter at class 0 than at class 2, same
// points), and the refinement then took that light away (user, video of 12:00: "some corners look right, then wrong
// again"). Right after state 0 a quick-pass room takes the threshold and the switches of the class it will be refined to
// (the wall pass's ones around each wall step, WallPassHook), and its ambient is taken again with that threshold
// (FUN_006a0f50; its copy in the sampler +0x650 as state 0 makes it with FUN_006ab110). Class 0 has few texels, so the
// tests cost little.
constexpr uintptr_t kClassWallMode = 0x00FF36A4, kClassWalls2D = 0x00FF36C4, kClassObjects3D = 0x00FF36D0;
bool g_classTables = false; // the code above checked (and the pass tables made writable) at install
bool CheckClassTables() {
    if (!GameAddr::IsFixed()) return false;
    const BYTE mode[] = {0x8B, 0x41, 0x08, 0x8A, 0x80, 0xA4, 0x36, 0xFF, 0x00, 0xC3};
    const BYTE walls[] = {0x8B, 0x44, 0x24, 0x04, 0x8B, 0x49, 0x08, 0x8D, 0x04, 0x40, 0x8A, 0x84, 0x08, 0xC4, 0x36, 0xFF, 0x00, 0xC2, 0x04, 0x00};
    BYTE objects[sizeof walls];
    std::memcpy(objects, walls, sizeof walls);
    objects[13] = 0xD0;
    BYTE got[sizeof walls] = {};
    if (!MemPatch::ReadBytes(0x006A8CB0, got, sizeof mode) || std::memcmp(got, mode, sizeof mode) != 0) return false;
    if (!MemPatch::ReadBytes(0x006A8CE0, got, sizeof walls) || std::memcmp(got, walls, sizeof walls) != 0) return false;
    if (!MemPatch::ReadBytes(0x006A8D00, got, sizeof walls) || std::memcmp(got, objects, sizeof walls) != 0) return false;
    DWORD old = 0;
    return VirtualProtect(reinterpret_cast<void*>(kClassWalls2D), 0x18, PAGE_READWRITE, &old) != 0;
}
int ClassIndex(const BYTE* room, int cls) {
    __try {
        const uintptr_t entries = *reinterpret_cast<const uintptr_t*>(room + 4);
        if (!entries || cls < 0 || cls > 3) return -1;
        const int idx = *reinterpret_cast<const int*>(entries + cls * 64 + 8);
        return idx >= 0 && idx < 3 ? idx : -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
std::atomic<long> g_quickThreshold{0}, g_quickTests{0};
void QuickLikeRefinement(BYTE* room) {
    const int target = RoomLightQueue::QuickPassTarget(room); // a room at a lower class by its own LOD keeps the game's
    if (target <= 0) return;
    const float thr = target < 4 && g_thrByClass[target] > 0.0f ? g_thrByClass[target] : TopThreshold();
    const int to = g_classTables ? ClassIndex(room, target) : -1;
    __try {
        if (to >= 0) {
            room[0x639] = *reinterpret_cast<const BYTE*>(kClassWallMode + to);
            if (!room[0x18]) // indoor: the passes state 0 set up
                for (int pass = 0; pass < 3; pass++) {
                    BYTE* f = room + (pass == 0 ? 0x1B0 : pass == 1 ? 0x2A0 : 0x390);
                    f[0] = *reinterpret_cast<const BYTE*>(kClassWalls2D + 3 * pass + to);
                    f[1] = *reinterpret_cast<const BYTE*>(kClassObjects3D + 3 * pass + to);
                }
            g_quickTests.fetch_add(1, std::memory_order_relaxed);
        }
        float& own = *reinterpret_cast<float*>(room + 0x63C);
        if (!(thr > 0.0f) || !(own > thr + 1e-7f)) return; // at that threshold already
        own = thr;
        g_quickThreshold.fetch_add(1, std::memory_order_relaxed);
        const uintptr_t b = *reinterpret_cast<const uintptr_t*>(room + 0xC8), e = *reinterpret_cast<const uintptr_t*>(room + 0xCC);
        if (room[0x18] || !kRoomAmbientFn || !b || e <= b) return; // no ambient step (outdoor), or the unlit constant
        reinterpret_cast<void(__fastcall*)(void*)>(kRoomAmbientFn)(room);
        std::memcpy(room + 0x650, room + 0x110, 16);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
    }
}

void MergeStackedAmbientSafe(BYTE* room) {
    __try {
        MergeStackedAmbient(room);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
    }
}

bool AmbientActive() {
    return g_ambReady && g_installed.load(std::memory_order_relaxed) && g_indoorReady && g_indoorOn.load(std::memory_order_relaxed);
}

// ---- F8 (development build): the solves of the rooms that share light through an opening, since the world loaded ----
// A room lit right after the lot loads can look different from the same room lit again after a lamp moves (user,
// 29/09): each solve of such a room notes its thread, LOD class, light list, ambient and the wall-pass counters.
struct SolveNote {
    DWORD tick;
    uintptr_t room, mgr;
    int id, level, cls, shown, state, lights, cross, cam, flag19;
    uint32_t lot; // the story manager's lot id (low half)
    char event; // 'S' ambient step done (state 0), 'W' wall pass done, 'Q' sent by Apex, 'H' held by Apex until its solve ends, 'I' / 'F' invalidated (plain / on a flag change) by the caller printed last
    bool gather, boosted, merged;
    float norm, c4[3], wallBase;
    long moved, ghostWalls, odd;
    uintptr_t caller; // 'I' / 'F': the return address of the invalidate (who restarted the room)
};
std::mutex g_journalMx;
constexpr size_t kJournal = 4096;
SolveNote g_journal[kJournal];
size_t g_journalCount = 0; // notes written since the world loaded (the ring keeps the last kJournal)
bool ReadSolveNote(const BYTE* room, SolveNote& n) {
    __try {
        n.mgr = *reinterpret_cast<const uintptr_t*>(room);
        n.id = *reinterpret_cast<const int*>(room + 0xC);
        n.level = n.mgr ? *reinterpret_cast<const int*>(n.mgr + 0x88) : -99;
        n.cam = n.mgr ? *reinterpret_cast<const int*>(n.mgr + 0x284) : -99;
        n.lot = n.mgr ? *reinterpret_cast<const uint32_t*>(n.mgr + 0x90) : 0;
        n.flag19 = *reinterpret_cast<const BYTE*>(room + 0x19);
        n.state = *reinterpret_cast<const int*>(room + 0xF0);
        n.cls = *reinterpret_cast<const int*>(room + 0xF4);
        n.shown = *reinterpret_cast<const int*>(room + 0x100);
        n.norm = *reinterpret_cast<const float*>(room + 0x160);
        for (int k = 0; k < 3; k++) n.c4[k] = *reinterpret_cast<const float*>(room + 0x110 + k * 4);
        n.wallBase = *reinterpret_cast<const float*>(room + 0x640);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

using RoomSolveStart_t = void(__thiscall*)(void* room);
void __fastcall RoomSolveStartHook(BYTE* room) {
    reinterpret_cast<RoomSolveStart_t>(kRoomSolveStart)(room);
    if (room) RoomLightQueue::NoteSolveStart(room); // lamp switches all at once wait for solves begun after the switch
    if (room && AmbientActive()) {
        if (ThreadId() != g_gatherThread.load(std::memory_order_relaxed)) g_otherThread.fetch_add(1, std::memory_order_relaxed); // e.g. the lot impostor's synchronous solve
        else {
            NoteClassThreshold(room);
            QuickLikeRefinement(room);
            MergeStackedAmbientSafe(room);
        }
    }
    if (Recorder::Verbose())
        if (room) NoteSolve(room, 'S');
}

// The wall pass of a room with its group's ramp base; the room's own base is back for the floor, ceiling and object passes
float SwapWallBase(BYTE* room, float base) {
    __try {
        const float own = *reinterpret_cast<const float*>(room + 0x640);
        *reinterpret_cast<float*>(room + 0x640) = base;
        return own;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return base;
    }
}
using WallPass_t = char(__thiscall*)(void* room, int arg, float budget);
// A quick-pass room's wall step reads the wall tests of the class it will be refined to (see QuickLikeRefinement): the
// pass-3 bytes of its class index take the target's for the call (the gather thread solves one room at a time)
struct WallTests {
    int idx = -1;
    BYTE walls = 0, objects = 0;
};
WallTests SwapWallTests(BYTE* room) {
    WallTests saved;
    if (!g_classTables) return saved;
    const int target = RoomLightQueue::QuickPassTarget(room);
    if (target <= 0) return saved;
    int cls = -1;
    __try {
        cls = *reinterpret_cast<const int*>(room + 0xF4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return saved;
    }
    const int cur = ClassIndex(room, cls), to = ClassIndex(room, target);
    if (cur < 0 || to < 0 || cur == to) return saved;
    BYTE* walls = reinterpret_cast<BYTE*>(kClassWalls2D + 9 + cur);
    BYTE* objects = reinterpret_cast<BYTE*>(kClassObjects3D + 9 + cur);
    saved = WallTests{cur, *walls, *objects};
    *walls = *reinterpret_cast<const BYTE*>(kClassWalls2D + 9 + to);
    *objects = *reinterpret_cast<const BYTE*>(kClassObjects3D + 9 + to);
    return saved;
}
void RestoreWallTests(const WallTests& saved) {
    if (saved.idx < 0) return;
    *reinterpret_cast<BYTE*>(kClassWalls2D + 9 + saved.idx) = saved.walls;
    *reinterpret_cast<BYTE*>(kClassObjects3D + 9 + saved.idx) = saved.objects;
}
char WallPassCall(BYTE* room, int arg, float budget) {
    const WallTests tests = SwapWallTests(room);
    char done = 0;
    __try {
        done = reinterpret_cast<WallPass_t>(kWallPass)(room, arg, budget);
    } __finally {
        RestoreWallTests(tests);
    }
    return done;
}
char __fastcall WallPassHook(BYTE* room, void*, int arg, float budget) {
    float saved = 0.0f;
    bool swapped = false;
    if (room && AmbientActive() && ThreadId() == g_gatherThread.load(std::memory_order_relaxed)) {
        const auto it = g_wallBase.find(reinterpret_cast<uintptr_t>(room));
        if (it != g_wallBase.end() && it->second.id == *reinterpret_cast<const int*>(room + 0xC) &&
            it->second.mgr == *reinterpret_cast<const uintptr_t*>(room)) {
            saved = SwapWallBase(room, it->second.base);
            swapped = true;
        }
    }
    BYTE* const outer = t_wallRoom;
    t_wallRoom = room; // the wall notes of the F7 capture name the room
    const char done = WallPassCall(room, arg, budget);
    t_wallRoom = outer;
    if (swapped) SwapWallBase(room, saved);
    return done;
}

void ForgetAmbientLot(uintptr_t tracker) {
    std::lock_guard<std::mutex> lock(g_ambMx);
    const auto belongs = [tracker](const auto& entry) { return entry.first.tracker == tracker; };
    std::erase_if(g_ambOrig, belongs);
    std::erase_if(g_ambCanon, belongs);
    std::erase_if(g_ambApplied, belongs);
    std::erase_if(g_ambQueuedAt, belongs);
    std::erase_if(g_ambToQueue, belongs);
    std::erase_if(g_ambGroups, belongs);
    std::erase_if(g_ambGroupAt, belongs);
    std::erase_if(g_ambDirty, [tracker](const DepKey& key) { return key.tracker == tracker; });
    std::erase_if(g_mgrTracker, [tracker](const auto& entry) { return entry.second == tracker; });
}
void ClearAmbient() {
    std::lock_guard<std::mutex> lk(g_ambMx);
    g_groupBudgetAt = 0; g_groupBudget = 16;
    g_ambOrig.clear();
    g_ambCanon.clear();
    g_ambApplied.clear();
    g_ambQueuedAt.clear();
    g_mgrTracker.clear();
    g_ambToQueue.clear();
    g_ambGroups.clear();
    g_ambDirty.clear();
    g_ambGroupAt.clear();
}

// ---- 5. Wall light lined up with the wall (every wall; option "paredesSemEmendaEntreAndares") ----
// Wall object (room+0xD8 list), per LOD class c: +c*0x10+0x28 columns, +0x2C rows (N), +0x30 rows per piece, +0x34
// columns per piece; atlas block at +c*0x20+0x58 = {x0, y0, x1, y1, .., +0x1C atlas}; +0xF0 run vector, +0x110 origin.
using WallSamples_t = void(__thiscall*)(void* wall, int piece, int cls, void* batch);
using WallBlur_t = void(__fastcall*)(void* room);
using WallSolve_t = void(__thiscall*)(void* room, void* batch, void* atlas, char* flags, void* sampler, char ambient);
std::atomic<bool> g_alignReady{false}, g_alignOn{true}, g_alignRequeue{false};
std::atomic<long> g_alignRows{0};   // wall samples moved to the height their row is drawn at
std::atomic<long> g_alignOdd{0};    // wall pieces left as the game has them (not the layout above)
std::atomic<long> g_alignEdges{0};  // wall edge rows kept out of the vertical blur
std::atomic<long> g_ghostWalls{0};  // walls blurred across their edges with the rows lit beyond them
std::atomic<long> g_ghostPoints{0}; // points lit beyond the edges

struct WallBlock {
    int x0, y0, x1, y1;
};

// After FUN_006ac070 filled the batch with one piece of a wall: row k (texel y0 + N-1-k) is lit at k*3/N, but the block
// is drawn from the centre of its bottom row at the wall's foot to the centre of its top row at the wall's top
// (FUN_006ac200), so row k shows at k*3/(N-1). Each sample goes to the height its row is drawn at, checked first against
// the game's own formula (FUN_006abdd0), so a wall laid out any other way is left alone.
bool AlignWallSamples(uintptr_t wall, int cls, uintptr_t batch, WallBlock& block, int& rowsOut) {
    __try {
        if (cls < 0 || cls > 2) return false;
        const int rows = *reinterpret_cast<const int*>(wall + cls * 0x10 + 0x2C), cols = *reinterpret_cast<const int*>(wall + cls * 0x10 + 0x28);
        const WallBlock& r = *reinterpret_cast<const WallBlock*>(wall + cls * 0x20 + 0x58);
        if (rows < 2 || rows > 4096 || cols < 1 || cols > 65536 || r.y1 - r.y0 + 1 != rows || r.x0 < 0 || r.y0 < 0 || r.x1 < r.x0 || r.x1 - r.x0 >= 4096) return false;
        const uintptr_t b = *reinterpret_cast<const uintptr_t*>(batch), e = *reinterpret_cast<const uintptr_t*>(batch + 4);
        if (!b || e < b || (e - b) % 0x30 || (e - b) / 0x30 > 1u << 20) return false;
        const float oy = *reinterpret_cast<const float*>(wall + 0x114), runY = *reinterpret_cast<const float*>(wall + 0xF4);
        const float lit = 3.0f / static_cast<float>(rows), drawn = 3.0f / static_cast<float>(rows - 1);
        for (uintptr_t p = b; p < e; p += 0x30) { // the whole piece is checked before anything moves
            const int k = r.y0 + rows - 1 - *reinterpret_cast<const uint16_t*>(p + 0x22);
            const int i = *reinterpret_cast<const uint16_t*>(p + 0x20) - r.x0;
            const float y = *reinterpret_cast<const float*>(p + 4);
            if (k < 0 || k >= rows || i < 0 || i > r.x1 - r.x0 || std::fabs(y - (oy + k * lit + (i + 0.5f) * runY / cols)) > 1e-3f) return false;
        }
        long moved = 0;
        for (uintptr_t p = b; p < e; p += 0x30) {
            const int k = r.y0 + rows - 1 - *reinterpret_cast<const uint16_t*>(p + 0x22);
            *reinterpret_cast<float*>(p + 4) += k * (drawn - lit);
            moved += k > 0;
        }
        g_alignRows.fetch_add(moved, std::memory_order_relaxed);
        block = r;
        rowsOut = rows;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The class-2 blur (FUN_0069f650: [0x01158B1C] passes of [1 2 1] along the rows, then down the columns, each clamped to
// the wall's block) stops at the top and bottom of every wall. Where two walls meet at a floor line (the rooms of an
// atrium) each side then smooths towards its own inside and the line shows as a crease wherever the light
// changes fast with height (a sconce under the line). For those walls the rows just beyond the edges are lit too: the
// game's own solve (FUN_006a31d0) on copies of the edge row moved 1 and 2 rows up (top) or down (bottom), written to a
// private buffer. The blur then runs over the wall with those rows around it, as if the wall went on, so the wall below
// and the wall above end on the same smoothed light. Other walls keep their top and bottom rows out of the vertical blur
// (blurred along the wall only), so they end on the light at their edge.
struct GhostRows {
    int x0 = 0, n = 0;          // the block's first column and width
    int filled[2] = {0, 0};     // columns lit: [0] above the top row, [1] below the bottom row
    std::vector<uint32_t> px;   // 4 rows of n: 2 above, 1 above, 1 below, 2 below (packed like the atlas)
};
std::mutex g_ghostMx;
std::unordered_map<uintptr_t, GhostRows> g_ghosts; // wall -> the rows beyond its class-2 block (this wall pass)
struct PieceNote {
    uintptr_t wall = 0;
    int rows = 0;
    WallBlock block{};
};
PieceNote g_piece;            // the piece WallSamplesHook just lined up, lit next by the wall pass (light tree thread)

// ---- Wall notes for the F7 light capture (06/10, user: "a capture that measures it"; an outside sconce lit its wall well
// below the lamp). Every wall piece the solve lays out (WallSamplesHook, any thread): its atlas block, rows, the base and
// the heights its bottom and top rows are lit at, its line on the ground and its normal, and the room being solved.
// WallNotesOnRay finds the piece a screen ray meets and reports it with the room's lamps. Read-only, bounded.
struct WallNote {
    uintptr_t wall = 0, room = 0;
    int cls = 0, piece = 0, rows = 0, cols = 0, colLo = 0, colHi = 0, roomId = -1, story = -99;
    bool outdoor = false; // room +0x18: room 0 or a roofless room
    uintptr_t mgr = 0;    // the room's story manager
    float dx = 0, dz = 0; // the wall's run (wall +0xF0, world xz)
    WallBlock block{};
    float oy = 0, litLo = 0, litHi = 0;
    float x0 = 0, z0 = 0, x1 = 0, z1 = 0; // the bottom row's first and last sample (world xz)
    float n[3] = {};
    DWORD tick = 0;
};
std::mutex g_wallNoteMx;
std::unordered_map<uint64_t, WallNote> g_wallNotes; // (wall, class, piece) -> note
bool ReadWallPiece(uintptr_t wall, int cls, uintptr_t batch, WallNote& w) { // POD only (SEH)
    __try {
        if (cls < 0 || cls > 2) return false;
        w.rows = *reinterpret_cast<const int*>(wall + cls * 0x10 + 0x2C);
        w.cols = *reinterpret_cast<const int*>(wall + cls * 0x10 + 0x28);
        w.block = *reinterpret_cast<const WallBlock*>(wall + cls * 0x20 + 0x58);
        w.oy = *reinterpret_cast<const float*>(wall + 0x114);
        w.dx = *reinterpret_cast<const float*>(wall + 0xF0);
        w.dz = *reinterpret_cast<const float*>(wall + 0xF8);
        const uintptr_t b = *reinterpret_cast<const uintptr_t*>(batch), e = *reinterpret_cast<const uintptr_t*>(batch + 4);
        if (w.rows < 2 || w.rows > 4096 || !b || e <= b || (e - b) % 0x30 || (e - b) / 0x30 > 1u << 20) return false;
        int lo = 1 << 30, hi = -1;
        float lit0 = 1e9f, litN = -1e9f;
        for (uintptr_t p = b; p < e; p += 0x30) {
            const int col = *reinterpret_cast<const uint16_t*>(p + 0x20), k = w.block.y0 + w.rows - 1 - *reinterpret_cast<const uint16_t*>(p + 0x22);
            const float* s = reinterpret_cast<const float*>(p);
            if (k == 0) {
                lit0 = std::min(lit0, s[1]);
                if (col < lo) { lo = col; w.x0 = s[0]; w.z0 = s[2]; w.n[0] = s[4]; w.n[1] = s[5]; w.n[2] = s[6]; }
                if (col > hi) { hi = col; w.x1 = s[0]; w.z1 = s[2]; }
            }
            if (k == w.rows - 1) litN = std::max(litN, s[1]);
        }
        if (hi < 0) return false;
        w.colLo = lo;
        w.colHi = hi;
        w.litLo = lit0;
        w.litHi = litN;
        if (BYTE* room = t_wallRoom) {
            w.room = reinterpret_cast<uintptr_t>(room);
            w.roomId = *reinterpret_cast<const int*>(room + 0xC);
            w.outdoor = room[0x18] != 0;
            w.mgr = *reinterpret_cast<const uintptr_t*>(room);
            const uintptr_t mgr = *reinterpret_cast<const uintptr_t*>(room);
            if (mgr) w.story = *reinterpret_cast<const int*>(mgr + 0x88);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void NoteWallPiece(uintptr_t wall, int piece, int cls, uintptr_t batch) {
    WallNote w;
    if (!ReadWallPiece(wall, cls, batch, w)) return;
    w.wall = wall;
    w.cls = cls;
    w.piece = piece;
    w.tick = GetTickCount();
    std::lock_guard<std::mutex> lk(g_wallNoteMx);
    if (g_wallNotes.size() > 20000) g_wallNotes.clear();
    g_wallNotes[(static_cast<uint64_t>(wall) << 8) ^ (static_cast<uint64_t>(cls) << 6) ^ static_cast<uint64_t>(piece & 63)] = w;
}
bool g_ghostSolve = false;    // the game's solve is lighting our rows beyond the edges (light tree thread)
constexpr int kMaxGhosts = 512;
alignas(16) BYTE g_ghostSamples[kMaxGhosts * 0x30];

// The rooms of an atrium group: their walls meet the other story's walls in view, and both are lit at LOD class 2 (the
// camera's story, and the story below it raised by 4.). Room 0 is left out: the outside walls of the stories under the
// camera are lit at class 0 (no blur), so the wall above must end on the exact light at the line, not on a blurred one.
bool GhostRoom(const BYTE* room) {
    const auto it = g_wallBase.find(reinterpret_cast<uintptr_t>(room));
    return it != g_wallBase.end() && it->second.id == *reinterpret_cast<const int*>(room + 0xC) && it->second.mgr == *reinterpret_cast<const uintptr_t*>(room);
}

// The game blurs this room's walls in mode 0 (the blur above): its passes, else 0
uint32_t BlurPassesFor(const BYTE* room) {
    __try {
        const uint32_t passes = *reinterpret_cast<const uint32_t*>(kWallBlurPasses);
        return *reinterpret_cast<const int*>(room + 0xF4) == 2 && passes < 16 && *reinterpret_cast<const BYTE*>(kWallBlurMode) == 0 ? passes : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ---- Wall light laid out where the wall is drawn (06/10, wall_heights.h). The game lays a wall's light rows out from wall
// +0x114 and draws the wall mesh where its vertices say; on a house on a foundation the outside walls are drawn about 2 m
// lower (the F7 whole-scene survey: -2.02 m for most outside pieces of stories 1..4, 0 for indoor walls, and a facade at its
// base), so the light showed 2 m off. Each piece's samples move by the measured drawn foot - its base; a piece whose wall
// was not drawn yet keeps its base and its room is solved again once the wall is measured away from it. Rules guessed
// from the story floors (20e03ce, 70d6238) were right on some stories and wrong on others.
std::atomic<long> g_foundationPieces{0}; // pieces moved to their drawn foot
float MeasuredDrop(uintptr_t wall, int cls, uintptr_t batch) {
    WallNote w;
    if (!ReadWallPiece(wall, cls, batch, w)) return 0.0f;
    float ax = w.x0, az = w.z0, bx = w.x1, bz = w.z1;
    float ext = 0.05f;
    const float L = std::sqrt((bx - ax) * (bx - ax) + (bz - az) * (bz - az));
    if (L < 0.05f || w.colHi <= w.colLo) { // one column: a short run along the wall
        const float dl = std::sqrt(w.dx * w.dx + w.dz * w.dz);
        if (!(dl > 1e-3f)) return 0.0f;
        bx = ax + w.dx / dl * 0.1f;
        bz = az + w.dz / dl * 0.1f;
        ext = 0.15f;
    } else
        ext += 0.5f * L / static_cast<float>(w.colHi - w.colLo);
    float foot = 0.0f;
    if (WallHeights::DrawnFoot(ax, az, bx, bz, ext, w.oy, foot)) {
        const float drop = foot - w.oy;
        return std::isfinite(drop) && std::fabs(drop) > 0.02f && std::fabs(drop) < 4.0f ? drop : 0.0f;
    }
    if (w.mgr) WallHeights::NotePending(MgrTracker(w.mgr), w.story, w.roomId, ax, az, bx, bz, ext, w.oy);
    return 0.0f;
}
void DropWallSamples(uintptr_t batch, float dy) {
    __try {
        const uintptr_t b = *reinterpret_cast<const uintptr_t*>(batch), e = *reinterpret_cast<const uintptr_t*>(batch + 4);
        if (!b || e <= b || (e - b) % 0x30 || (e - b) / 0x30 > 1u << 20) return;
        for (uintptr_t p = b; p < e; p += 0x30) *reinterpret_cast<float*>(p + 4) += dy;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

void __fastcall WallSamplesHook(void* wall, void*, int piece, int cls, void* batch) {
    reinterpret_cast<WallSamples_t>(kWallSamples)(wall, piece, cls, batch);
    const bool gather = ThreadId() == g_gatherThread.load(std::memory_order_relaxed); // g_piece and g_ghosts: that thread only
    if (gather) g_piece = PieceNote{};
    // lit where the wall is drawn (MeasuredDrop, wall_heights.h), after the rows are lined up
    const float drop = MeasuredDrop(reinterpret_cast<uintptr_t>(wall), cls, reinterpret_cast<uintptr_t>(batch));
    if (!g_alignOn.load(std::memory_order_relaxed)) {
        if (drop != 0.0f) {
            DropWallSamples(reinterpret_cast<uintptr_t>(batch), drop);
            g_foundationPieces.fetch_add(1, std::memory_order_relaxed);
        }
        NoteWallPiece(reinterpret_cast<uintptr_t>(wall), piece, cls, reinterpret_cast<uintptr_t>(batch));
        return;
    }
    PieceNote note{reinterpret_cast<uintptr_t>(wall)};
    const bool aligned = AlignWallSamples(note.wall, cls, reinterpret_cast<uintptr_t>(batch), note.block, note.rows);
    if (drop != 0.0f) {
        DropWallSamples(reinterpret_cast<uintptr_t>(batch), drop);
        g_foundationPieces.fetch_add(1, std::memory_order_relaxed);
    }
    NoteWallPiece(note.wall, piece, cls, reinterpret_cast<uintptr_t>(batch)); // the heights as lit (lined up or not)
    if (!aligned) {
        g_alignOdd.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (cls != 2 || !gather) return;
    g_piece = note;
    if (piece == 0) { // a new pass over this wall: nothing of the last one is kept
        std::lock_guard<std::mutex> lk(g_ghostMx);
        g_ghosts.erase(note.wall);
    }
}

// Copies of the piece's top-row samples (k = N-1) one and two rows higher, and of its bottom-row samples (k = 0) one and
// two rows lower, each on its own texel of the 4 x n buffer. False when the piece has neither edge.
int MakeGhosts(uintptr_t batch, const PieceNote& p, BYTE* out, int max) {
    __try {
        const uintptr_t b = *reinterpret_cast<const uintptr_t*>(batch), e = *reinterpret_cast<const uintptr_t*>(batch + 4);
        if (!b || e < b || (e - b) % 0x30) return 0;
        const float step = 3.0f / static_cast<float>(p.rows - 1);
        int n = 0;
        for (uintptr_t s = b; s < e; s += 0x30) {
            const int k = p.block.y0 + p.rows - 1 - *reinterpret_cast<const uint16_t*>(s + 0x22);
            if (k != 0 && k != p.rows - 1) continue;
            const int i = *reinterpret_cast<const uint16_t*>(s + 0x20) - p.block.x0;
            for (int g = 1; g <= 2; g++) {
                if (n >= max) return n;
                BYTE* d = out + static_cast<size_t>(n++) * 0x30;
                std::memcpy(d, reinterpret_cast<const void*>(s), 0x30);
                const bool top = k == p.rows - 1;
                *reinterpret_cast<float*>(d + 4) += top ? g * step : -g * step;
                *reinterpret_cast<uint16_t*>(d + 0x20) = static_cast<uint16_t>(i);
                *reinterpret_cast<uint16_t*>(d + 0x22) = static_cast<uint16_t>(top ? 2 - g : 1 + g); // rows 0, 1 above; 2, 3 below
            }
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void __fastcall WallSolveHook(BYTE* room, void*, void* batch, void* atlas, char* flags, void* sampler, char ambient) {
    const auto solve = reinterpret_cast<WallSolve_t>(kWallSolve);
    solve(room, batch, atlas, flags, sampler, ambient);
    if (ThreadId() != g_gatherThread.load(std::memory_order_relaxed)) return;
    const PieceNote p = g_piece;
    g_piece = PieceNote{};
    if (!p.wall || !room || !g_alignOn.load(std::memory_order_relaxed) || !GhostRoom(room) || !BlurPassesFor(room)) return;
    const int count = MakeGhosts(reinterpret_cast<uintptr_t>(batch), p, g_ghostSamples, kMaxGhosts);
    if (!count) return;
    const int n = p.block.x1 - p.block.x0 + 1;
    std::lock_guard<std::mutex> lk(g_ghostMx);
    GhostRows& g = g_ghosts[p.wall];
    if (g.n != n || g.x0 != p.block.x0) g = GhostRows{p.block.x0, n, {0, 0}, std::vector<uint32_t>(static_cast<size_t>(n) * 4)};
    struct {
        BYTE *b, *e, *c;
    } vec{g_ghostSamples, g_ghostSamples + count * 0x30, g_ghostSamples + kMaxGhosts * 0x30};
    struct {
        uint32_t* base;
        int pitch;
    } desc{g.px.data(), n * 4};
    g_ghostSolve = true;
    solve(room, &vec, &desc, flags, sampler, ambient);
    g_ghostSolve = false;
    for (int s = 0; s < count; s += 2) g.filled[*reinterpret_cast<const uint16_t*>(g_ghostSamples + s * 0x30 + 0x22) >= 2]++;
    g_ghostPoints.fetch_add(count, std::memory_order_relaxed);
}

inline uint32_t Avg4(uint32_t a, uint32_t b) { return (a | b) - (((a ^ b) >> 1) & 0x7F7F7F7Fu); }
// The game's pass along one row (FUN_0069f650, mode 0)
void BlurRow(uint32_t* p, uint32_t n) {
    if (n < 2) return;
    uint32_t prev = p[0];
    for (uint32_t j = 0; j + 1 < n; j++) {
        const uint32_t side = Avg4(p[j + 1], prev);
        prev = p[j];
        p[j] = Avg4(prev, side);
    }
    const uint32_t last = p[n - 1];
    p[n - 1] = Avg4(Avg4(last, prev), last);
}
// Its pass down one column of `rows` rows (stride n), clamped at both ends
void BlurColumn(uint32_t* p, int rows, int n) {
    uint32_t prev = p[0];
    for (int r = 0; r < rows; r++) {
        const uint32_t cur = p[static_cast<size_t>(r) * n], next = r + 1 < rows ? p[static_cast<size_t>(r + 1) * n] : cur;
        p[static_cast<size_t>(r) * n] = Avg4(Avg4(prev, next), cur);
        prev = cur;
    }
}

// The class-2 blocks of a room's walls
struct WallJob {
    uintptr_t wall;
    WallBlock r;
};
int ReadWallJobs(const BYTE* room, WallJob* out, int max, uintptr_t& atlas, uint32_t& pitch) {
    __try {
        atlas = *reinterpret_cast<const uintptr_t*>(room + 0x680);
        pitch = *reinterpret_cast<const uint32_t*>(room + 0x684);
        const uintptr_t wb = *reinterpret_cast<const uintptr_t*>(room + 0xD8), we = *reinterpret_cast<const uintptr_t*>(room + 0xDC);
        if (!atlas || !pitch || !wb || we < wb || (we - wb) / 4 > 65536) return 0;
        int n = 0;
        for (uintptr_t w = wb; w < we && n < max; w += 4) {
            const uintptr_t wall = *reinterpret_cast<const uintptr_t*>(w);
            if (!wall) continue;
            const WallBlock& r = *reinterpret_cast<const WallBlock*>(wall + 2 * 0x20 + 0x58);
            if (r.x0 < 0 || r.y0 < 0 || r.x1 < r.x0 || r.y1 <= r.y0 || r.x1 - r.x0 >= 4096 || r.y1 - r.y0 >= 4096 || static_cast<uint32_t>(r.x1 + 1) * 4 > pitch) continue;
            out[n++] = WallJob{wall, r};
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
// Rows [first, first+count) of a block between the atlas and a buffer (n words per row)
bool CopyRows(uintptr_t atlas, uint32_t pitch, const WallBlock& r, int first, int count, uint32_t* buf, bool toAtlas) {
    __try {
        const size_t n = static_cast<size_t>(r.x1 - r.x0 + 1);
        for (int k = 0; k < count; k++) {
            uint32_t* row = reinterpret_cast<uint32_t*>(atlas + static_cast<size_t>(pitch) * (r.y0 + first + k)) + r.x0;
            if (toAtlas) std::memcpy(row, buf + k * n, n * 4);
            else std::memcpy(buf + k * n, row, n * 4);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

constexpr int kMaxWallJobs = 4096;
WallJob g_wallJobs[kMaxWallJobs];
std::mutex g_edgeMx; // g_wallJobs and the buffers of one blur at a time
void BlurWalls(BYTE* room) {
    const auto blur = reinterpret_cast<WallBlur_t>(kWallBlur);
    const uint32_t passes = room && g_alignOn.load(std::memory_order_relaxed) ? BlurPassesFor(room) : 0;
    if (!passes) return blur(room);
    std::lock_guard<std::mutex> lk(g_edgeMx);
    uintptr_t atlas = 0;
    uint32_t pitch = 0;
    const int jobs = ReadWallJobs(room, g_wallJobs, kMaxWallJobs, atlas, pitch);
    // Before the game's blur: every block as the solve left it
    std::vector<std::vector<uint32_t>> before(jobs);
    for (int j = 0; j < jobs; j++) {
        const WallBlock& r = g_wallJobs[j].r;
        before[j].resize(static_cast<size_t>(r.x1 - r.x0 + 1) * (r.y1 - r.y0 + 1));
        if (!CopyRows(atlas, pitch, r, 0, r.y1 - r.y0 + 1, before[j].data(), false)) before[j].clear();
    }
    // The rows lit beyond the edges in this wall pass (only walls whose every column got them)
    std::vector<GhostRows> ghosts(jobs);
    if (ThreadId() == g_gatherThread.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> gk(g_ghostMx);
        for (int j = 0; j < jobs; j++)
            if (auto it = g_ghosts.find(g_wallJobs[j].wall); it != g_ghosts.end()) {
                ghosts[j] = std::move(it->second);
                g_ghosts.erase(it);
            }
    }
    blur(room);
    long edges = 0, crossed = 0;
    for (int j = 0; j < jobs; j++) {
        if (before[j].empty()) continue;
        const WallBlock& r = g_wallJobs[j].r;
        const int n = r.x1 - r.x0 + 1, rows = r.y1 - r.y0 + 1;
        const GhostRows& g = ghosts[j];
        const bool fits = g.n == n && g.x0 == r.x0 && g.px.size() == static_cast<size_t>(n) * 4;
        const bool above = fits && g.filled[0] >= n, below = fits && g.filled[1] >= n;
        // The wall with the rows beyond it, blurred the game's way: the rows along, then the columns, each pass
        const int top = above ? 2 : 0, total = top + rows + (below ? 2 : 0);
        std::vector<uint32_t> ext(static_cast<size_t>(total) * n);
        if (above) std::memcpy(ext.data(), g.px.data(), static_cast<size_t>(n) * 2 * 4);
        std::memcpy(ext.data() + static_cast<size_t>(top) * n, before[j].data(), before[j].size() * 4);
        if (below) std::memcpy(ext.data() + static_cast<size_t>(top + rows) * n, g.px.data() + static_cast<size_t>(n) * 2, static_cast<size_t>(n) * 2 * 4);
        for (uint32_t pass = 0; pass < passes; pass++) {
            for (int k = 0; k < total; k++) BlurRow(ext.data() + static_cast<size_t>(k) * n, n);
            for (int x = 0; x < n; x++) BlurColumn(ext.data() + x, total, n);
        }
        // An edge with nothing beyond it keeps its own light, blurred along the wall only
        for (const int k : {0, rows - 1}) {
            if (k == 0 ? above : below) continue;
            uint32_t* row = ext.data() + static_cast<size_t>(top + k) * n;
            std::memcpy(row, before[j].data() + static_cast<size_t>(k) * n, static_cast<size_t>(n) * 4);
            for (uint32_t pass = 0; pass < passes; pass++) BlurRow(row, n);
            edges++;
        }
        if (CopyRows(atlas, pitch, r, 0, rows, ext.data() + static_cast<size_t>(top) * n, true) && (above || below)) crossed++;
    }
    g_alignEdges.fetch_add(edges, std::memory_order_relaxed);
    g_ghostWalls.fetch_add(crossed, std::memory_order_relaxed);
}

void __fastcall WallBlurHook(BYTE* room) {
    BlurWalls(room);
    if (Recorder::Verbose())
        if (room) NoteSolve(room, 'W');
}

// Development build: one note per solve step of a room that takes or gives light through an opening or is merged
// with another story's room (a lot's other rooms are left out)
void NoteSolve(BYTE* room, char event, uintptr_t caller) {
    SolveNote n{};
    n.caller = caller;
    if (!ReadSolveNote(room, n)) return;
    n.tick = GetTickCount();
    n.room = reinterpret_cast<uintptr_t>(room);
    n.event = event;
    n.gather = ThreadId() == g_gatherThread.load(std::memory_order_relaxed);
    n.boosted = Boosted(n.room, n.id, n.mgr);
    n.lights = static_cast<int>(ListSize(room));
    n.cross = -1;
    if (n.gather) { // g_rooms and g_wallBase belong to that thread
        if (const auto it = g_rooms.find(n.room); it != g_rooms.end() && it->second.id == n.id && it->second.mgr == n.mgr) n.cross = static_cast<int>(it->second.cross.size());
        const auto wb = g_wallBase.find(n.room);
        n.merged = wb != g_wallBase.end() && wb->second.id == n.id && wb->second.mgr == n.mgr;
    }
    if (!n.boosted && !n.merged && n.cross <= 0 && !Recorder::Active()) return; // a recording keeps every room (light update trace)
    n.moved = g_alignRows.load(std::memory_order_relaxed);
    n.ghostWalls = g_ghostWalls.load(std::memory_order_relaxed);
    n.odd = g_alignOdd.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_journalMx);
    g_journal[g_journalCount++ % kJournal] = n;
}
// Development build: who restarts rooms. Entry hooks on the game's two invalidates (0x0069EED0, 0x0069F160) note the room with
// the caller in the journal (29/09: an outdoor room was solved again every ~60 ms for 4.5 s, never shown, and nothing of
// Apex sent it). Only rooms the journal keeps (boosted, merged or taking other stories' lights).
using Invalidate_t = void(__thiscall*)(void* room, char full, char keep);
using InvalidateFlag_t = void(__thiscall*)(void* room, char flag);
void __fastcall InvalidateNoteHook(BYTE* room, void*, char full, char keep) {
    if (room && g_installed.load(std::memory_order_relaxed)) NoteSolve(room, 'I', reinterpret_cast<uintptr_t>(_ReturnAddress()));
    reinterpret_cast<Invalidate_t>(EntryChain::Next(EntryChain::Site::RoomInvalidate, EntryChain::Layer::LevelLightShare))(room, full, keep);
}
void __fastcall InvalidateFlagNoteHook(BYTE* room, void*, char flag) {
    if (room && g_installed.load(std::memory_order_relaxed) && *reinterpret_cast<const char*>(room + 0x19) != flag)
        NoteSolve(room, 'F', reinterpret_cast<uintptr_t>(_ReturnAddress()));
    reinterpret_cast<InvalidateFlag_t>(EntryChain::Next(EntryChain::Site::RoomInvalidateFlag, EntryChain::Layer::LevelLightShare))(room, flag);
}

// The end of a room's solve (06/10, light update trace): FUN_006a0e00, step 8 of the budgeted solve (its call at
// 0x6A3E65 in FUN_006a3c90), unlocks the maps the solve wrote and gives them back to the room, which shows them from the
// next frame. Noted 'E' in the solve journal while a recording runs, and told to Faster Room Lighting: a quick pass is
// shown only once its room's solve ended (RoomLightQueue::NoteSolveEnd). Steam 1.67.2 (the call checked at install).
constexpr uintptr_t kFinalizeCall = 0x006A3E65, kFinalize = 0x006A0E00;
using Finalize_t = void(__thiscall*)(void* room);
void __fastcall FinalizeHook(BYTE* room) {
    reinterpret_cast<Finalize_t>(kFinalize)(room);
    if (!room) return;
    RoomLightQueue::NoteSolveEnd(room);
    if (Recorder::Active()) NoteSolve(room, 'E');
}
std::atomic<bool> g_finalizeReady{false}, g_lockStepReady{false};

// Step 1 of a room's solve (06/10 review): FUN_0069fa40 binds the room's texture set and locks its maps (0x00618DF0, the
// lock pointers room+0x1C4 / +0x2B4 / +0x3A4); its only call is 0x1B after the state-0 call (0x006A3D26 on Steam 1.67.2),
// "mov ecx,esi; fstp st0; call; test al,al; je". AtriumHold keeps a map only when the game locks it here
// (InMapLockStep), never a texture something else locks on the render thread while a room is mid-solve (custom content
// streaming in, Apex's LUT).
uintptr_t kLockStep = 0;
using LockStep_t = char(__thiscall*)(void* room);
thread_local int t_lockStep = 0;
char __fastcall LockStepHook(BYTE* room) {
    char ok = 0;
    t_lockStep++;
    __try {
        ok = reinterpret_cast<LockStep_t>(kLockStep)(room);
    } __finally {
        t_lockStep--;
    }
    return ok;
}

void ClearJournal() {
    std::lock_guard<std::mutex> lk(g_journalMx);
    g_journalCount = 0;
}
std::string JournalText() {
    std::vector<SolveNote> notes;
    size_t total = 0;
    {
        std::lock_guard<std::mutex> lk(g_journalMx);
        total = g_journalCount;
        const size_t kept = std::min(total, kJournal);
        for (size_t k = total - kept; k < total; k++) notes.push_back(g_journal[k % kJournal]);
    }
    std::string s = std::format("\n==== SOLVES (rooms sharing light through an opening, since the world loaded: {} notes, the last {} shown) ====\n", total,
                                std::min<size_t>(notes.size(), 1000));
    s += "time | thread (G = light tree, O = other) | S = ambient step done, W = wall pass done, Q = sent to gather by Apex, H = held by Apex until its solve "
         "ends | room id, story | LOD class solving / shown | state | lights (of other stories) | B boosted, M merged | normalisation, ambient, wall ramp base | "
         "wall samples moved, walls blurred across, pieces left alone (running totals) | lot id (low half), camera story, room flag +0x19 [| caller of an invalidate]\n";
    if (notes.empty()) return s + "No notes.\n";
    const DWORD t0 = notes.front().tick;
    for (size_t k = notes.size() > 1000 ? notes.size() - 1000 : 0; k < notes.size(); k++) {
        const SolveNote& n = notes[k];
        s += std::format("{:8.3f} {} {} room {} story {} | class {}/{} st {} | lights {} ({}) | {}{} | norm {:.3f} c4 ({:.3f} {:.3f} {:.3f}) base {:.2f} | {} {} {} | {:08X} {} {}\n",
                         (n.tick - t0) / 1000.0, n.gather ? 'G' : 'O', n.event, n.id, n.level, n.cls, n.shown, n.state, n.lights, n.cross, n.boosted ? 'B' : '-',
                         n.merged ? 'M' : '-', n.norm, n.c4[0], n.c4[1], n.c4[2], n.wallBase, n.moved, n.ghostWalls, n.odd, n.lot, n.cam, n.flag19);
        if (n.caller) s.insert(s.size() - 1, std::format(" | {:08X}", n.caller));
    }
    return s;
}

// Development build: the notes written since fromTick (GetTickCount), each with its tick (the recorder, recorder.cpp)
std::vector<std::pair<DWORD, std::string>> JournalLinesSince(DWORD fromTick) {
    std::vector<std::pair<DWORD, std::string>> out;
    std::lock_guard<std::mutex> lk(g_journalMx);
    const size_t kept = std::min(g_journalCount, kJournal);
    for (size_t k = g_journalCount - kept; k < g_journalCount; k++) {
        const SolveNote& n = g_journal[k % kJournal];
        if (static_cast<int32_t>(n.tick - fromTick) < 0) continue;
        std::string s = std::format("{} {} room {} story {} | class {}/{} st {} | lights {} ({}) | {}{} | c4 ({:.3f} {:.3f} {:.3f}) | lot {:08X} cam {} flag {}",
                                    n.gather ? 'G' : 'O', n.event, n.id, n.level, n.cls, n.shown, n.state, n.lights, n.cross, n.boosted ? 'B' : '-',
                                    n.merged ? 'M' : '-', n.c4[0], n.c4[1], n.c4[2], n.lot, n.cam, n.flag19);
        if (n.caller) s += std::format(" | caller {:08X}", n.caller);
        out.emplace_back(n.tick, std::move(s));
    }
    return out;
}

void ShareIndoorLights(BYTE* treeLevel, BYTE* room) {
    const int id = *reinterpret_cast<const int*>(room + 0xC);
    const uintptr_t key = reinterpret_cast<uintptr_t>(room);
    MaybeClearRooms();
    auto old = g_rooms.find(key); // the list was just rebuilt without the other stories' lamps
    if (old != g_rooms.end() && (id > 0 || old->second.indoor)) g_rooms.erase(old);
    if (id > 0) BeginTaker(key); // what it took (and the boosts that came with it) is taken again below, or not
    if (id <= 0 || !g_indoorReady || !g_indoorOn.load(std::memory_order_relaxed)) return;
    const uintptr_t rmgr = *reinterpret_cast<const uintptr_t*>(room);
    if (!rmgr) return;
    const int S = *reinterpret_cast<const int*>(rmgr + 0x88);
    const int level = *reinterpret_cast<const int*>(treeLevel + 0x1A0);
    const uintptr_t tracker = *reinterpret_cast<const uintptr_t*>(treeLevel + 4);
    const bool log = !kPublicBuild;
    if (S < 0 || S > 7 || level < -4 || level > 7 || !tracker || TreeLevel(tracker, level) != reinterpret_cast<uintptr_t>(treeLevel) || StoryManager(tracker, S) != rmgr) {
        if (log) GatherLog(tracker, std::format("story {} room {}: gathered through tree level {} of tracker {:08X}, which does not match (skipped)", S, id, level, tracker));
        return;
    }
    g_gatherThread = ThreadId();
    NoteLotManagers(tracker); // for the ambient of stacked rooms (RoomSolveStartHook knows only the room)
    Xform xf;
    if (!ReadXform(rmgr, xf)) return;
    // what this gather sees of the other stories' lamps (AuditTakers); the audit's own counts of this room are kept
    TakerSeen& seen = g_seen[key];
    if (seen.mgr != rmgr || seen.tracker != tracker || seen.id != id) seen = TakerSeen{};
    seen.mgr = rmgr;
    seen.tracker = tracker;
    seen.level = S;
    seen.id = id;
    seen.stories.clear();
    RoomSpan span;
    std::vector<int> rooms;
    struct Candidate {
        float dist; // horizontal distance from the lamp to this room's tiles, m
        uintptr_t light;
        int home;
    };
    std::vector<Candidate> cands;
    // Recorded before the first lamp is added (and each lamp before it goes in), so a fault in the middle of the gather
    // never leaves a lamp in the list that the point solve does not test
    RoomInfo* info = nullptr;
    OpeningMask boundaryMasks[8];
    bool boundaryRead[8] = {}, boundaryValid[8] = {};
    RoomSpan boundarySpans[8];
    bool spanRead[8] = {}, spanValid[8] = {};
    const auto boundaryMask = [&](int boundary) -> const OpeningMask* {
        if (!boundaryRead[boundary]) {
            boundaryRead[boundary] = true;
            const uintptr_t manager = StoryManager(tracker, boundary);
            const uintptr_t floor = manager ? LevelFor(manager) : 0;
            boundaryValid[boundary] = floor && BuildOpeningMask(StoryManager(tracker, boundary - 1), manager, floor, boundaryMasks[boundary]);
        }
        return boundaryValid[boundary] ? &boundaryMasks[boundary] : nullptr;
    };
    const auto boundarySpan = [&](int boundary, const OpeningMask& opening) -> const RoomSpan* {
        if (!spanRead[boundary]) {
            spanRead[boundary] = true;
            spanValid[boundary] = ReadRoomSpan(rmgr, id, opening, boundarySpans[boundary]);
        }
        return spanValid[boundary] ? &boundarySpans[boundary] : nullptr;
    };
    // Keep adjacent lamps first under the existing 64-light cap.
    for (int distance = 1; distance <= 7; ++distance) for (const int direction : {1, -1}) {
        if (info && info->cross.size() >= 64) break;
        const int U = S + direction * distance;
        if (U < 0 || U > 7) continue;
        const uintptr_t tlU = TreeLevel(tracker, U);
        if (!*reinterpret_cast<const uintptr_t*>(tlU)) continue;
        const int B = std::max(S, U); // the story whose floor lies between them
        const uintptr_t mgrB = StoryManager(tracker, B), levelB = LevelFor(mgrB);
        if (!levelB) {
            if (log) GatherLog(tracker, std::format("story {} room {}: story {} has no known floor object", S, id, B));
            continue;
        }
        const OpeningMask* endpointMask = boundaryMask(B);
        if (!endpointMask || !endpointMask->openings) {
            if (log) GatherLog(tracker, std::format("story {} room {}: no opening in the floor of story {}", S, id, B));
            continue;
        }
        const OpeningMask& mask = *endpointMask;
        const RoomSpan* endpointSpan = boundarySpan(B, mask);
        if (!endpointSpan || !endpointSpan->any) {
            if (log) GatherLog(tracker, std::format("story {} room {}: no lighting tile quadrant has this room's id", S, id));
            return;
        }
        span = *endpointSpan;
        if (!span.nearOpening) {
            if (log) GatherLog(tracker, std::format("story {} room {}: none of the {} openings of story {} is within {} m", S, id, mask.openings, B, kOpeningReach));
            continue;
        }
        // Distant stories must have an opening at every boundary. This is only
        // a conservative gather filter; IndoorPass tests the actual ray below.
        bool connected = true;
        for (int boundary = std::min(S, U) + 1; boundary < B; ++boundary) {
            const OpeningMask* intermediate = boundaryMask(boundary);
            const RoomSpan* intermediateSpan = intermediate ? boundarySpan(boundary, *intermediate) : nullptr;
            if (!intermediate || !intermediate->openings || !intermediateSpan || !intermediateSpan->nearOpening) {
                connected = false;
                break;
            }
        }
        if (!connected) continue;
        int ids[256];
        const int nIds = RoomsNearOpenings(StoryManager(tracker, U), mask, ids, 256);
        rooms.assign(ids, ids + nIds);
        if (rooms.empty()) {
            if (log) GatherLog(tracker, std::format("story {} room {}: near the openings of story {}, but no room of story {} is", S, id, B, U));
            continue;
        }
        NoteDeps(tracker, U, rooms, S, id);
        std::string lamps;
        cands.clear();
        seen.stories.push_back(SeenStory{U, rooms, {}});
        std::vector<SeenLamp>& seenLamps = seen.stories.back().lamps; // this story's only (the next push may move it)
        WalkRegistry(tlU, [&](uintptr_t entry) {
            const int home = *reinterpret_cast<const int*>(entry + 0x1C);
            const uintptr_t light = *reinterpret_cast<const uintptr_t*>(entry + 0x24);
            if (!light || std::find(rooms.begin(), rooms.end(), home) == rooms.end()) return;
            float lp[3];
            ToLocal(xf, reinterpret_cast<const float*>(light + 0x120), lp);
            const bool closeBy = mask.Near(lp[0], lp[2]);
            // distance to the room's tiles, to take the nearest lamps first. No range test: +0x130 is the range only for
            // some classes (wall lights, type 7, hold 0, 0.1, 1 or garbage there), and testing it dropped the sconces of a
            // double-height room (in-game test, 2026-09-29); the game's own evaluation fades the light with distance.
            const float dx = std::max({span.x0 - lp[0], 0.0f, lp[0] - span.x1}), dz = std::max({span.z0 - lp[2], 0.0f, lp[2] - span.z1});
            const float dist = std::sqrt(dx * dx + dz * dz);
            const bool reaches = closeBy && dist <= 30.0f;
            const bool wrapped = reaches && EvalWrapped(light);
            const bool takes = wrapped && GameTakesLight(entry, light);
            const bool listed = takes && InList(room, light);
            if (seenLamps.size() < 4096) seenLamps.push_back(SeenLamp{light, entry, home, lp[0], lp[2], reaches, takes});
            if (log)
                lamps += std::format(" L{:08X}(room {}, near {}, reach {}, class {}, game {}, listed {})", light, home, closeBy ? 1 : 0, reaches ? 1 : 0, wrapped ? 1 : 0,
                                     takes ? 1 : 0, listed ? 1 : 0);
            if (takes && !listed) cands.push_back(Candidate{dist, light, home});
        });
        std::sort(seenLamps.begin(), seenLamps.end(), SeenBefore);
        // the nearest lamps first, 64 per room at most
        std::sort(cands.begin(), cands.end(), [](const Candidate& a, const Candidate& b) { return a.dist < b.dist; });
        for (const Candidate& c : cands) {
            if (info && info->cross.size() >= 64) break;
            if (!info) {
                info = &g_rooms[key];
                *info = RoomInfo{rmgr, tracker, S, id, true, {}};
                NoteIndoorRoom(tracker, S, id, true);
                MarkTaker(key, id, rmgr, tracker, S);
            }
            const Cross x{c.light, U, c.home, levelB};
            info->cross.insert(std::upper_bound(info->cross.begin(), info->cross.end(), x), x);
            reinterpret_cast<AddRoomLight_t>(kAddRoomLight)(room, reinterpret_cast<void*>(c.light));
            // the lamp's room is seen through the opening too: the camera story's LOD, and one new solve the first time
            if (void* mgrU = reinterpret_cast<void*>(StoryManager(tracker, U)))
                if (void* giver = reinterpret_cast<RoomById_t>(kRoomById)(mgrU, c.home))
                    if (AddGiver(key, reinterpret_cast<uintptr_t>(giver), c.home, reinterpret_cast<uintptr_t>(mgrU), tracker, U) && QueueRoom(tracker, U, c.home)) g_indoorQueued.fetch_add(1, std::memory_order_relaxed);
            if (!kPublicBuild) {
                if (g_crossLamps.size() > 4096) g_crossLamps.clear();
                g_crossLamps.insert(c.light);
            }
            g_indoorAdded.fetch_add(1, std::memory_order_relaxed);
        }
        if (log) {
            std::string rs;
            for (int r : rooms) rs += std::format(" {}", r);
            GatherLog(tracker, std::format("story {} room {}: story {} floor has {} opening quadrants; rooms of story {} near them:{}; {} lamps taken; lamps:{}", S, id, B,
                                           mask.openings, U, rs, cands.size(), lamps.empty() ? " none" : lamps));
        }
    }
    if (!info) NoteIndoorRoom(tracker, S, id, false);
}

// Rooms of a lot near a stair opening (receivers on both sides), plus the rooms that took lamps before: they gather again
void QueueOpeningRooms(uintptr_t tracker, std::vector<std::pair<int, int>>* sent = nullptr) {
    std::vector<std::pair<int, int>> todo;
    BoostedRoomsOf(tracker, todo);
    {
        std::lock_guard<std::mutex> lk(g_indoorListMx);
        for (const IndoorRoom& r : g_indoorList)
            if (r.tracker == tracker) todo.emplace_back(r.level, r.id);
    }
    std::string summary; // development build: per story, whether its floor object is known and its openings
    if (g_indoorOn.load(std::memory_order_relaxed)) {
        OpeningMask mask;
        for (int B = 1; B <= 7; B++) {
            const uintptr_t mgrB = StoryManager(tracker, B), levelB = mgrB ? LevelFor(mgrB) : 0;
            if (!kPublicBuild && mgrB) summary += levelB ? "" : std::format(" story {}: no floor object;", B);
            if (!levelB) continue;
            const bool read = BuildOpeningMask(StoryManager(tracker, B - 1), mgrB, levelB, mask);
            if (!kPublicBuild) summary += std::format(" story {}: {} openings{};", B, mask.openings, read ? "" : " (fault)");
            if (!read || !mask.openings) continue;
            for (const int S : {B - 1, B}) {
                const uintptr_t mgrS = StoryManager(tracker, S);
                if (!mgrS) continue;
                int ids[256];
                const int n = RoomsNearOpenings(mgrS, mask, ids, 256);
                for (int i = 0; i < n; i++) todo.emplace_back(S, ids[i]);
            }
        }
    }
    std::sort(todo.begin(), todo.end());
    todo.erase(std::unique(todo.begin(), todo.end()), todo.end());
    if (sent) *sent = todo;
    int queued = 0;
    for (const auto& [S, r] : todo)
        if (QueueRoom(tracker, S, r, true)) queued++;
    g_indoorQueued.fetch_add(queued, std::memory_order_relaxed);
    if (!kPublicBuild && (!todo.empty() || !summary.empty()))
        GatherLog(tracker, std::format("rooms near openings sent to gather again: {} of {} (the others were waiting already) |{}", queued, todo.size(), summary));
}

// Rooms taking lamps of other stories, checked against those lamps as they are now (05/10, user, atrium house: "the second
// floor is not receiving the correction"; recording 19:54:44 and its wall seams: at 43.98 s rooms 20, 10, 16, 17, 18 of
// story 2, 21, 23, 25 of story 0 and story 3's gathered without the sconce of room 19 just under the floor line (41 -> 40
// lights, 30 -> 29 of another story), while room 19 itself, gathered 0.1 s later, still had it where it was; the upper wall
// over it went from 2.26 to 0.11 at the line. They gathered while the game was still re-registering the lamp after the edit,
// and nothing sent them again: the lamp ended exactly as it was before, so no signature changed). What a room saw of the
// lamps of the rooms near the openings at its gather (TakerSeen) is compared with the registry once the lot's lamps are
// quiet: a lamp added or gone, moved more than kAuditMoveM, or now taken or not by the game's own checks sends it to gather
// again.
// Light tree thread (BeforeRoomUpdate), every kAuditEveryMs per lot; a room is sent at most kAuditSends times in 10 s.
constexpr DWORD kAuditEveryMs = 100, kAuditQuietMs = 150, kAuditResendMs = 600;
constexpr int kAuditSends = 3;
std::atomic<DWORD> g_lampEditAt{0}; // any lamp edit (LampMarkFilter, NoteLampEditing)
void AuditTakers(uintptr_t tracker, DWORD now) {
    if (g_seen.empty() || !g_indoorOn.load(std::memory_order_relaxed) || ThreadId() != g_gatherThread.load(std::memory_order_relaxed)) return;
    if (const DWORD edit = g_lampEditAt.load(std::memory_order_relaxed); edit && now - edit < kAuditQuietMs) return; // the edit's own sends first
    struct Reg {
        uintptr_t entry, light;
        int home;
    };
    std::vector<Reg> regs[8];
    bool walked[8] = {};
    std::vector<SeenLamp> cur;
    std::vector<std::pair<std::pair<int, int>, uintptr_t>> send; // ((story, room), the lamp that differs), sent after the walk over g_seen
    for (auto it = g_seen.begin(); it != g_seen.end();) {
        TakerSeen& t = it->second;
        if (t.tracker != tracker) {
            ++it;
            continue;
        }
        BYTE* room = reinterpret_cast<BYTE*>(it->first);
        if (t.level < 0 || t.level > 7 || StoryManager(tracker, t.level) != t.mgr ||
            reinterpret_cast<BYTE*>(reinterpret_cast<RoomById_t>(kRoomById)(reinterpret_cast<void*>(t.mgr), t.id)) != room) {
            it = g_seen.erase(it); // its story was rebuilt or the room is gone: its next gather records it again
            continue;
        }
        const int state = RoomState(room);
        if (t.stories.empty() || state < 2 || state > 5 || (t.backoffUntil && static_cast<int32_t>(now - t.backoffUntil) < 0) ||
            (t.sentAt && now - t.sentAt < kAuditResendMs)) {
            ++it; // waiting for its gather (that records it again), or sent a moment ago
            continue;
        }
        Xform xf;
        if (!ReadXform(t.mgr, xf)) {
            ++it;
            continue;
        }
        g_auditChecks.fetch_add(1, std::memory_order_relaxed);
        bool differs = false;
        uintptr_t which = 0; // the lamp that differs
        for (const SeenStory& s : t.stories) {
            if (s.story < 0 || s.story > 7) continue;
            if (!walked[s.story]) {
                walked[s.story] = true;
                const uintptr_t tl = TreeLevel(tracker, s.story);
                if (*reinterpret_cast<const uintptr_t*>(tl))
                    WalkRegistry(tl, [&](uintptr_t entry) {
                        if (regs[s.story].size() < 8192)
                            regs[s.story].push_back(Reg{entry, *reinterpret_cast<const uintptr_t*>(entry + 0x24), *reinterpret_cast<const int*>(entry + 0x1C)});
                    });
            }
            cur.clear();
            for (const Reg& r : regs[s.story]) {
                if (!r.light || std::find(s.rooms.begin(), s.rooms.end(), r.home) == s.rooms.end()) continue;
                float lp[3];
                ToLocal(xf, reinterpret_cast<const float*>(r.light + 0x120), lp);
                cur.push_back(SeenLamp{r.light, r.entry, r.home, lp[0], lp[2], false, false});
            }
            std::sort(cur.begin(), cur.end(), SeenBefore);
            differs = cur.size() != s.lamps.size();
            for (size_t k = 0; k < std::min(cur.size(), s.lamps.size()) && !which; k++) {
                const SeenLamp& was = s.lamps[k];
                const SeenLamp& is = cur[k];
                if (is.light != was.light) // one of them is not in the other list: the new one, else the one gone
                    which = std::none_of(s.lamps.begin(), s.lamps.end(), [&](const SeenLamp& l) { return l.light == is.light; }) ? is.light : was.light;
                else if (is.home != was.home || std::fabs(is.x - was.x) > kAuditMoveM || std::fabs(is.z - was.z) > kAuditMoveM) which = is.light;
                else if (was.reach && (EvalWrapped(is.light) && GameTakesLight(is.entry, is.light)) != was.takes) which = is.light;
            }
            if (which) differs = true;
            else if (differs) which = cur.size() > s.lamps.size() ? cur.back().light : s.lamps.back().light; // (approximate)
            if (differs) break;
        }
        if (differs) {
            if (now - t.sendsFrom > 10000) {
                t.sendsFrom = now;
                t.sends = 0;
            }
            if (++t.sends > kAuditSends) { // what it sees keeps changing (a lamp switching itself): leave it to the edits for a while
                t.backoffUntil = (now + 10000) | 1;
                g_auditGaveUp.fetch_add(1, std::memory_order_relaxed);
            } else {
                t.sentAt = now | 1;
                send.push_back({{t.level, t.id}, which});
            }
        }
        ++it;
    }
    // within kUrgentMs of a lamp edit: urgent, right after the edit's own rooms (solved with them), and gathered in their
    // story's next update (the lamps are quiet by now); otherwise at the scheduler's own pace (no pause)
    const DWORD edit = g_lampEditAt.load(std::memory_order_relaxed);
    const bool afterEdit = edit && now - edit < kUrgentMs;
    for (const auto& [room, lamp] : send) {
        // urgent only where that lamp lights the room (g_litBy): the others change nothing a player sees
        const bool urgent = afterEdit && LitByAny(DepKey{tracker, room.first, room.second}, &lamp, 1);
        if (QueueRoom(tracker, room.first, room.second, false, urgent ? 1 : -1, urgent)) g_auditSent.fetch_add(1, std::memory_order_relaxed);
    }
}

// The rooms the game found changed on this story (the treeLevel+0x8 set: buckets +0xC, count +0x10, node {id, next})
int ChangedRooms(const BYTE* tl, int* ids, int max) {
    const uintptr_t buckets = *reinterpret_cast<const uintptr_t*>(tl + 0xC);
    const uint32_t count = *reinterpret_cast<const uint32_t*>(tl + 0x10);
    if (!buckets || !count || count >= (1u << 20)) return 0;
    const uintptr_t endNode = *reinterpret_cast<const uintptr_t*>(buckets + count * 4);
    uintptr_t slot = buckets;
    uintptr_t node = *reinterpret_cast<const uintptr_t*>(slot);
    int guard = 0, n = 0;
    while (node == 0 && guard++ < (1 << 20)) node = *reinterpret_cast<const uintptr_t*>(slot += 4);
    guard = 0;
    while (node && node != endNode && guard++ < 100000 && n < max) {
        ids[n++] = *reinterpret_cast<const int*>(node);
        node = *reinterpret_cast<const uintptr_t*>(node + 4);
        int g2 = 0;
        while (node == 0 && g2++ < (1 << 20)) node = *reinterpret_cast<const uintptr_t*>(slot += 4);
    }
    return n;
}

// Per lot (light thread): the g_indoorGen its rooms near openings were last sent at, and how many of its stories' floor
// objects were known then. A lot loads its rooms and gathers them before (or while) its floors are set, so the count is
// checked every 2 s: when it grows, the rooms near openings gather again (in-game test: after a restart the light
// through the stairwell was gone until a floor was edited).
struct LotState {
    long gen = 0;
    int floorsKnown = 0, openings = -1;
    DWORD nextCheck = 0;
    DWORD settleAt = 0; // once more, after the lot's first solves settled: at the latest then (0 = not due)
    DWORD armedAt = 0, quietSince = 0; // when it was armed; since when none of `watch` waits for a solve (0 = busy)
    DWORD windowsArmed = 0;
    unsigned windowPass = 3;
    int windowStory = -99;
    DWORD auditAt = 0; // the next AuditTakers of this lot
    std::vector<std::pair<int, int>> watch; // (story, room) sent by the last QueueOpeningRooms
    // The lot's story managers when last seen: a lot whose lighting was rebuilt (the camera left it and came back, or
    // its tracker's address was reused by another lot) is a new lot for this state (user, 29/09: "sometimes I even have
    // to reload the save for the indoor fix to work": the old counts matched the new lot, so nothing was sent again)
    uintptr_t mgrs[8] = {};
};
std::unordered_map<uintptr_t, LotState> g_lots;
void FlushDepWaits(uintptr_t tracker);
bool DepWaitsFor(uintptr_t tracker);

// Whether a lot still has work of the indoor light between stories: a watched room gathering, waiting or being solved
// (states 1, 2, 3), a room held back while solved, or a lamp burst still waiting
bool LotBusy(uintptr_t tracker, const std::vector<std::pair<int, int>>& watch) {
    for (const auto& [S, r] : watch) {
        void* mgr = *reinterpret_cast<void* const*>(TreeLevel(tracker, S));
        const BYTE* room = mgr ? static_cast<const BYTE*>(reinterpret_cast<RoomById_t>(kRoomById)(mgr, r)) : nullptr;
        if (!room) continue;
        const int state = *reinterpret_cast<const int*>(room + 0xF0);
        if (state >= 1 && state <= 3) return true;
    }
    {
        std::lock_guard<std::mutex> lk(g_deferredMx);
        if (std::any_of(g_deferred.begin(), g_deferred.end(), [&](const DeferredRoom& d) { return d.tracker == tracker; })) return true;
    }
    {
        std::lock_guard<std::mutex> lk(g_ambMx);
        if (std::any_of(g_ambToQueue.begin(), g_ambToQueue.end(), [&](const auto& entry) { return entry.first.tracker == tracker; })) return true;
    }
    return DepWaitsFor(tracker);
}

int FloorObjectsKnown(uintptr_t tracker) {
    int n = 0;
    for (int s = 1; s <= 7; s++) {
        const uintptr_t mgr = StoryManager(tracker, s);
        n += mgr && LevelFor(mgr);
    }
    return n;
}
// The lot's openings over every story (the rooms get their ids after the floors are set, so this can change without a
// floor edit)
int OpeningCount(uintptr_t tracker) {
    static OpeningMask mask; // light thread only
    int n = 0;
    for (int B = 1; B <= 7; B++) {
        const uintptr_t mgrB = StoryManager(tracker, B), levelB = mgrB ? LevelFor(mgrB) : 0;
        if (levelB && BuildOpeningMask(StoryManager(tracker, B - 1), mgrB, levelB, mask)) n += mask.openings;
    }
    return n;
}

bool AmbientRoomExists(const DepKey& key) {
    __try {
        void* mgr = reinterpret_cast<void*>(StoryManager(key.tracker, key.level));
        const BYTE* room = mgr ? static_cast<const BYTE*>(reinterpret_cast<RoomById_t>(kRoomById)(mgr, key.room)) : nullptr;
        return room && !room[0x18];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Called on the light thread before the changed-room set is walked. Take a snapshot first:
// the game's evaluator may move a paired window entry to another room's registry.
// A window the game takes back (06/10, every load of the atrium house since the morning: "144 evaluated, 50 changed" two or
// three times within half a second, each time the same 50 windows, and each time their rooms solved again): the state an
// entry had before Apex's last update of it (room, lit), seen again within kWindowFlipMs, means the game set it back; Apex
// then leaves that entry as the game keeps it.
struct WindowState {
    int room = 0;
    BYTE lit = 0;
    bool operator==(const WindowState&) const = default;
};
struct WindowFlip {
    WindowState before, after;
    DWORD at = 0;
};
constexpr DWORD kWindowFlipMs = 10000;
std::unordered_map<uintptr_t, WindowFlip> g_windowFlips; // light tree thread: entry -> Apex's last update of it that changed it
std::atomic<long> g_windowsTakenBack{0};
// 0 = not a window entry of tl (left alone), 1 = evaluated (now: its state before; after: after the update), -1 = fault
int RecheckWindowEntry(uintptr_t entry, uintptr_t tl, const WindowFlip* last, WindowState& now, WindowState& after, bool& skipped) {
    skipped = false;
    __try {
        if (*reinterpret_cast<const uintptr_t*>(entry + 0x14) != tl) return 0;
        const uintptr_t light = *reinterpret_cast<const uintptr_t*>(entry + 0x24);
        if (!light) return 0;
        const int type = *reinterpret_cast<const int*>(light + 0xB0);
        if (type != 7 && type != 8) return 0;
        const uintptr_t vt = *reinterpret_cast<const uintptr_t*>(entry);
        if (!vt || *reinterpret_cast<const uintptr_t*>(vt + 8) != kLightEntryUpdate) return 0;
        now = WindowState{*reinterpret_cast<const int*>(entry + 0x1C), static_cast<BYTE>(*reinterpret_cast<const BYTE*>(light + 0x100) & 0x20)};
        if (last && now == last->before && !(last->after == last->before)) { // the game set it back: leave it
            skipped = true;
            after = now;
            return 1;
        }
        reinterpret_cast<void(__thiscall*)(void*)>(kLightEntryUpdate)(reinterpret_cast<void*>(entry));
        after = WindowState{*reinterpret_cast<const int*>(entry + 0x1C), static_cast<BYTE>(*reinterpret_cast<const BYTE*>(light + 0x100) & 0x20)};
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        return -1;
    }
}
uint32_t LotIdPart(uintptr_t tracker, int offset);
void RecheckLotWindows(uintptr_t tracker, unsigned pass) {
    if (!kLightEntryUpdate) return;
    std::vector<std::pair<uintptr_t, uintptr_t>> entries;
    std::unordered_set<uintptr_t> seen;
    for (int story = -4; story <= 7; story++) {
        const uintptr_t tl = TreeLevel(tracker, story);
        if (!*reinterpret_cast<const uintptr_t*>(tl)) continue;
        WalkRegistry(tl, [&](uintptr_t entry) {
            if (seen.insert(entry).second) entries.emplace_back(entry, tl);
        });
    }
    const DWORD tick = GetTickCount();
    if (g_windowFlips.size() > 16384) g_windowFlips.clear();
    int checked = 0, changed = 0, takenBack = 0;
    for (const auto& [entry, tl] : entries) {
        const auto flip = g_windowFlips.find(entry);
        const WindowFlip* last = flip != g_windowFlips.end() && tick - flip->second.at < kWindowFlipMs ? &flip->second : nullptr;
        WindowState now{}, after{};
        bool skipped = false;
        if (RecheckWindowEntry(entry, tl, last, now, after, skipped) != 1) continue;
        checked++;
        if (skipped) {
            takenBack++;
            continue;
        }
        if (!(after == now)) {
            changed++;
            g_windowFlips[entry] = WindowFlip{now, after, tick};
        } else if (flip != g_windowFlips.end()) g_windowFlips.erase(flip);
    }
    g_windowsTakenBack.fetch_add(takenBack, std::memory_order_relaxed);
    if (!kPublicBuild)
        LOG_INFO(std::format("[LevelLightShare] Window activation recheck: lot {:08X}, pass {}, {} evaluated, {} changed{}", LotIdPart(tracker, 0x90), pass + 1,
                             checked, changed, takenBack ? std::format(", {} left as the game took them back", takenBack) : std::string()));
}

// A member whose own state 0 is still to come (waiting for its gather or its solve, or picked and not started: sub-step
// +0xEC still 0): it merges with the group's target then, so the ambient pass does not send it again (06/10, recording
// 11:01: that send threw away its gather and its quick pass, and the atrium's stories came last)
bool AmbientMergeAhead(const DepKey& key) {
    BYTE* room = SafeRoomById(key.tracker, key.level, key.room);
    if (!room) return false;
    __try {
        const int state = *reinterpret_cast<const int*>(room + 0xF0);
        return state == 1 || state == 2 || (state == 3 && *reinterpret_cast<const int*>(room + 0xEC) == 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
enum class GroupApply { None, Waiting, Applied };
GroupApply ApplyGroupAmbient(const DepKey& key, unsigned& budget);
bool GroupColourPending(const DepKey& key);
bool ApplyIdleAmbient(const DepKey& key, const Applied& wanted);
void BeforeRoomUpdate(BYTE* tl) {
    const uintptr_t mgr = *reinterpret_cast<const uintptr_t*>(tl);
    if (!mgr) return;
    const uintptr_t tracker = *reinterpret_cast<const uintptr_t*>(tl + 4);
    const int L = *reinterpret_cast<const int*>(tl + 0x1A0);
    if (L < 0 || L > 7 || !tracker || TreeLevel(tracker, L) != reinterpret_cast<uintptr_t>(tl)) return;
    if (L == 0) { // once per lot and update
        // stacked rooms whose normalisation or ambient ramp changed in another member's solve (MergeStackedAmbient)
        std::vector<DepKey> send;
        {
            std::lock_guard<std::mutex> lk(g_ambMx);
            const DWORD now = GetTickCount();
            DepKey attempted[128]; unsigned attempts = 0;
            if (now - g_groupBudgetAt >= 50) { g_groupBudgetAt = now; g_groupBudget = 16; }
            for (auto it = g_ambToQueue.begin(); it != g_ambToQueue.end();)
                if (it->first.tracker == tracker) {
                    if (!AmbientRoomExists(it->first)) {
                        g_ambQueuedAt.erase(it->first);
                        it = g_ambToQueue.erase(it);
                        continue;
                    }
                    if (GroupColourPending(it->first)) {
                        const DepKey root = g_ambGroups.find(it->first)->second.front().key;
                        if (std::find(attempted, attempted + attempts, root) != attempted + attempts) { ++it; continue; }
                        if (attempts < std::size(attempted)) attempted[attempts++] = root;
                    }
                    const GroupApply coordinated = ApplyGroupAmbient(it->first, g_groupBudget);
                    if (coordinated == GroupApply::Waiting) { ++it; continue; }
                    const auto applied = g_ambApplied.find(it->first);
                    const Applied& wanted = it->second;
                    if (applied != g_ambApplied.end() && !Differs(applied->second, wanted.c4, wanted.norm, wanted.wallBase)) {
                        it = g_ambToQueue.erase(it);
                        continue;
                    }
                    if (ApplyIdleAmbient(it->first, wanted)) {
                        g_ambQueuedAt.erase(it->first);
                        it = g_ambToQueue.erase(it);
                        continue;
                    }
                    if (AmbientMergeAhead(it->first)) { ++it; continue; } // its own state 0 merges with the target
                    DWORD& last = g_ambQueuedAt[it->first];
                    if (RoomAmbientPolicy::AmbientUpdateDue(now, last)) {
                        send.push_back(it->first);
                        last = now ? now : 1;
                    }
                    ++it; // a queued/busy room remains pending until a solve confirms convergence
                } else ++it;
        }
        for (const DepKey& k : send)
            if (QueueRoom(tracker, k.level, k.room, true)) g_indoorQueued.fetch_add(1, std::memory_order_relaxed);
        FlushDeferred(tracker);
        FlushDepWaits(tracker);
        bool due = false;
        const long gen = g_indoorGen.load(std::memory_order_relaxed);
        if (g_lots.size() > 4096) g_lots.clear();
        LotState& lot = g_lots[tracker];
        uintptr_t mgrs[8];
        for (int s = 0; s <= 7; s++) mgrs[s] = StoryManager(tracker, s);
        if (std::memcmp(mgrs, lot.mgrs, sizeof mgrs) != 0) { // rebuilt: count its floors and openings from scratch
            const long keepGen = lot.gen;
            lot = LotState{};
            lot.gen = keepGen;
            std::memcpy(lot.mgrs, mgrs, sizeof mgrs);
            due = true;
            g_lotRebuilds.fetch_add(1, std::memory_order_relaxed);
        }
        if (lot.gen != gen) {
            lot.gen = gen;
            due = true;
        }
        const DWORD now = GetTickCount();
        const uintptr_t ground = StoryManager(tracker, 0);
        const int shown = ground ? *reinterpret_cast<const int*>(ground + 0x284) : -99;
        if (static_cast<int32_t>(now - lot.nextCheck) >= 0) {
            lot.nextCheck = now + 2000;
            const int known = FloorObjectsKnown(tracker);
            const int openings = known ? OpeningCount(tracker) : 0;
            if (known > lot.floorsKnown || (openings != lot.openings && lot.openings >= 0)) due = true;
            lot.floorsKnown = known;
            lot.openings = openings;
        }
        if (g_dirtyReady.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lk(g_levelsMx);
            for (int s = 0; s <= 7; s++) {
                const uintptr_t m = StoryManager(tracker, s);
                auto it = m ? std::find(g_dirtyMgrs.begin(), g_dirtyMgrs.end(), m) : g_dirtyMgrs.end();
                if (it != g_dirtyMgrs.end()) {
                    g_dirtyMgrs.erase(it);
                    due = true;
                }
            }
            if (g_dirtyMgrs.empty()) g_dirtyReady = false;
        }
        // A lot that just found its openings sends its rooms near them once more when the boosts, the stacked ambient and
        // the other story's lamps of the first round are all in place: the rooms are then lit the way a lamp moved by hand
        // lights them (user, 29/09: "entering the lot it should already be right"). That is as soon as the first round is
        // over (none of its rooms waits for a solve for kSettleQuiet), at the latest kSettleMax after (atrium house F8,
        // 29/09: the fixed 6 s came 2.5 s after the last solve of the first round; the lot looked right only then).
        constexpr DWORD kSettleMin = 500, kSettleQuiet = 400, kSettleMax = 6000;
        if (due && lot.openings > 0 && !lot.settleAt) { // armed once: later dues do not push it back
            lot.settleAt = (now + kSettleMax) | 1;
            lot.armedAt = now;
            lot.quietSince = 0;
        } else if (lot.settleAt) {
            bool fire = static_cast<int32_t>(now - lot.settleAt) >= 0;
            if (!fire && now - lot.armedAt >= kSettleMin) {
                if (LotBusy(tracker, lot.watch)) lot.quietSince = 0;
                else if (!lot.quietSince) lot.quietSince = now | 1;
                else fire = now - lot.quietSince >= kSettleQuiet;
            }
            if (fire) {
                lot.settleAt = 0;
                due = true;
                g_settles.fetch_add(1, std::memory_order_relaxed);
                if (now - lot.armedAt < kSettleMax - 50) g_settlesEarly.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (due || lot.windowStory != shown) {
            lot.windowStory = shown;
            lot.windowsArmed = now;
            lot.windowPass = 0;
        }
        if (RoomAmbientPolicy::WindowRecheckDue(now, lot.windowsArmed, lot.windowPass)) {
            RecheckLotWindows(tracker, lot.windowPass);
            ++lot.windowPass;
        }
        // the rooms a lamp's move left waiting (never lit by it): once the lamps are quiet, at the scheduler's own pace
        if (!g_editWaits.empty()) {
            const DWORD edit = g_lampEditAt.load(std::memory_order_relaxed);
            const bool quiet = !edit || now - edit >= kAuditQuietMs;
            std::vector<DepKey> go;
            std::erase_if(g_editWaits, [&](const EditWait& w) {
                if (now - w.at > 10000) return true; // its lot is gone
                if (w.key.tracker != tracker || !quiet) return false;
                go.push_back(w.key);
                return true;
            });
            for (const DepKey& k : go)
                if (QueueRoom(tracker, k.level, k.room, true)) g_indoorQueued.fetch_add(1, std::memory_order_relaxed);
        }
        if (due) QueueOpeningRooms(tracker, &lot.watch);
        else if (!lot.auditAt || static_cast<int32_t>(now - lot.auditAt) >= 0) { // (the rooms near openings just sent record again anyway)
            lot.auditAt = (now + kAuditEveryMs) | 1;
            AuditTakers(tracker, now);
        }
    }
}

void* RoomOfTreeLevel(const BYTE* tl, int id) {
    __try {
        void* mgr = *reinterpret_cast<void* const*>(tl);
        return mgr ? reinterpret_cast<RoomById_t>(kRoomById)(mgr, id) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
bool ReadTreeLevelKey(uintptr_t tl, uintptr_t& tracker, int& level) {
    __try {
        tracker = *reinterpret_cast<const uintptr_t*>(tl + 4);
        level = *reinterpret_cast<const int*>(tl + 0x1A0);
        return tracker != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool MarkRoomNow(BYTE* tl, int id) {
    __try {
        reinterpret_cast<MarkRoom_t>(kMarkRoom)(tl, id);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}
// Lamp edits first: the marks held while their room was being solved, for this story only (its own update calls this, so
// the tree level is alive), given back once that solve is over (or after kHoldMaxMs) through the game's own mark, before
// the game's update walks the changed rooms: the walk sends them as it sends any marked room (AfterChangedWalk included).
void FlushHeldMarks(BYTE* tl) {
    struct Give {
        int room;
        bool user;
    };
    Give give[32];
    int n = 0;
    {
        std::lock_guard<std::mutex> lk(g_heldMx);
        if (g_held.empty()) return;
        const DWORD now = GetTickCount();
        for (auto it = g_held.begin(); it != g_held.end();) {
            if (now - it->at > kHoldDropMs) { // its story is gone
                it = g_held.erase(it);
                continue;
            }
            if (it->tl != reinterpret_cast<uintptr_t>(tl)) {
                ++it;
                continue;
            }
            const void* room = RoomOfTreeLevel(tl, it->room);
            if (room && RoomState(room) == 3 && now - it->at < kHoldMaxMs) {
                ++it;
                continue;
            }
            if (room && n < static_cast<int>(std::size(give))) give[n++] = Give{it->room, it->user};
            it = g_held.erase(it);
        }
    }
    for (int k = 0; k < n; k++)
        if (MarkRoomNow(tl, give[k].room)) {
            LevelLightShare::NoteLampMark(reinterpret_cast<uintptr_t>(tl), give[k].room, give[k].user, true); // held only for pure edits
            g_heldGiven.fetch_add(1, std::memory_order_relaxed);
        }
}

void __fastcall RoomUpdateHook(BYTE* tl) {
    if (g_installed.load(std::memory_order_relaxed) && g_indoorReady && tl) {
        FlushHeldMarks(tl);
        __try {
            BeforeRoomUpdate(tl);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_faults.fetch_add(1, std::memory_order_relaxed);
        }
    }
    reinterpret_cast<RoomUpdate_t>(kRoomUpdate)(tl);
}

// The room update walks the rooms changed on its story (the tl+8 set, filled inside the update itself when a lamp entry
// is dirty: FUN_006c7160) and then empties the set (0x6C7497). Just before that, the rooms of the other stories that take
// the lamps of those rooms are sent to gather again (never the story being updated: its pending set is walked next).
// What the rooms of other stories take from a room: each of its lamps (lit flag +0x100 & 0x20, lit colour +0xE0,
// intensity +0x10, head +0x120, range +0x130; the cone of spot types 4 and 5 at +0x170..+0x1A0, so turning a sconce in
// place counts) and its walls (room+0x30: the point test of the other stories tests the lamp's room walls)
// Where each real lamp is and how far it shines, at the last send (window lights, types 7 and 8, are left out: no range
// at +0x130, F8: 0, 1122, 3e-4, 1e-19 on RectangleWindowLight, and they follow the sky). A lamp counts as moved when it is
// more than kMoveMin from where it was at the last send, or its range changed by more than 2%: animated lamps (candles,
// fires: "animated" in the lot light bridge log) wobble every few frames, and as a move they restarted the rooms taking
// them without end (29/09 F8: 1028 such sends while the camera stood still; user: "between indoor floors it got worse,
// some lights dimmer"). A slow drag adds up against the last send, so it is still followed.
struct LampAt {
    uintptr_t light;
    float x, y, z, range;
};
constexpr float kMoveMin = 0.10f; // m
struct DepSig {
    uint64_t all, shape;
    std::vector<LampAt> lamps; // sorted by light
};
std::unordered_map<DepKey, DepSig, DepHash> g_depSig; // last state sent on (under g_depsMx)
std::atomic<long> g_lampWobbles{0};                  // lamp values changed without a move worth sending at once
std::atomic<long> g_depShapeSends{0}; // lamps added, deleted or moved, walls changed: sent at once
std::atomic<long> g_outdoorQuiet{0};   // outside of a floor marked changed with its lamps and walls as they were: no other floor sent
bool LampsMoved(const std::vector<LampAt>& was, const std::vector<LampAt>& now) {
    size_t i = 0;
    for (const LampAt& n : now) {
        while (i < was.size() && was[i].light < n.light) i++;
        if (i == was.size() || was[i].light != n.light) return true;
        const LampAt& w = was[i];
        const float dx = n.x - w.x, dy = n.y - w.y, dz = n.z - w.z;
        if (dx * dx + dy * dy + dz * dz > kMoveMin * kMoveMin) return true; // NaN: not a move
        if (std::fabs(n.range - w.range) > 0.02f * std::max(std::fabs(w.range), 1.0f)) return true;
    }
    return false;
}
// `shape` = which lamps the room has and its walls; the value hash = that plus every lamp field listed above
uint64_t RoomLampSignature(uintptr_t tl, int room, uint64_t* shapeOut, std::vector<LampAt>* lampsOut) {
    uint64_t h = 1469598103934665603ull, shape = h;
    const auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    const auto mixShape = [&](uint64_t v) {
        mix(v);
        shape = (shape ^ v) * 1099511628211ull;
    };
    const auto dwords = [&mix](uintptr_t at, int n) {
        for (int k = 0; k < n; k++) mix(*reinterpret_cast<const uint32_t*>(at + k * 4));
    };
    lampsOut->clear();
    WalkRegistry(tl, [&](uintptr_t entry) {
        if (*reinterpret_cast<const int*>(entry + 0x1C) != room) return;
        const uintptr_t light = *reinterpret_cast<const uintptr_t*>(entry + 0x24);
        if (!light) return;
        mixShape(light);
        mix(*reinterpret_cast<const BYTE*>(light + 0x100) & 0x20);
        // the object's flags the gather's checks read (GameTakesLight): a lamp the other stories could not take for a moment
        // and that ends as it was still changes this, so they gather again (05/10, see AuditTakers)
        if (const uintptr_t info = *reinterpret_cast<const uintptr_t*>(entry + 0x20)) mix(*reinterpret_cast<const BYTE*>(info + 0x90) & 0x6);
        dwords(light + 0x10, 4);
        dwords(light + 0xE0, 3);
        dwords(light + 0x120, 3);
        dwords(light + 0x130, 1);
        const int type = *reinterpret_cast<const int*>(light + 0xB0);
        if (type == 4 || type == 5) dwords(light + 0x170, 13);
        if (type != 7 && type != 8 && lampsOut->size() < 4096) {
            const float* p = reinterpret_cast<const float*>(light + 0x120);
            lampsOut->push_back(LampAt{light, p[0], p[1], p[2], *reinterpret_cast<const float*>(light + 0x130)});
        }
    });
    std::sort(lampsOut->begin(), lampsOut->end(), [](const LampAt& a, const LampAt& b) { return a.light < b.light; });
    void* mgr = *reinterpret_cast<void* const*>(tl);
    if (const BYTE* r = mgr ? static_cast<const BYTE*>(reinterpret_cast<RoomById_t>(kRoomById)(mgr, room)) : nullptr) {
        const uintptr_t b = *reinterpret_cast<const uintptr_t*>(r + 0x30), e = *reinterpret_cast<const uintptr_t*>(r + 0x34);
        mixShape(e >= b ? (e - b) / 4 : 0);
        for (uintptr_t p = b; p < e && p - b < 1024 * 4; p += 4) mixShape(*reinterpret_cast<const uint32_t*>(p));
    }
    *shapeOut = shape;
    return h;
}
// Compares a room's lamps with the last send and records the new state: 0 = nothing changed, 1 = values only (switched,
// dimmed, recoloured, turned, wobbling), 2 = sent at once, stopping a solve in progress (first sight, a lamp added or
// deleted, a wall changed: the taking rooms' lists may point to a lamp being deleted), 3 = sent at once, a solve in progress
// kept (the same lamps, one moved: every pointer stays valid and the solve reads where the lamp is). Caller holds g_depsMx.
int LampChange(const DepKey& key, uintptr_t tl, std::vector<uintptr_t>* lightsOut = nullptr) {
    uint64_t shape = 0;
    std::vector<LampAt> lamps;
    const uint64_t sig = RoomLampSignature(tl, key.room, &shape, &lamps);
    if (lightsOut) // the room's lamps now (window lights left out, as in the signature's list)
        for (const LampAt& l : lamps) lightsOut->push_back(l.light);
    if (g_depSig.size() > 8192) g_depSig.clear();
    auto it = g_depSig.find(key);
    if (it == g_depSig.end()) {
        g_depSig.emplace(key, DepSig{sig, shape, std::move(lamps)});
        return 2;
    }
    DepSig& s = it->second;
    if (s.all == sig) return 0;
    const bool reshaped = s.shape != shape;
    const bool moved = reshaped || LampsMoved(s.lamps, lamps);
    s.all = sig;
    s.shape = shape;
    if (moved) {
        s.lamps = std::move(lamps); // else the positions of the last send stay: a slow drag adds up
        g_depShapeSends.fetch_add(1, std::memory_order_relaxed);
        return reshaped ? 2 : 3;
    }
    g_lampWobbles.fetch_add(1, std::memory_order_relaxed);
    return 1;
}

// Rooms taking lamps whose values changed: the first change sends them at once (without stopping a solve in progress,
// QueueRoom's defer), changes within kDepRecent of a send wait until the lamps are quiet for kDepQuiet (at most kDepMaxWait
// after the first wait), then send them once more (FlushDepWaits, from the room update)
constexpr DWORD kDepQuiet = 300, kDepRecent = 1000, kDepMaxWait = 1500;
struct DepWait {
    DWORD first, due;
};
std::unordered_map<DepKey, DepWait, DepHash> g_depWait;  // (lot, story, room) of the taking room (under g_depsMx)
std::unordered_map<DepKey, DWORD, DepHash> g_depSentAt; // when it was last sent for a value change (under g_depsMx)
std::atomic<long> g_depCoalesced{0};                     // value changes folded into a later send

void FlushDepWaits(uintptr_t tracker) {
    std::vector<DepKey> send;
    {
        std::lock_guard<std::mutex> lk(g_depsMx);
        if (g_depWait.empty()) return;
        const DWORD now = GetTickCount();
        for (auto it = g_depWait.begin(); it != g_depWait.end();)
            if (now - it->second.first > 10000) // its lot is gone or never updated again
                it = g_depWait.erase(it);
            else if (it->first.tracker == tracker && (static_cast<int32_t>(now - it->second.due) >= 0 || now - it->second.first >= kDepMaxWait)) {
                send.push_back(it->first);
                g_depSentAt[it->first] = now;
                it = g_depWait.erase(it);
            } else
                ++it;
    }
    for (const DepKey& k : send)
        if (QueueRoom(k.tracker, k.level, k.room, true)) g_indoorQueued.fetch_add(1, std::memory_order_relaxed);
}
bool DepWaitsFor(uintptr_t tracker) {
    std::lock_guard<std::mutex> lk(g_depsMx);
    return std::any_of(g_depWait.begin(), g_depWait.end(), [&](const auto& kv) { return kv.first.tracker == tracker; });
}

void AfterChangedWalk(BYTE* tl) {
    if (!*reinterpret_cast<const uintptr_t*>(tl)) return;
    const uintptr_t tracker = *reinterpret_cast<const uintptr_t*>(tl + 4);
    const int L = *reinterpret_cast<const int*>(tl + 0x1A0);
    if (L < 0 || L > 7 || !tracker || TreeLevel(tracker, L) != reinterpret_cast<uintptr_t>(tl)) return;
    int ids[512];
    const int n = ChangedRooms(tl, ids, 512);
    // The lamp edits LampMarkFilter noted for this story in this update (taken even when the walk found nothing: they
    // belong to this update)
    LampMarkNote notes[64];
    int noted = 0;
    {
        std::lock_guard<std::mutex> lk(g_lampMarkMx);
        for (auto it = g_lampMarks.begin(); it != g_lampMarks.end();)
            if (it->tl == reinterpret_cast<uintptr_t>(tl)) {
                if (noted < static_cast<int>(std::size(notes))) notes[noted++] = *it;
                it = g_lampMarks.erase(it);
            } else
                ++it;
    }
    if (!n) return;
    const auto noteOf = [&](int id) -> const LampMarkNote* {
        for (int k = 0; k < noted; k++)
            if (notes[k].room == id) return &notes[k];
        return nullptr;
    };
    // sent now stopping a solve in progress / sent now keeping one (a lamp moved, a player's edit) / now or after the burst
    std::vector<std::pair<int, int>> now, kept, later;
    std::vector<int> own; // rooms of this story a lamp edit changed: the walk just sent them; urgent
    // Gathered at once (see "Lamp edits first"): this story's rooms whose registered lamp moved or changed a value, with
    // no lamp added or removed, and the rooms of other stories they send
    std::vector<int> ownSoon, ownLate;
    std::vector<std::pair<int, int>> soonRooms;
    // the lamps of the rooms whose lamp moved (rooms they never lit wait, g_litBy), and the rooms a player's value edit sends
    std::vector<uintptr_t> movedLights;
    std::vector<std::pair<int, int>> atOnce;
    // The outdoor rooms of the other stories (room 0 and the roofless rooms, 05/10): they take this story's outdoor lamps
    std::vector<std::pair<int, int>> outdoorRooms;
    const uintptr_t mgrL = *reinterpret_cast<const uintptr_t*>(tl);
    for (int S = 0; S <= 7; S++) {
        const uintptr_t mgrS = S == L ? 0 : *reinterpret_cast<const uintptr_t*>(TreeLevel(tracker, S));
        if (!mgrS) continue;
        outdoorRooms.emplace_back(S, 0);
        if (g_indoorReady) {
            int roofless[256];
            const int nr = RooflessRoomIds(mgrS, roofless, static_cast<int>(std::size(roofless)));
            for (int k = 0; k < nr; k++) outdoorRooms.emplace_back(S, roofless[k]);
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_depsMx);
        for (int i = 0; i < n; i++) {
            if (ids[i] < 0) continue;
            const LampMarkNote* note = noteOf(ids[i]);
            if (note || noted) own.push_back(ids[i]); // the lamp's room, and the neighbours the game's mark sent with it
            if (note && note->pure) ownSoon.push_back(ids[i]); // a lamp registered in it moved, switched or changed a value
            const DepKey key{tracker, L, ids[i]};
            const auto deps = g_deps.find(key); // rooms of other stories taking its lamps near an opening (room 0 too)
            const bool outdoor = ids[i] == 0 || (g_indoorReady && RooflessRoom(mgrL, ids[i])); // its lamps light the other stories
            if (!outdoor && deps == g_deps.end()) continue;
            // only when what the other stories take from it really changed (RoomLampSignature). The game marks a room
            // changed for more than a lamp change (the light entry update 0x6C7BA0 does it for any lit lamp whose entry
            // is updated, changed or not; the exact trigger in the test house is not known), and two rooms taking each
            // other's lamps then sent each other to gather again without end (atrium house: rooms 19 and 20 of stories 1
            // and 2, dozens of gathers in a row in the F8, each resetting their lighting LOD to 0)
            std::vector<uintptr_t> lights;
            int change = LampChange(key, reinterpret_cast<uintptr_t>(tl), &lights);
            const bool moved = change == 3; // the same lamps, one moved (a drag): see g_litBy
            if (!change) {
                if (ids[i] == 0) g_outdoorQuiet.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            if (outdoor) own.push_back(ids[i]); // its outdoor lamps changed (a lamp deleted: no note, the removal marks it)
            const bool soon = note && note->pure && change != 2; // a lamp registered in it changed, none added or removed
            if (change == 2) ownLate.push_back(ids[i]); // a lamp of it added or removed: its gather waits for the game
            if (change == 1 && note && note->user) change = 3; // colour, intensity, on / off by a player or a Sim: at once
            auto& out = change == 2 ? now : change == 3 ? kept : later;
            const size_t from = out.size();
            // an outdoor room of this floor: the outdoor rooms of the other floors 0..7 take its lamps (part 2)
            if (outdoor) out.insert(out.end(), outdoorRooms.begin(), outdoorRooms.end());
            if (deps != g_deps.end()) out.insert(out.end(), deps->second.begin(), deps->second.end());
            if (soon) soonRooms.insert(soonRooms.end(), out.begin() + static_cast<std::ptrdiff_t>(from), out.end());
            if (moved) movedLights.insert(movedLights.end(), lights.begin(), lights.end());
            else if (change == 3) atOnce.insert(atOnce.end(), out.begin() + static_cast<std::ptrdiff_t>(from), out.end()); // a player's value
        }
        std::sort(atOnce.begin(), atOnce.end());
        std::sort(soonRooms.begin(), soonRooms.end());
        for (auto* v : {&now, &kept, &later}) { // a room taking lamps of several changed rooms: once
            std::sort(v->begin(), v->end());
            v->erase(std::unique(v->begin(), v->end()), v->end());
        }
        const auto in = [](const std::vector<std::pair<int, int>>& v, const std::pair<int, int>& p) { return std::binary_search(v.begin(), v.end(), p); };
        std::erase_if(kept, [&](const std::pair<int, int>& p) { return in(now, p); });
        std::erase_if(later, [&](const std::pair<int, int>& p) { return in(now, p) || in(kept, p); });
        if (!later.empty()) { // value changes: the first one goes now, the rest of the burst waits (FlushDepWaits)
            const DWORD tick = GetTickCount();
            if (g_depWait.size() > 8192) g_depWait.clear();
            if (g_depSentAt.size() > 8192) g_depSentAt.clear();
            std::erase_if(later, [&](const std::pair<int, int>& p) {
                const DepKey k{tracker, p.first, p.second};
                if (p.first == L) return true;
                if (const auto w = g_depWait.find(k); w != g_depWait.end()) {
                    w->second.due = tick + kDepQuiet;
                    g_depCoalesced.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
                if (const auto sent = g_depSentAt.find(k); sent != g_depSentAt.end() && tick - sent->second < kDepRecent) {
                    g_depWait[k] = DepWait{tick, tick + kDepQuiet};
                    g_depCoalesced.fetch_add(1, std::memory_order_relaxed);
                    return true;
                }
                g_depSentAt[k] = tick;
                return false;
            });
        }
        for (const auto* v : {&now, &kept})
            for (const auto& [S, r] : *v) g_depWait.erase(DepKey{tracker, S, r}); // sent now: nothing left to wait for
    }
    // Lamp edits first: this story's rooms the edit changed were just sent by the game's walk (state 1, countdown 5): the
    // scheduler takes them first, and for a registered lamp that moved or changed a value the pending walk that follows in
    // this same update gathers them (a room in both lists waits: a lamp of it was added or removed)
    std::sort(own.begin(), own.end());
    own.erase(std::unique(own.begin(), own.end()), own.end());
    for (int id : own)
        if (void* room = RoomOfTreeLevel(tl, id)) {
            const LampMarkNote* note = noteOf(id);
            const bool soon = std::find(ownSoon.begin(), ownSoon.end(), id) != ownSoon.end() && std::find(ownLate.begin(), ownLate.end(), id) == ownLate.end();
            MarkUrgent(room, note || id == 0 ? 0 : 1, soon);
            if (soon) GatherSoon(room);
        }
    const int tier = own.empty() ? -1 : 1; // the rooms of other stories taking the edited lamps: right after the lamp's own
    for (const auto& [S, r] : now)
        if (S != L && QueueRoom(tracker, S, r, false, tier, false)) g_indoorQueued.fetch_add(1, std::memory_order_relaxed);
    const DWORD waitAt = GetTickCount();
    for (const auto& p : kept) {
        if (p.first == L) continue;
        // a moved lamp: an indoor room it never lit waits until the lamps are quiet (its light does not change while the
        // lamp is behind the floors; the wait sends it once, in case the move brought the lamp into view)
        const DepKey dk{tracker, p.first, p.second};
        const bool outdoorDep = p.second == 0 || (g_indoorReady && RooflessRoom(StoryManager(tracker, p.first), p.second));
        if (!outdoorDep && !std::binary_search(atOnce.begin(), atOnce.end(), p) && !LitByAny(dk, movedLights.data(), movedLights.size())) {
            const auto w = std::find_if(g_editWaits.begin(), g_editWaits.end(), [&](const EditWait& e) { return e.key == dk; });
            if (w != g_editWaits.end()) w->at = waitAt;
            else if (g_editWaits.size() < 1024) g_editWaits.push_back(EditWait{dk, waitAt});
            g_editWaited.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (QueueRoom(tracker, p.first, p.second, true, tier, std::binary_search(soonRooms.begin(), soonRooms.end(), p)))
            g_indoorQueued.fetch_add(1, std::memory_order_relaxed);
    }
    for (const auto& [S, r] : later)
        if (QueueRoom(tracker, S, r, true)) g_indoorQueued.fetch_add(1, std::memory_order_relaxed);
}

using SetClear_t = void(__thiscall*)(void* set, uintptr_t buckets, uintptr_t count);
void __fastcall ChangedClearHook(BYTE* set, void*, uintptr_t buckets, uintptr_t count) {
    if (g_installed.load(std::memory_order_relaxed) && g_indoorReady && set) {
        __try {
            AfterChangedWalk(set - 8);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            g_faults.fetch_add(1, std::memory_order_relaxed);
        }
    }
    reinterpret_cast<SetClear_t>(kChangedClear)(set, buckets, count);
}

struct GatherStamp { uintptr_t mgr; int id; DWORD started; uint32_t serial; };
std::mutex g_gatherStampMx;
std::unordered_map<uintptr_t, GatherStamp> g_gatherStamps;
uint32_t g_gatherSerial = 0; // under g_gatherStampMx: one per gather (never 0)
uint32_t g_markSerial = 0;   // under g_gatherStampMx: g_gatherSerial at the latest lamp mark (NoteLampMark)
void NoteGatherStamp(BYTE* room, DWORD started) {
    const uintptr_t mgr = *reinterpret_cast<const uintptr_t*>(room);
    const int id = *reinterpret_cast<const int*>(room + 0xC);
    std::lock_guard<std::mutex> lock(g_gatherStampMx);
    if (g_gatherStamps.size() > 8192) g_gatherStamps.clear();
    if (++g_gatherSerial == 0) g_gatherSerial = 1;
    g_gatherStamps[reinterpret_cast<uintptr_t>(room)] = {mgr, id, started, g_gatherSerial};
}
uint32_t GatherSerialImpl(const BYTE* room, uintptr_t mgr, int id) {
    std::lock_guard<std::mutex> lock(g_gatherStampMx);
    const auto it = g_gatherStamps.find(reinterpret_cast<uintptr_t>(room));
    return it != g_gatherStamps.end() && it->second.mgr == mgr && it->second.id == id ? it->second.serial : 0;
}
uint32_t GatherSerial(const BYTE* room) {
    uintptr_t mgr = 0;
    int id = 0;
    __try {
        mgr = *reinterpret_cast<const uintptr_t*>(room);
        id = *reinterpret_cast<const int*>(room + 0xC);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return GatherSerialImpl(room, mgr, id);
}
void __fastcall OutdoorGather(BYTE* treeLevel, void*, BYTE* room) {
    const DWORD started = GetTickCount();
    reinterpret_cast<AddWorldLights_t>(kAddWorldLights)(treeLevel, room);
    if (!g_installed.load(std::memory_order_relaxed) || !room) return;
    __try {
        NoteRoomStructure(room);
        ShareOutdoorLights(treeLevel, room);
        ShareIndoorLights(treeLevel, room);
        ShareRooflessLights(treeLevel, room); // after the indoor share, which starts the room's record again
        NoteGatherStamp(room, started);
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
    bool basis = false;             // basis builder has no native wall test: include the recipient segment too
    BYTE soft = 0;                  // the solving room's +0x639 (FUN_0069fc40 reads it: wall height test / soft shadows of this pass)
    float thr = 0.0f;               // the solving room's +0x63C: the game drops a light whose r+g+b is under it (0x69FE40)
    // The lamp of another story IndoorShadow just let through: the game's own wall test of the solving room that follows
    // for it (0x69FE93, called with the same lamp position) starts where its ray enters this room's story (GameWallTest)
    uintptr_t enterLight = 0;
    float enterLamp[3] = {}, enterAt[3] = {};
    bool enterHard = false; // that test with the walls' real ends (no soft penumbra): an outdoor lamp of a lower story (OutdoorEntry)
};
SolveCtx g_ctx;
bool g_enterReady = false; // GameWallTest is in (Install)
std::atomic<long> g_enterTests{0}, g_outdoorEnters{0}, g_penumbraLifted{0};
struct BatchCentre {
    uintptr_t begin = 0, end = 0;
    alignas(16) float c[4] = {};
} g_batch;
bool g_copyInBatch = false; // a wrapped copy of a batch sample is being solved (SolvePointBatch, light tree thread): it stands for that sample
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
struct DiagRec {
    int level, home; // home = -1: the room's own light (or a world light)
    uintptr_t light;
    float lpos[3], p[3], n[3], lum;
    float mine = -1.0f; // our share (cross-floor lights), -1 = not tested
    int game = -1;      // the game's wall test for this sample: 1 passed, 0 blocked, -1 not run
    float gameT = 1.0f;
    bool batch = false, culledList = false;
    int type = -1; // light+0xB0 when recorded
    int room = 0;  // the lit room (0 = outside)
    // indoor lamps (4.): where the ray crossed the floor between the stories (-1 = not reached), what was there, and why the
    // light stopped (0 passed, 1 floor, 2 wall of the lamp's room)
    int why = -1, cx = -1, cz = -1, cq = -1, belowRoom = -2;
    uint32_t keyHi = 0, keyLo = 0;
    float ch = 0.0f, ct = -1.0f;
};
std::mutex g_diagMx;
std::vector<DiagRec> g_diag;
std::atomic<long> g_diagSeen{0};
int g_diagIndoorUsed = 0, g_diagOutdoorUsed = 0; // records per part since the last dump (under g_diagMx)
std::atomic<bool> g_diagFull[2]{};
// What IndoorPassImpl met at the floor crossing, for the next Diag record (light tree thread; development build)
struct PassDebug {
    int cx = -1, cz = -1, cq = -1, belowRoom = -2;
    uint32_t keyHi = 0, keyLo = 0;
    float ch = 0.0f, ct = -1.0f;
} g_passDbg;
int g_lastRec = -1; // record of the light evaluated last (the game's wall test for it comes right after)

bool RoomStillSame(const RoomInfo& info, const BYTE* room) {
    __try {
        return *reinterpret_cast<const uintptr_t*>(room) == info.mgr && *reinterpret_cast<const uintptr_t*>(TreeLevel(info.tracker, info.level)) == info.mgr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const RoomInfo* SolveInfo(BYTE* room) {
    if (!g_installed.load(std::memory_order_relaxed) || g_rooms.empty()) return nullptr; // (SolvePoint: gather thread only)
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

// Development build, F8 "SEAM": the wall samples of the rooms of an atrium group within 1 m of the floor line between
// their stories, with the summed light (LightPointWithAllLights, before the solve's normalisation and curve) and what the
// normalisation and the curve FUN_0069ec40 make of it, to find which stage leaves a step at the line
struct SeamRec {
    int room, level, cls; // cls: the room's LOD class (room+0xF4) at that solve
    float p[3], n[3], sum, norm, fin, base;
};
std::mutex g_seamMx;
std::vector<SeamRec> g_seam;
std::atomic<bool> g_seamFull{false};
struct RecordedSeam { DWORD tick; uint32_t lot; SeamRec sample; float rgb[3]; };
constexpr size_t kRecordedSeamLimit = 8192;
std::mutex g_recordedSeamMx;
std::vector<RecordedSeam> g_recordedSeams;
size_t g_recordedSeamNext = 0;
std::atomic<bool> g_recordSeams{false};
std::atomic<uint32_t> g_recordedSeamEpoch{0};
DWORD g_recordedSeamStart = 0;
bool MakeSeamRec(BYTE* room, const float* out, const float* sample, SeamRec& r) {
    __try {
        if (std::fabs(sample[5]) > 0.3f) return false; // walls only (floor and ceiling normals are vertical)
        const uintptr_t mgr = *reinterpret_cast<const uintptr_t*>(room);
        const int S = *reinterpret_cast<const int*>(mgr + 0x88);
        const float base = *reinterpret_cast<const float*>(mgr + 0x98); // this story's lowest floor (world)
        const float y = sample[1];
        // near this story's floor (the line with the story below) or 3 m above it (the line with the story above)
        if (std::fabs(y - base) > 1.0f && std::fabs(y - (base + 3.0f)) > 1.0f) return false;
        const float norm = *reinterpret_cast<const float*>(room + 0x160);
        float c[3], lum = 0.0f;
        for (int k = 0; k < 3; k++) lum += (c[k] = out[k] * norm) / 3.0f;
        const float curve = lum >= 0.01f ? (2.0f / (1.0f + std::exp(-lum)) - 1.0f) / lum : 1.0f; // FUN_0069ec40
        r = SeamRec{*reinterpret_cast<const int*>(room + 0xC), S, *reinterpret_cast<const int*>(room + 0xF4), {sample[0], y, sample[2]}, {sample[4], sample[5], sample[6]}, out[0] + out[1] + out[2], norm,
                    (c[0] + c[1] + c[2]) * curve, base};
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void RecordSeam(BYTE* room, const float* out, const float* sample) {
    if (g_seamFull.load(std::memory_order_relaxed)) return;
    SeamRec r{};
    if (!MakeSeamRec(room, out, sample, r)) return;
    std::lock_guard<std::mutex> lk(g_seamMx);
    if (g_seam.size() < 30000) g_seam.push_back(r);
    if (g_seam.size() >= 30000) g_seamFull.store(true, std::memory_order_relaxed);
}

uint32_t LotIdPart(uintptr_t tracker, int offset);
void RecordRequestedSeam(BYTE* room, const float* out, const float* sample) {
    if (!g_recordSeams.load(std::memory_order_relaxed) || room[0x18]) return;
    const uint32_t epoch = g_recordedSeamEpoch.load(std::memory_order_relaxed);
    SeamRec record{};
    if (!MakeSeamRec(room, out, sample, record)) return;
    // Keep actual joined rows only, rather than every texel in the metre around them.
    if (std::fabs(record.p[1] - record.base) > 0.025f
        && std::fabs(record.p[1] - record.base - 3.0f) > 0.025f) return;
    const uintptr_t tracker = MgrTracker(*reinterpret_cast<const uintptr_t*>(room));
    if (!tracker) return;
    RecordedSeam entry{GetTickCount(), LotIdPart(tracker, 0x90), record, {out[0], out[1], out[2]}};
    std::lock_guard<std::mutex> lock(g_recordedSeamMx);
    if (!g_recordSeams.load(std::memory_order_relaxed) || epoch != g_recordedSeamEpoch.load(std::memory_order_relaxed)) return;
    if (g_recordedSeams.size() < kRecordedSeamLimit) g_recordedSeams.push_back(entry);
    else {
        g_recordedSeams[g_recordedSeamNext] = entry;
        g_recordedSeamNext = (g_recordedSeamNext + 1) % kRecordedSeamLimit;
    }
}

float* SolvePoint(BYTE* room, float* out, void* list2D, void* list3D, void* flags, void* sample, bool batch) {
    // g_ctx belongs to the light tree thread: a solve on any other thread leaves it alone (it would clear the context of a
    // solve in progress there and let its lamps of another story through the floor)
    const DWORD gatherThread = g_gatherThread.load(std::memory_order_relaxed);
    if (ThreadId() != gatherThread) {
        if (gatherThread && g_installed.load(std::memory_order_relaxed)) g_otherThread.fetch_add(1, std::memory_order_relaxed);
        return reinterpret_cast<SolvePoint_t>(kSolvePoint)(room, out, list2D, list3D, flags, sample);
    }
    const SolveCtx prev = g_ctx;
    g_ctx.info = SolveInfo(room);
    g_ctx.list2D = list2D;
    g_ctx.flags = static_cast<const char*>(flags);
    g_ctx.batch = batch && g_ctx.info && (g_copyInBatch || BatchCentreFor(sample));
    g_ctx.soft = room[0x639];
    g_ctx.thr = *reinterpret_cast<const float*>(room + 0x63C);
    g_ctx.enterLight = 0;
    g_lastRec = -1;
    float* r = reinterpret_cast<SolvePoint_t>(kSolvePoint)(room, out, list2D, list3D, flags, sample);
    if (batch && !g_ghostSolve && g_recordSeams.load(std::memory_order_relaxed)) RecordRequestedSeam(room, out, static_cast<const float*>(sample));
    if (!kPublicBuild)
        if (batch && !g_ghostSolve && g_diagArmed.load(std::memory_order_relaxed) && g_wallBase.count(reinterpret_cast<uintptr_t>(room))) RecordSeam(room, out, static_cast<const float*>(sample));
    g_ctx = prev;
    return r;
}
// Walls block light on outdoor floors (user 05/10, F7 + screenshots: a wall lamp outside lit the floor of a yard with no
// roof behind the wall, and an upper deck behind its half wall). Outdoor floors (room 0 and roofless rooms, which the
// game solves as outdoor, +0x18) are drawn with max(floor map, ground atlas) by lot_light_bridge DrawFloorAtlas; the
// atlas carries the outdoor lamps (room 0's list) with no walls, and the game solves floor texels without a wall test.
// So each outdoor floor texel of a story >= 1 also gets, in its map's alpha (0 for every outdoor texel in the game), the
// share of room 0's lamp light that walls block there: room 0's list evaluated at the texel with the normal wrapped
// (w = 1: N.L ignored, only walls decide; a lamp below a floor with a clear path still counts), once with the 2D wall
// test (all of room 0's walls, plus this file's cross-story test) and once without. The floor shader takes
// atlas x (1 - alpha). Maps solved before keep alpha 0 and the whole atlas.
// The alpha reaches the texel through kOutdoorAlpha (0x006A333B: "cmp byte [ebx+18h],0; xorps xmm1,xmm1", xmm1 = an
// outdoor texel's alpha), which now loads out[3]; out[3] = 0 for every other outdoor sample, as the game had it.
constexpr uintptr_t kOutdoorAlpha = 0x006A333B;
const BYTE kOutdoorAlphaBytes[] = {0x80, 0x7B, 0x18, 0x00, 0x0F, 0x57, 0xC9, 0x0F, 0x85}; // cmp byte [ebx+18h],0; xorps xmm1,xmm1; jne
bool g_floorMaskReady = false;
std::atomic<bool> g_floorWallsOn{true};
__declspec(naked) void OutdoorAlphaThunk() {
    __asm {
        movss xmm1, dword ptr [esp + 30h] // out[3]: out is [esp+20h] in FUN_006a31d0, +4 for this call's return address
        cmp byte ptr [ebx + 18h], 0
        ret
    }
}
float Lum3(const float* c) { return c[0] * 0.2126f + c[1] * 0.7152f + c[2] * 0.0722f; }
// Walls block light on objects (user 05/10, F7: a telescope in a yard without a roof, behind a wall, took the red wall
// lamp outside through Apex's per-pixel object lamps; the game's own rig had that lamp too). Objects are drawn on the
// render thread and the walls live on the light tree thread, so room 0's walls of each story (its LightingWall list
// room+0xD8..+0xDC: every outside face, yards included) are copied here when that room is solved: start +0x110 (world),
// along +0xF0 (world), 3 m high ([0x00FF37DC], the height the wall samples use). WallBlocks tests a lamp-to-object
// segment against the copy on the render thread.
struct WallSeg {
    float x0, z0, x1, z1, y0, y1;
};
std::mutex g_wallSnapMx;
std::unordered_map<uintptr_t, std::vector<WallSeg>> g_wallSnap; // story manager -> its outside walls
std::atomic<uint32_t> g_wallSnapGen{1};
std::atomic<bool> g_objectWallsOn{true};
constexpr int kMaxStoryWalls = 4096;
WallSeg g_wallRead[kMaxStoryWalls]; // light tree thread only
// -1 when the room cannot be read
int ReadStoryWalls(const BYTE* room0, uintptr_t& mgr) {
    int n = 0;
    __try {
        mgr = *reinterpret_cast<const uintptr_t*>(room0);
        const uintptr_t b = *reinterpret_cast<const uintptr_t*>(room0 + 0xD8), e = *reinterpret_cast<const uintptr_t*>(room0 + 0xDC);
        if (!mgr || e < b || (e - b) / 4 > static_cast<uintptr_t>(kMaxStoryWalls)) return -1;
        for (uintptr_t p = b; p < e; p += 4) {
            const uintptr_t w = *reinterpret_cast<const uintptr_t*>(p);
            if (!w) continue;
            const float* o = reinterpret_cast<const float*>(w + 0x110);
            const float* d = reinterpret_cast<const float*>(w + 0xF0);
            const float* nn = reinterpret_cast<const float*>(w + 0x150); // the face normal: the origin sits 5 cm out on it, the wall line is behind
            if (!std::isfinite(o[0]) || !std::isfinite(o[1]) || !std::isfinite(o[2]) || !std::isfinite(d[0]) || !std::isfinite(d[2]) || !std::isfinite(nn[0]) || !std::isfinite(nn[2])) continue;
            const float cx = o[0] - nn[0] * 0.05f, cz = o[2] - nn[2] * 0.05f;
            g_wallRead[n++] = {cx, cz, cx + d[0], cz + d[2], o[1], o[1] + 3.0f};
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return n;
}
void CaptureStoryWalls(BYTE* room0) {
    uintptr_t mgr = 0;
    const int n = ReadStoryWalls(room0, mgr);
    if (n < 0) return;
    std::vector<WallSeg> segs(g_wallRead, g_wallRead + n);
    // the wall tests at the heights the walls are drawn at (a house on a foundation: about 2 m under +0x114; a lamp outside
    // lit a closed roofless room under walls tested 2 m too high, 06/10)
    for (WallSeg& sg : segs) {
        float foot = 0.0f;
        if (WallHeights::DrawnFoot(sg.x0, sg.z0, sg.x1, sg.z1, 0.05f, sg.y0, foot) && std::fabs(foot - sg.y0) > 0.02f && std::fabs(foot - sg.y0) < 4.0f) {
            sg.y1 += foot - sg.y0;
            sg.y0 = foot;
        }
    }
    std::lock_guard<std::mutex> lk(g_wallSnapMx);
    auto& slot = g_wallSnap[mgr];
    const bool same = slot.size() == segs.size() && (segs.empty() || std::memcmp(slot.data(), segs.data(), segs.size() * sizeof(WallSeg)) == 0);
    if (same) return;
    slot = std::move(segs);
    g_wallSnapGen.fetch_add(1, std::memory_order_relaxed);
}

// Room 0 of the room's story when that story is 1 or higher (story 0 is the lot's grass, which floor shaders never read)
BYTE* MaskedFloorRoom0(BYTE* room) {
    __try {
        const uintptr_t mgr = *reinterpret_cast<const uintptr_t*>(room);
        if (*reinterpret_cast<const int*>(mgr + 0x88) < 1) return nullptr;
        return static_cast<BYTE*>(reinterpret_cast<RoomById_t>(kRoomById)(reinterpret_cast<void*>(mgr), 0));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// The per-light 2D wall lists the game builds for a wall batch (FUN_006a30b0 -> FUN_0069dff0 from the batch centre to each
// light of the room's list, only for batches of more than 3 samples), built here in our own memory for the outdoor floor
// batches the mask tests (05/10). With no list FUN_0069d4c0 tests every wall of the story (room+0x78), for every lamp and
// every floor texel, twice per texel: a room 0 with decks solved many times slower, and its lamps followed an edit late. A
// floor batch is one piece of a tile (FUN_006a3500 -> FUN_006aabe0), smaller than the wall pieces the game culls this way.
// Layout read by LightPointWithAllLights (list2D->begin + i * 0x10) and FUN_0069d4c0 ({begin, end} of wall indices).
struct WallList {
    int* b;
    int* e;
    int* c;
    void* pad;
};
static_assert(sizeof(WallList) == 16, "LightPointWithAllLights steps 0x10 per light");
struct WallLists {
    WallList* b;
    WallList* e;
    WallList* c;
    void* pad;
};
struct BatchLists {
    uintptr_t room = 0;
    uint32_t serial = 0;
    bool ok = false;
    std::vector<int> pool;
    std::vector<WallList> lists;
    WallLists head{};
};
BatchLists g_maskLists[2]; // [0] the floor's room, [1] room 0 of its story (light tree thread only)
uint32_t g_batchSerial = 0; // light tree thread: bumped at the first sample of every batch (SolvePointBatch)
std::atomic<long> g_maskListBatches{0}, g_maskListFallbacks{0};

bool ReadLightsAndWalls(const BYTE* room, uintptr_t* lights, int max, int& n, size_t& walls) {
    __try {
        const uintptr_t* lb = *reinterpret_cast<const uintptr_t* const*>(room + 0xC8);
        const uintptr_t* le = *reinterpret_cast<const uintptr_t* const*>(room + 0xCC);
        const uintptr_t wb = *reinterpret_cast<const uintptr_t*>(room + 0x30), we = *reinterpret_cast<const uintptr_t*>(room + 0x34);
        if (!lb || le < lb || le - lb > max || we < wb) return false;
        n = static_cast<int>(le - lb);
        for (int i = 0; i < n; i++) lights[i] = lb[i];
        walls = (we - wb) / 4;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CullForLight(BYTE* room, uintptr_t light, const float* centre, int* base, size_t cap, WallList& out) {
    __try {
        alignas(16) float pos[4];
        reinterpret_cast<LightPos_t>(kLightPos)(reinterpret_cast<void*>(light), pos);
        IntVec v{base, base, base + cap}; // room for every wall: FUN_0069dff0 never grows it
        reinterpret_cast<WallCull_t>(kWallCull)(room + 0x30, &v, centre, pos);
        if (v.b != base || v.e < base || v.e > base + cap) return false;
        out = WallList{v.b, v.e, v.c, nullptr};
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The lists of `room` for the current batch (built at its first masked texel, from the centre g_batch.c), null = every wall
// (as before)
void* MaskWallLists(int slot, BYTE* room) {
    BatchLists& L = g_maskLists[slot];
    if (L.room == reinterpret_cast<uintptr_t>(room) && L.serial == g_batchSerial) return L.ok ? &L.head : nullptr;
    L.room = reinterpret_cast<uintptr_t>(room);
    L.serial = g_batchSerial;
    L.ok = false;
    uintptr_t lights[512];
    int n = 0;
    size_t walls = 0;
    if (!kWallCull || !kLightPos || !ReadLightsAndWalls(room, lights, static_cast<int>(std::size(lights)), n, walls) || !n || walls > 8192) {
        g_maskListFallbacks.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    L.pool.assign(static_cast<size_t>(n) * (walls + 1), 0);
    L.lists.assign(static_cast<size_t>(n), WallList{});
    for (int i = 0; i < n; i++)
        if (!CullForLight(room, lights[i], g_batch.c, L.pool.data() + static_cast<size_t>(i) * (walls + 1), walls + 1, L.lists[i])) {
            g_maskListFallbacks.fetch_add(1, std::memory_order_relaxed);
            return nullptr;
        }
    L.head = WallLists{L.lists.data(), L.lists.data() + n, L.lists.data() + n, nullptr};
    L.ok = true;
    g_maskListBatches.fetch_add(1, std::memory_order_relaxed);
    return &L.head;
}

float* __fastcall SolvePointBatch(BYTE* room, void*, float* out, void* list2D, void* list3D, void* flags, void* sample) {
    // The first sample of a batch (this call runs once per sample, in order; the batch vector is reused, so its address does
    // not tell batches apart): a new serial for the floor wall lists, and for room 0 its story's outside walls for
    // WallBlocks (light tree thread)
    if (kBatchSamples && ThreadId() == g_gatherThread.load(std::memory_order_relaxed)) {
        __try {
            if (*reinterpret_cast<const uintptr_t*>(kBatchSamples) == reinterpret_cast<uintptr_t>(sample)) {
                ++g_batchSerial;
                if (g_objectWallsOn.load(std::memory_order_relaxed) && *reinterpret_cast<const int*>(room + 0xC) == 0) CaptureStoryWalls(room);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
    // Every thread (user 05/10: the light between stories "does not work every time"): the game also solves rooms off the
    // light tree thread (loading, lot impostors); skipping those left some texels of a floor masked and others not. The
    // game's 2D wall test works on any thread; SolvePoint calls the game directly there (no cross-story context).
    const float* s = static_cast<const float*>(sample);
    BYTE* room0 = g_floorMaskReady && room[0x18] && g_floorWallsOn.load(std::memory_order_relaxed) && s[5] >= 0.9f ? MaskedFloorRoom0(room) : nullptr;
    if (!room0) {
        float* r = SolvePoint(room, out, list2D, list3D, flags, sample, true);
        if (g_floorMaskReady && room[0x18]) out[3] = 0.0f; // outdoor: the game's alpha 0
        return r;
    }
    // An outdoor floor texel of a story >= 1: its own light is solved once, with the walls tested (user 05/10, top view:
    // a yard opened by a gap in its wall becomes part of room 0, and the game, which never wall-tests floor texels, lit the
    // yard floor right behind the solid wall with the lamp outside). The same lamps and the real normal, the 2D test on:
    // the light comes in through the gap only. (The game's untested solve is not run: it was replaced anyway.)
    const char* f = static_cast<const char*>(flags);
    char walls[2] = {1, f ? f[1] : 0};
    // The walls each lamp can meet between this batch and the lamp (MaskWallLists; the game builds none for floor texels,
    // which it never wall-tests): every wall of the story otherwise, as before
    void* ownLists = list2D;
    void* zeroLists = room0 == room ? list2D : nullptr;
    bool inBatch = false;
    if (ThreadId() == g_gatherThread.load(std::memory_order_relaxed) && BatchCentreFor(sample)) {
        inBatch = true;
        if ((g_batch.end - g_batch.begin) / 0x30 > 3) { // as the game: lists only for batches of more than 3 samples
            if (!ownLists) ownLists = MaskWallLists(0, room);
            if (!zeroLists) zeroLists = room0 == room ? ownLists : MaskWallLists(1, room0);
        }
    }
    float* r = SolvePoint(room, out, ownLists, list3D, walls, sample, true);
    out[3] = 0.0f;
    alignas(16) float vis[4] = {}, all[4] = {}, wrapped[12];
    std::memcpy(wrapped, s, sizeof wrapped);
    wrapped[7] = 1.0f; // normal w = 1: the lights' evaluation wraps (N.L ignored)
    char testWalls[2] = {1, 0}, noWalls[2] = {0, 0};
    g_copyInBatch = inBatch; // the copy stands for this batch's sample: the lamp's story walls are culled for it too
    SolvePoint(room0, vis, zeroLists, nullptr, testWalls, wrapped, true);
    SolvePoint(room0, all, nullptr, nullptr, noWalls, wrapped, true);
    g_copyInBatch = false;
    const float total = Lum3(all);
    if (total > 1e-4f) out[3] = std::clamp(1.0f - Lum3(vis) / total, 0.0f, 1.0f);
    return r;
}
float* __fastcall SolvePointSingle(BYTE* room, void*, float* out, void* list2D, void* list3D, void* flags, void* sample) {
    return SolvePoint(room, out, list2D, list3D, flags, sample, false);
}

// ---- Rooms lit only by lamps of another story (30/09, F6 105204; user: "a room did not take the brightness of rooms for a
// while", mostly right after loading). Rooms 5 and 7 of story 2 took the green lamp of story 1 (rooms near an opening take
// the lamps of the story below) and the floor test blocked every point of it: ambient (0, 0, 0), normalisation inf, the
// Brightness slider did nothing to them; room 6, almost all blocked, x98. FUN_006a0230 (thiscall(room, float brightest[4]),
// its only call 0x006A13B4 in FUN_006a0f50): +0x160 = limit / max(the lamps' vfunc+0x30, the brightest sample x k) when
// that is under the low limit [0x01158B24] (or over the high one [0x01158B20]). With the lamps all blocked that is limit / 0
// = inf; the ambient (average x +0x160) is then 0 x inf = NaN, which the clamp (maxps with 0) turns into 0. A room whose
// lamps are all of another story gets no boost (at most 1): the light coming through an opening is not spread over the
// whole room, and with none coming the room takes the unlit colour (the top-up), as an empty room does. Any value that is
// not finite becomes 1. Steam 1.67.2 only (fixed addresses, the call checked).
uintptr_t kRoomNorm = 0x006A0230, kRoomNormCall = 0x006A13B4; // resolved by GameAddr at install (these are the Steam values)
using RoomNorm_t = void(__thiscall*)(BYTE* room, const float* brightest);
std::atomic<long> g_normNotFinite{0}, g_normCrossOnly{0};
const Cross* FindCross(const RoomInfo& info, uintptr_t light); // below
bool CrossOnly(const BYTE* room, const RoomInfo& info) {
    const uintptr_t* b = *reinterpret_cast<const uintptr_t* const*>(room + 0xC8);
    const uintptr_t* e = *reinterpret_cast<const uintptr_t* const*>(room + 0xCC);
    if (!b || e <= b || e - b > 4096) return false;
    for (const uintptr_t* p = b; p < e; p++)
        if (!*p || !FindCross(info, *p)) return false;
    return true;
}
void __fastcall RoomNormHook(BYTE* room, void*, const float* brightest) {
    reinterpret_cast<RoomNorm_t>(kRoomNorm)(room, brightest);
    __try {
        float& norm = *reinterpret_cast<float*>(room + 0x160);
        if (!std::isfinite(norm)) {
            norm = 1.0f;
            g_normNotFinite.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (norm > 1.0f && ThreadId() == g_gatherThread.load(std::memory_order_relaxed))
            if (const RoomInfo* info = SolveInfo(room); info && info->indoor && CrossOnly(room, *info)) {
                norm = 1.0f;
                g_normCrossOnly.fetch_add(1, std::memory_order_relaxed);
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
    }
}

// ---- The 4 directional basis maps get the floor test too (30/09, third agent study; F7 128 / F8 10:45: a TV in a closed
// room upstairs took the green lamp of the story below). The story's LightBasisMap0..3 (64x64, 1 texel per metre, locked at
// room+0x584 + i*0x28 in FUN_0069fa40, gated by room+0x629) are filled by FUN_006a09f0 (from FUN_006a3b80 at 0x006A3C0A,
// state 7 of the room solve FUN_006a3c90, indoor rooms of high-quality lots): one sample per tile centre, and for each light
// of the solving room's own list FUN_0069f280 (stdcall(pos, light, float acc[4][4]) ret 0xC; its only call 0x006A0C56,
// "mov ecx, edi" = the room just before it; pos = the sample 0.5 m up) adds the light's rig colour (vfunc+0x10) weighted
// towards the 4 basis directions. No threshold, wall or floor test there, and it never goes through LightPointWithAllLights,
// so the lamps of another story taken near an opening lit the whole room through the floor. A lamp of another story
// (FindCross) is now tested at that point with IndoorShadow (the floor test only: no 2D wall flags, so it passes or not):
// blocked, it adds nothing. Imported lamps also need wall tests on all ray segments: unlike the floor/wall
// solve, the basis builder has no recipient wall test of its own. Test008 uses these guarded maps without the
// floor-map shader cap; the cap remains the fallback if this validated hook is unavailable. Steam 1.67.2 only.
uintptr_t kBasisLight = 0x0069F280, kBasisLightCall = 0x006A0C56; // resolved by GameAddr at install (these are the Steam values)
using BasisLight_t = void(__stdcall*)(const float* pos, void* light, float* acc);
std::atomic<long> g_basisTests{0}, g_basisBlocked{0};
std::atomic<bool> g_basisGuardReady{false};
void IndoorShadow(const RoomInfo& info, void* light, const float* sample, float* colour); // below
// The time Apex's own tests take inside the game's room solves (LightEvalHook, this hook; ApexSolveMs)
std::atomic<uint64_t> g_apexSolveCycles{0};
void __fastcall BasisLightHook(BYTE* room, void*, const float* pos, void* light, float* acc) {
    if (g_installed.load(std::memory_order_relaxed) && g_indoorReady && ThreadId() == g_gatherThread.load(std::memory_order_relaxed))
        if (const RoomInfo* info = SolveInfo(room); info && info->indoor && FindCross(*info, reinterpret_cast<uintptr_t>(light)) &&
                                                     !FindCross(*info, reinterpret_cast<uintptr_t>(light))->outdoor) {
            const SolveCtx prev = g_ctx;
            g_ctx = SolveCtx{};
            g_ctx.info = info;
            g_ctx.soft = room[0x639];
            const char basisWallFlags[2] = {1, 0};
            g_ctx.flags = basisWallFlags;
            g_ctx.basis = true;
            alignas(16) float s[12] = {pos[0], pos[1], pos[2], pos[3]}; // the sample: position, no normal
            float pass[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            const uint64_t timedFrom = __rdtsc();
            IndoorShadow(*info, light, s, pass);
            g_apexSolveCycles.fetch_add(__rdtsc() - timedFrom, std::memory_order_relaxed);
            g_ctx = prev;
            g_basisTests.fetch_add(1, std::memory_order_relaxed);
            if (!(pass[0] > 0.0f)) {
                g_basisBlocked.fetch_add(1, std::memory_order_relaxed);
                return; // behind the floor: this lamp adds nothing to the texel
            }
            if (pass[0] < 1.0f) {
                alignas(16) float contribution[16] = {};
                reinterpret_cast<BasisLight_t>(kBasisLight)(pos, light, contribution);
                for (int i = 0; i < 16; ++i) acc[i] += contribution[i] * pass[0];
                return; // attenuate this lamp alone; leave earlier lamps in the accumulator unchanged
            }
        }
    reinterpret_cast<BasisLight_t>(kBasisLight)(pos, light, acc);
}

// The share of a lamp of another story that reaches a point of an indoor room, tested as the basis maps test it (the floor
// crossed through an opening, the walls of the lamp's room on the way, the receiving segment too); -1 when the lamp is not
// one Apex's gather took for that room (FindCross), or off the light tree thread (g_rooms belongs to it)
float CrossLampReachImpl(BYTE* room, void* light, const float* point) {
    if (!g_installed.load(std::memory_order_relaxed) || !g_indoorReady || ThreadId() != g_gatherThread.load(std::memory_order_relaxed)) return -1.0f;
    const RoomInfo* info = SolveInfo(room);
    if (!info || !info->indoor) return -1.0f;
    const Cross* cross = FindCross(*info, reinterpret_cast<uintptr_t>(light));
    if (!cross || cross->outdoor) return -1.0f;
    const SolveCtx prev = g_ctx;
    g_ctx = SolveCtx{};
    g_ctx.info = info;
    g_ctx.soft = room[0x639];
    const char wallFlags[2] = {1, 0};
    g_ctx.flags = wallFlags;
    g_ctx.basis = true;
    alignas(16) float s[12] = {point[0], point[1], point[2], 1.0f};
    float pass[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    __try {
        IndoorShadow(*info, light, s, pass);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        pass[0] = 0.0f;
    }
    g_ctx = prev;
    return std::isfinite(pass[0]) ? std::clamp(pass[0], 0.0f, 1.0f) : 0.0f;
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

// homeRoom: the lamp's room on its story (0, or a roofless room: its own walls close it, 05/10)
float WallPassImpl(uintptr_t tracker, int roomLevel, int home, int homeRoom, void* light, const void* sample, bool& culledList) {
    alignas(16) float pos[4];
    reinterpret_cast<LightPos_t>(kLightPos)(light, pos);
    float keep = 1.0f;
    const int lo = std::min(home, roomLevel), hi = std::max(home, roomLevel);
    for (int floor = lo; floor <= hi; floor++) {
        if (floor == roomLevel) continue;
        void* mgr = *reinterpret_cast<void* const*>(TreeLevel(tracker, floor));
        if (!mgr) continue;
        BYTE* room0 = floor == home && homeRoom > 0 ? static_cast<BYTE*>(reinterpret_cast<RoomById_t>(kRoomById)(mgr, homeRoom)) : nullptr;
        if (!room0) room0 = static_cast<BYTE*>(reinterpret_cast<RoomById_t>(kRoomById)(mgr, 0));
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
float WallPass(uintptr_t tracker, int roomLevel, int home, int homeRoom, void* light, const void* sample, bool& culledList) {
    __try {
        return WallPassImpl(tracker, roomLevel, home, homeRoom, light, sample, culledList);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (g_swapAt) {
            *g_swapAt = g_swapSaved;
            g_swapAt = nullptr;
        }
        g_faults.fetch_add(1, std::memory_order_relaxed);
        return 1.0f;
    }
}

const Cross* FindCross(const RoomInfo& info, uintptr_t light) {
    auto it = std::lower_bound(info.cross.begin(), info.cross.end(), Cross{light});
    return it == info.cross.end() || it->light != light ? nullptr : &*it;
}
int HomeFloor(const RoomInfo& info, uintptr_t light) {
    const Cross* c = FindCross(info, light);
    return c ? c->floor : -1;
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
    if (g_diagFull[info.indoor ? 1 : 0].load(std::memory_order_relaxed)) return;
    if (!ActiveLot(info.mgr)) return;
    const float* lp = reinterpret_cast<const float*>(static_cast<const BYTE*>(light) + 0x120);
    const float dx = sample[0] - lp[0], dy = sample[1] - lp[1], dz = sample[2] - lp[2];
    if (dx * dx + dy * dy + dz * dz > 3.5f * 3.5f) return;
    g_diagSeen.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_diagMx);
    // separate budgets: the outdoor lamps of every loaded lot would fill the record before an indoor point is seen
    int& used = info.indoor ? g_diagIndoorUsed : g_diagOutdoorUsed;
    const int limit = info.indoor ? 40000 : 4000;
    if (used >= limit) return;
    used++;
    if (used == limit) g_diagFull[info.indoor ? 1 : 0].store(true, std::memory_order_relaxed);
    DiagRec r{info.level, home, reinterpret_cast<uintptr_t>(light), {lp[0], lp[1], lp[2]}, {sample[0], sample[1], sample[2]},
              {sample[4], sample[5], sample[6]}, colour[0] + colour[1] + colour[2], mine, -1, 1.0f, g_ctx.batch, culledList};
    r.type = *reinterpret_cast<const int*>(static_cast<const BYTE*>(light) + 0xB0);
    r.room = info.id;
    if (info.indoor) {
        r.cx = g_passDbg.cx, r.cz = g_passDbg.cz, r.cq = g_passDbg.cq, r.belowRoom = g_passDbg.belowRoom;
        r.keyHi = g_passDbg.keyHi, r.keyLo = g_passDbg.keyLo, r.ch = g_passDbg.ch, r.ct = g_passDbg.ct;
    }
    g_diag.push_back(r);
    g_lastRec = static_cast<int>(g_diag.size()) - 1;
}

// Where a lamp's ray enters the solving room's story (IndoorPassImpl and OutdoorEntry, for GameWallTest)
struct RayEntry {
    bool valid = false;
    float lamp[3] = {}, at[3] = {};
};

// A floor a player placed at that quadrant of the story: not the air next to walls (the bare 0x40000000 key), a foundation,
// a removed floor or never-built space, which FloorAt all counts as floor (RemovedFloorKey). What cannot be read counts as a
// floor.
bool PlacedFloorAt(uintptr_t level, int ix, int iz, int q) {
    const uintptr_t grid = *reinterpret_cast<const uintptr_t*>(level + 0x264);
    if (!grid) return true;
    const uintptr_t data = *reinterpret_cast<const uintptr_t*>(grid);
    const int w = *reinterpret_cast<const int*>(grid + 0x10), h = *reinterpret_cast<const int*>(grid + 0x14);
    if (!data || ix < 0 || iz < 0 || ix >= w || iz >= h || w > 1024 || h > 1024) return true;
    const uint32_t* key = reinterpret_cast<const uint32_t*>(data + (static_cast<size_t>(iz) * w + ix) * 40 + 8 + q * 8);
    if (key[0] == 0xFFFFFFF8u && key[1] == 0xFFFFFFFFu) return false;
    return !(key[0] & 0x40000000u);
}

// Where an outdoor lamp of a lower story enters an outdoor room's story through open air (06/10, user's F7 16:30 and F8
// 16:29:57: a sconce of the ground floor left the upper story's half wall of a balcony dark beside the lit wall next to it;
// all 21 recorded points of the half wall were blocked by the game's wall test of the upper story and passed by ours). That
// test (0x0069FC40) has no wall base, and its soft mode shades rays passing near a wall's end, so a ray rising past the
// corner of the balcony below its floor was blocked by walls that stand above it. The game's test (GameWallTest) then runs
// from where the ray reaches this story's lowest floor, as for indoor rooms (IndoorPassImpl's entry); the lamp's own story
// is WallPass's. Where the ray comes up through a floor of this story (a deck, a balcony) the whole ray stays the game's.
bool OutdoorEntryImpl(const RoomInfo& info, int home, void* light, const float* sample, RayEntry& entry) {
    if (home < 0 || home >= info.level || info.level > 7) return false;
    const uintptr_t mgr = StoryManager(info.tracker, info.level);
    const uintptr_t level = mgr ? LevelFor(mgr) : 0;
    Xform xf;
    if (!mgr || !level || LevelManager(level) != mgr || !ReadXform(info.mgr, xf)) return false;
    alignas(16) float pos[4];
    reinterpret_cast<LightPos_t>(kLightPos)(light, pos);
    float P[3], Q[3];
    ToLocal(xf, pos, P);
    ToLocal(xf, sample, Q);
    const float dy = Q[1] - P[1];
    const float h = *reinterpret_cast<const float*>(mgr + 0x98) - *reinterpret_cast<const float*>(mgr + 0xD4); // the story's lowest floor, lot space
    if (!(std::fabs(h) < 1000.0f) || dy < 1e-4f) return false; // the lamp below the point only
    float t = (h - P[1]) / dy;
    if (t >= 1.0f && std::fabs(Q[1] - h) < 0.02f) t = 0.9999f; // a point on that floor's plane: tested where it is
    if (!(t > 0.0f && t < 1.0f)) return false;                 // the lamp not below that floor, or the point not above it
    const float x = P[0] + t * (Q[0] - P[0]), z = P[2] + t * (Q[2] - P[2]);
    const int ix = static_cast<int>(std::floor(x)), iz = static_cast<int>(std::floor(z));
    if (PlacedFloorAt(level, ix, iz, Quadrant(x - ix, z - iz))) return false;
    entry.valid = true;
    for (int k = 0; k < 3; ++k) {
        entry.lamp[k] = pos[k];
        entry.at[k] = pos[k] + t * (sample[k] - pos[k]);
    }
    return true;
}
bool OutdoorEntry(const RoomInfo& info, int home, void* light, const float* sample, RayEntry& entry) {
    __try {
        return OutdoorEntryImpl(info, home, light, sample, entry);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        return false; // the game's whole-ray test, as before
    }
}

void CrossFloorShadow(const RoomInfo& info, void* light, const float* sample, float* colour) {
    const Cross* cross = FindCross(info, reinterpret_cast<uintptr_t>(light));
    const int home = cross ? cross->floor : -1;
    float mine = -1.0f;
    bool culledList = false;
    const bool lit = colour[0] > 0.0f || colour[1] > 0.0f || colour[2] > 0.0f;
    const float before[3] = {colour[0], colour[1], colour[2]};
    if (home >= 0 && lit && g_ctx.flags && g_ctx.flags[0]) { // the game tests 2D walls in this batch: so do we, on the lamp's floor
        g_wallTests.fetch_add(1, std::memory_order_relaxed);
        mine = WallPass(info.tracker, info.level, home, cross->outdoor ? cross->room : 0, light, sample, culledList);
        if (mine <= 0.0f) g_wallBlocked.fetch_add(1, std::memory_order_relaxed);
        if (mine < 1.0f)
            for (int i = 0; i < 4; i++) colour[i] *= std::max(0.0f, mine);
        // a lamp of a lower story: this story's walls, tested next by the game, from where its ray enters this story
        RayEntry entry;
        if (mine > 0.0f && home < info.level && g_enterReady && !g_ctx.basis && OutdoorEntry(info, home, light, sample, entry)) {
            g_ctx.enterLight = reinterpret_cast<uintptr_t>(light);
            std::memcpy(g_ctx.enterLamp, entry.lamp, sizeof g_ctx.enterLamp);
            std::memcpy(g_ctx.enterAt, entry.at, sizeof g_ctx.enterAt);
            g_ctx.enterHard = true; // and without a wall end's penumbra (WallTestHard)
            g_outdoorEnters.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!kPublicBuild) {
        if (lit) {
            if (g_diagArmed.load(std::memory_order_relaxed) && !g_ghostSolve) Diag(info, light, sample, before, home, mine, culledList);
            else g_lastRec = -1; // as Diag does first: no record for the game's wall test that follows
        }
    }
}

// 4.: a lamp of another story reaches the point only through openings in every intervening floor, and
// past the walls of its own room (from the lamp to where the ray crosses that floor). Share of the light, 0 = none;
// why: 1 = a floor is in the way, 2 = a wall of the lamp's room.
float IndoorBoundaryPass(const RoomInfo& info, uintptr_t floorLevel, int B, void* light, const float* sample, float& crossing, int& why) {
    const auto blocked = [&why](int reason) {
        why = reason;
        return 0.0f;
    };
    if (!kPublicBuild) g_passDbg = PassDebug{};
    alignas(16) float pos[4];
    reinterpret_cast<LightPos_t>(kLightPos)(light, pos);
    const uintptr_t mgrB = StoryManager(info.tracker, B), mgrBelow = StoryManager(info.tracker, B - 1);
    Xform xf;
    if (!mgrB || LevelManager(floorLevel) != mgrB || !ReadXform(info.mgr, xf)) return blocked(1);
    float P[3], Q[3];
    ToLocal(xf, pos, P);
    ToLocal(xf, sample, Q);
    const float dy = Q[1] - P[1];
    float h = *reinterpret_cast<const float*>(mgrB + 0x98) - *reinterpret_cast<const float*>(mgrB + 0xD4); // the story's lowest floor, lot space
    if (!(std::fabs(h) < 1000.0f) || std::fabs(dy) < 1e-4f) return blocked(1);
    float t = -1.0f;
    for (int pass = 0; pass < 2; pass++) {
        t = (h - P[1]) / dy;
        // a point on that floor's plane (the top row of the walls below it, the bottom row of the walls above) is tested
        // where it is: it gets the lamp only through an opening too
        if (t >= 1.0f && std::fabs(Q[1] - h) < 0.02f) t = 0.9999f;
        else if (t <= 0.0f && std::fabs(P[1] - h) < 0.02f) t = 0.0001f;
        if (!(t > 0.0f && t < 1.0f)) {
            t = -1.0f; // lamp and point on the same side of that floor: nothing of it in the way
            break;
        }
        const float x = P[0] + t * (Q[0] - P[0]), z = P[2] + t * (Q[2] - P[2]);
        const int ix = static_cast<int>(std::floor(x)), iz = static_cast<int>(std::floor(z));
        const uintptr_t tile = LightTile(mgrB, ix, iz);
        if (!tile) return blocked(1); // outside the story
        const float th = *reinterpret_cast<const float*>(tile + 0x78); // the floor height of that tile
        if (pass == 0 && std::fabs(th - h) > 0.01f && std::fabs(th - h) < 50.0f) {
            h = th;
            continue;
        }
        // an opening: no floor on B there, over an indoor room of the story below (the rule of ReadOpenings)
        const int q = Quadrant(x - ix, z - iz);
        const uintptr_t below = mgrBelow ? LightTile(mgrBelow, ix, iz) : 0;
        if (!kPublicBuild) {
            g_passDbg = PassDebug{ix, iz, q, below ? TileRoom(below, q) : -1, 0, 0, h, t};
            const uintptr_t grid = *reinterpret_cast<const uintptr_t*>(floorLevel + 0x264);
            const uintptr_t data = grid ? *reinterpret_cast<const uintptr_t*>(grid) : 0;
            const int gw = grid ? *reinterpret_cast<const int*>(grid + 0x10) : 0, gh = grid ? *reinterpret_cast<const int*>(grid + 0x14) : 0;
            if (data && ix >= 0 && iz >= 0 && ix < gw && iz < gh) {
                const uint32_t* key = reinterpret_cast<const uint32_t*>(data + (static_cast<size_t>(iz) * gw + ix) * 40 + 8 + q * 8);
                g_passDbg.keyLo = key[0], g_passDbg.keyHi = key[1];
            }
        }
        if (!below || TileRoom(below, q) <= 0 || FloorAt(floorLevel, ix, iz, q) != 0) return blocked(1);
        break;
    }
    crossing = t;
    return 1.0f;
}

// Test every floor crossed by the real lamp-to-sample ray, then each foreign
// story's wall segment. The recipient's walls remain the native solve's job,
// from where the ray enters the recipient's story (entry, GameWallTest).
float IndoorPassImpl(const RoomInfo& info, const Cross& c, void* light, const float* sample, int& why, RayEntry* entry) {
    const auto blocked = [&why](int reason) { why = reason; return 0.0f; };
    if (info.level < 0 || info.level > 7 || c.floor < 0 || c.floor > 7 || info.level == c.floor) return blocked(1);
    const int direction = info.level > c.floor ? 1 : -1;
    struct RaySegment { int story; float begin, end; };
    RaySegment raySegments[8] = {};
    int count = 0;
    int currentStory = c.floor;
    float previous = 0.0f;
    for (int story = c.floor; story != info.level; story += direction) {
        const int boundary = direction > 0 ? story + 1 : story;
        const uintptr_t manager = StoryManager(info.tracker, boundary);
        const uintptr_t floor = boundary == std::max(info.level, c.floor) ? c.level : (manager ? LevelFor(manager) : 0);
        float t = -1.0f;
        if (!floor || IndoorBoundaryPass(info, floor, boundary, light, sample, t, why) <= 0.0f) return blocked(1);
        if (t > 0.0f) {
            if (t <= previous) return blocked(1);
            raySegments[count++] = RaySegment{currentStory, previous, t};
            currentStory = story + direction;
            previous = t;
        }
    }
    // Ghost rows and split-level samples need not cross every nominal story
    // plane. Test only intersected floors; retain the wall segment of the story
    // containing the ray's endpoint when it is outside the recipient's story.
    if (currentStory != info.level || g_ctx.basis) raySegments[count++] = RaySegment{currentStory, previous, 1.0f};
    if (!(g_ctx.flags && g_ctx.flags[0])) return 1.0f;
    alignas(16) float pos[4];
    reinterpret_cast<LightPos_t>(kLightPos)(light, pos);
    Xform xf;
    if (!ReadXform(info.mgr, xf)) return blocked(1);
    // Where the ray enters the solving room's story: its last floor crossing. The game's own 2D wall test of the solving
    // room, which follows in the point solve (0x69FE93), tests the ray from there (GameWallTest) instead of from the lamp:
    // this story's walls stand on that floor, and a lamp of another story that lies behind one of them only in plan (under
    // a ledge, a set-back upper wall, a recess) shines past it. Measured 05/10, atrium house, wall seams of 19:54:44: the
    // sconces of the lower west wall, 1 m behind the upper story's west wall, lit the upper walls nowhere (0.08-0.16 less
    // at the floor line all along the opposite wall than the lower room's top row at the same points).
    if (entry && !g_ctx.basis && count > 0 && currentStory == info.level) {
        entry->valid = true;
        for (int k = 0; k < 3; ++k) {
            entry->lamp[k] = pos[k];
            entry->at[k] = pos[k] + previous * (sample[k] - pos[k]);
        }
    }
    float transmission = 1.0f;
    if (g_ctx.basis) {
        // A raised room's walls can lie beyond the nominal story interval.
        // The native exterior wall collection carries the actual wall heights;
        // test the whole ray there before accepting a directional-map texel.
        // This is a veto only: segment tests below own glass/soft attenuation.
        for (int story = std::min(info.level, c.floor); story <= std::max(info.level, c.floor); ++story) {
            const uintptr_t manager = StoryManager(info.tracker, story);
            BYTE* exterior = manager ? static_cast<BYTE*>(reinterpret_cast<RoomById_t>(kRoomById)(reinterpret_cast<void*>(manager), 0)) : nullptr;
            if (!exterior) return blocked(2);
            float keep = 1.0f;
            g_swapAt = exterior + 0x639;
            g_swapSaved = *g_swapAt;
            *g_swapAt = g_ctx.soft;
            const bool passed = reinterpret_cast<WallTest_t>(kWallTest)(exterior, nullptr, pos, sample, &keep);
            *g_swapAt = g_swapSaved;
            g_swapAt = nullptr;
            if (!passed || !std::isfinite(keep) || keep <= 0.0f) return blocked(2);
        }
    }
    for (int i = 0; i < count; ++i) {
        const RaySegment& segment = raySegments[i];
        const float t = segment.end;
        const int story = segment.story;
        const uintptr_t manager = StoryManager(info.tracker, story);
        if (!manager) return blocked(2);
        int roomId = c.room;
        if (story != c.floor || segment.begin > 0.0f) {
            float midpoint[3], local[3];
            for (int k = 0; k < 3; ++k) midpoint[k] = pos[k] + (segment.begin + t) * 0.5f * (sample[k] - pos[k]);
            ToLocal(xf, midpoint, local);
            const int x = static_cast<int>(std::floor(local[0])), z = static_cast<int>(std::floor(local[2]));
            const uintptr_t tile = LightTile(manager, x, z);
            if (!tile) return blocked(2);
            roomId = TileRoom(tile, Quadrant(local[0] - x, local[2] - z));
        }
        BYTE* home = static_cast<BYTE*>(reinterpret_cast<RoomById_t>(kRoomById)(reinterpret_cast<void*>(manager), roomId));
        if (!home) {
            if (!g_ctx.basis && std::abs(c.floor - info.level) == 1) { // unchanged native-solve adjacent fallback: the game's
                if (entry) entry->valid = false;                       // wall test keeps the whole ray
                return 1.0f;
            }
            return blocked(2);
        }
        alignas(16) float from[4], to[12];
        std::memcpy(from, pos, sizeof from);
        std::memcpy(to, sample, sizeof to);
        for (int k = 0; k < 3; ++k) {
            from[k] = pos[k] + segment.begin * (sample[k] - pos[k]);
            to[k] = pos[k] + t * (sample[k] - pos[k]);
        }
        float keep = 1.0f;
        g_swapAt = home + 0x639;
        g_swapSaved = *g_swapAt;
        *g_swapAt = g_ctx.soft;
        const bool passed = reinterpret_cast<WallTest_t>(kWallTest)(home, nullptr, from, to, &keep);
        *g_swapAt = g_swapSaved;
        g_swapAt = nullptr;
        if (!passed || !std::isfinite(keep)) return blocked(2);
        transmission *= std::clamp(keep, 0.0f, 1.0f);
        if (transmission <= 0.0f) return blocked(2);
    }
    return transmission;
}

float IndoorPass(const RoomInfo& info, const Cross& c, void* light, const float* sample, int& why, RayEntry* entry) {
    __try {
        return IndoorPassImpl(info, c, light, sample, why, entry);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (g_swapAt) {
            *g_swapAt = g_swapSaved;
            g_swapAt = nullptr;
        }
        if (entry) entry->valid = false;
        g_faults.fetch_add(1, std::memory_order_relaxed);
        why = 1;
        return 0.0f; // never light through a floor that could not be checked
    }
}

void IndoorShadow(const RoomInfo& info, void* light, const float* sample, float* colour) {
    const bool lit = colour[0] > 0.0f || colour[1] > 0.0f || colour[2] > 0.0f;
    if (!lit) return;
    const Cross* c = FindCross(info, reinterpret_cast<uintptr_t>(light));
    const float before[3] = {colour[0], colour[1], colour[2]};
    float mine = -1.0f;
    int why = 0;
    if (c) {
        g_indoorTests.fetch_add(1, std::memory_order_relaxed);
        RayEntry entry;
        mine = IndoorPass(info, *c, light, sample, why, g_enterReady ? &entry : nullptr);
        if (why == 1) g_indoorFloorBlocked.fetch_add(1, std::memory_order_relaxed);
        else if (why == 2) g_indoorWallBlocked.fetch_add(1, std::memory_order_relaxed);
        if (mine < 1.0f)
            for (int i = 0; i < 4; i++) colour[i] *= std::max(0.0f, mine);
        if (mine > 0.0f && !c->lit && !g_ctx.basis) { // this lamp lights this room (once per gather; g_litBy)
            c->lit = true;
            NoteLit(DepKey{info.tracker, info.level, info.id}, info.mgr, reinterpret_cast<uintptr_t>(light));
        }
        if (mine > 0.0f && entry.valid) { // the game's wall test of this room for this lamp, next: from where the ray enters
            g_ctx.enterLight = reinterpret_cast<uintptr_t>(light);
            std::memcpy(g_ctx.enterLamp, entry.lamp, sizeof g_ctx.enterLamp);
            std::memcpy(g_ctx.enterAt, entry.at, sizeof g_ctx.enterAt);
            g_ctx.enterHard = false;
        }
    }
    if (!kPublicBuild) {
        g_lastRec = -1;
        // lamps of another story, and the room's own lamps that another story takes (to compare the game's own evaluation of
        // a lamp with ours on the other side of the floor; every own lamp would fill the record)
        if (g_diagArmed.load(std::memory_order_relaxed) && !g_ghostSolve && (c || g_crossLamps.count(reinterpret_cast<uintptr_t>(light)))) {
            Diag(info, light, sample, before, c ? c->floor : -1, mine, false);
            if (g_lastRec >= 0) {
                std::lock_guard<std::mutex> lk(g_diagMx);
                if (g_lastRec < static_cast<int>(g_diag.size())) g_diag[g_lastRec].why = why;
            }
        }
    }
}

// The time Apex's own tests take inside the game's room solves (06/10: is a slow "all the lights" the game's solve or ours?)
struct ApexCycles {
    uint64_t t0 = __rdtsc();
    ~ApexCycles() { g_apexSolveCycles.fetch_add(__rdtsc() - t0, std::memory_order_relaxed); }
};

template <int I> void __fastcall LightEvalHook(void* light, void*, const float* sample, const float* normal, float* colour) {
    reinterpret_cast<LightEval_t>(g_evalOrig[I])(light, sample, normal, colour);
    if (g_ctx.info && _ReturnAddress() == reinterpret_cast<void*>(kLightEvalReturn) && ThreadId() == g_gatherThread.load(std::memory_order_relaxed)) {
        const ApexCycles timed;
        g_ctx.enterLight = 0; // a lamp's entry point serves its own wall test only
        // A light the game is about to drop (0x69FE40: threshold > (b + g) + r, the same sums in the same order) needs no
        // test of ours: our tests only lower the colour, so it is dropped either way (bit-identical; most far lamps end here)
        if (g_ctx.thr > (colour[2] + colour[1]) + colour[0]) return;
        if (!g_ctx.info->indoor) CrossFloorShadow(*g_ctx.info, light, sample, colour);
        else if (const Cross* c = FindCross(*g_ctx.info, reinterpret_cast<uintptr_t>(light)); c && c->outdoor)
            CrossFloorShadow(*g_ctx.info, light, sample, colour); // a roofless room taking an outdoor lamp of another story
        else IndoorShadow(*g_ctx.info, light, sample, colour);
    }
}
using LightEvalHook_t = void(__fastcall*)(void* light, void* edx, const float* sample, const float* normal, float* colour); // = thiscall ret 0xC
const LightEvalHook_t kEvalHooks[] = {&LightEvalHook<0>, &LightEvalHook<1>, &LightEvalHook<2>, &LightEvalHook<3>,
                                  &LightEvalHook<4>, &LightEvalHook<5>, &LightEvalHook<6>, &LightEvalHook<7>, &LightEvalHook<8>};
static_assert(std::size(kEvalHooks) == std::size(kClasses));

// The game's wall test with the room's walls at their real ends (+0x639 = 0 for the call): in soft mode FUN_0069d4c0 tests
// each wall's widened segment (0x0069AA90 with the soft flag, wall+0x40 + 0x30) and shades by where the ray crosses it
// (0x0069A8C0), so a ray passing near a wall's end loses light without crossing the wall. Hard mode is the plain 2D
// crossing with no height test (0x0069AC04 skips it): GameWallTest asks it only after the soft test shaded a ray. For an
// outdoor lamp of a lower story that penumbra darkened the upper wall beside a balcony's corner (06/10). A fault in the
// game's test puts the byte back and passes the fault on.
bool WallTestHard(BYTE* room, void* idx, const float* from, const void* sample, float* t) {
    BYTE* const mode = room + 0x639;
    const BYTE saved = *mode;
    *mode = 0;
    bool ok = false;
    __try {
        ok = reinterpret_cast<WallTest_t>(kWallTest)(room, idx, from, sample, t);
    } __finally {
        *mode = saved;
    }
    return ok;
}

// The game's own wall test in LightPointWithAllLights (0x69FE93): for a lamp of another story IndoorShadow let through, the
// solving room's walls are tested from where its ray enters this room's story (IndoorPassImpl), with the game's own list
// of walls for that lamp; any other light as the game has it. Development build: the result is recorded for F8.
bool __fastcall GameWallTest(BYTE* room, void*, void* idx, const float* lightPos, const void* sample, float* t) {
    const float* from = lightPos;
    alignas(16) float entered[4];
    bool hard = false;
    if (ThreadId() == g_gatherThread.load(std::memory_order_relaxed) && g_ctx.enterLight) {
        // the same lamp: the position the game passes is the one IndoorPassImpl read (vfunc+0x24, bit for bit)
        if (lightPos && lightPos[0] == g_ctx.enterLamp[0] && lightPos[1] == g_ctx.enterLamp[1] && lightPos[2] == g_ctx.enterLamp[2]) {
            entered[0] = g_ctx.enterAt[0];
            entered[1] = g_ctx.enterAt[1];
            entered[2] = g_ctx.enterAt[2];
            entered[3] = lightPos[3];
            from = entered;
            hard = g_ctx.enterHard;
            g_enterTests.fetch_add(1, std::memory_order_relaxed);
        }
        g_ctx.enterLight = 0;
        g_ctx.enterHard = false;
    }
    bool ok = reinterpret_cast<WallTest_t>(kWallTest)(room, idx, from, sample, t);
    if (hard && t && (!ok || *t < 1.0f)) {
        // shaded: when the ray crosses no wall at its real ends (the hard test, no height test, so walls of any height
        // count), that shade was only the soft penumbra of a wall's end; else the soft test's result stays (it knows the
        // ray passed over a low wall)
        float hardT = 1.0f;
        if (WallTestHard(room, idx, from, sample, &hardT) && std::isfinite(hardT) && hardT > 0.0f) {
            ok = true;
            *t = hardT;
            g_penumbraLifted.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (!kPublicBuild && g_lastRec >= 0 && ThreadId() == g_gatherThread.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lk(g_diagMx);
        if (g_lastRec < static_cast<int>(g_diag.size())) {
            g_diag[g_lastRec].game = ok ? 1 : 0;
            g_diag[g_lastRec].gameT = *t;
        }
        g_lastRec = -1;
    }
    return ok;
}

void ClearDiag() {
    std::lock_guard<std::mutex> lk(g_diagMx);
    g_diag.clear();
    g_diagIndoorUsed = g_diagOutdoorUsed = 0;
    g_diagFull[0] = g_diagFull[1] = false;
}

// The trackers of every loaded lot (the light manager's lot hash): room 0 of floors 0..7 and the given indoor rooms
void RefreshTrackers(const IndoorRoom* indoor, size_t indoorCount) {
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
            if (tracker) {
                QueueOutdoorRegather(tracker);
                for (size_t i = 0; i < indoorCount; i++) // rooms holding lamps of another story (only lots still loaded)
                    if (indoor[i].tracker == tracker && QueueRoom(tracker, indoor[i].level, indoor[i].id)) g_indoorQueued.fetch_add(1, std::memory_order_relaxed);
            }
            node = *reinterpret_cast<const uintptr_t*>(node + 0x10);
            int g2 = 0;
            while (node == 0 && g2++ < (1 << 20)) node = *reinterpret_cast<const uintptr_t*>(slot += 4);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
    }
}

// Every loaded lot: room 0 of floors 0..7, and the rooms holding lamps of another story, gather again (after turning the
// option on or off). Render thread only.
void RefreshAllLots() {
    ClearDiag(); // the records show the solve that follows
    if (!kRootPtr) return;
    std::vector<IndoorRoom> indoor;
    {
        std::lock_guard<std::mutex> lk(g_indoorListMx);
        indoor = g_indoorList;
        if (!g_installed.load()) g_indoorList.clear();
    }
    RefreshTrackers(indoor.data(), indoor.size());
}

// The return address follows "call reg" (FF D0..FF D7): FF D2 (call edx) on Steam
bool AfterCallReg(uintptr_t ret) {
    const BYTE* p = reinterpret_cast<const BYTE*>(ret - 2);
    return GameAddr::IsFixed() ? std::memcmp(p, "\xFF\xD2", 2) == 0 : (p[0] == 0xFF && (p[1] & 0xF8) == 0xD0);
}

bool CallsTarget(uintptr_t site, uintptr_t target) {
    return *reinterpret_cast<const BYTE*>(site) == 0xE8 && site + 5 + *reinterpret_cast<const int32_t*>(site + 1) == target;
}

bool Redirect(uintptr_t site, uintptr_t target, const void* thunk, std::vector<MemPatch::PatchLocation>* patches = &g_patches) {
    DWORD orig = static_cast<DWORD>(target - (site + 5));
    const DWORD rel = static_cast<DWORD>(reinterpret_cast<uintptr_t>(thunk) - (site + 5));
    return MemPatch::WriteDWORD(site + 1, rel, patches, &orig);
}

// 4.: the room update's function pointer and the 5 floor calls. False (nothing left patched) when the game differs.
std::vector<MemPatch::PatchLocation> g_indoorPatches;
std::vector<MemPatch::PatchLocation> g_lodPatches; // the 4 LOD choice calls (optional part)
std::vector<MemPatch::PatchLocation> g_alignPatches; // 5.: the wall samples and wall blur calls (optional part)
bool InstallIndoor(std::string& why) {
    using GameAddr::Id;
    if (!GameAddr::Have({Id::LightBright, Id::AddRoomLight, Id::RoomUpdatePush, Id::RoomUpdate, Id::ChangedClearCall, Id::ChangedClear, Id::FloorSet, Id::FloorSetCall0,
                         Id::FloorSetCall1, Id::FloorSetCall2, Id::FloorSetCall3, Id::FloorRemove, Id::FloorRemoveCall, Id::LevelVtable},
                        &why)) {
        why = GameAddr::NotAvailable(why);
        return false;
    }
    bool ok = *reinterpret_cast<const BYTE*>(kRoomUpdatePush) == 0x68 && *reinterpret_cast<const uint32_t*>(kRoomUpdatePush + 1) == kRoomUpdate &&
              CallsTarget(kFloorRemoveCall, kFloorRemove) && CallsTarget(kChangedClearCall, kChangedClear) && kChangedClearCall > kRoomUpdate &&
              kChangedClearCall - kRoomUpdate < 0x400;
    for (uintptr_t site : kFloorSetCalls) ok = ok && CallsTarget(site, kFloorSet);
    if (!ok) {
        why = "the game code differs";
        return false;
    }
    g_floorSetTarget = kFloorSet;
    g_floorRemoveTarget = kFloorRemove;
    for (uintptr_t site : kFloorSetCalls) ok = ok && Redirect(site, kFloorSet, reinterpret_cast<const void*>(&FloorSetThunk), &g_indoorPatches);
    ok = ok && Redirect(kFloorRemoveCall, kFloorRemove, reinterpret_cast<const void*>(&FloorRemoveThunk), &g_indoorPatches);
    // Every level floor object at its construction too (optional): a lot built again without a floor set or remove call
    // (the camera left it and came back) still has its floors known. The ctor must be the one that writes the level vtable.
    if (ok && GameAddr::Have({Id::LevelCtorCall, Id::LevelCtor}) && CallsTarget(kLevelCtorCall, kLevelCtor) &&
        *reinterpret_cast<const uint16_t*>(kLevelCtor + 0x0D) == 0x06C7 && *reinterpret_cast<const uint32_t*>(kLevelCtor + 0x0F) == kLevelVtable &&
        !Redirect(kLevelCtorCall, kLevelCtor, reinterpret_cast<const void*>(&LevelCtorHook), &g_indoorPatches))
        LOG_WARNING("[LevelLightShare] Floor objects at their construction: could not patch the game (the floor set calls still find them)");
    ok = ok && Redirect(kChangedClearCall, kChangedClear, reinterpret_cast<const void*>(&ChangedClearHook), &g_indoorPatches);
    // Part 2: the changed-set hook sends room 0 of the other floors itself (filtered): the game's cascade goes (restored
    // before g_patches puts the jnz back)
    const std::vector<BYTE> noCascade = {0x90, 0xE9}, cascadeJl = {0x0F, 0x8C};
    ok = ok && MemPatch::WriteBytes(kCascadeJcc - 1, noCascade, &g_indoorPatches, &cascadeJl);
    DWORD orig = static_cast<DWORD>(kRoomUpdate);
    ok = ok && MemPatch::WriteDWORD(kRoomUpdatePush + 1, static_cast<DWORD>(reinterpret_cast<uintptr_t>(&RoomUpdateHook)), &g_indoorPatches, &orig);
    if (!ok) {
        MemPatch::RestoreAll(g_indoorPatches);
        g_indoorPatches.clear();
        why = "could not patch the game";
        return false;
    }
    // The camera story's LOD for the rooms seen through an opening (optional: without it the lower room keeps its coarse
    // wall light below the camera's story). FUN_0069e710 ends with "mov eax,[LodMax]; ret" at +0x4B.
    bool lod = GameAddr::Have({Id::LodChoice, Id::LodChoiceCall0, Id::LodChoiceCall1, Id::LodChoiceCall2, Id::LodChoiceCall3, Id::LodMax}) &&
               *reinterpret_cast<const BYTE*>(kLodChoice + 0x4B) == 0xA1 && *reinterpret_cast<const uint32_t*>(kLodChoice + 0x4C) == kLodMax;
    for (uintptr_t site : kLodChoiceCalls) lod = lod && CallsTarget(site, kLodChoice);
    for (uintptr_t site : kLodChoiceCalls) lod = lod && Redirect(site, kLodChoice, reinterpret_cast<const void*>(&LodChoiceHook), &g_lodPatches);
    if (!lod) {
        MemPatch::RestoreAll(g_lodPatches); // all 4 or none
        g_lodPatches.clear();
    }
    g_lodReady = lod;
    if (!lod) LOG_WARNING("[LevelLightShare] Lighting detail for rooms seen through an opening: the game code differs, left as the game has it");
    // One ambient for the rooms stacked through an opening (optional too)
    std::vector<MemPatch::PatchLocation> ambPatches;
    bool amb = GameAddr::Have({Id::RoomSolveStartCall, Id::RoomSolveStart, Id::WallPassCall, Id::WallPass}) && CallsTarget(kRoomSolveStartCall, kRoomSolveStart) &&
               CallsTarget(kWallPassCall, kWallPass);
    amb = amb && Redirect(kRoomSolveStartCall, kRoomSolveStart, reinterpret_cast<const void*>(&RoomSolveStartHook), &ambPatches) &&
          Redirect(kWallPassCall, kWallPass, reinterpret_cast<const void*>(&WallPassHook), &ambPatches);
    if (!amb) MemPatch::RestoreAll(ambPatches); // both or none
    else g_lodPatches.insert(g_lodPatches.end(), ambPatches.begin(), ambPatches.end());
    g_ambReady = amb;
    if (!amb) LOG_WARNING("[LevelLightShare] One ambient for rooms stacked through an opening: the game code differs, left as the game has it");
    g_classTables = amb && CheckClassTables(); // the quick pass takes its refinement's wall tests (QuickLikeRefinement)
    ReadThresholdTable();
    // the end of every room solve, for the quick pass and the recorder's light update trace (FinalizeHook)
    if (GameAddr::IsFixed() && CallsTarget(kFinalizeCall, kFinalize))
        g_finalizeReady = Redirect(kFinalizeCall, kFinalize, reinterpret_cast<const void*>(&FinalizeHook), &g_lodPatches);
    // the solve's step 1, where it locks the maps a lamp switch or an atrium holds (LockStepHook)
    if (const uintptr_t site = kRoomSolveStartCall ? kRoomSolveStartCall + 0x1B : 0;
        site && std::memcmp(reinterpret_cast<const void*>(site - 4), "\x8B\xCE\xDD\xD8\xE8", 5) == 0 &&
        std::memcmp(reinterpret_cast<const void*>(site + 5), "\x84\xC0\x0F\x84", 4) == 0) {
        kLockStep = site + 5 + *reinterpret_cast<const int32_t*>(site + 1);
        g_lockStepReady = Redirect(site, kLockStep, reinterpret_cast<const void*>(&LockStepHook), &g_lodPatches);
    }
    if (!g_lockStepReady) LOG_WARNING("[LevelLightShare] The room solve's map lock step: the game code differs, lamp switches and atriums change room by room");
    if (amb && !g_classTables) LOG_INFO("[LevelLightShare] Quick pass with the refinement's wall tests: not on this build (only its light threshold)");
    // No boost for rooms lit only by lamps of another story (optional; RoomNormHook)
    std::vector<MemPatch::PatchLocation> normPatches;
    kRoomNormCall = GameAddr::Get(Id::RoomNormCall);
    kRoomNorm = GameAddr::Get(Id::RoomNorm);
    const bool norm = GameAddr::Have({Id::RoomNormCall, Id::RoomNorm}) && CallsTarget(kRoomNormCall, kRoomNorm) &&
                      Redirect(kRoomNormCall, kRoomNorm, reinterpret_cast<const void*>(&RoomNormHook), &normPatches);
    if (norm) g_lodPatches.insert(g_lodPatches.end(), normPatches.begin(), normPatches.end());
    else {
        MemPatch::RestoreAll(normPatches);
        LOG_WARNING("[LevelLightShare] Rooms lit only by lamps of another story: the game code differs (or not Steam 1.67.2), left as the game has it");
    }
    // The floor test in the 4 basis maps (optional; BasisLightHook). 0x006A0C4C: lea edx,[esp+0xC8]; push edx; mov ecx,edi; call
    std::vector<MemPatch::PatchLocation> basisPatches;
    const BYTE basisBytes[] = {0x8D, 0x94, 0x24, 0xC8, 0x00, 0x00, 0x00, 0x52, 0x8B, 0xCF};
    kBasisLightCall = GameAddr::Get(Id::BasisLightCall);
    kBasisLight = GameAddr::Get(Id::BasisLight);
    const bool basis = GameAddr::Have({Id::BasisLightCall, Id::BasisLight}) && std::memcmp(reinterpret_cast<const void*>(kBasisLightCall - sizeof basisBytes), basisBytes, sizeof basisBytes) == 0 &&
                       CallsTarget(kBasisLightCall, kBasisLight) &&
                       Redirect(kBasisLightCall, kBasisLight, reinterpret_cast<const void*>(&BasisLightHook), &basisPatches);
    g_basisGuardReady.store(basis, std::memory_order_relaxed);
    if (basis) g_lodPatches.insert(g_lodPatches.end(), basisPatches.begin(), basisPatches.end());
    else {
        MemPatch::RestoreAll(basisPatches);
        LOG_WARNING("[LevelLightShare] The floor test in the directional maps: the game code differs (or not Steam 1.67.2), left as the game has it");
    }
    return true;
}

void RefreshSoon() {
    if (ThreadId() == g_renderThread.load()) RefreshAllLots();
    else g_refreshRequested = true; // done by the next OnPresent on the render thread
}

// ---- F8 (development build): the stories of every loaded lot with rooms, as the indoor part sees them ----
// The trackers of every loaded lot (up to max)
int AllTrackers(uintptr_t* out, int max) {
    int n = 0;
    __try {
        const uintptr_t root = kRootPtr ? *reinterpret_cast<const uintptr_t*>(kRootPtr) : 0;
        const uintptr_t lightMgr = root ? *reinterpret_cast<const uintptr_t*>(root + 0x1C0) : 0;
        const uintptr_t tree = lightMgr ? *reinterpret_cast<const uintptr_t*>(lightMgr + 0xD4) : 0;
        if (!tree) return 0;
        const uintptr_t buckets = *reinterpret_cast<const uintptr_t*>(tree + 0x58);
        const uint32_t bucketCount = *reinterpret_cast<const uint32_t*>(tree + 0x5C);
        if (!buckets || !bucketCount || bucketCount >= (1u << 20)) return 0;
        const uintptr_t endNode = *reinterpret_cast<const uintptr_t*>(buckets + bucketCount * 4);
        uintptr_t slot = buckets;
        uintptr_t node = *reinterpret_cast<const uintptr_t*>(slot);
        int guard = 0;
        while (node == 0 && guard++ < (1 << 20)) node = *reinterpret_cast<const uintptr_t*>(slot += 4);
        guard = 0;
        while (node && node != endNode && guard++ < 100000) {
            const uintptr_t tracker = *reinterpret_cast<const uintptr_t*>(node + 8);
            if (tracker && n < max) out[n++] = tracker;
            node = *reinterpret_cast<const uintptr_t*>(node + 0x10);
            int g2 = 0;
            while (node == 0 && g2++ < (1 << 20)) node = *reinterpret_cast<const uintptr_t*>(slot += 4);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

uintptr_t SafeStoryManager(uintptr_t tracker, int level) {
    __try {
        return StoryManager(tracker, level);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// 5.: every room of one story (the ids its lighting tiles hold, and room 0) lights its walls again
struct RigRoomWatch { DepKey key; uintptr_t mgr; };
struct RigWait { std::vector<RigRoomWatch> rooms; DWORD armed; bool fallbackSent = false; };
std::unordered_map<uintptr_t, RigWait> g_rigWait; // render thread, only after a lamp-driven lot refresh
DWORD g_rigPollAt = 0, g_ambientPollAt = 0;
unsigned g_rigCursor = 0;
// waiting: the room is queued (state 2) and its solve has not started, so its gather will read the lamps as they are now
// (06/10, "all the lights" of a big lot: the safety net sent such rooms again, a second invalidation for nothing)
bool FreshLampSolveImpl(BYTE* room, DWORD changed, bool waiting) {
    const uintptr_t mgr = *reinterpret_cast<const uintptr_t*>(room);
    const int id = *reinterpret_cast<const int*>(room + 0xC);
    if (!waiting) {
        std::lock_guard<std::mutex> lock(g_gatherStampMx);
        const auto stamp = g_gatherStamps.find(reinterpret_cast<uintptr_t>(room));
        if (stamp == g_gatherStamps.end() || stamp->second.mgr != mgr || stamp->second.id != id) return false;
        // a gather in the same tick as the change counts when it came after the latest lamp mark (06/10, recording 12:01:
        // the switched lamp's own room, gathered by the game's walk in that tick, was solved a second time for nothing)
        if (!RoomAmbientPolicy::GatherAfterChange(stamp->second.started, changed)
            && !(stamp->second.started == changed && stamp->second.serial > g_markSerial)) return false;
    }
    const uintptr_t tracker = MgrTracker(mgr);
    if (!tracker) return false;
    const int level = *reinterpret_cast<const int*>(mgr + 0x88);
    if (StoryManager(tracker, level) != mgr) return false;
    const DepKey key{tracker, level, id};
    // An atrium target waiting for this room (g_ambToQueue) is not a reason to gather it again (06/10, recording 11:17: the
    // lamp's own room was solved twice, and its waiting atrium members were sent back to their gather, losing their quick
    // pass): its own state 0 merges with the target, and a member solved before the target changed is sent again by the
    // room update's ambient pass (BeforeRoomUpdate)
    {
        std::lock_guard<std::mutex> lock(g_depsMx);
        if (g_depWait.contains(key)) return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_deferredMx);
        if (std::any_of(g_deferred.begin(), g_deferred.end(), [&](const DeferredRoom& d) {
            return d.tracker == tracker && d.level == key.level && d.id == id;
        })) return false;
    }
    return true;
}
bool FreshLampSolve(BYTE* room, DWORD changed) {
    __try {
        const int state = *reinterpret_cast<const int*>(room + 0xF0);
        if (!RoomAmbientPolicy::RetainFreshSolve(state)) return false;
        return FreshLampSolveImpl(room, changed, state == 2);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
int CachedStoryRooms(uintptr_t tracker, int level, int* ids, int max);
// The room's light list holds one of these lamps (unreadable: true, the room is sent as before)
bool HoldsAnyLamp(const BYTE* room, const uintptr_t* lamps, int count) {
    __try {
        const uintptr_t* b = *reinterpret_cast<const uintptr_t* const*>(room + 0xC8);
        const uintptr_t* e = *reinterpret_cast<const uintptr_t* const*>(room + 0xCC);
        if (!b || e < b || e - b > 4096) return true;
        for (const uintptr_t* p = b; p < e; p++)
            for (int k = 0; k < count; k++)
                if (*p == lamps[k]) return true;
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}
// lamps / lampCount: only the rooms whose light list holds one of them (a lamp moved: the rooms that take it)
int RequeueStory(uintptr_t tracker, int level, DWORD changed = 0, int* skipped = nullptr, std::vector<RigRoomWatch>* watch = nullptr,
                 const uintptr_t* lamps = nullptr, int lampCount = 0) {
    int ids[1024], n = 0, queued = 0;
    __try {
        const uintptr_t mgr = StoryManager(tracker, level);
        if (!mgr) return 0;
        const int w = *reinterpret_cast<const int*>(mgr + 0x264), h = *reinterpret_cast<const int*>(mgr + 0x268);
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return 0;
        ids[n++] = 0;
        const int cached = CachedStoryRooms(tracker, level, ids + n, static_cast<int>(std::size(ids)) - n);
        if (cached >= 0) n += cached;
        else for (int iz = 0; iz < h; iz++)
            for (int ix = 0; ix < w; ix++)
                if (const uintptr_t tile = LightTile(mgr, ix, iz))
                    for (int q = 0; q < 4; q++)
                        if (const int id = TileRoom(tile, q); id > 0 && n < static_cast<int>(std::size(ids)) && std::find(ids, ids + n, id) == ids + n) ids[n++] = id;
        for (int k = 0; k < n; k++) {
            BYTE* room = static_cast<BYTE*>(reinterpret_cast<RoomById_t>(kRoomById)(reinterpret_cast<void*>(mgr), ids[k]));
            if (lampCount > 0 && (!room || !HoldsAnyLamp(room, lamps, lampCount))) continue;
            if (watch && room) watch->push_back({{tracker, level, ids[k]}, mgr});
            if (changed && room && FreshLampSolve(room, changed)) {
                if (skipped) ++*skipped;
                continue;
            }
            queued += QueueRoom(tracker, level, ids[k], true);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
    }
    return queued;
}
// Every room of every loaded lot lights again (after an option that changes them). Render thread. Basements too (stories
// -4..-1; 30/09: "Refresh the lighting" left them out).
// The after-load refresh alone (06/10, user: "when entering the lot it takes long to correct"; log 12:18: the world went
// live, its rooms near openings gathered again and settled in 3.8 s, and then the after-load refresh sent all 75 rooms of
// 16 lots once more, 6 s more): a room gathered again since the world went live already has every lamp of its lot
// (the reason for that refresh, 30/09: a room solved before the other stories' lamps were registered), so it keeps that
// solve (running, waiting or finished), like a lamp edit's safety net keeps a fresh one
std::atomic<DWORD> g_worldLiveAt{0};
// The rooms solve with a larger share of the frame for this long after the world goes live (SettlingAfterLoad; the log
// above: 4.4 ms of solving a frame at 80 fps while the lots corrected themselves)
constexpr DWORD kSettleAfterLoadMs = 15000;
void RequeueAllRooms(const char* why) {
    uintptr_t trackers[256];
    const int lots = AllTrackers(trackers, 256);
    const DWORD live = g_worldLiveAt.load(std::memory_order_relaxed);
    const bool afterLoad = live && std::string_view(why) == "Refresh the lighting (after loading)";
    int queued = 0, kept = 0;
    for (int t = 0; t < lots; t++)
        for (int level = -4; level <= 7; level++) queued += RequeueStory(trackers[t], level, afterLoad ? live : 0, afterLoad ? &kept : nullptr);
    LOG_INFO(std::format("[LevelLightShare] {}: {} rooms of {} lots light again{}", why, queued, lots,
                         afterLoad ? std::format(" ({} gathered again since the world went live keep their solve)", kept) : std::string()));
}

// Rooms at Night (unlit_rooms.cpp): every room (id > 0) of every loaded lot, stories -4..7. The list of (lot, story, id)
// is built from the lighting tiles at most every 3 s (a slider being dragged calls this every frame); each room is looked
// up again by id, so a room gone meanwhile is skipped. visit returns true to send the room to gather again (held while
// it is being solved). Render thread.
struct RoomRef {
    uintptr_t tracker;
    int level, id;
};
std::vector<RoomRef> g_roomRefs;
DWORD g_roomRefsAt = 0;
int CollectStoryRooms(uintptr_t tracker, int level, RoomRef* out, int max) {
    int n = 0;
    __try {
        const uintptr_t mgr = StoryManager(tracker, level);
        if (!mgr) return 0;
        const int w = *reinterpret_cast<const int*>(mgr + 0x264), h = *reinterpret_cast<const int*>(mgr + 0x268);
        if (w <= 0 || h <= 0 || w > 1024 || h > 1024) return 0;
        for (int iz = 0; iz < h; iz++)
            for (int ix = 0; ix < w; ix++)
                if (const uintptr_t tile = LightTile(mgr, ix, iz))
                    for (int q = 0; q < 4; q++) {
                        const int id = TileRoom(tile, q);
                        if (id <= 0 || n >= max) continue;
                        bool known = false;
                        for (int k = 0; k < n && !known; k++) known = out[k].id == id;
                        if (!known) out[n++] = RoomRef{tracker, level, id};
                    }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
    }
    return n;
}
BYTE* SafeRoomById(uintptr_t tracker, int level, int id) {
    __try {
        void* mgr = reinterpret_cast<void*>(StoryManager(tracker, level));
        return mgr ? static_cast<BYTE*>(reinterpret_cast<RoomById_t>(kRoomById)(mgr, id)) : nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
// QueueRoom under SEH (a lot unloaded between the list and the send)
bool AmbientNear(const float* a, const float* b) {
    for (int k = 0; k < 4; k++) if (!std::isfinite(a[k]) || !std::isfinite(b[k]) || std::fabs(a[k] - b[k]) > 1e-5f) return false;
    return true;
}
bool AmbientIdentity(BYTE* room, uintptr_t& mgr, int& level, int& id) {
    __try {
        mgr = *reinterpret_cast<const uintptr_t*>(room);
        level = *reinterpret_cast<const int*>(mgr + 0x88);
        id = *reinterpret_cast<const int*>(room + 0xC);
        return id > 0 && level >= 0 && level <= 7 && !room[0x18];
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool AmbientHeld(BYTE* room, const Applied& old, const float* second, bool unlit) {
    __try {
        const int state = *reinterpret_cast<const int*>(room + 0xF0);
        if ((state != 4 && state != 5) || room[0x18]) return false;
        if (unlit && *reinterpret_cast<const uintptr_t*>(room + 0xCC) > *reinterpret_cast<const uintptr_t*>(room + 0xC8)) return false;
        return AmbientNear(reinterpret_cast<const float*>(room + 0x110), old.c4)
            && (!second || AmbientNear(reinterpret_cast<const float*>(room + 0x120), second))
            && std::fabs(*reinterpret_cast<const float*>(room + 0x160) - old.norm) <= 1e-5f;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool WriteSecondAmbient(BYTE* room, const float* second) {
    __try { std::memcpy(room + 0x120, second, 16); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// Called with g_ambMx held, from the native room-update hook. No other-member
// source-readiness gate: only the already solved, compatible colour presentation waits.
bool GroupColourPending(const DepKey& key) {
    const auto group = g_ambGroups.find(key);
    if (group == g_ambGroups.end() || group->second.empty()) return false;
    const auto armed = g_ambGroupAt.find(group->second.front().key);
    return armed != g_ambGroupAt.end() && !RoomAmbientPolicy::GroupWaitExpired(GetTickCount(), armed->second);
}
GroupApply ApplyGroupAmbient(const DepKey& key, unsigned& budget) {
    const auto group = g_ambGroups.find(key);
    if (group == g_ambGroups.end() || group->second.size() < 2 || group->second.size() > 12) return GroupApply::None;
    const DepKey root = group->second.front().key;
    const auto armed = g_ambGroupAt.find(root);
    if (armed == g_ambGroupAt.end()) return GroupApply::None;
    const auto fallback = [&]() {
        g_ambGroupAt.erase(root);
        g_groupFallbacks.fetch_add(1, std::memory_order_relaxed);
        return GroupApply::None;
    };
    if (RoomAmbientPolicy::GroupWaitExpired(GetTickCount(), armed->second)) return fallback();
    if (group->second.size() > budget) return GroupApply::Waiting;
    struct Prepared { BYTE* room; DepKey key; Applied previous, wanted; };
    Prepared ready[12]{};
    unsigned count = 0;
    for (const auto& member : group->second) {
        const auto old = g_ambApplied.find(member.key);
        const auto next = g_ambToQueue.find(member.key);
        if (old == g_ambApplied.end() || next == g_ambToQueue.end()
            || !RoomAmbientPolicy::AmbientMapsCompatible(old->second.norm, next->second.norm, old->second.wallBase, next->second.wallBase)) return fallback();
        for (float value : next->second.c4) if (!std::isfinite(value)) return fallback();
        if (count && (Differs(ready[0].wanted, next->second.c4, next->second.norm, next->second.wallBase)
            || !AmbientNear(ready[0].wanted.c4, next->second.c4))) return fallback();
        BYTE* room = SafeRoomById(member.key.tracker, member.key.level, member.key.room);
        uintptr_t mgr = 0; int level = 0, id = 0;
        if (!room || !AmbientIdentity(room, mgr, level, id) || level != member.key.level || id != member.key.room
            || SafeStoryManager(member.key.tracker, level) != mgr) return fallback();
        if (!AmbientHeld(room, old->second, nullptr, false)) return GroupApply::Waiting;
        ready[count++] = Prepared{room, member.key, old->second, next->second};
    }
    // Validate the complete group before the first write; native room solves continue
    // freely while waiting. Scale, ramp and light-map textures are never replaced here.
    for (unsigned i = 0; i < count; ++i)
        if (!AmbientHeld(ready[i].room, ready[i].previous, nullptr, false)) return GroupApply::Waiting;
    for (unsigned i = 0; i < count; ++i) WriteAmbient(ready[i].room, ready[i].wanted.c4, 0);
    bool valid = true;
    for (unsigned i = 0; i < count; ++i) valid &= AmbientHeld(ready[i].room, ready[i].wanted, nullptr, false);
    if (!valid) {
        for (unsigned i = 0; i < count; ++i)
            if (AmbientHeld(ready[i].room, ready[i].wanted, nullptr, false)) WriteAmbient(ready[i].room, ready[i].previous.c4, 0);
        return fallback();
    }
    for (unsigned i = 0; i < count; ++i) {
        g_ambApplied.find(ready[i].key)->second = ready[i].wanted;
        g_ambQueuedAt.erase(ready[i].key);
    }
    budget -= count;
    g_ambGroupAt.erase(root);
    g_groupCommits.fetch_add(1, std::memory_order_relaxed);
    g_groupRigsPending.store(true, std::memory_order_relaxed);
    return GroupApply::Applied; // callers retire converged pending entries without invalidating their iterators
}
// Called with g_ambMx held. Change RGB only: map scale and wall ramp remain authoritative.
bool ApplyIdleAmbient(const DepKey& key, const Applied& wanted) {
    if (GroupColourPending(key)) return false;
    const auto old = g_ambApplied.find(key);
    if (old == g_ambApplied.end()
        || !RoomAmbientPolicy::AmbientMapsCompatible(old->second.norm, wanted.norm, old->second.wallBase, wanted.wallBase)) return false;
    BYTE* room = SafeRoomById(key.tracker, key.level, key.room);
    uintptr_t mgr = 0; int level = 0, id = 0;
    if (!room || !AmbientIdentity(room, mgr, level, id) || level != key.level || id != key.room
        || SafeStoryManager(key.tracker, level) != mgr || !AmbientHeld(room, old->second, nullptr, false)) return false;
    for (float value : wanted.c4) if (!std::isfinite(value)) return false;
    WriteAmbient(room, wanted.c4, 0);
    if (!AmbientHeld(room, wanted, nullptr, false)) return false;
    old->second = wanted;
    return true;
}
DepKey g_ambientCursorKey{};
bool g_ambientCursorValid = false;
void ApplyPendingAmbient() {
    const DWORD now = GetTickCount();
    if (static_cast<int32_t>(now - g_ambientPollAt) < 0) return;
    g_ambientPollAt = now + 50;
    size_t visited = 0; int changed = 0;
    {
        std::lock_guard<std::mutex> lock(g_ambMx);
        auto it = g_ambientCursorValid ? g_ambToQueue.find(g_ambientCursorKey) : g_ambToQueue.end();
        if (it == g_ambToQueue.end()) it = g_ambToQueue.begin();
        const size_t limit = std::min<size_t>(128, g_ambToQueue.size());
        while (!g_ambToQueue.empty() && visited++ < limit && changed < 16) {
            if (ApplyIdleAmbient(it->first, it->second)) {
                g_ambQueuedAt.erase(it->first);
                it = g_ambToQueue.erase(it);
                ++changed;
            } else ++it;
            if (it == g_ambToQueue.end()) it = g_ambToQueue.begin();
        }
        g_ambientCursorValid = it != g_ambToQueue.end();
        if (g_ambientCursorValid) g_ambientCursorKey = it->first;
    }
    // Furniture is refreshed by the lot-completion watcher, not once per member.
}
int WatchedRoomState(const RigRoomWatch& watch) {
    __try {
        if (StoryManager(watch.key.tracker, watch.key.level) != watch.mgr) return -1;
        const BYTE* room = static_cast<const BYTE*>(reinterpret_cast<RoomById_t>(kRoomById)(reinterpret_cast<void*>(watch.mgr), watch.key.room));
        if (!room || *reinterpret_cast<const uintptr_t*>(room) != watch.mgr
            || *reinterpret_cast<const int*>(room + 0xC) != watch.key.room) return -1;
        return *reinterpret_cast<const int*>(room + 0xF0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return -1; }
}
bool RigWorkPending(uintptr_t tracker) {
    {
        std::lock_guard<std::mutex> lock(g_ambMx);
        if (std::any_of(g_ambToQueue.begin(), g_ambToQueue.end(), [tracker](const auto& item) { return item.first.tracker == tracker; })) return true;
    }
    {
        std::lock_guard<std::mutex> lock(g_deferredMx);
        if (std::any_of(g_deferred.begin(), g_deferred.end(), [tracker](const DeferredRoom& item) { return item.tracker == tracker; })) return true;
    }
    return DepWaitsFor(tracker);
}
void RefreshCompletedLotRigs() {
    const DWORD now = GetTickCount();
    if (g_rigWait.empty() || static_cast<int32_t>(now - g_rigPollAt) < 0) return;
    g_rigPollAt = now + 50;
    bool refresh = false;
    uintptr_t lots[256];
    unsigned count = 0;
    for (const auto& item : g_rigWait) if (count < std::size(lots)) lots[count++] = item.first;
    const unsigned start = g_rigCursor % count, visit = std::min(count, 4u);
    g_rigCursor = (start + visit) % count;
    for (unsigned n = 0; n < visit; ++n) {
        auto it = g_rigWait.find(lots[(start + n) % count]);
        if (it == g_rigWait.end()) continue;
        RigWait& wait = it->second;
        bool complete = !wait.rooms.empty(), valid = true;
        for (const auto& room : wait.rooms) {
            const int state = WatchedRoomState(room);
            if (state < 0) { valid = false; break; }
            if (state != 4 && state != 5) complete = false;
        }
        if (!valid) { g_rigWait.erase(it); continue; }
        complete = complete && !RigWorkPending(it->first);
        if (complete) {
            refresh = true;
            g_rigWait.erase(it);
            continue;
        }
        if (RoomAmbientPolicy::RigFallbackDue(now, wait.armed, wait.fallbackSent)) {
            refresh = true;
            wait.fallbackSent = true;
        }
        if (now - wait.armed >= 6000) g_rigWait.erase(it);
    }
    if (refresh) ObjectLightBridge::RequestRigRefresh();
}
bool StageAmbient(BYTE* room, const float* oldOwn, const float* newOwn, const float* oldSecond, const float* newSecond, bool& changed) {
    changed = false;
    if (!AmbientActive()) return false;
    uintptr_t mgr = 0; int level = 0, id = 0;
    if (!AmbientIdentity(room, mgr, level, id)) return false;
    const uintptr_t tracker = MgrTracker(mgr);
    if (!tracker || StoryManager(tracker, level) != mgr) return false;
    const DepKey key{tracker, level, id};
    std::lock_guard<std::mutex> lock(g_ambMx);
    const auto own = g_ambOrig.find(key);
    const auto applied = g_ambApplied.find(key);
    if (own == g_ambOrig.end() || applied == g_ambApplied.end() || !g_ambGroups.contains(key)) return false;
    const bool unlit = !oldOwn;
    if (oldOwn && !AmbientNear(own->second.c4, oldOwn)) return false;
    if (!AmbientHeld(room, applied->second, unlit ? own->second.c4 : oldSecond, unlit)) return false;
    if (AmbientNear(own->second.c4, newOwn) && unlit) return true;
    if (!WriteSecondAmbient(room, newSecond)) return false;
    std::memcpy(own->second.c4, newOwn, 16);
    g_ambDirty.insert(key);
    changed = true;
    return true;
}

// The story of a tree level (tl+0x1A0, as BeforeRoomUpdate reads it); -99 when unreadable
int StoryOfTreeLevel(uintptr_t tl) {
    __try {
        return *reinterpret_cast<const int*>(tl + 0x1A0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -99;
    }
}
bool QueueRoomSafe(uintptr_t tracker, int level, int id) {
    __try {
        return QueueRoom(tracker, level, id, true);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_faults.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
}
std::vector<uintptr_t> g_roomRefTrackers; // the loaded lots when g_roomRefs was built
std::vector<uintptr_t> g_roomRefManagers; // story managers can change while the lot tracker survives
int CachedStoryRooms(uintptr_t tracker, int level, int* ids, int max) {
    if (level < -4 || level > 7 || g_roomRefsDirty.load() || GetTickCount() - g_roomRefsAt > 3000) return -1;
    const auto lot = std::lower_bound(g_roomRefTrackers.begin(), g_roomRefTrackers.end(), tracker);
    if (lot == g_roomRefTrackers.end() || *lot != tracker) return -1;
    const size_t manager = static_cast<size_t>(lot - g_roomRefTrackers.begin()) * 12 + level + 4;
    if (manager >= g_roomRefManagers.size() || g_roomRefManagers[manager] != SafeStoryManager(tracker, level)) return -1;
    int count = 0;
    for (const auto& ref : g_roomRefs) if (ref.tracker == tracker && ref.level == level) {
        if (count >= max) return -1;
        ids[count++] = ref.id;
    }
    return count;
}
// lazy (the once-a-second scan of switched-off lamps): a change of the loaded lots does not walk the tiles again (lots
// stream in and out while the camera moves); the rooms of lots gone are skipped, new lots wait for the next walk (maxAge)
int ForEachRoomImpl(bool (*visit)(unsigned char* room, void* ctx), void* ctx, int* queued, DWORD maxAge = 3000, bool lazy = false, void (*ack)(unsigned char*, bool, void*) = nullptr) {
    LoadAddresses(); // also when the light between stories never installed (Rooms at Night alone; review 30/09, M4)
    if (!kRootPtr || !kRoomById) return 0;
    const DWORD now = GetTickCount();
    // The loaded lots now: a lot gone or rebuilt (its tracker left the tree) drops the cached list at once, so no room of
    // a freed lot is ever written or sent (review 30/09, H1)
    uintptr_t trackers[256];
    const int lots = AllTrackers(trackers, 256);
    std::vector<uintptr_t> nowSet(trackers, trackers + lots);
    std::sort(nowSet.begin(), nowSet.end());
    const bool lotsChanged = nowSet != g_roomRefTrackers;
    std::vector<uintptr_t> managers;
    managers.reserve(nowSet.size() * 12);
    for (uintptr_t tracker : nowSet)
        for (int level = -4; level <= 7; level++) managers.push_back(SafeStoryManager(tracker, level));
    const bool managersChanged = managers != g_roomRefManagers;
    const bool structureChanged = g_roomRefsDirty.exchange(false);
    if (RoomAmbientPolicy::RebuildRoomList(g_roomRefs.empty(), structureChanged || now - g_roomRefsAt > maxAge, lotsChanged, managersChanged, lazy)) {
        if (lotsChanged || managersChanged) {
            // Streaming another lot must not discard valid colours/groups in this lot.
            for (size_t i = 0; i < g_roomRefTrackers.size(); ++i) {
                const uintptr_t previous = g_roomRefTrackers[i];
                const auto current = std::lower_bound(nowSet.begin(), nowSet.end(), previous);
                bool changed = current == nowSet.end() || *current != previous;
                if (!changed) {
                    const size_t j = static_cast<size_t>(current - nowSet.begin());
                    changed = g_roomRefManagers.size() < (i + 1) * 12 || managers.size() < (j + 1) * 12
                        || !std::equal(g_roomRefManagers.begin() + i * 12, g_roomRefManagers.begin() + (i + 1) * 12, managers.begin() + j * 12);
                }
                if (changed) ForgetAmbientLot(previous);
            }
            UnlitRooms::OnRoomsChanged();
        }
        g_roomRefsAt = now;
        g_roomRefs.clear();
        g_roomRefTrackers = nowSet;
        g_roomRefManagers = managers;
        static RoomRef buf[1024];
        for (int t = 0; t < lots; t++)
            for (int level = -4; level <= 7; level++) {
                const int n = CollectStoryRooms(trackers[t], level, buf, 1024);
                g_roomRefs.insert(g_roomRefs.end(), buf, buf + n);
            }
    }
    const bool filter = nowSet != g_roomRefTrackers; // lazy and the lots changed: only the lots still loaded
    int visited = 0, sent = 0;
    for (const RoomRef& r : g_roomRefs) {
        if (filter && !std::binary_search(nowSet.begin(), nowSet.end(), r.tracker)) continue;
        BYTE* room = SafeRoomById(r.tracker, r.level, r.id);
        if (!room) continue;
        visited++;
        if (visit(room, ctx)) {
            const bool queued = QueueRoomSafe(r.tracker, r.level, r.id);
            if (queued) sent++;
            if (ack) ack(room, queued, ctx);
        }
    }
    if (queued) *queued = sent;
    return visited;
}

// ---- Rooms still lit by a lamp switched off (30/09) ----
// A room's light list (room+0xC8..+0xCC) holds the lights its last gather took. The gather's filter (GameTakesLight: the
// lit flag +0x100 & 0x20 and LightBright, FUN_006bc520: the lit colour +0xE0..+0xE8 summed >= [0x010459E4]) leaves a lamp
// switched off out. F8 30/09 01:26 (house C49C001BCF2DEA20, every lamp off; the game switches a lamp off by its intensity,
// the lit flag stays): after the load, room 2 of story 1 still held two of its lamps switched off for ~25 s (lights 4,
// then 2 once the user switched a lamp on; user: "right after entering the game the background colour was wrong, after a
// while it fixed itself"). Once a second an indoor room at rest whose list still holds a lamp the filter refuses now is
// sent to gather again, once per such set of lamps. Left out: room 0 (the outside stays as it is), window lights (types 7
// and 8 follow the sky) and street lamps (lot 0).
constexpr DWORD kStaleEvery = 1000;
constexpr DWORD kStaleRoomsAge = 10000; // the room list (tile walk) is made again at most this often for this scan
DWORD g_staleAt = 0;
std::unordered_map<uintptr_t, uint64_t> g_staleSent; // room -> the switched-off lamps it was sent for (render thread)
std::atomic<long> g_staleSends{0};
uint64_t StaleLamps(const BYTE* room, bool& busy) {
    busy = false;
    __try {
        if (*reinterpret_cast<const int*>(room + 0xC) <= 0) return 0;
        const int state = *reinterpret_cast<const int*>(room + 0xF0);
        if (RoomAmbientPolicy::SolverOwnsAmbient(state)) { busy = true; return 0; } // preserve the last sent signature while the list is being made
        const uintptr_t* b = *reinterpret_cast<const uintptr_t* const*>(room + 0xC8);
        const uintptr_t* e = *reinterpret_cast<const uintptr_t* const*>(room + 0xCC);
        uint64_t sig = 0;
        for (const uintptr_t* p = b; p < e && p - b < 4096; p++) {
            const uintptr_t L = *p;
            if (!L) continue;
            const int type = *reinterpret_cast<const int*>(L + 0xB0);
            if (type == 7 || type == 8) continue;
            if (type == 0xB && (*reinterpret_cast<const uint32_t*>(L + 0xC0) | *reinterpret_cast<const uint32_t*>(L + 0xC4)) == 0) continue;
            if ((*reinterpret_cast<const BYTE*>(L + 0x100) & 0x20) && reinterpret_cast<LightBright_t>(kLightBright)(reinterpret_cast<void*>(L))) continue;
            uint64_t h = static_cast<uint64_t>(L) * 0x9E3779B97F4A7C15ull;
            sig += (h ^ (h >> 29)) | 1;
        }
        return sig;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
bool VisitStale(unsigned char* room, void*) {
    bool busy = false;
    const uint64_t sig = StaleLamps(room, busy);
    if (busy) return false; // a solve is not evidence that the previously seen off lamps disappeared
    const uintptr_t key = reinterpret_cast<uintptr_t>(room);
    if (!sig) {
        g_staleSent.erase(key);
        return false;
    }
    const auto [it, fresh] = g_staleSent.try_emplace(key, sig);
    if (!fresh && it->second == sig) return false; // sent for these lamps already (the gather kept them: not again)
    it->second = sig;
    g_staleSends.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---- Development tools: the F6 room tracer. While a recording runs, every 100 ms, each indoor room of the loaded lots
// is written once and again whenever its ambient (+0x110, the walls' colour, and +0x120), normalisation, solve state,
// LOD class or light count changes (the Rooms at Night retint writes +0x110 without a solve, so it shows here too). ----
struct RoomSnap {
    float amb2[4];
    int lights;
};
bool ReadRoomExtra(const BYTE* room, RoomSnap& s) {
    __try {
        std::memcpy(s.amb2, room + 0x120, sizeof s.amb2);
        s.lights = static_cast<int>(ListSize(room));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
std::unordered_map<uint64_t, uint64_t> g_roomTrace; // (lot, story, room) -> hash of the last written state
std::vector<std::string> TraceRoomsImpl(bool reset) {
    std::vector<std::string> out;
    if (reset) g_roomTrace.clear();
    struct Ctx {
        std::vector<std::string>* out;
    } ctx{&out};
    ForEachRoomImpl(
        [](unsigned char* room, void* p) {
            auto& c = *static_cast<Ctx*>(p);
            SolveNote n{};
            RoomSnap x{};
            if (!ReadSolveNote(room, n) || !ReadRoomExtra(room, x)) return false;
            const auto q = [](float v) { return static_cast<int64_t>(std::llround(static_cast<double>(v) * 100000.0)); };
            uint64_t key = 1469598103934665603ull, state = 1469598103934665603ull;
            for (int64_t v : {static_cast<int64_t>(n.lot), static_cast<int64_t>(n.level), static_cast<int64_t>(n.id)}) key = (key ^ static_cast<uint64_t>(v)) * 1099511628211ull;
            for (int64_t v : {q(n.c4[0]), q(n.c4[1]), q(n.c4[2]), q(x.amb2[0]), q(x.amb2[1]), q(x.amb2[2]), q(n.norm), static_cast<int64_t>(n.state), static_cast<int64_t>(n.cls),
                              static_cast<int64_t>(n.shown), static_cast<int64_t>(x.lights), static_cast<int64_t>(n.cam)})
                state = (state ^ static_cast<uint64_t>(v)) * 1099511628211ull;
            if (g_roomTrace.size() > 20000) g_roomTrace.clear();
            auto [it, fresh] = g_roomTrace.try_emplace(key, state);
            if (!fresh && it->second == state) return false;
            it->second = state;
            c.out->push_back(std::format("[room] {} lot {:08X} story {} room {} | state {} class {}/{} | lights {} | ambient ({:.4f} {:.4f} {:.4f}) second ({:.4f} {:.4f} {:.4f}) | norm {:.3f} | story shown {}",
                                         fresh ? "new" : "changed", n.lot, n.level, n.id, n.state, n.cls, n.shown, x.lights, n.c4[0], n.c4[1], n.c4[2], x.amb2[0], x.amb2[1],
                                         x.amb2[2], n.norm, n.cam));
            return false;
        },
        &ctx, nullptr, 3000, true);
    return out;
}

// Half of a lot's id (manager +0x94 high, +0x90 low; the F8 lot list prints them together)
uint32_t LotIdPart(uintptr_t tracker, int offset) {
    __try {
        const uintptr_t mgr = StoryManager(tracker, 0);
        return mgr ? *reinterpret_cast<const uint32_t*>(mgr + offset) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

struct StoryRow {
    int lightW = 0, lightH = 0, floorW = -1, floorH = -1; // lighting tile grid; floor grid (-1: no floor grid)
    int tiles = 0, indoorQuads = 0, floorQuads = 0, openings = 0;
    float lowest = 0.0f; // the lowest floor, lot space (mgr+0x98 - mgr+0xD4)
};
bool ReadStoryRow(uintptr_t mgr, uintptr_t mgrBelow, uintptr_t level, StoryRow& r) {
    __try {
        r.lightW = *reinterpret_cast<const int*>(mgr + 0x264);
        r.lightH = *reinterpret_cast<const int*>(mgr + 0x268);
        r.lowest = *reinterpret_cast<const float*>(mgr + 0x98) - *reinterpret_cast<const float*>(mgr + 0xD4);
        const uintptr_t grid = level ? *reinterpret_cast<const uintptr_t*>(level + 0x264) : 0;
        if (grid) {
            r.floorW = *reinterpret_cast<const int*>(grid + 0x10);
            r.floorH = *reinterpret_cast<const int*>(grid + 0x14);
        }
        if (r.lightW <= 0 || r.lightH <= 0 || r.lightW > 1024 || r.lightH > 1024) return true;
        for (int iz = 0; iz < r.lightH; iz++)
            for (int ix = 0; ix < r.lightW; ix++) {
                const uintptr_t tile = LightTile(mgr, ix, iz);
                if (!tile) continue;
                r.tiles++;
                for (int q = 0; q < 4; q++) {
                    const bool indoor = TileRoom(tile, q) > 0, floor = level && FloorAt(level, ix, iz, q) != 0;
                    r.indoorQuads += indoor;
                    r.floorQuads += floor;
                    const uintptr_t below = mgrBelow ? LightTile(mgrBelow, ix, iz) : 0;
                    r.openings += below && TileRoom(below, q) > 0 && level && !floor; // the rule of ReadOpenings
                }
            }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A story as two maps side by side (development build, F8): per tile, the room of quadrant 0 in base 36 ('.' = outside,
// ' ' = no tile, 'L' = a lamp registered on the story) and the class of its floor keys; plus the most
// common floor key values.
struct StoryMap {
    int w = 0, h = 0;
    char rooms[64][65] = {};
    char floors[64][65] = {};
    uint32_t keyLo[12] = {}, keyHi[12] = {}, keyCount[12] = {};
    int keys = 0, otherKeys = 0;
    uint32_t head0[12] = {}, head1[12] = {}, headCount[12] = {}; // the record's first two dwords (before the keys)
    int heads = 0;
};
bool ReadStoryMap(uintptr_t tl, uintptr_t mgr, uintptr_t level, const Xform& xf, StoryMap& m) {
    __try {
        m.w = std::min(*reinterpret_cast<const int*>(mgr + 0x264), 64);
        m.h = std::min(*reinterpret_cast<const int*>(mgr + 0x268), 64);
        if (m.w <= 0 || m.h <= 0) return true;
        const uintptr_t grid = level ? *reinterpret_cast<const uintptr_t*>(level + 0x264) : 0;
        const uintptr_t data = grid ? *reinterpret_cast<const uintptr_t*>(grid) : 0;
        const int gw = grid ? *reinterpret_cast<const int*>(grid + 0x10) : 0, gh = grid ? *reinterpret_cast<const int*>(grid + 0x14) : 0;
        for (int iz = 0; iz < m.h; iz++) {
            for (int ix = 0; ix < m.w; ix++) {
                const uintptr_t tile = LightTile(mgr, ix, iz);
                const int room = tile ? TileRoom(tile, 0) : -1;
                m.rooms[iz][ix] = room < 0 ? ' ' : room == 0 ? '.' : "0123456789abcdefghijklmnopqrstuvwxyz"[room % 36];
                // floor per tile: '0' never built, '-' removed (an opening), 'a' / 'B' the bare 40000000 along walls (B: with the
                // 0x08000000 bit of the high dword), '#' / 'F' a floor (F: with that bit), '*' quadrants that differ
                char cls = 0;
                if (data && ix < gw && iz < gh)
                    for (int q = 0; q < 4; q++) {
                        const uint32_t* key = reinterpret_cast<const uint32_t*>(data + (static_cast<size_t>(iz) * gw + ix) * 40 + 8 + q * 8);
                        const bool empty = key[0] == 0xFFFFFFF8u && key[1] == 0xFFFFFFFFu, flag = (key[1] & 0x08000000u) != 0;
                        const char c = empty ? '0' : RemovedFloorKey(key) ? '-' : key[0] == 0x40000000u ? (flag ? 'B' : 'a') : (flag ? 'F' : '#');
                        cls = q == 0 || cls == c ? c : '*';
                        int k = 0;
                        while (k < m.keys && (m.keyLo[k] != key[0] || m.keyHi[k] != key[1])) k++;
                        if (k < m.keys) m.keyCount[k]++;
                        else if (m.keys < 12) {
                            m.keyLo[m.keys] = key[0], m.keyHi[m.keys] = key[1], m.keyCount[m.keys] = 1;
                            m.keys++;
                        } else
                            m.otherKeys++;
                    }
                if (data && ix < gw && iz < gh) {
                    const uint32_t* rec = reinterpret_cast<const uint32_t*>(data + (static_cast<size_t>(iz) * gw + ix) * 40);
                    int k = 0;
                    while (k < m.heads && (m.head0[k] != rec[0] || m.head1[k] != rec[1])) k++;
                    if (k < m.heads) m.headCount[k]++;
                    else if (m.heads < 12) {
                        m.head0[m.heads] = rec[0], m.head1[m.heads] = rec[1], m.headCount[m.heads] = 1;
                        m.heads++;
                    }
                }
                m.floors[iz][ix] = data ? cls : '?';
            }
        }
        WalkRegistry(tl, [&](uintptr_t entry) {
            const uintptr_t light = *reinterpret_cast<const uintptr_t*>(entry + 0x24);
            if (!light) return;
            float lp[3];
            ToLocal(xf, reinterpret_cast<const float*>(light + 0x120), lp);
            const int ix = static_cast<int>(std::floor(lp[0])), iz = static_cast<int>(std::floor(lp[2]));
            if (ix >= 0 && iz >= 0 && ix < m.w && iz < m.h) m.rooms[iz][ix] = 'L';
        });
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The tiles of a story whose 4 floor quadrants are not all the same, and those around its newest room: per quadrant its
// key and room, plus the lighting
// tile's height and its 7 flag words (+0x48). Development build, F8.
struct MixedTile {
    int ix, iz;
    uint32_t hi[4], lo[4];
    uint32_t head[2];
    int room[4];
    float height;
    uint16_t flags[7];
};
int ReadMixedTiles(uintptr_t mgr, uintptr_t level, MixedTile* out, int max) {
    int n = 0;
    __try {
        const int w = std::min(*reinterpret_cast<const int*>(mgr + 0x264), 64), h = std::min(*reinterpret_cast<const int*>(mgr + 0x268), 64);
        const uintptr_t grid = level ? *reinterpret_cast<const uintptr_t*>(level + 0x264) : 0;
        const uintptr_t data = grid ? *reinterpret_cast<const uintptr_t*>(grid) : 0;
        const int gw = grid ? *reinterpret_cast<const int*>(grid + 0x10) : 0, gh = grid ? *reinterpret_cast<const int*>(grid + 0x14) : 0;
        if (!data) return 0;
        // the story's newest room (highest id): a test room just built; its tiles and the ring around them are listed too
        int newest = 0;
        for (int iz = 0; iz < h; iz++)
            for (int ix = 0; ix < w; ix++)
                if (const uintptr_t tile = LightTile(mgr, ix, iz))
                    for (int q = 0; q < 4; q++) newest = std::max(newest, TileRoom(tile, q));
        const auto nearNewest = [&](int ix, int iz) {
            for (int dz = -1; dz <= 1; dz++)
                for (int dx = -1; dx <= 1; dx++)
                    if (const uintptr_t tile = LightTile(mgr, ix + dx, iz + dz))
                        for (int q = 0; q < 4; q++)
                            if (newest > 0 && TileRoom(tile, q) == newest) return true;
            return false;
        };
        for (int iz = 0; iz < h && iz < gh; iz++)
            for (int ix = 0; ix < w && ix < gw; ix++) {
                const uint32_t* key = reinterpret_cast<const uint32_t*>(data + (static_cast<size_t>(iz) * gw + ix) * 40 + 8);
                bool same = true;
                for (int q = 1; q < 4; q++) same &= key[q * 2] == key[0] && key[q * 2 + 1] == key[1];
                if ((same && !nearNewest(ix, iz)) || n >= max) continue;
                MixedTile& t = out[n++];
                t.ix = ix, t.iz = iz;
                const uintptr_t tile = LightTile(mgr, ix, iz);
                for (int q = 0; q < 4; q++) {
                    t.lo[q] = key[q * 2], t.hi[q] = key[q * 2 + 1];
                    t.room[q] = tile ? TileRoom(tile, q) : -1;
                }
                t.head[0] = key[-2], t.head[1] = key[-1];
                t.height = tile ? *reinterpret_cast<const float*>(tile + 0x78) : 0.0f;
                for (int k = 0; k < 7; k++) t.flags[k] = tile ? reinterpret_cast<const uint16_t*>(tile + 0x48)[k] : 0;
            }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

std::string IndoorMaps(uintptr_t tracker) {
    std::string s;
    Xform xf;
    const uintptr_t mgr0 = SafeStoryManager(tracker, 0);
    if (!mgr0 || !ReadXform(mgr0, xf)) return s;
    for (int st = 0; st <= 7; st++) {
        const uintptr_t mgr = SafeStoryManager(tracker, st);
        if (!mgr) continue;
        auto m = std::make_unique<StoryMap>();
        if (!ReadStoryMap(TreeLevel(tracker, st), mgr, LevelFor(mgr), xf, *m)) {
            s += std::format("  map of story {}: unreadable\n", st);
            continue;
        }
        s += std::format("  map of story {} (rows z 0..{}, columns x 0..{}): rooms | floor (0 never built, - removed = opening, a / B bare 40000000, # / F "
                         "floor, * mixed); keys:",
                         st, m->h - 1, m->w - 1);
        for (int k = 0; k < m->keys; k++) s += std::format(" {:08X}:{:08X} x{}", m->keyHi[k], m->keyLo[k], m->keyCount[k]);
        s += "; record heads:";
        for (int k = 0; k < m->heads; k++) s += std::format(" {:08X}:{:08X} x{}", m->head0[k], m->head1[k], m->headCount[k]);
        s += std::format("{}\n", m->otherKeys ? std::format(" (+{} others)", m->otherKeys) : "");
        for (int iz = 0; iz < m->h; iz++) s += std::format("  {:2} {} | {}\n", iz, std::string_view(m->rooms[iz], m->w), std::string_view(m->floors[iz], m->w));
        if (st < 1 || st > 3) continue;
        auto mixed = std::make_unique<MixedTile[]>(160);
        const int n = ReadMixedTiles(mgr, LevelFor(mgr), mixed.get(), 160);
        s += std::format("  mixed tiles of story {} and the tiles around its newest room ({}; quadrants 0 -z, 1 +x, 2 +z, 3 -x: key hi:lo room):\n", st, n);
        for (int i = 0; i < n; i++) {
            const MixedTile& t = mixed[i];
            s += std::format("   ({:2},{:2}) h={:.2f}", t.ix, t.iz, t.height);
            for (int q = 0; q < 4; q++) s += std::format(" | {:08X}:{:08X} r{}", t.hi[q], t.lo[q], t.room[q]);
            s += std::format(" | head {:08X}:{:08X}", t.head[0], t.head[1]);
            s += " | flags";
            for (int k = 0; k < 7; k++) s += std::format(" {:04X}", t.flags[k]);
            s += "\n";
        }
    }
    return s;
}

// Pairs of wall samples at the same place across the floor line of an atrium group: the one just under it (lower room)
// and the one just over it (upper room), with the summed light, the normalisation and the value after the curve
std::string SeamDiagText() {
    std::vector<SeamRec> recs;
    {
        std::lock_guard<std::mutex> lk(g_seamMx);
        recs.swap(g_seam);
        g_seamFull = false;
    }
    std::string s = std::format("\n==== SEAM (atrium walls at the floor line, {} samples) ====\n", recs.size());
    if (recs.empty()) return s + "No samples (recorded while the diagnostics are armed, from the solves of rooms in an atrium group).\n";
    s += "latest solves first | lower room sample (just under the line) | upper room sample (just over it): LOD class, y, summed light, normalisation, after the curve\n";
    int pairs = 0;
    double sumRatio = 0.0, sumLower = 0.0, sumUpper = 0.0;
    std::vector<std::pair<int, int>> printed;
    for (auto lit = recs.rbegin(); lit != recs.rend(); ++lit) { // latest first: the walls as the solves left them
        const SeamRec& lo = *lit;
        const float under = lo.base + 3.0f - lo.p[1];
        if (under < -0.02f || under > 0.45f) continue;
        const SeamRec* best = nullptr;
        float bestD = 1e9f;
        for (auto uit = recs.rbegin(); uit != recs.rend(); ++uit) {
            const SeamRec& up = *uit;
            if (up.level != lo.level + 1 || lo.n[0] * up.n[0] + lo.n[1] * up.n[1] + lo.n[2] * up.n[2] < 0.9f) continue;
            const float over = up.p[1] - up.base;
            const float dx = up.p[0] - lo.p[0], dz = up.p[2] - lo.p[2];
            if (over < -0.05f || over > 0.45f || std::fabs(dx) > 0.2f || std::fabs(dz) > 0.2f) continue;
            const float d = dx * dx + dz * dz + (up.p[1] - lo.p[1]) * (up.p[1] - lo.p[1]);
            if (d < bestD) bestD = d, best = &up; // on a tie the latest stays
        }
        if (!best) continue;
        const std::pair<int, int> key{static_cast<int>(lo.p[0] * 8), static_cast<int>(lo.p[2] * 8)};
        if (std::find(printed.begin(), printed.end(), key) != printed.end()) continue;
        printed.push_back(key);
        pairs++;
        sumLower += lo.fin, sumUpper += best->fin;
        if (lo.fin > 1e-4f) sumRatio += best->fin / lo.fin;
        if (pairs <= 120)
            s += std::format("({:.2f} {:.2f}) n({:.2f} {:.2f}) | room {} c{} y={:.2f} sum={:.3f} norm={:.3f} fin={:.3f} | room {} c{} y={:.2f} sum={:.3f} norm={:.3f} fin={:.3f}\n",
                             lo.p[0], lo.p[2], lo.n[0], lo.n[2], lo.room, lo.cls, lo.p[1], lo.sum, lo.norm, lo.fin, best->room, best->cls, best->p[1], best->sum, best->norm,
                             best->fin);
    }
    s += std::format("{} places | mean after the curve: lower {:.3f}, upper {:.3f} | mean ratio upper/lower {:.3f}\n", pairs, pairs ? sumLower / pairs : 0.0,
                     pairs ? sumUpper / pairs : 0.0, pairs ? sumRatio / pairs : 0.0);
    return s;
}

// F8: floors whose kept copy of their story manager (+0x238) differs from the one found through their lot (LevelManager);
// with mgr, only those whose stale copy names mgr (the floors the copy would have paired with that story by mistake)
int StaleFloorCopies(uintptr_t mgr) {
    std::lock_guard<std::mutex> lk(g_levelsMx);
    int n = 0;
    for (const uintptr_t l : g_levels) {
        const uintptr_t copy = LevelManagerCopy(l);
        if (copy && copy != LevelManager(l) && (!mgr || copy == mgr)) n++;
    }
    return n;
}

// F8: every floor object that names the story through its lot: "address (world level W, floor | ceiling layer)"
std::string FloorsNaming(uintptr_t mgr) {
    std::string s;
    std::lock_guard<std::mutex> lk(g_levelsMx);
    for (auto it = g_levels.rbegin(); it != g_levels.rend(); ++it)
        if (LevelManager(*it) == mgr)
            s += std::format("{}{:08X} (world level {}, {})", s.empty() ? "" : ", ", *it, LevelWorld(*it), LevelOwnFloor(*it) ? "its floor" : "the ceiling layer");
    return s.empty() ? "none" : s;
}

std::string IndoorDiagText() {
    int linked = 0, same = 0, stale = 0, noLot = 0, known = 0;
    {
        std::lock_guard<std::mutex> lk(g_levelsMx);
        known = static_cast<int>(g_levels.size());
        for (const uintptr_t l : g_levels) {
            const uintptr_t fresh = LevelManager(l), copy = LevelManagerCopy(l);
            linked += fresh != 0;
            same += fresh && fresh == copy;
            stale += fresh && copy && fresh != copy;
            noLot += !fresh && copy;
        }
    }
    std::string s = std::format("\n==== STORIES INDOORS (indoor lamps through stair openings) ====\n{} | floor objects known: {}, linked to a story through their lot: {} "
                                "({} the same as their own copy, {} with a stale copy, not used), a copy but no lot lighting now: {} | floor changes: {}, lots updated "
                                "after them: {}\n",
                                !g_indoorReady ? "Not installed" : g_indoorOn ? "On" : "Off", known, linked, same, stale, noLot, g_floorEdits.load(),
                                g_floorRefreshes.load());
    s += std::format("Lamps taken through openings: {} | points tested: {}, a floor in the way: {}, a wall of the lamp's room: {} | rooms sent to gather again: {}\n",
                     g_indoorAdded.load(), g_indoorTests.load(), g_indoorFloorBlocked.load(), g_indoorWallBlocked.load(), g_indoorQueued.load());
    uintptr_t trackers[256];
    const int lots = AllTrackers(trackers, 256);
    int shown = 0;
    for (int t = 0; t < lots; t++) {
        std::string rows;
        bool rooms = false;
        for (int st = 0; st <= 7; st++) {
            const uintptr_t mgr = SafeStoryManager(trackers[t], st);
            if (!mgr) continue;
            const uintptr_t level = LevelFor(mgr);
            StoryRow r;
            if (!ReadStoryRow(mgr, st > 0 ? SafeStoryManager(trackers[t], st - 1) : 0, level, r)) {
                rows += std::format("  story {}: unreadable\n", st);
                continue;
            }
            rooms |= r.indoorQuads > 0;
            const int stale = StaleFloorCopies(mgr);
            rows += std::format("  story {}: manager {:08X}, floor object {:08X}{}{} | lighting tiles {}x{} ({} tiles, {} indoor quadrants) | floor grid {}x{}, {} "
                                "quadrants with a floor | openings (no floor over an indoor room below): {} | lowest floor {:.2f}\n",
                                st, mgr, level, level && LevelManagerCopy(level) != mgr ? " (its own copy of the link is stale)" : "",
                                stale ? std::format(" ({} other floor(s) with a stale copy naming this story, ignored)", stale) : std::string(), r.lightW, r.lightH, r.tiles,
                                r.indoorQuads, r.floorW, r.floorH, r.floorQuads, r.openings, r.lowest);
            rows += std::format("    objects naming story {} through the lot (newest first): {}\n", st, FloorsNaming(mgr));
        }
        if (!rooms) continue; // lots without indoor rooms (parks, empty lots) are left out
        shown++;
        s += std::format("lot {:08X}{:08X} (tracker {:08X}):\n{}{}", LotIdPart(trackers[t], 0x94), LotIdPart(trackers[t], 0x90), trackers[t], rows, IndoorMaps(trackers[t]));
    }
    s += std::format("({} loaded lots, {} with indoor rooms shown)\n", lots, shown);
    std::vector<std::string> log;
    {
        std::lock_guard<std::mutex> lk(g_gatherLogMx);
        log.swap(g_gatherLog);
    }
    s += std::format("-- indoor gathers since the last dump ({}; lamp flags: near an opening, class wrapped, the game takes it, already in the list) --\n", log.size());
    for (const std::string& l : log) s += l + "\n";
    return s;
}

} // namespace

namespace LevelLightShare {
void BeginSeamRecording() {
    std::lock_guard<std::mutex> lock(g_recordedSeamMx);
    g_recordedSeams.clear();
    g_recordedSeams.reserve(kRecordedSeamLimit);
    g_recordedSeamNext = 0;
    g_recordedSeamStart = GetTickCount();
    g_recordedSeamEpoch.fetch_add(1, std::memory_order_relaxed);
    g_recordSeams.store(true, std::memory_order_relaxed);
}
std::string EndSeamRecording(bool save) {
    std::vector<RecordedSeam> samples;
    size_t first = 0;
    DWORD started = 0;
    {
        std::lock_guard<std::mutex> lock(g_recordedSeamMx);
        g_recordSeams.store(false, std::memory_order_relaxed);
        samples.swap(g_recordedSeams);
        first = g_recordedSeamNext;
        started = g_recordedSeamStart;
        g_recordedSeamNext = 0;
    }
    if (!save) return {};
    std::string csv = "elapsed_ms,lot,story,room,class,x,y,z,nx,ny,nz,story_base,raw_r,raw_g,raw_b,normalization,after_curve_sum\n";
    for (size_t i = 0; i < samples.size(); ++i) {
        const auto& entry = samples[(first + i) % samples.size()];
        const auto& s = entry.sample;
        csv += std::format("{},{:08X},{},{},{},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f},{:.6f}\n",
            static_cast<DWORD>(entry.tick - started), entry.lot, s.level, s.room, s.cls,
            s.p[0], s.p[1], s.p[2], s.n[0], s.n[1], s.n[2], s.base, entry.rgb[0], entry.rgb[1], entry.rgb[2], s.norm, s.fin);
    }
    return csv;
}
bool StageAmbientBaseChange(unsigned char* room, const float* oldOwn, const float* newOwn, const float* oldSecond, const float* newSecond) {
    bool changed = false;
    return StageAmbient(room, oldOwn, newOwn, oldSecond, newSecond, changed);
}
bool HoldsAmbientBase(unsigned char* room, const float* ownColour, const float* second) {
    uintptr_t mgr = 0; int level = 0, id = 0;
    if (!AmbientActive() || !AmbientIdentity(room, mgr, level, id)) return false;
    const uintptr_t tracker = MgrTracker(mgr);
    if (!tracker || SafeStoryManager(tracker, level) != mgr) return false;
    std::lock_guard<std::mutex> lock(g_ambMx);
    const DepKey key{tracker, level, id};
    const auto own = g_ambOrig.find(key);
    const auto applied = g_ambApplied.find(key);
    return own != g_ambOrig.end() && applied != g_ambApplied.end() && g_ambGroups.contains(key)
        && AmbientNear(own->second.c4, ownColour) && AmbientHeld(room, applied->second, second, false);
}
bool StageUnlitAmbientChange(unsigned char* room, const float* target, bool& changed) {
    return StageAmbient(room, nullptr, target, nullptr, target, changed);
}
void ApplyAmbientBaseChanges() {
    std::lock_guard<std::mutex> lock(g_ambMx);
    std::unordered_set<DepKey, DepHash> done;
    for (const auto& dirty : g_ambDirty) {
        if (done.contains(dirty)) continue;
        const auto group = g_ambGroups.find(dirty);
        if (group == g_ambGroups.end()) continue;
        float norm = INFINITY, base = INFINITY, colour[4] = {}, weight = 0;
        for (const auto& member : group->second) {
            const auto own = g_ambOrig.find(member.key);
            if (own == g_ambOrig.end()) continue;
            norm = std::min(norm, own->second.norm);
            base = std::min(base, own->second.base);
        }
        if (!std::isfinite(norm) || !std::isfinite(base)) continue;
        for (const auto& member : group->second) {
            const auto own = g_ambOrig.find(member.key);
            if (own == g_ambOrig.end()) continue;
            const float w = static_cast<float>(std::max(member.area, 1));
            RoomAmbientPolicy::AccumulateAmbient(colour, own->second.c4, own->second.norm, norm, w);
            weight += w;
        }
        if (weight <= 0) continue;
        for (float& value : colour) value /= weight;
        Applied wanted{}; std::memcpy(wanted.c4, colour, 16); wanted.norm = norm; wanted.wallBase = base;
        bool compatible = group->second.size() >= 2 && group->second.size() <= 12;
        for (const auto& member : group->second) {
            const auto previous = g_ambApplied.find(member.key);
            compatible &= previous != g_ambApplied.end()
                && RoomAmbientPolicy::AmbientMapsCompatible(previous->second.norm, norm, previous->second.wallBase, base);
        }
        for (const auto& member : group->second) {
            done.insert(member.key);
            g_ambToQueue[member.key] = wanted;
        }
        if (compatible) g_ambGroupAt.try_emplace(group->second.front().key, GetTickCount());
        else if (!group->second.empty()) g_ambGroupAt.erase(group->second.front().key);
    }    g_ambDirty.clear();
}

bool Install(std::string& error) {
    if (g_installed) return true;
    g_basisGuardReady.store(false, std::memory_order_relaxed);
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
    // The game's own wall test (GameWallTest): a lamp of another story is tested against the solving room's walls from
    // where its ray enters that room's story (05/10); development build: its result recorded for F8. Optional: without it
    // those walls test the whole ray, as before.
    g_enterReady = solveOk && Redirect(kWallTestCall, kWallTest, reinterpret_cast<const void*>(&GameWallTest));
    if (solveOk && !g_enterReady) LOG_WARNING("[LevelLightShare] Walls of the lit room from where a lamp of another story enters it: could not patch the game, the whole ray is tested as before");
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
    // Indoor lamps through stair openings: optional part, only with the evaluation wrapped (it tests the floor per point)
    std::string indoorWhy = classes ? "" : "the light evaluation is not wrapped";
    g_indoorReady = classes && InstallIndoor(indoorWhy);
    if (!g_indoorReady) LOG_WARNING("[LevelLightShare] Indoor lamps through stair openings: " + indoorWhy);
    // Wall light lined up with the wall (5.): optional part, both calls or none. The blur must read its passes and mode
    // where the addresses came from (mov ecx,[passes] at +0x1E, cmp byte [mode],0 at +0x86) and blur class 2 only.
    std::vector<MemPatch::PatchLocation> alignPatches;
    bool align = GameAddr::Have({Id::WallSamplesCall, Id::WallSamples, Id::WallBlurCall, Id::WallBlur, Id::WallBlurPasses, Id::WallBlurMode}) &&
                 CallsTarget(kWallSamplesCall, kWallSamples) && CallsTarget(kWallBlurCall, kWallBlur) &&
                 std::memcmp(reinterpret_cast<const void*>(kWallBlur + 0x0D), "\x83\xBE\xF4\x00\x00\x00\x02", 7) == 0 &&
                 std::memcmp(reinterpret_cast<const void*>(kWallBlur + 0x1E), "\x8B\x0D", 2) == 0 && *reinterpret_cast<const uint32_t*>(kWallBlur + 0x20) == kWallBlurPasses &&
                 std::memcmp(reinterpret_cast<const void*>(kWallBlur + 0x86), "\x80\x3D", 2) == 0 && *reinterpret_cast<const uint32_t*>(kWallBlur + 0x88) == kWallBlurMode;
    // The batch solve lights the rows beyond the edges too: it must be the function whose LightPointWithAllLights call
    // the point hooks wrap (0x6A3336 lies 0x166 into it on Steam)
    align = align && GameAddr::Have({Id::WallSolveCall, Id::WallSolve}) && CallsTarget(kWallSolveCall, kWallSolve) && kBatchSolveCall > kWallSolve &&
            kBatchSolveCall - kWallSolve < 0x300;
    align = align && Redirect(kWallSamplesCall, kWallSamples, reinterpret_cast<const void*>(&WallSamplesHook), &alignPatches) &&
            Redirect(kWallSolveCall, kWallSolve, reinterpret_cast<const void*>(&WallSolveHook), &alignPatches) &&
            Redirect(kWallBlurCall, kWallBlur, reinterpret_cast<const void*>(&WallBlurHook), &alignPatches);
    if (!align) MemPatch::RestoreAll(alignPatches);
    else g_alignPatches = std::move(alignPatches);
    g_alignReady = align;
    if (!align) LOG_WARNING("[LevelLightShare] Seamless walls between floors: the game code differs, left as the game has it");
    else if (g_alignOn) g_alignRequeue = true; // walls lit before this lined up too
    if (!kPublicBuild) {
        std::string why;
        if (!EntryChain::Install(EntryChain::Site::RoomInvalidate, EntryChain::Layer::LevelLightShare, reinterpret_cast<void*>(&InvalidateNoteHook), &why) ||
            !EntryChain::Install(EntryChain::Site::RoomInvalidateFlag, EntryChain::Layer::LevelLightShare, reinterpret_cast<void*>(&InvalidateFlagNoteHook), &why))
            LOG_WARNING("[LevelLightShare] Invalidate notes for the F8 journal not installed: " + why);
    }
    // Walls block light on outdoor floors (see SolvePointBatch): needs the batch call redirected; Steam bytes only
    g_floorMaskReady = solveOk && GameAddr::IsFixed() && kBatchSolveCall == 0x006A3336 && kRoomById &&
                       MemPatch::ValidateBytes(reinterpret_cast<LPVOID>(kOutdoorAlpha), kOutdoorAlphaBytes, sizeof(kOutdoorAlphaBytes));
    if (g_floorMaskReady) {
        BYTE call[7] = {0xE8, 0, 0, 0, 0, 0x90, 0x90};
        const DWORD rel = static_cast<DWORD>(reinterpret_cast<uintptr_t>(&OutdoorAlphaThunk) - (kOutdoorAlpha + 5));
        std::memcpy(call + 1, &rel, 4);
        g_floorMaskReady = MemPatch::WriteBytes(kOutdoorAlpha, std::vector<BYTE>(call, call + 7), &g_patches);
    }
    LOG_INFO(std::string("[LevelLightShare] Walls block light on floors: ") + (g_floorMaskReady ? "ready" : "left as before (code differs)"));
    g_indoorGen.fetch_add(1);
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    WallHeights::Install(); // where the walls are drawn: their light rows and the wall tests follow it
    g_installed = true;
    RefreshSoon();
    LOG_INFO(std::format("[LevelLightShare] Installed (walls of the light's story: {} of {} classes; indoor lamps through stair openings: {}; seamless walls between floors: {})",
                         classes, std::size(kClasses), g_indoorReady ? "yes" : "no", g_alignReady ? "yes" : "no"));
    return true;
}

// Lamp edits first: forget the urgent rooms and the noted marks; held marks are sent the way QueueRoom sends a room when
// their rooms can still be reached (requeue: Uninstall on the render thread, the hooks that would give them back go)
void ClearLampEdits(bool requeue) {
    std::vector<HeldMark> held;
    {
        std::lock_guard<std::mutex> lk(g_heldMx);
        held.swap(g_held);
    }
    {
        std::lock_guard<std::mutex> lk(g_lampMarkMx);
        g_lampMarks.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_urgentMx);
        g_urgent.clear();
        g_urgentSize.store(0, std::memory_order_relaxed);
    }
    if (!requeue || !kRoomById || !kInvalidateRoom || !kSetInsert) return;
    uintptr_t trackers[256];
    const int lots = AllTrackers(trackers, 256);
    for (const HeldMark& h : held) {
        uintptr_t tracker = 0;
        int level = 99;
        if (!ReadTreeLevelKey(h.tl, tracker, level) || level < -4 || level > 7 || TreeLevel(tracker, level) != h.tl) continue;
        if (std::find(trackers, trackers + lots, tracker) == trackers + lots) continue; // its lot is gone
        QueueRoomSafe(tracker, level, h.room);
    }
}

void Uninstall() {
    if (!g_installed) return;
    WallHeights::Uninstall();
    g_rigWait.clear();
    ClearLampEdits(ThreadId() == g_renderThread.load());
    g_installed = false;
    g_basisGuardReady.store(false, std::memory_order_relaxed);
    g_indoorReady = false;
    g_lodReady = false;
    g_ctx = {};
    g_ambReady = false;
    g_finalizeReady = false;
    g_lockStepReady = false;
    MemPatch::RestoreAll(g_lodPatches);
    g_lodPatches.clear();
    const bool alignWasOn = g_alignReady.exchange(false) && g_alignOn; // the walls as the game lights them, again (below)
    MemPatch::RestoreAll(g_alignPatches);
    g_alignPatches.clear();
    ClearBoosts();
    ClearAmbient();
    MemPatch::RestoreAll(g_indoorPatches);
    g_indoorPatches.clear();
    MemPatch::RestoreAll(g_patches);
    g_patches.clear();
    g_enterReady = false;
    g_floorMaskReady = false;
    {
        std::lock_guard<std::mutex> lk(g_wallSnapMx);
        g_wallSnap.clear();
    }
    g_wallSnapGen.fetch_add(1);
    g_evalClasses = 0;
    if (!kPublicBuild) {
        EntryChain::Remove(EntryChain::Site::RoomInvalidate, EntryChain::Layer::LevelLightShare);
        EntryChain::Remove(EntryChain::Site::RoomInvalidateFlag, EntryChain::Layer::LevelLightShare);
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_clearRooms = true;
    RefreshSoon(); // the lists drop the other floors' lamps at their next gather
    // OnPresent (which runs queued relights) stops with Night Lighting: relight now when on the render thread
    if (alignWasOn && ThreadId() == g_renderThread.load()) RelightAllRooms("Seamless walls between floors off");
    LOG_INFO("[LevelLightShare] Uninstalled");
}

bool IsInstalled() { return g_installed.load(); }
bool BasisFloorGuardReady() {
    return g_installed.load(std::memory_order_relaxed) && g_indoorReady && g_basisGuardReady.load(std::memory_order_relaxed);
}

void OnPresent() {
    // rooms solved before their walls were drawn, now measured away from their base: solved again (wall_heights.h)
    if (g_installed.load(std::memory_order_relaxed)) {
        const std::vector<WallHeights::RoomKey> again = WallHeights::TakeRequeue();
        int sent = 0;
        for (const WallHeights::RoomKey& r : again) sent += QueueRoomSafe(r.tracker, r.level, r.id) ? 1 : 0;
        if (sent) LOG_INFO(std::format("[LevelLightShare] {} rooms light again: their walls are drawn away from the base their light used (wall heights measured)", sent));
    }
    const DWORD structureNow = GetTickCount();
    if (g_structurePending.load(std::memory_order_acquire) && RoomAmbientPolicy::StructureRefreshDue(structureNow, g_structureRefreshAt)) {
        std::vector<uintptr_t> rooms;
        {
            std::lock_guard<std::mutex> lock(g_structureMx);
            rooms.swap(g_structureRooms);
            g_structurePending.store(false, std::memory_order_release);
        }
        g_structureRefreshAt = structureNow ? structureNow : 1;
        g_roomRefsDirty = true;
        for (uintptr_t room : rooms) UnlitRooms::OnRoomChanged(room);
    }
    if (g_groupRigsPending.exchange(false, std::memory_order_relaxed)) ObjectLightBridge::RequestRigRefresh();
    g_renderThread = ThreadId();
    ApplyPendingAmbient();
    RefreshCompletedLotRigs();
    if (g_refreshRequested.exchange(false)) RefreshAllLots();
    if (g_alignRequeue.exchange(false)) LevelLightShare::RelightAllRooms(g_alignOn.load() ? "Seamless walls between floors on" : "Seamless walls between floors off");
    // Whole-world relights asked in a burst (install, options, sliders) run once, 250 ms after the last ask (29/09: 59 + 83 rooms
    // and 25 lot stories within 4 s at install)
    if (DWORD at = g_relightAllAt.load(std::memory_order_relaxed); at && static_cast<int32_t>(GetTickCount() - at) >= 0 && g_relightAllAt.compare_exchange_strong(at, 0)) {
        std::string why;
        {
            std::lock_guard<std::mutex> lk(g_relightWhyMx);
            why.swap(g_relightWhy);
        }
        RequeueAllRooms(why.c_str());
    }
    // 4.: floor edits settle for 250 ms, then their rooms near openings gather again.
    DWORD ft = g_floorTick.load(std::memory_order_relaxed);
    if (RoomAmbientPolicy::FloorEditReady(GetTickCount(), ft) && g_floorTick.compare_exchange_strong(ft, 0)) {
        g_roomRefsDirty = true;
        UnlitRooms::OnRoomsChanged();
        std::lock_guard<std::mutex> lk(g_levelsMx);
        g_lastLevel = 0; // the next floor change of the same object marks it again
        for (auto it = g_dirtyLevels.begin(); it != g_dirtyLevels.end();) {
            const uintptr_t mgr = LevelManager(it->first);
            if (mgr) {
                if (g_dirtyMgrs.size() < 256 && std::find(g_dirtyMgrs.begin(), g_dirtyMgrs.end(), mgr) == g_dirtyMgrs.end()) g_dirtyMgrs.push_back(mgr);
                it = g_dirtyLevels.erase(it);
            } else if (!LevelAlive(it->first) || --it->second <= 0)
                it = g_dirtyLevels.erase(it);
            else
                ++it; // no lighting manager yet (the lot is still loading): again after the next wait
        }
        if (!g_dirtyLevels.empty()) g_floorTick = GetTickCount() | 1;
        if (!g_dirtyMgrs.empty()) {
            g_dirtyReady = true;
            g_floorRefreshes.fetch_add(1, std::memory_order_relaxed);
        }
    }
    // Rooms still holding a lamp switched off: gathered again
    if (const DWORD now = GetTickCount(); now - g_staleAt >= kStaleEvery && kLightBright) {
        g_staleAt = now;
        if (g_staleSent.size() > 4096) g_staleSent.clear();
        ForEachRoomImpl(&VisitStale, nullptr, nullptr, kStaleRoomsAge, true);
    }
}

void OnWorldChanged() {
    {
        std::lock_guard<std::mutex> lock(g_structureMx);
        g_roomStructures.clear();
        g_structureRooms.clear();
        g_structurePending = false;
    }
    g_structureRefreshAt = 0;
    g_roomRefsDirty = true;
    g_groupRigsPending = false;
    g_rigWait.clear();
    g_rigCursor = 0;
    g_rigPollAt = g_ambientPollAt = 0;
    {
        std::lock_guard<std::mutex> lock(g_gatherStampMx);
        g_gatherStamps.clear();
    }
    g_clearRooms = true;
    g_roomRefs.clear(); // Rooms at Night's room list (review H1)
    g_roomRefTrackers.clear();
    g_roomRefManagers.clear();
    g_staleSent.clear();
    ClearBoosts(); // the rooms of the previous world (their addresses get reused)
    ClearAmbient();
    ClearJournal();
    {
        std::lock_guard<std::mutex> lk(g_deferredMx);
        g_deferred.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_depsMx); // the lamp bursts of the previous world
        g_depWait.clear();
        g_depSentAt.clear();
    }
    ClearLampEdits(false); // the rooms and tree levels of the previous world
    std::lock_guard<std::mutex> lk(g_ghostMx);
    g_ghosts.clear();
}

// A world loaded at night gets no lot relight (NightTerrainRelight, g_pendingLoad), and its lots were solved during the
// load screen: a lot may find its floors, openings and lamps then in any order, and its settle could fire before the
// lot was complete (29/09, user: "only when I opened the game some lights failed"; Night Lighting off and on fixed it).
// One more round of the rooms near openings once the world is drawn (only those rooms, not the whole lots).
// Development tools: the story each loaded lot shows (story 0 manager +0x284, the "cam" of the solve journal: it follows
// the floor switches of the lot being played; +0x288 is set on more than one lot, so no single lot is picked here)
bool DisplayLevelRaw(uintptr_t tracker, int& story) {
    __try {
        const uintptr_t mgr = StoryManager(tracker, 0);
        if (!mgr) return false;
        story = *reinterpret_cast<const int*>(mgr + 0x284);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
std::vector<std::string> TraceRooms(bool reset) { return TraceRoomsImpl(reset); }

int LoadedLots() {
    LoadAddresses();
    if (!kRootPtr) return 0;
    uintptr_t trackers[256];
    return AllTrackers(trackers, 256);
}

int DisplayLevels(uint32_t* lots, int* stories, int max) {
    LoadAddresses();
    if (!kRootPtr) return 0;
    uintptr_t trackers[256];
    const int count = AllTrackers(trackers, 256);
    int n = 0;
    for (int t = 0; t < count && n < max; t++) {
        int story = 0;
        if (!DisplayLevelRaw(trackers[t], story)) continue;
        lots[n] = LotIdPart(trackers[t], 0x90);
        stories[n++] = story;
    }
    return n;
}

bool LoadedRoomsBusy() {
    if (g_roomRefs.empty() || GetTickCount() - g_roomRefsAt > 3000) return true;
    {
        std::lock_guard<std::mutex> lock(g_ambMx);
        if (!g_ambToQueue.empty()) return true;
    }
    for (const auto& ref : g_roomRefs) {
        const uintptr_t mgr = SafeStoryManager(ref.tracker, ref.level);
        if (!mgr) continue;
        if (const int state = WatchedRoomState({{ref.tracker, ref.level, ref.id}, mgr}); state >= 1 && state <= 3) return true;
    }
    return false;
}
void OnWorldLive() {
    g_roomRefsAt = 0; // require a fresh enumeration after the load screen before an early refresh
    g_worldLiveAt.store(GetTickCount() | 1, std::memory_order_relaxed);
    g_indoorGen.fetch_add(1);
    if (!kPublicBuild) LOG_INFO("[LevelLightShare] " APEX_VERSION_STRING ": World live, the rooms near stair openings of every lot gather once more");
}

void SetIndoor(bool on) {
    if (g_indoorOn.exchange(on) != on) g_indoorGen.fetch_add(1); // every lot's rooms near openings gather again
}

bool AllFloorsDetailed() { return g_installed.load(std::memory_order_relaxed) && g_lodReady && g_allFloors.load(std::memory_order_relaxed); }
// Render thread: the copy of the outside walls (CaptureStoryWalls) as a 4 m grid, rebuilt when the copy changes
namespace {
struct WallGrid {
    uint32_t gen = 0;
    std::vector<WallSeg> segs;
    std::unordered_map<int64_t, std::vector<int>> cells;
    std::vector<uint32_t> seen; // per segment: the query that last tested it (a segment spans several cells)
    uint32_t query = 0;
} g_grid;
constexpr float kCell = 4.0f;
int64_t CellKey(int cx, int cz) { return (static_cast<int64_t>(cx) << 32) ^ static_cast<uint32_t>(cz); }
void RefreshGrid() {
    const uint32_t gen = g_wallSnapGen.load(std::memory_order_relaxed);
    if (gen == g_grid.gen) return;
    g_grid.gen = gen;
    g_grid.segs.clear();
    g_grid.cells.clear();
    {
        std::lock_guard<std::mutex> lk(g_wallSnapMx);
        for (const auto& [mgr, v] : g_wallSnap) g_grid.segs.insert(g_grid.segs.end(), v.begin(), v.end());
    }
    g_grid.seen.assign(g_grid.segs.size(), 0);
    for (int i = 0; i < static_cast<int>(g_grid.segs.size()); i++) {
        const WallSeg& s = g_grid.segs[i];
        const int x0 = static_cast<int>(std::floor(std::min(s.x0, s.x1) / kCell)), x1 = static_cast<int>(std::floor(std::max(s.x0, s.x1) / kCell));
        const int z0 = static_cast<int>(std::floor(std::min(s.z0, s.z1) / kCell)), z1 = static_cast<int>(std::floor(std::max(s.z0, s.z1) / kCell));
        for (int cx = x0; cx <= x1; cx++)
            for (int cz = z0; cz <= z1; cz++) g_grid.cells[CellKey(cx, cz)].push_back(i);
    }
}
} // namespace

bool WallBlocks(const float lamp[3], const float point[3], float nearSkip) {
    if (!g_objectWallsOn.load(std::memory_order_relaxed) || !g_installed.load(std::memory_order_relaxed)) return false;
    RefreshGrid();
    if (g_grid.segs.empty()) return false;
    const float ax = lamp[0], az = lamp[2], bx = point[0], bz = point[2];
    const float rx = bx - ax, rz = bz - az;
    if (std::fabs(rx) + std::fabs(rz) > 200.0f) return false; // not a lamp in reach
    // A wall crossed within 20 cm of the point is the wall the object sits in (code review 05/10: windows, doors, wall
    // lamps and wall decor have their origin on their wall's line, so every lamp in front of them crossed it at the very
    // end of the ray and was dropped; around each sconce only the wall's own baked light was left). Loose objects behind
    // a wall (a yard's telescope, 1 m or more from it) are still blocked.
    const float len = std::sqrt(rx * rx + rz * rz);
    const float tEnd = len > nearSkip ? 1.0f - nearSkip / len : 0.0f; // nearSkip = 0.2 m unless the caller passes more
    if (++g_grid.query == 0) {
        std::fill(g_grid.seen.begin(), g_grid.seen.end(), 0u);
        g_grid.query = 1;
    }
    const int x0 = static_cast<int>(std::floor(std::min(ax, bx) / kCell)), x1 = static_cast<int>(std::floor(std::max(ax, bx) / kCell));
    const int z0 = static_cast<int>(std::floor(std::min(az, bz) / kCell)), z1 = static_cast<int>(std::floor(std::max(az, bz) / kCell));
    for (int cx = x0; cx <= x1; cx++)
        for (int cz = z0; cz <= z1; cz++) {
            const auto it = g_grid.cells.find(CellKey(cx, cz));
            if (it == g_grid.cells.end()) continue;
            for (int i : it->second) {
                if (g_grid.seen[i] == g_grid.query) continue;
                g_grid.seen[i] = g_grid.query;
                const WallSeg& s = g_grid.segs[i];
                const float sx = s.x1 - s.x0, sz = s.z1 - s.z0;
                const float den = rx * sz - rz * sx;
                if (std::fabs(den) < 1e-6f) continue; // parallel
                const float qx = s.x0 - ax, qz = s.z0 - az;
                const float t = (qx * sz - qz * sx) / den; // along lamp -> point
                const float u = (qx * rz - qz * rx) / den; // along the wall
                if (t <= 0.001f || t >= tEnd || u < 0.0f || u > 1.0f) continue;
                const float y = lamp[1] + t * (point[1] - lamp[1]); // the ray's height where it crosses the wall line
                if (y > s.y0 + 0.02f && y < s.y1 - 0.02f) return true;
            }
        }
    return false;
}
struct NoteLamp {
    uintptr_t ptr = 0;
    int type = 0;
    float pos[3] = {}, colour[3] = {}, range = 0;
    bool cone = false;
    float a1[3] = {}, a2[3] = {}, c1 = 0, c2 = 0;
};
// The lamps of a noted wall's room (POD only: SEH); the count, -1 when unreadable
int ReadNoteLamps(uintptr_t room, NoteLamp* out, int max) {
    __try {
        const uintptr_t* lb = *reinterpret_cast<const uintptr_t* const*>(room + 0xC8);
        const uintptr_t* le = *reinterpret_cast<const uintptr_t* const*>(room + 0xCC);
        if (!lb || le < lb || le - lb >= 512) return -1;
        int n = 0;
        for (const uintptr_t* p = lb; p < le && n < max; p++) {
            const BYTE* L = reinterpret_cast<const BYTE*>(*p);
            if (!L) continue;
            NoteLamp& o = out[n++];
            o.ptr = *p;
            o.type = *reinterpret_cast<const int*>(L + 0xB0);
            std::memcpy(o.pos, L + 0x120, sizeof o.pos);
            std::memcpy(o.colour, L + 0xE0, sizeof o.colour);
            o.range = *reinterpret_cast<const float*>(L + 0x130);
            o.cone = *reinterpret_cast<const uintptr_t*>(L) == GameAddr::Get(GameAddr::Id::LightVtable5);
            if (o.cone) {
                std::memcpy(o.a1, L + 0x1A0, sizeof o.a1);
                std::memcpy(o.a2, L + 0x190, sizeof o.a2);
                o.c1 = *reinterpret_cast<const float*>(L + 0x174);
                o.c2 = *reinterpret_cast<const float*>(L + 0x170);
            }
        }
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
std::string WallSurvey(const std::vector<SurveyPoint>& pts) {
    std::vector<WallNote> notes;
    {
        std::lock_guard<std::mutex> lk(g_wallNoteMx);
        notes.reserve(g_wallNotes.size());
        for (const auto& [k, w] : g_wallNotes) notes.push_back(w);
    }
    if (notes.empty()) return "no wall piece noted yet (the walls are noted when they are solved: refresh the lighting, then capture)\n";
    // the points on a 1 m grid (world xz)
    std::unordered_map<int64_t, std::vector<int>> grid;
    auto cell = [](float x, float z) { return (static_cast<int64_t>(std::floor(x)) << 32) ^ static_cast<int64_t>(static_cast<uint32_t>(static_cast<int32_t>(std::floor(z)))); };
    for (int i = 0; i < static_cast<int>(pts.size()); i++) grid[cell(pts[i].x, pts[i].z)].push_back(i);
    struct Span {
        int draw = -1, count = 0;
        float lo = 1e30f, hi = -1e30f;
    };
    struct Row {
        const WallNote* w;
        std::vector<Span> spans;
    };
    std::vector<Row> rows;
    std::map<std::pair<int, int>, std::map<int, int>> summary; // (story, outdoor) -> (drawn foot - lit base, cm) -> pieces
    for (const WallNote& w : notes) {
        const float ux = w.x1 - w.x0, uz = w.z1 - w.z0, L = std::sqrt(ux * ux + uz * uz);
        if (L < 0.05f || w.colHi <= w.colLo) continue;
        const float cw = L / static_cast<float>(w.colHi - w.colLo), ex = 0.5f * cw + 0.05f;
        const float minX = std::min(w.x0, w.x1) - ex - 0.3f, maxX = std::max(w.x0, w.x1) + ex + 0.3f;
        const float minZ = std::min(w.z0, w.z1) - ex - 0.3f, maxZ = std::max(w.z0, w.z1) + ex + 0.3f;
        std::map<int, Span> byDraw;
        for (int cx = static_cast<int>(std::floor(minX)); cx <= static_cast<int>(std::floor(maxX)); cx++)
            for (int cz = static_cast<int>(std::floor(minZ)); cz <= static_cast<int>(std::floor(maxZ)); cz++) {
                const auto it = grid.find((static_cast<int64_t>(cx) << 32) ^ static_cast<int64_t>(static_cast<uint32_t>(cz)));
                if (it == grid.end()) continue;
                for (int i : it->second) {
                    const SurveyPoint& p = pts[i];
                    const float dx = p.x - w.x0, dz = p.z - w.z0;
                    const float along = (dx * ux + dz * uz) / L, across = std::fabs(dx * uz - dz * ux) / L;
                    if (across > 0.3f || along < -ex || along > L + ex) continue;
                    Span& sp = byDraw[p.draw];
                    sp.draw = p.draw;
                    sp.count++;
                    sp.lo = std::min(sp.lo, p.y);
                    sp.hi = std::max(sp.hi, p.y);
                }
            }
        if (byDraw.empty()) continue;
        Row r{&w, {}};
        for (const auto& [d, sp] : byDraw) r.spans.push_back(sp);
        // the span that best covers the lit wall (overlap with [oy - 3, oy + 3]) is this piece's drawn wall
        const Span* best = nullptr;
        float bestOverlap = -1e30f;
        for (const Span& sp : r.spans) {
            const float ov = std::min(sp.hi, w.oy + 3.0f) - std::max(sp.lo, w.oy - 3.0f);
            if (ov > bestOverlap) bestOverlap = ov, best = &sp;
        }
        if (best) summary[{w.story, w.outdoor ? 1 : 0}][static_cast<int>(std::lround((best->lo - w.oy) * 100.0f))]++;
        rows.push_back(std::move(r));
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return a.w->story != b.w->story ? a.w->story < b.w->story : a.w->outdoor != b.w->outdoor ? a.w->outdoor > b.w->outdoor : a.w->roomId < b.w->roomId;
    });
    std::string s = std::format("{} wall pieces noted, {} of them with drawn vertices on their line ({} wall vertices on screen)\n", notes.size(), rows.size(), pts.size());
    s += "  summary per story (outdoor = room 0 and roofless rooms): drawn foot - lit base (m) x pieces\n";
    for (const auto& [key, deltas] : summary) {
        s += std::format("    story {} {}:", key.first, key.second ? "outdoor" : "indoor ");
        for (const auto& [cm, count] : deltas) s += std::format(" {:+.2f} x{}", cm / 100.0f, count);
        s += "\n";
    }
    s += "  pieces: story, room, class, wall / piece, lit base (wall +0x114), lit bottom row (after Apex), drawn spans [foot..top] per draw\n";
    int shown = 0;
    for (const Row& r : rows) {
        if (++shown > 400) {
            s += std::format("    ... {} more\n", rows.size() - 400);
            break;
        }
        const WallNote& w = *r.w;
        s += std::format("    story {} room {}{} class {} wall {:08X}/{}: lit base {:.3f}, bottom row lit at {:.3f}; drawn", w.story, w.roomId, w.outdoor ? " (outdoor)" : "", w.cls,
                         w.wall, w.piece, w.oy, w.litLo);
        for (const Span& sp : r.spans) s += std::format(" [#{} {:.3f}..{:.3f}, {} vertices]", sp.draw, sp.lo, sp.hi, sp.count);
        s += "\n";
    }
    return s;
}
std::string WallNotesOnRay(const float o[3], const float d[3], float* hitOut) {
    std::vector<WallNote> notes;
    {
        std::lock_guard<std::mutex> lk(g_wallNoteMx);
        notes.reserve(g_wallNotes.size());
        for (const auto& [k, w] : g_wallNotes) notes.push_back(w);
    }
    const WallNote* best = nullptr;
    float bestT = 1e30f, hit[3] = {}, along = 0, len = 0;
    for (const WallNote& w : notes) {
        const float ux = w.x1 - w.x0, uz = w.z1 - w.z0, L = std::sqrt(ux * ux + uz * uz);
        if (L < 0.05f || w.colHi <= w.colLo) continue;
        const float cw = L / static_cast<float>(w.colHi - w.colLo), nx = -uz / L, nz = ux / L;
        const float den = d[0] * nx + d[2] * nz;
        if (std::fabs(den) < 1e-7f) continue;
        const float t = ((w.x0 - o[0]) * nx + (w.z0 - o[2]) * nz) / den;
        if (t <= 0 || t >= bestT) continue;
        const float h[3] = {o[0] + t * d[0], o[1] + t * d[1], o[2] + t * d[2]};
        const float a = ((h[0] - w.x0) * ux + (h[2] - w.z0) * uz) / L;
        if (a < -0.5f * cw || a > L + 0.5f * cw || h[1] < w.oy - 0.05f || h[1] > w.oy + 3.05f) continue;
        best = &w;
        bestT = t;
        std::memcpy(hit, h, sizeof hit);
        along = a;
        len = L;
    }
    if (!best) return std::format("no wall piece noted on the pixel's ray ({} pieces noted since the lots were lit; the walls are noted when they are solved, so relight the lot first if it was lit before this build)", notes.size());
    if (hitOut) std::memcpy(hitOut, hit, sizeof hit);
    const WallNote& w = *best;
    const float h = hit[1] - w.oy;
    const float col = w.colLo + along / len * static_cast<float>(w.colHi - w.colLo);
    const float kDrawn = h / 3.0f * static_cast<float>(w.rows - 1);
    const int kRow = std::clamp(static_cast<int>(std::lround(kDrawn)), 0, w.rows - 1);
    std::string s = std::format("piece {} of wall {:08X} (class {}, story {}, room {} {:08X}), noted {:.1f} s ago\n", w.piece, w.wall, w.cls, w.story, w.roomId, w.room,
                                (GetTickCount() - w.tick) / 1000.0);
    s += std::format("      atlas block x {}..{} y {}..{} ({} rows, wall {} columns; this piece columns {}..{}); row k at the bottom is texel y {}\n", w.block.x0, w.block.x1,
                     w.block.y0, w.block.y1, w.rows, w.cols, w.colLo, w.colHi, w.block.y0 + w.rows - 1);
    s += std::format("      base (wall +0x114) y {:.3f}; drawn from {:.3f} to {:.3f}; lit: bottom row at {:.3f} (+{:.3f}), top row at {:.3f} (+{:.3f})\n", w.oy, w.oy, w.oy + 3.0f,
                     w.litLo, w.litLo - w.oy, w.litHi, w.litHi - w.oy);
    s += std::format("      line on the ground ({:.2f}, {:.2f}) -> ({:.2f}, {:.2f}), normal ({:.3f} {:.3f} {:.3f})\n", w.x0, w.z0, w.x1, w.z1, w.n[0], w.n[1], w.n[2]);
    s += std::format("      the pixel: world ({:.3f} {:.3f} {:.3f}), {:.3f} m above the base, {:.2f} m along the line; reads row k {:.2f} (drawn) = atlas texel ({:.1f}, {})\n", hit[0],
                     hit[1], hit[2], h, along, kDrawn, col, w.block.y0 + w.rows - 1 - kRow);
    // the room's lamps, as the solve sees them
    if (!w.room) return s + "      the room is not known (its wall pass was not seen)\n";
    NoteLamp lamps[64];
    const int n = ReadNoteLamps(w.room, lamps, 64);
    if (n < 0) return s + "      the room's lamps could not be read\n";
    s += std::format("      lamps of that room ({}): height above the wall's base, distance in front of the wall (along its normal), range, colour, cone\n", n);
    const float nx = w.n[0], nz = w.n[2], nl = std::sqrt(nx * nx + nz * nz);
    for (int i = 0; i < n; i++) {
        const NoteLamp& l = lamps[i];
        const float front = nl > 1e-4f ? ((l.pos[0] - hit[0]) * nx + (l.pos[2] - hit[2]) * nz) / nl : 0.0f;
        const float dx = l.pos[0] - hit[0], dz = l.pos[2] - hit[2];
        const std::string cone = l.cone ? std::format(", cone 1 axis ({:.2f} {:.2f} {:.2f}) cos {:.3f}, cone 2 axis ({:.2f} {:.2f} {:.2f}) cos {:.3f}", l.a1[0], l.a1[1],
                                                      l.a1[2], l.c1, l.a2[0], l.a2[1], l.a2[2], l.c2)
                                        : std::string();
        s += std::format("        L{:08X} type {} at ({:.2f} {:.2f} {:.2f}): {:+.3f} m above the base, {:+.3f} m in front, {:.2f} m from the pixel along the ground, range {:.3g}, colour ({:.2f} {:.2f} {:.2f}){}\n",
                         l.ptr, l.type, l.pos[0], l.pos[1], l.pos[2], l.pos[1] - w.oy, front, std::sqrt(dx * dx + dz * dz), l.range, l.colour[0], l.colour[1], l.colour[2], cone);
    }
    return s;
}
bool OnWallLine(const float point[3], float dirX, float dirZ, float maxDist) {
    if (!g_installed.load(std::memory_order_relaxed)) return false;
    RefreshGrid();
    if (g_grid.segs.empty()) return false;
    const float px = point[0], pz = point[2], y = point[1] + 0.5f;
    const int cx0 = static_cast<int>(std::floor((px - maxDist) / kCell)), cx1 = static_cast<int>(std::floor((px + maxDist) / kCell));
    const int cz0 = static_cast<int>(std::floor((pz - maxDist) / kCell)), cz1 = static_cast<int>(std::floor((pz + maxDist) / kCell));
    for (int cx = cx0; cx <= cx1; cx++)
        for (int cz = cz0; cz <= cz1; cz++) {
            const auto it = g_grid.cells.find(CellKey(cx, cz));
            if (it == g_grid.cells.end()) continue;
            for (int i : it->second) {
                const WallSeg& s = g_grid.segs[i];
                if (y < s.y0 || y > s.y1) continue;
                const float sx = s.x1 - s.x0, sz = s.z1 - s.z0, len2 = sx * sx + sz * sz;
                if (len2 < 1e-6f) continue;
                const float len = std::sqrt(len2);
                if (std::fabs(sx * dirZ - sz * dirX) > 0.17f * len) continue; // not along the given direction (about 10 degrees)
                const float t = std::clamp(((px - s.x0) * sx + (pz - s.z0) * sz) / len2, 0.0f, 1.0f);
                const float dx = s.x0 + t * sx - px, dz = s.z0 + t * sz - pz;
                if (dx * dx + dz * dz <= maxDist * maxDist) return true;
            }
        }
    return false;
}
void SetObjectWalls(bool on) { g_objectWallsOn.store(on, std::memory_order_relaxed); }

void SetFloorWalls(bool on) {
    if (g_floorWallsOn.exchange(on) != on && g_installed.load() && g_floorMaskReady) RelightAllRooms(on ? "Walls block light on floors on" : "Walls block light on floors off");
}
bool FloorWallsActive() { return g_installed.load(std::memory_order_relaxed) && g_floorMaskReady && g_floorWallsOn.load(std::memory_order_relaxed); }

void SetAllFloors(bool on) {
    if (g_allFloors.exchange(on) != on && g_lodReady) RelightAllRooms(on ? "Every floor in full detail on" : "Every floor in full detail off");
}
void SetAllLotsHighQuality(bool on) { g_allLotsHigh.store(on, std::memory_order_relaxed); }

void SetWallAlign(bool on) {
    if (g_alignOn.exchange(on) != on && g_alignReady) g_alignRequeue = true; // every room lights its walls again
}

std::vector<std::pair<unsigned long, std::string>> JournalSince(unsigned long fromTick) {
    std::vector<std::pair<unsigned long, std::string>> out;
    for (auto& [t, s] : JournalLinesSince(fromTick)) out.emplace_back(t, std::move(s));
    return out;
}

std::vector<SolveEvent> JournalEventsSince(unsigned long fromTick) {
    std::vector<SolveEvent> out;
    std::lock_guard<std::mutex> lk(g_journalMx);
    const size_t kept = std::min(g_journalCount, kJournal);
    for (size_t k = g_journalCount - kept; k < g_journalCount; k++) {
        const SolveNote& n = g_journal[k % kJournal];
        if (static_cast<int32_t>(n.tick - fromTick) < 0) continue;
        out.push_back(SolveEvent{n.tick, n.event, n.lot, n.level, n.id, n.cls, n.shown, n.state, n.merged});
    }
    return out;
}

bool TreeLevelLot(uintptr_t treeLevel, uint32_t& lot, int& story) {
    __try {
        const uintptr_t mgr = *reinterpret_cast<const uintptr_t*>(treeLevel);
        if (!mgr) return false;
        lot = *reinterpret_cast<const uint32_t*>(mgr + 0x90);
        story = *reinterpret_cast<const int*>(treeLevel + 0x1A0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int ForEachRoom(bool (*visit)(unsigned char* room, void* ctx), void* ctx, int* queued, void (*ack)(unsigned char*, bool, void*)) {
    return ForEachRoomImpl(visit, ctx, queued, 3000, false, ack);
}

// Every room of one loaded lot (stories -4..7, room 0 too) lights again: a lamp of it switched or moved (lamp_mark_filter.cpp,
// 30/09). Render thread. Returns the rooms sent, -1 when the lot is no longer loaded. With lamps (05/10: lamps that only
// moved), only the rooms whose list holds one of them: a move changes no room's list, and the rooms that take the lamp were
// sent at once by the lamp edit itself (AfterChangedWalk), so this is the safety net, and their fresh solves are kept.
// The lamps are held by indoor rooms of two stories or more (lamps taken through a stair opening, part 4.): a lamp edit's
// targeted refresh left such a room lit through the closed walls around it until every room of the lot lit again (06/10,
// recording 22:09:14), so LampMarkFilter relights the whole lot at once for them. Render thread.
bool LampsCrossStories(uintptr_t tracker, const uintptr_t* lamps, int lampCount) {
    if (!tracker || !lamps || lampCount <= 0) return false;
    LoadAddresses();
    if (!kRoomById) return false;
    int stories = 0;
    for (int level = -4; level <= 7; level++) {
        int ids[1024], n = 0;
        bool held = false;
        __try {
            const uintptr_t mgr = StoryManager(tracker, level);
            if (!mgr) continue;
            const int w = *reinterpret_cast<const int*>(mgr + 0x264), h = *reinterpret_cast<const int*>(mgr + 0x268);
            if (w <= 0 || h <= 0 || w > 1024 || h > 1024) continue;
            const int cached = CachedStoryRooms(tracker, level, ids, static_cast<int>(std::size(ids)));
            if (cached >= 0) n = cached;
            else for (int iz = 0; iz < h; iz++)
                for (int ix = 0; ix < w; ix++)
                    if (const uintptr_t tile = LightTile(mgr, ix, iz))
                        for (int q = 0; q < 4; q++)
                            if (const int id = TileRoom(tile, q); id > 0 && n < static_cast<int>(std::size(ids)) && std::find(ids, ids + n, id) == ids + n) ids[n++] = id;
            for (int k = 0; k < n && !held; k++) {
                if (ids[k] <= 0) continue;
                BYTE* room = static_cast<BYTE*>(reinterpret_cast<RoomById_t>(kRoomById)(reinterpret_cast<void*>(mgr), ids[k]));
                held = room && HoldsAnyLamp(room, lamps, lampCount);
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
        if (held && ++stories >= 2) return true;
    }
    return false;
}
int RelightLot(uintptr_t tracker, const char* why, unsigned long changedAt, const uintptr_t* lamps, int lampCount) {
    LoadAddresses();
    if (!kRootPtr || !kRoomById || !kInvalidateRoom || !kSetInsert || !tracker) return -1;
    uintptr_t trackers[256];
    const int lots = AllTrackers(trackers, 256);
    if (std::find(trackers, trackers + lots, tracker) == trackers + lots) return -1;
    int queued = 0, skipped = 0;
    RigWait wait{{}, GetTickCount(), false};
    for (int level = -4; level <= 7; level++) queued += RequeueStory(tracker, level, changedAt, &skipped, &wait.rooms, lamps, lamps ? lampCount : 0);
    if (!wait.rooms.empty() && wait.rooms.size() <= 128 && (g_rigWait.size() < 256 || g_rigWait.contains(tracker)))
        g_rigWait[tracker] = std::move(wait);
    else {
        g_rigWait.erase(tracker);
        UnlitRooms::RigsAgainIn(1500);
    }
    LOG_INFO(std::format("[LevelLightShare] {}: {} rooms of lot {:08X} light again, {} retain their fresh solve (running or finished){}", why, queued, LotIdPart(tracker, 0x90), skipped,
                         lamps && lampCount > 0 ? std::format(" (only the rooms holding the {} lamp{} moved)", lampCount, lampCount == 1 ? "" : "s") : std::string()));
    return queued;
}

// A switched lamp's home (06/10 evening): its room id (light+8, what the object rigs' gather compares, FUN_006bb270) on the
// story whose lowest floor (mgr+0x98, world) is the highest at or under the lamp (+0x124), and whether it is on now (the lit
// bit and colour, as LampMarkFilter reads them). The lamp mark's room is only the first room the game marked: room 0 of
// every story in the 13:52 recording, so every switch counted as an outdoor lamp and sent the outdoor rooms of every story.
// False when unreadable or when that story has no such room (the caller keeps the mark's story and room).
bool ReadLampHomeRaw(uintptr_t light, int& id, float& y, bool& on) {
    __try {
        id = *reinterpret_cast<const int*>(light + 8);
        y = *reinterpret_cast<const float*>(light + 0x124);
        const float* lit = reinterpret_cast<const float*>(light + 0xE0);
        on = (*reinterpret_cast<const BYTE*>(light + 0x100) & 0x20) && lit[0] + lit[1] + lit[2] > 1e-3f;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool ReadStoryBase(uintptr_t mgr, float& base) {
    __try {
        base = *reinterpret_cast<const float*>(mgr + 0x98);
        return std::isfinite(base);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool LampHome(uintptr_t tracker, uintptr_t light, int& story, int& room, bool& on) {
    int id = -1;
    float y = 0.0f;
    if (!light || !ReadLampHomeRaw(light, id, y, on) || id < 0 || id > 65535 || !std::isfinite(y)) return false;
    int best = INT_MIN;
    float bestBase = -1e30f;
    for (int S = -4; S <= 7; S++) {
        const uintptr_t mgr = SafeStoryManager(tracker, S);
        float base;
        if (mgr && ReadStoryBase(mgr, base) && base <= y + 0.01f && base > bestBase) {
            bestBase = base;
            best = S;
        }
    }
    if (best == INT_MIN || !SafeRoomById(tracker, best, id)) return false;
    story = best;
    room = id;
    return true;
}
// A room's light list length (SIZE_MAX when unreadable)
size_t SafeListSize(const BYTE* room) {
    __try {
        return ListSize(room);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SIZE_MAX;
    }
}

// A lamp switched (06/10, user approved "improve it"; recording 12:01: one sconce sent 17 rooms of its lot through this safety
// net, ~1.5 s of solving in the background, story 3 and every outdoor room among them). A switch changes the light of the
// rooms that hold the lamp, of its own room, and of the rooms that take it through a stair opening once it is on: those of
// its story and the stories next to it near the openings between them (the gather of part 4 takes lamps of S - 1 and S + 1
// only, near an opening); an outdoor lamp (room 0 or a roofless room) also lights the outdoor rooms of every story. Only
// those rooms are sent (a fresh solve kept, as above); a lamp that moved too, or into another room, still sends the lot.
// The lamp's own story and room are its home (LampHome); when only lamps switched off, a room with no light in its list
// is left alone (no lamp reached it before the switch, so none of them changes it: rooms 22 and 24 of the 13:52 recording).
int RelightLampSwitch(uintptr_t tracker, const char* why, unsigned long changedAt, const LampSwitch* switched, int count,
                      const uintptr_t* moved, int movedCount) {
    LoadAddresses();
    if (!kRootPtr || !kRoomById || !kInvalidateRoom || !kSetInsert || !tracker || count <= 0) return -1;
    uintptr_t trackers[256];
    const int lots = AllTrackers(trackers, 256);
    if (std::find(trackers, trackers + lots, tracker) == trackers + lots) return -1;
    std::vector<uintptr_t> lamps(moved, moved + std::max(movedCount, 0));
    for (int k = 0; k < count; k++) lamps.push_back(switched[k].light);
    int queued = 0, skipped = 0;
    RigWait wait{{}, GetTickCount(), false};
    for (int level = -4; level <= 7; level++)
        queued += RequeueStory(tracker, level, changedAt, &skipped, &wait.rooms, lamps.data(), static_cast<int>(lamps.size()));
    // the rooms the switched lamps can reach without holding them yet
    std::vector<std::pair<int, int>> reach;
    bool outdoorLamp = false, anyOn = false;
    int homes = 0;
    OpeningMask mask;
    for (int k = 0; k < count; k++) {
        int L = StoryOfTreeLevel(switched[k].tl), own = switched[k].room;
        bool on = true; // unknown: as if switched on (its rooms are not left alone)
        if (LampHome(tracker, switched[k].light, L, own, on)) homes++;
        else if (L < -4 || L > 7 || TreeLevel(tracker, L) != switched[k].tl) continue;
        anyOn = anyOn || on;
        reach.emplace_back(L, own);
        const uintptr_t mgrL = StoryManager(tracker, L);
        if (own == 0 || (g_indoorReady && mgrL && RooflessRoom(mgrL, own))) outdoorLamp = true;
        if (!g_indoorOn.load(std::memory_order_relaxed)) continue;
        for (const int B : {L, L + 1}) { // the floors under and over the lamp's story
            if (B < 1 || B > 7) continue;
            const uintptr_t mgrB = StoryManager(tracker, B), levelB = mgrB ? LevelFor(mgrB) : 0;
            if (!levelB || !BuildOpeningMask(StoryManager(tracker, B - 1), mgrB, levelB, mask) || !mask.openings) continue;
            for (const int S : {B - 1, B})
                if (const uintptr_t mgrS = StoryManager(tracker, S)) {
                    int ids[256];
                    const int n = RoomsNearOpenings(mgrS, mask, ids, 256);
                    for (int i = 0; i < n; i++) reach.emplace_back(S, ids[i]);
                }
        }
    }
    if (outdoorLamp)
        for (int S = 0; S <= 7; S++)
            if (const uintptr_t mgrS = StoryManager(tracker, S)) {
                reach.emplace_back(S, 0);
                int roofless[256];
                const int nr = g_indoorReady ? RooflessRoomIds(mgrS, roofless, static_cast<int>(std::size(roofless))) : 0;
                for (int i = 0; i < nr; i++) reach.emplace_back(S, roofless[i]);
            }
    std::sort(reach.begin(), reach.end());
    reach.erase(std::unique(reach.begin(), reach.end()), reach.end());
    int unlit = 0;
    for (const auto& [S, id] : reach) {
        BYTE* room = SafeRoomById(tracker, S, id);
        if (!room) continue;
        if (changedAt && FreshLampSolve(room, changedAt)) {
            skipped++;
            continue;
        }
        if (!anyOn && SafeListSize(room) == 0) { // only lamps switched off, and none of them reached it
            unlit++;
            continue;
        }
        if (QueueRoomSafe(tracker, S, id)) {
            queued++;
            if (const uintptr_t mgr = SafeStoryManager(tracker, S)) wait.rooms.push_back({{tracker, S, id}, mgr});
        }
    }
    if (!wait.rooms.empty() && wait.rooms.size() <= 128 && (g_rigWait.size() < 256 || g_rigWait.contains(tracker)))
        g_rigWait[tracker] = std::move(wait);
    else {
        g_rigWait.erase(tracker);
        UnlitRooms::RigsAgainIn(1500);
    }
    LOG_INFO(std::format("[LevelLightShare] {}: {} rooms of lot {:08X} light again, {} retain their fresh solve (running or finished), {} with no light "
                         "left alone (the rooms holding the {} lamp{} or within its reach{}; {} of them found in their own room)",
                         why, queued, LotIdPart(tracker, 0x90), skipped, unlit, count, count == 1 ? "" : "s", outdoorLamp ? ", outdoor rooms of every story" : "",
                         homes));
    return queued;
}

// Lamp edits first (see "Lamp edits first" above)
void NoteLampMark(uintptr_t tl, int room, bool user, bool pure) {
    if (!g_installed.load(std::memory_order_relaxed) || !g_indoorReady) return;
    {
        std::lock_guard<std::mutex> stamps(g_gatherStampMx);
        g_markSerial = g_gatherSerial; // gathers after this mark see the lamp as it is now (FreshLampSolveImpl)
    }
    std::lock_guard<std::mutex> lk(g_lampMarkMx);
    for (LampMarkNote& n : g_lampMarks)
        if (n.tl == tl && n.room == room) {
            n.user = n.user || user;
            n.pure = n.pure && pure; // a lamp of the room moved in from another: its gather waits
            return;
        }
    if (g_lampMarks.size() >= 256) g_lampMarks.erase(g_lampMarks.begin()); // notes whose update never came
    g_lampMarks.push_back(LampMarkNote{tl, room, user, pure});
}

bool HoldLampMark(uintptr_t tl, int room, bool user) {
    if (!g_installed.load(std::memory_order_relaxed) || !g_indoorReady || !kMarkRoom || !kRoomById || !tl) return false;
    const void* r = RoomOfTreeLevel(reinterpret_cast<const BYTE*>(tl), room);
    if (!r || RoomState(r) != 3) return false;
    std::lock_guard<std::mutex> lk(g_heldMx);
    for (HeldMark& h : g_held)
        if (h.tl == tl && h.room == room) {
            h.user = h.user || user;
            return true;
        }
    if (g_held.size() >= 256) return false;
    g_held.push_back(HeldMark{tl, room, GetTickCount(), user});
    g_heldMarks.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// The story the camera shows first, then the ones below it (seen from outside and through the cutaway), then the ones above
// (05/10, recording 19:42: all lamps of a house switched on, 26 rooms solved over about a second in the queue's order)
float CameraStoryFactor(const void* room) {
    __try {
        const uintptr_t mgr = *static_cast<const uintptr_t*>(room);
        if (!mgr) return 1.0f;
        const int level = *reinterpret_cast<const int*>(mgr + 0x88), cam = *reinterpret_cast<const int*>(mgr + 0x284);
        return level == cam ? 4.0f : level < cam ? 2.0f : 1.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1.0f;
    }
}

float LampUrgency(const void* room) {
    const int tier = UrgentTier(room);
    return tier < 0 ? 1.0f : (tier == 0 ? 1.0e6f : 1.0e5f) * CameraStoryFactor(room);
}

bool SettlingAfterLoad() {
    const DWORD live = g_worldLiveAt.load(std::memory_order_relaxed);
    return live && GetTickCount() - live < kSettleAfterLoadMs;
}

// The other members of a room's atrium group (false: not in one)
bool OtherMembers(const void* room, std::vector<DepKey>& others) {
    others.clear();
    uintptr_t mgr = 0;
    int level = 0, id = 0;
    if (!room || !AmbientIdentity(static_cast<BYTE*>(const_cast<void*>(room)), mgr, level, id)) return false;
    const uintptr_t tracker = MgrTracker(mgr);
    if (!tracker) return false;
    std::lock_guard<std::mutex> lk(g_ambMx);
    const auto it = g_ambGroups.find(DepKey{tracker, level, id});
    if (it == g_ambGroups.end() || it->second.size() < 2) return false;
    for (const auto& member : it->second)
        if (member.key.level != level || member.key.room != id) others.push_back(member.key);
    return true;
}

// (06/10, user: "it applies first on the light's story, and only then on the others"; recording 11:17: after the lamp's
// room the scheduler took the camera story's rooms, then the stories below, so the atrium's member on story 0 came 1.5 s
// and the one beside the lamp 3 s after the lamp's own). A member counts when any member of its group is a lamp edit's
// (recording 12:01: the atrium's room beside the lamp's, sent by the lamp's safety net and so not urgent, came 2.8 s late).
bool StackedWithEdit(const void* room) {
    std::vector<DepKey> others;
    if (!OtherMembers(room, others)) return false;
    if (UrgentTier(room) >= 0) return true;
    for (const DepKey& key : others)
        if (const BYTE* member = SafeRoomById(key.tracker, key.level, key.room); member && UrgentTier(member) >= 0) return true;
    return false;
}

float CrossLampReach(const void* room, const void* light, const float* point) {
    if (!room || !light || !point) return -1.0f;
    return CrossLampReachImpl(static_cast<BYTE*>(const_cast<void*>(room)), const_cast<void*>(light), point);
}

bool InAtrium(const void* room) {
    std::vector<DepKey> others;
    return OtherMembers(room, others);
}

// Another member of the room's atrium group is waiting for its gather or its solve, or being solved (states 1 to 3)
bool GroupPending(const void* room) {
    std::vector<DepKey> others;
    if (!OtherMembers(room, others)) return false;
    for (const DepKey& key : others)
        if (const BYTE* member = SafeRoomById(key.tracker, key.level, key.room)) {
            const int state = RoomState(member);
            if (state >= 1 && state <= 3) return true;
        }
    return false;
}

// Apex's share of the room solves so far (LightEvalHook and the basis test), ms: cycles calibrated against the performance
// counter since the first call (-1 until 100 ms have passed)
double ApexSolveMs() {
    static uint64_t c0 = 0;
    static LARGE_INTEGER q0{};
    LARGE_INTEGER f{}, q{};
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&q);
    const uint64_t c = __rdtsc();
    if (!c0) {
        c0 = c;
        q0 = q;
        return -1.0;
    }
    const double ms = static_cast<double>(q.QuadPart - q0.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
    if (ms < 100.0 || c <= c0) return -1.0;
    return static_cast<double>(g_apexSolveCycles.load(std::memory_order_relaxed)) / (static_cast<double>(c - c0) / ms);
}

bool InMapLockStep() { return t_lockStep > 0; }
bool SolveHooksReady() { return g_finalizeReady.load(std::memory_order_relaxed) && g_lockStepReady.load(std::memory_order_relaxed); }

int SwitchRoomsPending(unsigned long since, int* visible) {
    struct Entry {
        uintptr_t room, mgr;
        int id;
    };
    Entry rooms[64];
    int n = 0;
    {
        const DWORD now = GetTickCount();
        std::lock_guard<std::mutex> lk(g_urgentMx);
        for (const UrgentRoom& u : g_urgent)
            if (static_cast<int32_t>(now - u.until) < 0 && n < static_cast<int>(std::size(rooms))) rooms[n++] = Entry{u.room, u.mgr, u.id};
    }
    int pending = 0, shown = 0;
    for (int k = 0; k < n; k++) {
        int level = 0, cam = 0, state = 0;
        if (!RoomViewState(rooms[k].room, rooms[k].mgr, rooms[k].id, level, cam, state) || level > cam) continue; // gone, or cut away above the camera's story
        const void* room = reinterpret_cast<const void*>(rooms[k].room);
        if (level < cam && !InAtrium(room)) continue; // under the camera's floor: seen only through an atrium's opening
        shown++;
        if ((state >= 1 && state <= 3) || !RoomLightQueue::SolvedSince(room, since)) pending++;
    }
    if (visible) *visible = shown;
    return pending;
}

bool LampEditPending() {
    if (!g_urgentSize.load(std::memory_order_relaxed)) return false;
    uintptr_t rooms[64];
    int n = 0;
    {
        const DWORD now = GetTickCount();
        std::lock_guard<std::mutex> lk(g_urgentMx);
        for (const UrgentRoom& u : g_urgent)
            if (static_cast<int32_t>(now - u.until) < 0 && n < static_cast<int>(std::size(rooms))) rooms[n++] = u.room;
    }
    for (int k = 0; k < n; k++) {
        uintptr_t mgr = 0;
        int id = 0;
        const void* room = reinterpret_cast<const void*>(rooms[k]);
        const int state = RoomKey(room, mgr, id) ? RoomState(room) : -1;
        if (state >= 1 && state <= 3) return true;
    }
    return false;
}

// A lamp moved or a value dragged within the last kDragQuietMs (05/10; RoomLightQueue no longer solves an edit's rooms in
// one go once it ends: that made 300-1300 ms frames while lamps were moved in Build mode)
std::atomic<DWORD> g_continuousEditAt{0};
constexpr DWORD kDragQuietMs = 200;
void NoteLampEditing(bool continuous) {
    const DWORD now = GetTickCount() | 1;
    g_lampEditAt.store(now, std::memory_order_relaxed); // AuditTakers waits for the edit's own sends
    if (continuous) g_continuousEditAt.store(now, std::memory_order_relaxed);
}
bool LampDragging() {
    const DWORD at = g_continuousEditAt.load(std::memory_order_relaxed);
    return at && GetTickCount() - at < kDragQuietMs;
}

void RelightAllRooms(const char* why) {
    LoadAddresses();
    if (!kRootPtr || !kRoomById || !kInvalidateRoom || !kSetInsert) return;
    if (!g_installed) { // nothing would run the coalesced relight (OnPresent runs only while Night Lighting is on): now
        RequeueAllRooms(why);
        return;
    }
    {
        std::lock_guard<std::mutex> lk(g_relightWhyMx);
        if (g_relightWhy.find(why) == std::string::npos) g_relightWhy += (g_relightWhy.empty() ? "" : " + ") + std::string(why);
    }
    g_relightAllAt.store((GetTickCount() + 250) | 1, std::memory_order_relaxed);
}

void SetDiagArmed(bool on) { g_diagArmed = on; }
bool DiagArmed() { return g_diagArmed.load(); }

std::string DiagText() {
    const bool wasArmed = g_diagArmed.exchange(true); // a dump arms the recording for the next one
    std::vector<DiagRec> recs;
    {
        std::lock_guard<std::mutex> lk(g_diagMx);
        recs.swap(g_diag);
        g_diagIndoorUsed = g_diagOutdoorUsed = 0;
        g_diagFull[0] = g_diagFull[1] = false;
    }
    if (!wasArmed && recs.empty())
        return std::format("\n==== ANDARES (luz externa entre andares) ====\n{}\nNo samples: recording them was off (it costs time in every light solve). It is on "
                           "now: let the lot relight (or use \"Relight lots now\") and save the diagnostics again.\n",
                           Status()) +
               IndoorDiagText() + SeamDiagText() + JournalText();
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
        s += std::format("A{} L{:08X} tipo{} casa{} luz({:.2f} {:.2f} {:.2f}) p({:.2f} {:.2f} {:.2f}) n({:.2f} {:.2f} {:.2f}) cor={:.3f} jogo={} t={:.2f} nosso={:.2f} lote={} filtro={} comodo={}{}\n",
                         r.level, r.light, r.type, r.home, r.lpos[0], r.lpos[1], r.lpos[2], r.p[0], r.p[1], r.p[2], r.n[0], r.n[1], r.n[2], r.lum, r.game, r.gameT, r.mine,
                         r.batch ? 1 : 0, r.culledList ? 1 : 0, r.room,
                         r.why < 0 ? std::string() : std::format(" | cruza=({},{},q{}) h={:.2f} t={:.3f} chave={:08X}:{:08X} baixo=comodo{} motivo={}", r.cx, r.cz, r.cq, r.ch, r.ct, r.keyHi, r.keyLo, r.belowRoom, r.why));
    return s + IndoorDiagText() + SeamDiagText() + JournalText();
}

std::string Status() {
    return std::format("Colour groups committed {} ({} native fallbacks) | ", g_groupCommits.load(), g_groupFallbacks.load()) + std::format("{} | outdoor lights carried to other stories: {} | stories updated: {} | walls of the light's story: {}/{} classes, {} tests, {} blocked | indoor "
                       "lamps through stair openings: {} | seamless walls between floors: {}{}{}{}{}",
                       g_installed ? "Active" : "Off", g_shared.load(), g_queued.load(), g_evalClasses.load(), std::size(kClasses), g_wallTests.load(), g_wallBlocked.load(),
                       !g_indoorReady ? std::string("not installed")
                                      : std::format("{}, {} taken, {} points tested ({} behind a floor, {} behind a wall), {} rooms updated, lighting detail {} ({} raised)",
                                                    g_indoorOn ? "on" : "off", g_indoorAdded.load(), g_indoorTests.load(), g_indoorFloorBlocked.load(),
                                                    g_indoorWallBlocked.load(), g_indoorQueued.load(), g_lodReady ? "on" : "not installed", g_lodBoosts.load()) +
                                          std::format(", stacked rooms ambient {} ({} merges; members' values taken at the merge {} in {:.1f} ms, already right {}, kept from their gather {}, only an older solve's {}; quick passes with the refinement's light threshold {} and wall tests {}), lots settled after loading {} ({} as soon as their rooms were done), lamp changes folded into "
                                                      "one update {} (lamps added, moved or removed, sent at once {}; lamp values changed without a move {}; outside of a floor marked changed without a change {}), lots rebuilt {}, floor objects made {}, rooms held until their solve ended {}, rooms still holding a lamp switched off gathered again {}",
                                                      g_ambReady ? "on" : "not installed", g_ambMerges.load(), g_canonTaken.load(), CanonMs(), g_canonKept.load(), g_canonReused.load(), g_canonOld.load(), g_quickThreshold.load(), g_quickTests.load(), g_settles.load(), g_settlesEarly.load(), g_depCoalesced.load(), g_depShapeSends.load(), g_lampWobbles.load(), g_outdoorQuiet.load(), g_lotRebuilds.load(), g_levelsMade.load(), g_deferredCount.load(), g_staleSends.load()) +
                                          std::format(", lamp edits first: rooms made urgent {}, gathered at once {}, marks held while their room was solved {} (given back {}), "
                                                      "floor batches with per-lamp wall lists {} (every wall tested {})",
                                                      g_urgentMarked.load(), g_gatherSoon.load(), g_heldMarks.load(), g_heldGiven.load(), g_maskListBatches.load(),
                                                      g_maskListFallbacks.load()) +
                                          std::format(", rooms taking lamps of another story checked against them {} (sent to gather again {}, left alone a while {}), "
                                                      "lamps of another story tested against the lit room's walls from where they enter its story {}{} (outdoors {}, past a wall end's penumbra {}), rooms a moved lamp never lit left until it is quiet {}",
                                                      g_auditChecks.load(), g_auditSent.load(), g_auditGaveUp.load(), g_enterTests.load(), g_enterReady ? "" : " (not installed)", g_outdoorEnters.load(), g_penumbraLifted.load(), g_editWaited.load()),
                       !g_alignReady ? std::string("not installed")
                                     : std::format("{} ({} wall samples moved to their drawn height, {} wall pieces left as the game has them, {} walls blurred across "
                                                   "their edges ({} points lit beyond them), {} edge rows kept out of the blur, {} wall pieces lit from their drawn foot)",
                                                   g_alignOn ? "on" : "off", g_alignRows.load(), g_alignOdd.load(), g_ghostWalls.load(), g_ghostPoints.load(),
                                                   g_alignEdges.load(), g_foundationPieces.load()) + " | wall heights: " + WallHeights::Status(),
                       g_otherThread.load() ? std::format(" | on another thread: {}", g_otherThread.load()) : "",
                       g_normCrossOnly.load() || g_normNotFinite.load()
                           ? std::format(" | rooms lit only by lamps of another story given no boost: {} (normalisation not finite: {})", g_normCrossOnly.load(), g_normNotFinite.load())
                           : "",
                       g_basisTests.load() ? std::format(" | directional maps: lamps of another story tested {}, behind a floor {}", g_basisTests.load(), g_basisBlocked.load()) : "",
                       g_faults.load() ? std::format(" | failures: {}", g_faults.load()) : "");
}

} // namespace LevelLightShare
