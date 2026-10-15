// Lamp drags: coalesced room solves. See drag_lite.h.
#include "drag_lite.h"
#include "tester_toggles.h"
#include "level_light_share.h"
#include "apex_log.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <format>
#include <iterator>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_set>

namespace DragLite {

thread_local Why t_why = Why::None;

namespace {
std::atomic<DWORD> g_lastStep{0}; // GetTickCount | 1 of the latest lamp step
std::atomic<DWORD> g_runFrom{0};  // GetTickCount | 1 of the first step of the current run (steps less than kReleaseMs apart)
std::atomic<bool> g_classic{true}; // Lamp lighting is Classic in effect (SetClassicLighting)
// Classic, relight only after release: the rooms held during the current drag (g_solveMx), the drag's log line after it
std::unordered_set<const void*> g_heldRooms;
bool g_classicDrag = false;               // render thread: this drag ran with the classic hold
std::atomic<bool> g_ownFirst{false};      // render thread writes: after the release, the lamp's own story still waits
std::atomic<bool> g_tracking{false};      // after a classic drag's release until its rooms are solved
std::atomic<long> g_trackSolves{0};       // room solves begun since that release
DWORD g_trackFrom = 0;                    // render thread: the release (GetTickCount)
size_t g_trackHeld = 0;                   // render thread: rooms held during that drag
std::string g_trackDrag;                  // render thread: that drag's own summary (frames, sends), joined to its one log line
std::mutex g_mx;
std::set<std::tuple<uintptr_t, int, int>> g_rooms; // (tracker, story, id): a send left out during the drag
std::atomic<long> g_deferredSends{0}, g_repicksHeld{0}, g_dragSolves{0};
long g_drags = 0, g_totalResent = 0, g_totalHeld = 0; // render thread (the totals of every drag so far)
bool g_wasEngaged = false;
std::atomic<DWORD> g_settleFrom{0}; // GetTickCount | 1 when the last drag ended
std::string g_last = "no drag yet";

// The room solves begun lately (render / light tree thread, under g_solveMx): the re-pick throttle
struct Started {
    const void* room = nullptr;
    DWORD at = 0;
};
std::mutex g_solveMx;
Started g_started[64];
int g_startedPos = 0;
std::atomic<DWORD> g_lastSolveAt{0};

// Frame timers for the Tester tab (render thread writes, float bits)
LARGE_INTEGER g_prevFrame{}, g_freq{};
std::atomic<uint32_t> g_frameMs{0}, g_dragAvgMs{0}, g_dragMaxMs{0};
std::atomic<int> g_dragFrames{0};
double g_runSum = 0.0, g_runMax = 0.0;
int g_runFrames = 0;
} // namespace

void NoteLampStep(const char*) {
    const DWORD now = GetTickCount() | 1;
    const DWORD last = g_lastStep.exchange(now, std::memory_order_relaxed);
    if (!last || now - last >= kReleaseMs) g_runFrom.store(now, std::memory_order_relaxed);
}
bool StepWithin(unsigned long ms) {
    const DWORD at = g_lastStep.load(std::memory_order_relaxed);
    return at && GetTickCount() - at < ms;
}

void SetClassicLighting(bool on) { g_classic.store(on, std::memory_order_relaxed); }
bool ClassicLighting() { return g_classic.load(std::memory_order_relaxed); }
bool ClassicReleaseOn() { return g_classic.load(std::memory_order_relaxed) && TesterToggles::classicAfterRelease.load(std::memory_order_relaxed); }
bool ClassicHolding() {
    if (!ClassicReleaseOn() || !(StepWithin(kReleaseMs) || LevelLightShare::LampDragging())) return false;
    // (a run of steps longer than kMaxHoldMs: the rooms are solved as the lamp moves, as without the hold)
    const DWORD from = g_runFrom.load(std::memory_order_relaxed);
    return !(from && StepWithin(kReleaseMs) && GetTickCount() - from > kMaxHoldMs);
}
bool HoldUntilRelease(const void* room, bool onPlayedLot) {
    if (!room || !ClassicHolding()) return false;
    if (!onPlayedLot && LevelLightShare::UrgentTierOf(room) < 0) return false; // another lot's own work goes on
    std::lock_guard<std::mutex> lk(g_solveMx);
    if (g_heldRooms.size() < 4096) g_heldRooms.insert(room);
    return true;
}

bool Dragging() { return StepWithin(kDragMs) || LevelLightShare::LampDragging(); }
bool Engaged() { return StepWithin(ClassicReleaseOn() ? kReleaseMs : kStillMs) || LevelLightShare::LampDragging(); }
bool Settling() {
    const DWORD from = g_settleFrom.load(std::memory_order_relaxed);
    return from && GetTickCount() - from < kSettleMs && !Engaged() && LevelLightShare::LampEditPending();
}
float SolveCapMs() {
    if (!TesterToggles::dragSpreadCap.load(std::memory_order_relaxed) || !Settling()) return 0.0f;
    // Classic, relight only after release: the lamp's own story takes the edit budget first, the others the spread cap
    return ClassicReleaseOn() && g_ownFirst.load(std::memory_order_relaxed) ? 0.0f : kSpreadCapMs;
}

void NoteSolveStarted(const void* room) {
    const DWORD now = GetTickCount() | 1;
    g_lastSolveAt.store(now, std::memory_order_relaxed);
    if (Engaged()) g_dragSolves.fetch_add(1, std::memory_order_relaxed);
    else if (g_tracking.load(std::memory_order_relaxed)) g_trackSolves.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_solveMx);
    for (Started& s : g_started)
        if (s.room == room) {
            s.at = now;
            return;
        }
    g_started[g_startedPos] = Started{room, now};
    g_startedPos = (g_startedPos + 1) % static_cast<int>(std::size(g_started));
}

bool HoldRepick(const void* room) {
    if (!room || !TesterToggles::dragRepickThrottle.load(std::memory_order_relaxed) || !Engaged()) return false;
    const DWORD now = GetTickCount();
    // the rooms that take the dragged lamp's light from elsewhere (urgency tier 1: through an opening, another story; tier 2:
    // the edit's safety net) keep their light until the lamp is still, as the game's own outdoor light settles after a move;
    // the lamp's own room and story (tier 0) and any other room follow it at most every kRepickMs, and at most one room solve
    // begins every kSolveGapMs (the lamp's own room is picked first: its priority is the highest)
    bool hold = LevelLightShare::UrgentTierOf(room) >= 1;
    const DWORD last = g_lastSolveAt.load(std::memory_order_relaxed);
    hold = hold || (last && now - last < kSolveGapMs);
    if (!hold) {
        std::lock_guard<std::mutex> lk(g_solveMx);
        for (const Started& s : g_started)
            if (s.room == room) {
                hold = now - s.at < kRepickMs;
                break;
            }
    }
    if (hold) g_repicksHeld.fetch_add(1, std::memory_order_relaxed);
    return hold;
}

bool DeferSend(uintptr_t tracker, int story, int id) {
    const Why why = t_why;
    if (why < Why::Fanout || why > Why::Census || !TesterToggles::dragDeferSends.load(std::memory_order_relaxed) || !Engaged()) return false;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_rooms.size() >= 4096) return false; // (full: sent now, as without the coalescing)
        g_rooms.emplace(tracker, story, id);
    }
    g_deferredSends.fetch_add(1, std::memory_order_relaxed);
    return true;
}

void OnPresent() {
    // frame timers (the Tester tab's readout)
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
        if (g_prevFrame.QuadPart && g_freq.QuadPart) {
            const double ms = static_cast<double>(now.QuadPart - g_prevFrame.QuadPart) * 1000.0 / static_cast<double>(g_freq.QuadPart);
            g_frameMs.store(std::bit_cast<uint32_t>(static_cast<float>(ms)), std::memory_order_relaxed);
            if (Dragging() && ms < 2000.0) {
                g_runSum += ms, g_runMax = std::max(g_runMax, ms), g_runFrames++;
            }
        }
        g_prevFrame = now;
    }
    if (Engaged()) {
        g_wasEngaged = true;
        if (ClassicHolding()) g_classicDrag = true;
        return;
    }
    // Classic, relight only after release: the lamp's own story first (SolveCapMs), and the drag's log line once every room
    // of the edit is solved (or kSettleMs after the release)
    if (g_tracking.load(std::memory_order_relaxed)) {
        int ownPending = 0;
        const bool editPending = LevelLightShare::LampEditPending();
        if (editPending) LevelLightShare::EditRoomsPending(0, nullptr, &ownPending);
        g_ownFirst.store(editPending && ownPending > 0, std::memory_order_relaxed);
        const DWORD ms = GetTickCount() - g_trackFrom;
        if (!editPending || ms >= kSettleMs) {
            g_tracking.store(false, std::memory_order_relaxed);
            g_ownFirst.store(false, std::memory_order_relaxed);
            LOG_INFO(std::format("[DragLite] Classic drag: {} rooms deferred during the drag, {} solves at the stop, {} {} ms after the release",
                                 g_trackHeld, g_trackSolves.load(std::memory_order_relaxed),
                                 editPending ? "rooms of the edit still waiting (timeout)" : "settled", ms) + g_trackDrag);
        }
    }
    // still: every send left out goes once, with no urgency (the spread cap holds the frame's solves meanwhile)
    std::set<std::tuple<uintptr_t, int, int>> due;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        due.swap(g_rooms);
    }
    if (!g_wasEngaged && due.empty()) return;
    if (g_wasEngaged) {
        g_settleFrom.store(GetTickCount() | 1, std::memory_order_relaxed);
        if (g_runFrames > 0) {
            g_dragAvgMs.store(std::bit_cast<uint32_t>(static_cast<float>(g_runSum / g_runFrames)), std::memory_order_relaxed);
            g_dragMaxMs.store(std::bit_cast<uint32_t>(static_cast<float>(g_runMax)), std::memory_order_relaxed);
            g_dragFrames.store(g_runFrames, std::memory_order_relaxed);
        }
        g_runSum = g_runMax = 0.0, g_runFrames = 0;
        if (g_classicDrag) { // the release of a drag held in Classic: its rooms are solved from now on
            {
                std::lock_guard<std::mutex> lk(g_solveMx);
                g_trackHeld = g_heldRooms.size();
                g_heldRooms.clear();
            }
            g_trackFrom = GetTickCount();
            g_trackSolves.store(0, std::memory_order_relaxed);
            g_ownFirst.store(true, std::memory_order_relaxed); // (the next frame measures it)
            g_tracking.store(true, std::memory_order_relaxed);
        }
    }
    const bool classicRelease = g_classicDrag && g_wasEngaged;
    g_classicDrag = false;
    g_wasEngaged = false;
    int sent = 0;
    const WhyScope why(Why::Correction);
    // "Classic: show all floors together" holds the edit's stories until every one of its rooms is solved: the sends left
    // out are then the edit's too (tier 2, like its safety net), so they change in the same frame as the others
    const int tier = ClassicLighting() && TesterToggles::classicTogether.load(std::memory_order_relaxed) ? 2 : -1;
    for (const auto& [tracker, story, id] : due) sent += LevelLightShare::QueueRoomId(tracker, story, id, tier) ? 1 : 0;
    const long sends = g_deferredSends.exchange(0), held = g_repicksHeld.exchange(0), dragSolves = g_dragSolves.exchange(0);
    g_totalResent += sent, g_totalHeld += held, g_drags++;
    g_last = std::format("Lamp drag ended: {} room solves during the drag ({} re-picks held), {} extra sends left out, {} rooms sent at the stop; "
                         "frames during the drag: {:.2f} ms on average, {:.1f} ms the worst ({} frames)",
                         dragSolves, held, sends, sent, LastDragAvgMs(), LastDragMaxMs(), LastDragFrames());
    if (classicRelease) g_trackDrag = std::format("; {} extra sends left out, {} sent at the stop; frames during the drag: {:.2f} ms on average, {:.1f} ms the worst ({} frames)",
                                                 sends, sent, LastDragAvgMs(), LastDragMaxMs(), LastDragFrames());
    else LOG_INFO("[DragLite] " + g_last); // (a classic drag logs once, when its rooms are solved)
}

std::string StatusText() {
    return std::format("{}; last: {} | totals: {} drags, {} rooms sent at the stops, {} re-picks held", Engaged() ? "DRAGGING" : "idle", g_last, g_drags, g_totalResent,
                       g_totalHeld);
}

float FrameMs() { return std::bit_cast<float>(g_frameMs.load(std::memory_order_relaxed)); }
float LastDragAvgMs() { return std::bit_cast<float>(g_dragAvgMs.load(std::memory_order_relaxed)); }
float LastDragMaxMs() { return std::bit_cast<float>(g_dragMaxMs.load(std::memory_order_relaxed)); }
int LastDragFrames() { return g_dragFrames.load(std::memory_order_relaxed); }

} // namespace DragLite
