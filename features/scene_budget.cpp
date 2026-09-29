// Scene node budget (see scene_budget.h and docs/features/performance.md, section "Spread new objects over frames (C6)").
//
// ---- The game side (Steam 1.67.2, TS3W.exe; research\engine_map\full.asm; addresses through framework/game_addresses.h) ----
//   Scene sub-object ("pending holder", [scene+8], ctor 0x006E4530): +0x18 counter (nodes processed by the last drain),
//   +0x20 / +0x24 the sentinel {next, prev} of a circular intrusive list of pending scene nodes.
//   Scene node (base ctor 0x006FD710, vtable 0x00FF9D00): +0x18 / +0x1C its pending link {next, prev} (0 = not queued),
//   +0x30 its owner (the pending holder; +0x2C of it = the spatial tree), refcounted (+4, vfunc +0 AddRef / +4 Release).
//   Queueing (all on the scene's code, no lock anywhere):
//     0x006FAC70 MarkDirty(node): if link.next == 0: link = self-loop, then 0x006E42E0(owner, node) = push_back on
//                owner+0x20 (link.prev = tail, link.next = sentinel). Same inline pattern in 0x006FCA20 (children),
//                0x006FCB10 (LOD band change), 0x006FD9F0 (SetOwner).
//     0x006E6xxx AddNode (0x006E64EF): the scene AddRefs the node (vfunc +0) and queues it (a new node's link is the
//                constructor's self-loop, so it is queued unconditionally).
//     0x006E4984 RemoveNode: if link.next != 0 the node is unlinked from whatever list holds it (link.prev->next =
//                link.next, link.next->prev = link.prev) and self-looped, then removed from the spatial tree (0x006FB490),
//                SetOwner(0) (vfunc +0x1C) and Released (vfunc +4). So a queued node is always referenced by its scene
//                and never freed while linked; the node destructor 0x006FD930 does not touch the link.
//   0x006E4130 the drain, thiscall(holder), ret (0xD1 bytes, checked byte for byte at Start):
//     splices the whole list into a local sentinel on its stack and empties the holder's list; [this+0x18] = 0; then
//     while the local list is not empty: l = local.prev (the most recently queued first); unlink it (l->prev->next =
//     &local, local.prev = l->prev); l->prev = l->next = 0; node = l - 0x18; node->vfunc+0x48(); bounds =
//     0x006FB4B0(node, &aligned32) (world AABB, movaps into the buffer); 0x006FAD70(node, bounds) (moves the node to the
//     spatial cell that contains it, or out of the tree); [this+0x18] += 1. Nodes queued during the loop go to the
//     holder's (now empty) list: processed by the next drain.
//   Callers (all direct): 0x006EBC49 Scene::BeginFrame (every frame), 0x006DF9B5, 0x006EDBC7 (a scene query: drains
//   first so its answer is current), 0x006EF07D, 0x006F226B (render-to-texture, when asked), 0x006F3CF1. Only the
//   BeginFrame CALL is redirected; the other five still drain everything, including what this feature left queued.
//
// ---- The copy with a budget (Hook_SceneDrain) ----
//   Exactly the loop above (same list operations in the same order, same calls, same counter), plus a stop test before
//   each node: at least kMinNodes, then stop at nodesPerFrame nodes or msPerFrame ms. The nodes not reached stay linked,
//   in their order, and the whole remainder is spliced back at the FRONT of the holder's list (before anything queued
//   during the loop), so they stay queued exactly as the game would have them: RemoveNode can still unlink them, MarkDirty
//   still sees them as queued, any other drain processes them. Nothing is copied out of the game's memory.
//   Why deferring is safe (VERIFIED parts from the code above; INFERRED where marked):
//     - no lifetime hazard: a linked node is owned by its scene (AddNode's AddRef, released only by RemoveNode after the
//       unlink), and the remainder is a well-formed part of the holder's list at all times;
//     - the game already tolerates queued nodes between drains: nodes queued after BeginFrame (animation, scripts,
//       streaming later in the frame) are rendered with their previous spatial cell until the next frame's drain, and
//       0x0071A540 runs a queued node's vfunc +0x48 itself when it needs it (INFERRED: that it covers every consumer);
//     - consumers that need a current tree drain it themselves first (0x006EDBC7, 0x006F226B...), and still do.
//   What a deferred node looks like (INFERRED): a new node is culled / not drawn until it is processed (appears a frame or
//   a few later); a moved node keeps its old spatial cell (may be culled at its old place for those frames).
//   The rule: camera still -> the game's drain; camera moving -> the budgeted copy; a node waited maxDeferMs -> the game's
//   drain (so the backlog cannot grow without end while the camera keeps moving).
//
// Part of Apex Radiance. Credits: @loinyx

#include "scene_budget.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "call_chain.h"
#include "game_addresses.h"
#include "lot_lighting_motion.h"
#include "memory_patch.h"
#include "imgui.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace SceneBudget {
namespace {

using CallChain::Layer;
using CallChain::Site;

// The drain as disassembled (Steam 1.67.2), wildcards only on the two CALL rel32s (0x006FB4B0 and 0x006FAD70)
constexpr const char* kDrainBody =
    "55 8B EC 83 E4 F0 83 EC 34 53 56 57 8B F9 8B 77 20 8B 5F 24 8D 47 20 3B F0 8D 4C 24 18 8B D1 89 "
    "74 24 18 89 5C 24 1C 89 10 89 48 04 75 0A 89 4C 24 1C 89 54 24 18 EB 0D 89 0B 8B 4C 24 18 89 51 "
    "04 8B 4C 24 1C 8D 54 24 18 39 10 75 07 89 40 04 89 00 EB 0E 8B 48 04 89 01 8B 10 89 42 04 8B 4C "
    "24 1C 8D 44 24 18 3B C8 C7 47 18 00 00 00 00 74 59 BB 01 00 00 00 8B 51 04 8B C1 83 C1 04 8D 74 "
    "24 18 89 32 8B 54 24 1C 8B 52 04 89 54 24 1C 8D 70 E8 C7 01 00 00 00 00 C7 00 00 00 00 00 8B 06 "
    "8B 50 48 8B CE FF D2 8D 44 24 20 50 8B CE E8 ?? ?? ?? ?? 50 8B CE E8 ?? ?? ?? ?? 8B 4C 24 1C 01 "
    "5F 18 8D 54 24 18 3B CA 75 AC 5F 5E 5B 8B E5 5D C3";
constexpr uint32_t kCounterOff = 0x18; // holder: nodes processed by the last drain
constexpr uint32_t kListOff = 0x20;    // holder: pending list sentinel {next, prev}
constexpr uint32_t kLinkOff = 0x18;    // node: its pending link
constexpr uint32_t kUpdateSlot = 0x48; // node vfunc: per-node update
constexpr uint32_t kMinNodes = 8;      // processed every frame whatever the budget (progress)
constexpr bool kSuspended = true;    // see Start
constexpr uint32_t kCountCap = 1u << 20;

struct Link {
    Link* next;
    Link* prev;
};

using FnDrain = void(__fastcall*)(void* holder, void* edx);
using FnUpdate = void(__fastcall*)(void* node, void* edx);
using FnBounds = void*(__fastcall*)(void* node, void* edx, void* out);
using FnSpatial = void(__fastcall*)(void* node, void* edx, void* bounds);

std::mutex g_ctrl;
bool g_started = false;
std::atomic<bool> g_on{false};
uintptr_t g_drainFn = 0;
FnBounds g_bounds = nullptr;   // 0x006FB4B0
FnSpatial g_spatial = nullptr; // 0x006FAD70

std::atomic<int> g_nodesPerFrame{512};
std::atomic<float> g_msPerFrame{2.0f};
std::atomic<int> g_maxDeferMs{500};

// render thread only: per pending holder (one per scene; a few at most), since when nodes we left have been waiting
struct Waiting {
    void* holder;
    uint64_t since;    // GetTickCount64 of the first budgeted frame that left nodes (0 = nothing of ours waiting)
    uint64_t lastSeen; // GetTickCount64 of this holder's previous drain call
};
constexpr int kHolders = 8;
constexpr uint64_t kEvictAfterMs = 1000; // a slot is reused only when its holder has not drained for this long
constexpr uint64_t kSteadyMs = 100;      // only holders drained every frame (the world scene) are budgeted
Waiting g_waiting[kHolders] = {};
double g_qpcMs = 0.0;

// The holder's slot, or nullptr when every slot belongs to a holder still in use (the caller then drains everything,
// so a holder can never lose its "waiting since" and escape the forced full drain). prevSeen = its previous call (0 = new).
Waiting* WaitingOf(void* holder, uint64_t now, uint64_t& prevSeen) {
    for (Waiting& w : g_waiting)
        if (w.holder == holder) {
            prevSeen = w.lastSeen;
            w.lastSeen = now;
            return &w;
        }
    prevSeen = 0;
    for (Waiting& w : g_waiting)
        if (!w.holder || now - w.lastSeen >= kEvictAfterMs) {
            w = {holder, 0, now};
            return &w;
        }
    return nullptr;
}
thread_local DrainNote t_note;

struct Counter {
    std::atomic<uint64_t> v{0};
    void Add(uint64_t n = 1) { v.fetch_add(n, std::memory_order_relaxed); }
    uint64_t Get() const { return v.load(std::memory_order_relaxed); }
};
Counter c_calls, c_fullStill, c_fullForced, c_budgeted, c_framesLeft, c_nodesBudgeted, c_nodesLeft;
std::atomic<uint32_t> g_maxLeft{0}, g_lastDone{0}, g_lastLeft{0};
std::atomic<float> g_lastMs{0.0f};

int64_t Qpc() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

// The budgeted copy of 0x006E4130 (see the header comment). Returns the nodes processed; *left = nodes still queued.
uint32_t BudgetedDrain(uint8_t* holder, uint32_t cap, int64_t deadline, uint32_t* left) {
    Link* const head = reinterpret_cast<Link*>(holder + kListOff);
    uint32_t* const counter = reinterpret_cast<uint32_t*>(holder + kCounterOff);
    Link local;
    // splice everything into the local list; the holder's list is empty afterwards (0x006E413E..0x006E4192)
    Link* const first = head->next;
    Link* const last = head->prev;
    if (first == head) {
        local.next = &local;
        local.prev = &local;
    } else {
        local.next = first;
        local.prev = last;
        first->prev = &local;
        last->next = &local;
    }
    head->next = head;
    head->prev = head;
    *counter = 0;
    uint32_t done = 0;
    alignas(16) float bounds[8] = {}; // 0x006FB4B0 stores two aligned xmm into it
    // the loop (0x006E41A6..0x006E41F8), newest first; local.prev is re-read after every node (game code may unlink others)
    while (local.prev != &local) {
        if (done >= kMinNodes && (done >= cap || Qpc() >= deadline)) break;
        Link* const l = local.prev;
        Link* const p = l->prev;
        p->next = &local;
        local.prev = p;
        l->prev = nullptr;
        l->next = nullptr;
        uint8_t* const node = reinterpret_cast<uint8_t*>(l) - kLinkOff;
        const uintptr_t vtable = *reinterpret_cast<const uintptr_t*>(node);
        reinterpret_cast<FnUpdate>(*reinterpret_cast<const uintptr_t*>(vtable + kUpdateSlot))(node, nullptr);
        void* const b = g_bounds(node, nullptr, bounds);
        g_spatial(node, nullptr, b);
        *counter += 1;
        done++;
    }
    // The rest goes back to the TAIL of the holder's list, in its order. The game queues at the tail (0x006E42E0) and the
    // drain takes from the tail (local.prev first), so the leftover, being newer in the list than anything queued
    // meanwhile, is processed first next frame; at the front it would be processed last and only cleared by the forced
    // full drain.
    uint32_t n = 0;
    if (local.next != &local) {
        Link* const f = local.next;
        Link* const t = local.prev;
        Link* const tail = head->prev; // == head when nothing was queued meanwhile
        tail->next = f;
        f->prev = tail;
        t->next = head;
        head->prev = t;
        for (Link* x = f;; x = x->next) { // counted for the statistics only
            n++;
            if (x == t || n >= kCountCap) break;
        }
    }
    *left = n;
    return done;
}

// Scene::BeginFrame's CALL of the drain (render thread), inner layer of the call chain
void __fastcall Hook_SceneDrain(void* holder, void* edx) {
    const FnDrain next = reinterpret_cast<FnDrain>(CallChain::Next(Site::SceneDrain, Layer::SceneBudget));
    t_note = DrainNote{};
    if (!g_on.load(std::memory_order_acquire) || !holder) {
        next(holder, edx);
        return;
    }
    t_note.seen = true;
    c_calls.Add();
    const bool moving = LotLightingMotion::SampleCameraMoving();
    const uint64_t now = GetTickCount64();
    uint64_t prevSeen = 0;
    Waiting* const wp = WaitingOf(holder, now, prevSeen);
    // Budget only a scene drained every frame (the world); a scene drawn once or now and then (UI / off-screen) gets
    // everything, so it is never drawn with objects missing
    const bool steady = wp && prevSeen && now - prevSeen <= kSteadyMs;
    const bool waitedTooLong = wp && wp->since && now - wp->since >= static_cast<uint64_t>(g_maxDeferMs.load(std::memory_order_relaxed));
    if (!moving || waitedTooLong || !steady) {
        (waitedTooLong && moving ? c_fullForced : c_fullStill).Add();
        next(holder, edx); // the game's drain: everything, the nodes we left included
        if (wp) wp->since = 0;
        return;
    }
    Waiting& w = *wp;
    const int64_t t0 = Qpc();
    const float ms = std::max(0.1f, g_msPerFrame.load(std::memory_order_relaxed));
    const int64_t deadline = t0 + static_cast<int64_t>(static_cast<double>(ms) / g_qpcMs);
    const uint32_t cap = static_cast<uint32_t>(std::max(static_cast<int>(kMinNodes), g_nodesPerFrame.load(std::memory_order_relaxed)));
    uint32_t left = 0;
    const uint32_t done = BudgetedDrain(static_cast<uint8_t*>(holder), cap, deadline, &left);
    const float took = static_cast<float>(static_cast<double>(Qpc() - t0) * g_qpcMs);
    c_budgeted.Add();
    c_nodesBudgeted.Add(done);
    g_lastDone.store(done, std::memory_order_relaxed);
    g_lastLeft.store(left, std::memory_order_relaxed);
    g_lastMs.store(took, std::memory_order_relaxed);
    t_note.budgeted = true;
    t_note.left = left;
    if (left) {
        c_framesLeft.Add();
        c_nodesLeft.Add(left);
        if (left > g_maxLeft.load(std::memory_order_relaxed)) g_maxLeft.store(left, std::memory_order_relaxed);
        if (!w.since) w.since = now;
    } else {
        w.since = 0;
    }
}

// True when the pattern (hex bytes, "??" = any) matches at `addr`
bool MatchAt(uintptr_t addr, const char* pattern) {
    std::vector<int> want;
    for (const char* p = pattern; *p;) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (p[0] == '?') {
            want.push_back(-1);
            while (*p == '?') p++;
            continue;
        }
        want.push_back(static_cast<int>(std::strtoul(std::string(p, 2).c_str(), nullptr, 16)));
        p += 2;
    }
    std::vector<uint8_t> have(want.size());
    if (!MemPatch::ReadBytes(addr, have.data(), have.size())) return false;
    for (size_t i = 0; i < want.size(); i++)
        if (want[i] >= 0 && have[i] != static_cast<uint8_t>(want[i])) return false;
    return true;
}

uintptr_t CallTargetAt(uintptr_t call) {
    uint8_t b[5] = {};
    if (!MemPatch::ReadBytes(call, b, 5) || b[0] != 0xE8) return 0;
    int32_t rel;
    std::memcpy(&rel, b + 1, 4);
    return call + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
}

} // namespace

bool Start(std::string* error) {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (g_started) return true;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    // Suspended (2026-09-29): a node held past its frame can be freed by the game while still linked (the node destructor
    // 0x006FD930 does not unlink +0x18), and the next drain then calls through freed memory: the likely cause of a crash
    // with a garbage EIP ~90 s after it was switched on. Off until nodes are unlinked on destruction.
    if (kSuspended) return fail("Turned off in this version: it could crash the game (being reworked)");
    std::string missing;
    if (!GameAddr::GroupAvailable("SceneNodeBudget", &missing)) return fail(GameAddr::NotAvailable(missing));
    const uintptr_t drain = GameAddr::Get(GameAddr::Id::SceneDrain);
    const uintptr_t boundsCall = GameAddr::Get(GameAddr::Id::SceneBoundsCall), spatialCall = GameAddr::Get(GameAddr::Id::SceneSpatialCall);
    const uintptr_t bounds = GameAddr::Get(GameAddr::Id::SceneNodeBounds), spatial = GameAddr::Get(GameAddr::Id::SceneNodeSpatial);
    // The whole drain must be the loop this copy reproduces, and its two calls must reach the functions the copy calls
    if (!MatchAt(drain, kDrainBody)) return fail(std::format("The scene's pending-node drain at {:#010x} is not the code Apex was written for", drain));
    if (boundsCall != drain + 0xAE || spatialCall != drain + 0xB6 || CallTargetAt(boundsCall) != bounds || CallTargetAt(spatialCall) != spatial)
        return fail(std::format("The calls inside the pending-node drain at {:#010x} are not the expected ones", drain));
    if (g_qpcMs == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcMs = 1000.0 / static_cast<double>(f.QuadPart);
    }
    g_drainFn = drain;
    g_bounds = reinterpret_cast<FnBounds>(bounds);
    g_spatial = reinterpret_cast<FnSpatial>(spatial);
    LotLightingMotion::SampleCameraMoving(); // parses the camera position now (Start's thread), not in the first frame
    g_on.store(true, std::memory_order_release); // before the CALL can reach the hook
    std::string err;
    if (!CallChain::Install(Site::SceneDrain, Layer::SceneBudget, reinterpret_cast<void*>(&Hook_SceneDrain), &err)) {
        g_on.store(false);
        return fail("Could not hook the scene's pending-node drain: " + err);
    }
    g_started = true;
    LOG_INFO(std::format("[SceneBudget] On: Scene::BeginFrame's drain call {:#010x} -> {:#010x} goes through Apex (bounds {:#010x}, spatial {:#010x}); while the camera "
                         "moves: at most {} nodes / {:.1f} ms per frame, longest wait {} ms",
                         CallChain::CallAddress(Site::SceneDrain), drain, bounds, spatial, g_nodesPerFrame.load(), g_msPerFrame.load(), g_maxDeferMs.load()));
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (!g_started) return;
    g_on.store(false, std::memory_order_release); // the hook runs the game's drain from now on (everything waiting included)
    CallChain::Remove(Site::SceneDrain, Layer::SceneBudget);
    g_started = false;
    const Stats s = GetStats();
    LOG_INFO(std::format("[SceneBudget] Off ({} drains, {} with a budget, {} left nodes for later, largest backlog {}, {} forced full drains)", s.calls, s.budgeted,
                         s.framesLeft, s.maxLeft, s.fullForced));
}

bool Running() { return g_on.load(std::memory_order_acquire); }

void SetNodesPerFrame(int n) { g_nodesPerFrame.store(std::clamp(n, static_cast<int>(kMinNodes), 65536)); }
void SetMsPerFrame(float ms) { g_msPerFrame.store(std::clamp(ms, 0.1f, 50.0f)); }
void SetMaxDeferMs(int ms) { g_maxDeferMs.store(std::clamp(ms, 16, 10000)); }

DrainNote TakeDrainNote() {
    DrainNote n = t_note;
    t_note = DrainNote{};
    return n;
}

Stats GetStats() {
    Stats s;
    s.calls = c_calls.Get();
    s.fullStill = c_fullStill.Get();
    s.fullForced = c_fullForced.Get();
    s.budgeted = c_budgeted.Get();
    s.framesLeft = c_framesLeft.Get();
    s.nodesBudgeted = c_nodesBudgeted.Get();
    s.nodesLeft = c_nodesLeft.Get();
    s.maxLeft = g_maxLeft.load();
    s.lastDone = g_lastDone.load();
    s.lastLeft = g_lastLeft.load();
    s.lastMs = g_lastMs.load();
    return s;
}

std::string StatusText() {
    if (!Running()) return "Off";
    const Stats s = GetStats();
    if (!s.calls) return "On (no frames yet)";
    if (!s.budgeted) return "On: camera still, the game places new objects as usual";
    return std::format("On: {} of {} frames spread new objects while moving (largest wait {} objects)", s.framesLeft, s.budgeted, s.maxLeft);
}

void RenderDeveloperUI() {
    if constexpr (kPublicBuild) return;
    if (!ImGui::GetCurrentContext()) return;
    const Stats s = GetStats();
    ImGui::TextUnformatted(("Spread new objects over frames: " + StatusText()).c_str());
    ImGui::TextDisabled("Drain call %#010x -> %#010x; drains %llu: game's (camera still) %llu, game's (a node waited too long) %llu, with a budget %llu",
                        static_cast<unsigned>(CallChain::CallAddress(Site::SceneDrain)), static_cast<unsigned>(g_drainFn), static_cast<unsigned long long>(s.calls),
                        static_cast<unsigned long long>(s.fullStill), static_cast<unsigned long long>(s.fullForced), static_cast<unsigned long long>(s.budgeted));
    ImGui::TextDisabled("With a budget: %llu nodes processed, %llu frames left nodes (%llu node-frames waiting), largest backlog %u; last: %u done, %u left, %.2f ms",
                        static_cast<unsigned long long>(s.nodesBudgeted), static_cast<unsigned long long>(s.framesLeft), static_cast<unsigned long long>(s.nodesLeft), s.maxLeft,
                        s.lastDone, s.lastLeft, s.lastMs);
    int nodes = g_nodesPerFrame.load();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderInt("Nodes per frame while moving##SbNodes", &nodes, static_cast<int>(kMinNodes), 4096)) SetNodesPerFrame(nodes);
    float ms = g_msPerFrame.load();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderFloat("ms per frame while moving##SbMs", &ms, 0.1f, 10.0f, "%.1f ms")) SetMsPerFrame(ms);
    int wait = g_maxDeferMs.load();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderInt("Longest wait (ms)##SbWait", &wait, 16, 5000)) SetMaxDeferMs(wait);
}

} // namespace SceneBudget
