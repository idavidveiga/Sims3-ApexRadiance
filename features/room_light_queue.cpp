// Faster room lighting (see room_light_queue.h and docs/features/performance.md, "Room lighting queue").
//
// ---- The game side (Steam 1.67.2, TS3W.exe; research\engine_map\full.asm; study of 29/09, re\out) ----
//   0x006C5E20 per-frame light tree update (root update, render thread): ..., then "mov ecx,esi; pop esi; jmp 0x006C5C20".
//   0x006C5C20 the scheduler, fastcall(tree = lightMgr+0xD4), plain ret: returns at once while [tree+0x74] (the current
//              room) is set; else asks 0x006A8190 for (room, priority) of every room of every level -4..7 of every lot,
//              sorts them, and makes the best one current if its priority is above 0.001 (0x0069E860: state 3, the
//              manager's polling set). The current room is cleared only by FinalizePrime (0x006A0E00 -> 0x006C4870) or an
//              invalidate (0x0069EED0 / 0x0069F160 -> 0x006C4870).
//   0x006A8190 calls the priority 0x0069E770 (fastcall(room), float in ST0) at 0x006A81DF, its only call: 0 unless the
//              room is in state 2; class 0: 10000 x {1 camera story or outdoor below, 0.8 indoor below, 0.5 above} x (0.5
//              on a lot not in high quality); class 1: 1000; class 2: 100; 0 when the class is above LodChoice.
//   0x006A3F80 thiscall(room, stopwatch*, float budget), ret 8: runs the budgeted solve 0x006A3C90 when room+0xF0 == 3
//              (the lot lighting pass reaches it through 0x006A8BA0 -> 0x006A88B0 for each room of its polling set).
//   0x0069EA70 at the end of each solve: class (+0xF4) -> shown (+0x100); below LodChoice: 0 -> 1 (0x0069EAA2
//              "mov edi,1; lea ebx,[edi+1]") or 1 -> 2, state 2 (solve again); else state 4 / 5.
//   0x0069EED0 / 0x0069F160 invalidate: class = LodChoice when the room was solved before (+0x100 != 4) and shown >=
//              LodChoice, else 0 ("jl" at 0x0069EF58 / 0x0069F1C5).
//   Stopwatch (0x18 bytes): ctor 0x004F35B0 thiscall(sw, kind 4 = ms, char start) ret 8, start 0x00408700 thiscall(sw),
//              elapsed 0x004F33C0 thiscall(sw) -> ST0 (the calls 0x00ADB94D / 0x00ADB956 / 0x00ADB9BA of the lot pass).
//   Priority lot: 0x006FDE10 (mov eax,[0x011D1CF8]: SceneObjectManager) and 0x006FDC80 thiscall(som, lotLo, lotHi) ret 8
//              (al: the lot is one of the two at +0x10D0 / +0x10E0), as the lot budget 0x00ADB120 asks them. A story
//              lighting manager's lot id is mgr+0x90 (low) / +0x94 (high).
//
// ---- The patches (each validated against the bytes the Steam build has; a part whose bytes differ stays off) ----
//   Priority: the CALL at 0x006A81DF -> PriorityHook (the game's value x1000 for the priority lot's rooms on or below the
//   camera story, x4 on the camera story, x2 below it; 0 stays 0).
//   Direct step: 0x0069EAA2 "BF 01 00 00 00 8D 5F 01" -> "8B F8 BB 02 00 00 00 90" (mov edi,eax; mov ebx,2; nop).
//   Keep the class: "7C 0B" at 0x0069EF58 and "7C 02" at 0x0069F1C5 -> "90 90".
//   Drain: the jmp at 0x006C5E39 -> PickHook: the scheduler, then, on the render thread and while the current room is a
//   freshly picked room of the priority lot in state 3, solve it with a stopwatch of its own and pick again, until the
//   drain budget (4 ms, 1 ms while the camera moves) is spent or the room did not finish.
//   Empty removal (05/10): 0x006C7610, thiscall(levelLights; idLo, idHi), ret 8, takes an object out of the level's five
//   light maps (+0x4C, +0x90, +0xD4, +0x118, +0x15C: hash maps {?, buckets, bucket count, element count at +0x10}) with
//   five calls of 0x006C7550, each a find (0x00D726F0) that does nothing when the ID is not there; 0x006C7690 runs it for
//   the twelve levels -4..7 of a lot on every object removal (60 finds), and its callers ignore eax. When the five element
//   counts are 0 nothing can be found: the entry hook (framework/entry_chain.h, layer RoomLightQueue) returns at once.
// Every write goes through MemPatch::WriteCodeSuspended (no thread inside the bytes) and is put back by Stop.
#include "room_light_queue.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "d3d9_hooks.h"
#include "entry_chain.h"
#include "game_addresses.h"
#include "lot_lighting_motion.h"
#include "level_light_share.h"
#include "lamp_mark_filter.h"
#include "room_light_fade.h"
#include "room_ambient_policy.h"
#include "memory_patch.h"
#include "imgui.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <unordered_set>
#include <unordered_map>

namespace {

using Priority_t = float(__fastcall*)(void* room);
using Pick_t = void(__fastcall*)(void* tree);
using SolveStep_t = void(__thiscall*)(void* room, void* stopwatch, float budget);
using SwCtor_t = void*(__thiscall*)(void* sw, int kind, int start);
using SwStart_t = void(__thiscall*)(void* sw);
using SwElapsed_t = float(__thiscall*)(void* sw);
using SomGet_t = void*(__cdecl*)();
using PrioLot_t = bool(__thiscall*)(void* som, uint32_t lo, uint32_t hi);

uintptr_t kPriorityCall = 0, kPriority = 0, kStepSite = 0, kKeepA = 0, kKeepB = 0, kPickJump = 0, kPick = 0, kSolveStep = 0;
uintptr_t kSwCtor = 0, kSwStart = 0, kSwElapsed = 0, kSomGet = 0, kPrioLot = 0;

struct Write {
    uintptr_t at = 0;
    BYTE orig[8] = {};
    size_t n = 0;
    bool done = false;
};
Write g_writes[5]; // priority call, step, keep A, keep B, pick jump
std::mutex g_ctrl;
bool g_running = false;
bool g_prioOn = false, g_stepOn = false, g_keepOn = false, g_drainOn = false, g_emptyOn = false;
std::atomic<long> g_emptyCalls{0}, g_emptySkipped{0};

// 0x006C7610 (see the header comment, "Empty removal"): nothing to take out of five empty maps
void __fastcall EmptyRemovalHook(uint8_t* level, void* edx, uint32_t idLo, uint32_t idHi) {
    using Fn = void(__fastcall*)(uint8_t*, void*, uint32_t, uint32_t);
    const auto next = reinterpret_cast<Fn>(EntryChain::Next(EntryChain::Site::LightObjectRemove, EntryChain::Layer::RoomLightQueue));
    g_emptyCalls.fetch_add(1, std::memory_order_relaxed);
    if (level && !(*reinterpret_cast<const uint32_t*>(level + 0x5C) | *reinterpret_cast<const uint32_t*>(level + 0xA0) |
                   *reinterpret_cast<const uint32_t*>(level + 0xE4) | *reinterpret_cast<const uint32_t*>(level + 0x128) |
                   *reinterpret_cast<const uint32_t*>(level + 0x16C))) {
        g_emptySkipped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    next(level, edx, idLo, idHi);
}
std::atomic<DWORD> g_renderThread{0};
constexpr const char* kPresentName = "RoomLightQueue";
constexpr const char* kFadePresentName = "RoomLightFade";

std::atomic<long> g_prioCalls{0}, g_prioBoosted{0}, g_drainFrames{0}, g_drainSolves{0}, g_drainFinished{0}, g_prioUrgent{0}, g_drainUrgent{0};
std::atomic<long long> g_drainMicros{0};

inline DWORD ThreadId() { return __readfsdword(0x24); }

bool PriorityLot(uintptr_t mgr) {
    void* som = reinterpret_cast<SomGet_t>(kSomGet)();
    if (!som) return false;
    return reinterpret_cast<PrioLot_t>(kPrioLot)(som, *reinterpret_cast<const uint32_t*>(mgr + 0x90), *reinterpret_cast<const uint32_t*>(mgr + 0x94));
}

// x1000 for the priority lot's rooms on or below the camera's story (x4 on it, x2 below it); 1 otherwise
float Factor(const BYTE* room) {
    __try {
        const uintptr_t mgr = *reinterpret_cast<const uintptr_t*>(room);
        if (!mgr) return 1.0f;
        const int level = *reinterpret_cast<const int*>(mgr + 0x88), cam = *reinterpret_cast<const int*>(mgr + 0x284);
        const float factor = RoomAmbientPolicy::FloorPriorityFactor(PriorityLot(mgr), LevelLightShare::AllFloorsDetailed(), level, cam);
        if (factor == 1.0f) return factor;
        g_prioBoosted.fetch_add(1, std::memory_order_relaxed);
        return factor;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1.0f;
    }
}

// A room waiting for its solve (state 2) at a class above what it should get now (its story left the camera's, or its lot
// lost high quality, after it was sent at a higher class: the kept class of part 3, or a ladder step): the game gives it 0
// and would leave it there until the camera comes back. The lowest priority instead: solved last, at its (higher) class.
bool Stranded(const BYTE* room) {
    __try {
        return *reinterpret_cast<const uintptr_t*>(room) && *reinterpret_cast<const int*>(room + 0xF0) == 2 && *reinterpret_cast<const int*>(room + 0xF4) > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Quick pass (see SetQuickPass): a lamp edit's room waiting (state 2) at a class above 0 while many lamps switch goes to
// class 0, the state the game's own invalidate gives a room (part 3 keeps the class instead); "no middle step" then takes
// it straight to its class after that solve. Render thread (the scheduler).
std::atomic<bool> g_quickPass{true};
std::atomic<long> g_quickRooms{0};
// Per room of the burst: whether its class-0 solve is shown (06/10 capture: the switch's safety net invalidated a room
// right after it was set to 0, which gave it back its class without any quick solve; it is set to 0 again until its
// quick solve is seen, and never after, so a refined room is not sent back to class 0)
std::unordered_map<uintptr_t, bool> g_quickDone;
long g_quickEvent = -1;
// When every room of the burst shows its quick solve (06/10): the time is logged ("showed its new light"), and the rest of
// the burst is refinement, given 4 ms of extra solving a frame instead of 12 so the frame rate holds (the capture: 35-39 ms
// of solving a frame for 2-3 s while the rooms already showed the right light)
DWORD g_quickStart = 0;
bool g_quickShown = false;
std::atomic<bool> g_refining{false}; // read by the lot lighting budget too (LotLightingMotion)
// Its class-0 solve is shown ("shown" +0x100 is 0 after it: "class 2/0" in the recorder), or the room finished
bool QuickSolved(const BYTE* room) {
    __try {
        const int state = *reinterpret_cast<const int*>(room + 0xF0);
        return state == 4 || state == 5 || *reinterpret_cast<const int*>(room + 0x100) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true; // gone
    }
}
void CheckQuickShown() {
    if (g_quickShown || g_quickDone.empty()) return;
    bool all = true;
    for (auto& [room, solved] : g_quickDone) {
        if (!solved) solved = QuickSolved(reinterpret_cast<const BYTE*>(room));
        all = all && solved;
    }
    if (!all) return;
    g_quickShown = true;
    g_refining = true;
    LOG_INFO(std::format("[RoomLightQueue] Many lamps: the {} rooms of the burst showed their new light (quick pass) after {} ms; refining in the background",
                         g_quickDone.size(), GetTickCount() - g_quickStart));
}
bool WaitingAboveClass0(const BYTE* room) {
    __try {
        return *reinterpret_cast<const int*>(room + 0xF0) == 2 && *reinterpret_cast<const int*>(room + 0xF4) > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool SetClass0(BYTE* room) {
    __try {
        *reinterpret_cast<int*>(room + 0xF4) = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void QuickPassRoom(BYTE* room) {
    if (!g_quickPass.load(std::memory_order_relaxed) || !LampMarkFilter::MassSwitchActive() || LevelLightShare::LampUrgency(room) <= 1.0f) return;
    const long burst = LampMarkFilter::MassEventId();
    if (burst != g_quickEvent) {
        g_quickEvent = burst;
        g_quickDone.clear();
        g_quickStart = GetTickCount();
        g_quickShown = false;
        g_refining = false;
    }
    const uintptr_t key = reinterpret_cast<uintptr_t>(room);
    const auto it = g_quickDone.find(key);
    if (it != g_quickDone.end() && (it->second || (it->second = QuickSolved(room)))) return; // its quick solve was shown
    if (!WaitingAboveClass0(room)) return;
    const bool first = it == g_quickDone.end();
    if (first && g_quickDone.size() >= 4096) return;
    if (first) g_quickDone.emplace(key, false);
    if (SetClass0(room) && first) g_quickRooms.fetch_add(1, std::memory_order_relaxed);
}

float __fastcall PriorityHook(BYTE* room) {
    if (room && g_prioOn) QuickPassRoom(room);
    const float p = reinterpret_cast<Priority_t>(kPriority)(room);
    if (!room || !g_prioOn) return p;
    if (!(p > 0.0f)) return Stranded(room) ? 1.0f : p;
    g_prioCalls.fetch_add(1, std::memory_order_relaxed);
    // a lamp edit's rooms (moved, switched, recoloured) before any other room: the lamp's own, then the stories taking it
    const float urgency = LevelLightShare::LampUrgency(room);
    if (urgency > 1.0f) g_prioUrgent.fetch_add(1, std::memory_order_relaxed);
    return p * Factor(room) * urgency;
}

// The current room, when it is a room of the priority lot in state 3 (picked, not started or budget left), else null
BYTE* DrainableRoom(const BYTE* tree) {
    __try {
        BYTE* room = *reinterpret_cast<BYTE* const*>(tree + 0x74);
        if (!room || *reinterpret_cast<const int*>(room + 0xF0) != 3) return nullptr;
        const uintptr_t mgr = *reinterpret_cast<const uintptr_t*>(room);
        if (!mgr || !*reinterpret_cast<const BYTE*>(mgr + 0x280) || !PriorityLot(mgr)) return nullptr;
        return room;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
BYTE* CurrentRoom(const BYTE* tree) {
    __try {
        return *reinterpret_cast<BYTE* const*>(tree + 0x74);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}

// The lot id of a room's story (mgr+0x90 low, +0x94 high), 0 when unreadable
uint64_t LotOf(const BYTE* room) {
    __try {
        const uintptr_t mgr = room ? *reinterpret_cast<const uintptr_t*>(room) : 0;
        return mgr ? (static_cast<uint64_t>(*reinterpret_cast<const uint32_t*>(mgr + 0x94)) << 32) | *reinterpret_cast<const uint32_t*>(mgr + 0x90) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
uint64_t g_lastLot = 0; // the lot of the room that was current at the end of the last pick (render thread)

// A lamp edit's rooms from the first waiting one to the last solved (05/10, to tune "Lamp edits first"): the time, the
// frames and how long the game solved rooms meanwhile (its own per-class solve time 0x011D1200..08, Steam addresses), one
// log line per edit (a drag is one edit while its rooms keep waiting). Render thread.
struct EditBurst {
    bool on = false;
    DWORD start = 0;
    long frames = 0;
    long long drainMicros = 0;
    float solved = 0.0f;
    long quick = 0; // g_quickRooms at the start
    double apexMs = -1.0; // LevelLightShare::ApexSolveMs at the start
};
EditBurst g_burst;
float GameSolveMs() {
    float t[3] = {};
    return GameAddr::IsFixed() && MemPatch::ReadBytes(0x011D1200, t, sizeof t) ? t[0] + t[1] + t[2] : 0.0f;
}
std::string ApexPart(double startMs) {
    const double now = LevelLightShare::ApexSolveMs();
    return startMs >= 0.0 && now >= startMs ? std::format("{:.0f} ms", now - startMs) : std::string("not measured yet");
}
void NoteEditBurst(bool lampEdit) {
    if (lampEdit) {
        if (!g_burst.on) g_burst = EditBurst{true, GetTickCount(), 0, g_drainMicros.load(std::memory_order_relaxed), GameSolveMs(), g_quickRooms.load(std::memory_order_relaxed), LevelLightShare::ApexSolveMs()};
        g_burst.frames++;
        return;
    }
    if (!g_burst.on) return;
    g_burst.on = false;
    const DWORD ms = GetTickCount() - g_burst.start;
    const float solved = GameSolveMs() - g_burst.solved;
    const float drained = static_cast<float>(g_drainMicros.load(std::memory_order_relaxed) - g_burst.drainMicros) / 1000.0f;
    LOG_INFO(std::format("[RoomLightQueue] Lamp edit: its rooms settled after {} ms over {} frames; the game solved rooms for {:.0f} ms ({:.1f} ms a frame), "
                         "{:.0f} ms of it right after the pick; {} rooms took the quick pass first; Apex's own tests inside those solves: {}",
                         ms, g_burst.frames, solved, g_burst.frames ? solved / static_cast<float>(g_burst.frames) : 0.0f, drained,
                         g_quickRooms.load(std::memory_order_relaxed) - g_burst.quick, ApexPart(g_burst.apexMs)));
}

std::atomic<BYTE*> g_tree{nullptr}; // the light tree the scheduler runs on (SolveInProgress)
int StateOf(const BYTE* room) {
    __try {
        return *reinterpret_cast<const int*>(room + 0xF0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

void __fastcall PickHook(BYTE* tree) {
    if (tree) g_tree.store(tree, std::memory_order_relaxed);
    const auto pick = reinterpret_cast<Pick_t>(kPick);
    BYTE* before = tree ? CurrentRoom(tree) : nullptr;
    pick(tree);
    if (!g_drainOn || !tree || ThreadId() != g_renderThread.load(std::memory_order_relaxed)) return;
    // Drain only when the room that was current last time is done (so the lot pass of its lot ran: that lot is not paused,
    // 0x00ADB8F0 checks +0x18 / +0x4E) and the new current room is of that same lot and is the priority lot's
    const uint64_t finishedLot = g_lastLot;
    // A lamp edit's rooms waiting (LevelLightShare, "Lamp edits first"): 12 ms instead of 4 with the camera still, and a lamp
    // edit's room still current from the last frame is solved further here too (the lot pass goes on with it anyway), so a
    // dragged lamp's light follows it within a frame or two
    const bool lampEdit = LevelLightShare::LampEditPending();
    NoteEditBurst(lampEdit);
    CheckQuickShown();
    if (!lampEdit) g_refining = false;
    BYTE* room = DrainableRoom(tree);
    if (before && !(lampEdit && room == before && LevelLightShare::LampUrgency(room) > 1.0f)) room = nullptr;
    if (room && (!finishedLot || LotOf(room) != finishedLot)) room = nullptr;
    if (room) {
        // a lamp being dragged: 6 ms (its own room follows it without the frame rate dropping, 05/10 recording 20:38:25)
        // many lamps whose rooms already show their quick solve: 4 ms (refinement in the background, see CheckQuickShown)
        const float budget = LotLightingMotion::SampleCameraMoving() ? 1.0f
                             : lampEdit ? (g_refining ? 4.0f : LevelLightShare::LampDragging() ? 6.0f : 12.0f) : 4.0f;
        if (lampEdit) g_drainUrgent.fetch_add(1, std::memory_order_relaxed);
        alignas(16) BYTE sw[32] = {};
        reinterpret_cast<SwCtor_t>(kSwCtor)(sw, 4, 0);
        reinterpret_cast<SwStart_t>(kSwStart)(sw);
        const auto elapsed = reinterpret_cast<SwElapsed_t>(kSwElapsed);
        const auto solve = reinterpret_cast<SolveStep_t>(kSolveStep);
        g_drainFrames.fetch_add(1, std::memory_order_relaxed);
        for (int n = 0; n < 16 && room; n++) {
            if (elapsed(sw) >= budget * 0.8f) break;
            solve(room, sw, budget);
            g_drainSolves.fetch_add(1, std::memory_order_relaxed);
            if (CurrentRoom(tree) == room) break; // the budget ran out inside it: the lot pass goes on next frame
            g_drainFinished.fetch_add(1, std::memory_order_relaxed);
            pick(tree);
            room = DrainableRoom(tree);
            if (room && LotOf(room) != finishedLot) room = nullptr;
        }
        g_drainMicros.fetch_add(static_cast<long long>(elapsed(sw) * 1000.0f), std::memory_order_relaxed);
    }
    g_lastLot = LotOf(CurrentRoom(tree));
}

bool Bytes(uintptr_t at, const char* expect, size_t n) {
    BYTE b[16];
    return at && MemPatch::ReadBytes(at, b, n) && std::memcmp(b, expect, n) == 0;
}
bool Put(Write& w, uintptr_t at, const BYTE* bytes, size_t n) {
    w = Write{at, {}, n, false};
    if (!MemPatch::ReadBytes(at, w.orig, n) || !MemPatch::WriteCodeSuspended(at, bytes, n)) return false;
    w.done = true;
    return true;
}
bool Rel32(uintptr_t at, uintptr_t target, BYTE op, BYTE* out) { // "op rel32" at `at` reaching `target`
    out[0] = op;
    const int32_t rel = static_cast<int32_t>(target - (at + 5));
    std::memcpy(out + 1, &rel, 4);
    return true;
}
uintptr_t JumpTarget(uintptr_t at) {
    BYTE b[5];
    if (!MemPatch::ReadBytes(at, b, 5) || b[0] != 0xE9) return 0;
    int32_t rel;
    std::memcpy(&rel, b + 1, 4);
    return at + 5 + rel;
}

void LoadAddresses() {
    using GameAddr::Get;
    using GameAddr::Id;
    kPriorityCall = Get(Id::RoomPriorityCall), kPriority = Get(Id::RoomPriority);
    kStepSite = Get(Id::LodStepSite), kKeepA = Get(Id::KeepClassA), kKeepB = Get(Id::KeepClassB);
    kPickJump = Get(Id::RoomPickJump), kPick = Get(Id::RoomPick), kSolveStep = Get(Id::RoomSolveStep);
    kSwCtor = Get(Id::StopwatchCtor), kSwStart = Get(Id::StopwatchStart), kSwElapsed = Get(Id::StopwatchElapsed);
    kSomGet = Get(Id::PriorityLotObject), kPrioLot = Get(Id::PriorityLotTest);
}

void RestoreAll() {
    for (int i = 4; i >= 0; i--) {
        Write& w = g_writes[i];
        if (!w.done) continue;
        if (MemPatch::WriteCodeSuspended(w.at, w.orig, w.n)) w.done = false;
        else LOG_WARNING(std::format("[RoomLightQueue] Could not put back the game code at {:#x}", w.at));
    }
    if (g_emptyOn) {
        EntryChain::Remove(EntryChain::Site::LightObjectRemove, EntryChain::Layer::RoomLightQueue);
        g_emptyOn = false;
    }
    g_prioOn = g_stepOn = g_keepOn = g_drainOn = false;
}

} // namespace

namespace RoomLightQueue {

bool Start(std::string* error) {
    std::lock_guard<std::mutex> lk(g_ctrl);
    if (g_running) return true;
    LoadAddresses();
    using GameAddr::Id;
    std::string why;
    // The helpers every part needs
    if (!GameAddr::Have({Id::PriorityLotObject, Id::PriorityLotTest}, &why) || !Bytes(kSomGet, "\xA1", 1) || !Bytes(kPrioLot, "\x8B\x44\x24\x04", 4)) {
        if (error) *error = "Faster room lighting: " + (why.empty() ? std::string("the game code differs (different game version?)") : GameAddr::NotAvailable(why));
        return false;
    }
    // 1. priority
    if (GameAddr::Have({Id::RoomPriorityCall, Id::RoomPriority}) && Bytes(kPriority, "\x83\xEC\x0C\x56\x8B\xF1\x57\x8B\x3E", 9)) {
        BYTE b[5];
        Rel32(kPriorityCall, reinterpret_cast<uintptr_t>(&PriorityHook), 0xE8, b);
        BYTE cur[5];
        g_prioOn = MemPatch::ReadBytes(kPriorityCall, cur, 5) && cur[0] == 0xE8 && kPriorityCall + 5 + *reinterpret_cast<int32_t*>(cur + 1) == kPriority &&
                   Put(g_writes[0], kPriorityCall, b, 5);
    }
    // 2. class 0 -> target in one step
    if (GameAddr::Have({Id::LodStepSite}) && Bytes(kStepSite, "\xBF\x01\x00\x00\x00\x8D\x5F\x01\xEB\x0C", 10)) {
        const BYTE b[8] = {0x8B, 0xF8, 0xBB, 0x02, 0x00, 0x00, 0x00, 0x90};
        g_stepOn = Put(g_writes[1], kStepSite, b, 8);
    }
    // 3. requeues keep the class (both invalidates or neither)
    if (GameAddr::Have({Id::KeepClassA, Id::KeepClassB}) && Bytes(kKeepA - 7, "\x83\xF9\x04\x74\x0F\x3B\xC8\x7C\x0B", 9) &&
        Bytes(kKeepB - 7, "\x83\xF9\x04\x74\x06\x3B\xC8\x7C\x02", 9)) {
        const BYTE nop2[2] = {0x90, 0x90};
        g_keepOn = Put(g_writes[2], kKeepA, nop2, 2) && Put(g_writes[3], kKeepB, nop2, 2);
        if (!g_keepOn && g_writes[2].done && MemPatch::WriteCodeSuspended(g_writes[2].at, g_writes[2].orig, 2)) g_writes[2].done = false;
    }
    // 4. more than one room per frame
    if (GameAddr::Have({Id::RoomPickJump, Id::RoomPick, Id::RoomSolveStep, Id::StopwatchCtor, Id::StopwatchStart, Id::StopwatchElapsed}) &&
        JumpTarget(kPickJump) == kPick && Bytes(kPick, "\x81\xEC\x14\x04\x00\x00\x55\x8B\xE9\x83\x7D\x74\x00", 13) &&
        Bytes(kSolveStep, "\x83\xB9\xF0\x00\x00\x00\x03\x75\x12", 9)) {
        g_drainOn = D3D9Hooks::RegisterPresent(kPresentName, [](D3D9Hooks::DeviceContext&, const RECT*, const RECT*, HWND, const RGNDATA*) {
            g_renderThread.store(ThreadId(), std::memory_order_relaxed);
            return D3D9Hooks::HookAction::Continue;
        });
        BYTE b[5];
        Rel32(kPickJump, reinterpret_cast<uintptr_t>(&PickHook), 0xE9, b);
        g_drainOn = g_drainOn && Put(g_writes[4], kPickJump, b, 5);
        if (!g_drainOn) D3D9Hooks::UnregisterAll(kPresentName);
    }
    // 5. removals from empty light maps return at once
    if (GameAddr::Have({Id::LightObjectRemove})) {
        std::string err;
        g_emptyOn = EntryChain::Install(EntryChain::Site::LightObjectRemove, EntryChain::Layer::RoomLightQueue, reinterpret_cast<void*>(&EmptyRemovalHook), &err);
        if (!g_emptyOn) LOG_WARNING("[RoomLightQueue] Empty removal not used: " + err);
    }
    if (!g_prioOn && !g_stepOn && !g_keepOn && !g_drainOn && !g_emptyOn) {
        if (error) *error = "Faster room lighting: the game code differs (different game version?)";
        return false;
    }
    // 6. smooth light changes indoors (room_light_fade.h): the fades advance at every frame boundary
    D3D9Hooks::RegisterPresent(kFadePresentName, [](D3D9Hooks::DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
        RoomLightFade::OnPresent(ctx.device);
        return D3D9Hooks::HookAction::Continue;
    });
    g_running = true;
    LOG_INFO(std::format("[RoomLightQueue] Started: viewed lot first {}, no middle step {}, requeues keep the class {}, several rooms per frame {}, empty removals skipped {}",
                         g_prioOn ? "yes" : "no", g_stepOn ? "yes" : "no", g_keepOn ? "yes" : "no", g_drainOn ? "yes" : "no", g_emptyOn ? "yes" : "no"));
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> lk(g_ctrl);
    if (!g_running) return;
    D3D9Hooks::UnregisterAll(kPresentName);
    D3D9Hooks::UnregisterAll(kFadePresentName);
    RoomLightFade::Clear();
    RestoreAll();
    g_running = std::any_of(std::begin(g_writes), std::end(g_writes), [](const Write& w) { return w.done; });
    LOG_INFO(g_running ? "[RoomLightQueue] Stopped, but some game code could not be put back" : "[RoomLightQueue] Stopped");
}

bool Running() {
    std::lock_guard<std::mutex> lk(g_ctrl);
    return g_running;
}

void SetQuickPass(bool on) { g_quickPass.store(on, std::memory_order_relaxed); }
bool QuickPass() { return g_quickPass.load(std::memory_order_relaxed); }
bool Refining() { return g_refining.load(std::memory_order_relaxed); }

bool SolveInProgress() {
    if (ThreadId() != g_renderThread.load(std::memory_order_relaxed)) return false;
    const BYTE* tree = g_tree.load(std::memory_order_relaxed);
    const BYTE* room = tree ? CurrentRoom(tree) : nullptr;
    return room && StateOf(room) == 3;
}

// The game's own cumulative solve time per class (0x011D1200/04/08, ms; 0x006C2380 adds each finished solve)
std::string SolveTimes() {
    float t[3] = {};
    if (!MemPatch::ReadBytes(0x011D1200, t, sizeof t) || !GameAddr::IsFixed()) return "";
    return std::format(" | the game's solve time by class: {:.1f} / {:.1f} / {:.1f} s", t[0] / 1000.0f, t[1] / 1000.0f, t[2] / 1000.0f);
}

std::string StatusText() {
    if (!Running()) return "Off";
    const long frames = g_drainFrames.load(), solves = g_drainSolves.load();
    return std::format("On | viewed lot first {} ({} of {} priorities raised; a lamp edit's rooms first {}), no middle step {}, requeues keep the class {}, several rooms per frame {} ({} frames, "
                       "{} extra solves, {} finished, {:.1f} ms in all; {} frames with a lamp edit's rooms waiting), empty removals skipped {} ({} of {}), "
                       "quick pass for many lamps {} ({} rooms){}",
                       g_prioOn ? "on" : "off", g_prioBoosted.load(), g_prioCalls.load(), g_prioUrgent.load(), g_stepOn ? "on" : "off", g_keepOn ? "on" : "off",
                       g_drainOn ? "on" : "off", frames, solves, g_drainFinished.load(), g_drainMicros.load() / 1000.0, g_drainUrgent.load(), g_emptyOn ? "on" : "off", g_emptySkipped.load(),
                       g_emptyCalls.load(), g_quickPass.load() ? "on" : "off", g_quickRooms.load(), SolveTimes()) +
           " | smooth light changes: " + RoomLightFade::Status();
}

void RenderDeveloperUI() {
    if (kPublicBuild) return;
    ImGui::TextWrapped("Room lighting queue: %s", StatusText().c_str());
    ImGui::TextWrapped("Smooth light changes indoors: %s", RoomLightFade::Status().c_str());
}

} // namespace RoomLightQueue
