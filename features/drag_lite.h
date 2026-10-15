#pragma once
// Lamp drags: coalesced room solves (ported from 2.12.4's drag fixes onto 2.12.3 + Real-time outdoor lamps).
// While a lamp (or the object holding one) is dragged in Build/Buy mode, Apex's extra room sends (the fan-out to rooms behind
// openings, room 0's dependents, the lifted-lamp retries, the openings' door checks, the audits) wait and are sent once each
// when the lamp is still, with no urgency; the room priority call holds a room solved moments ago; after the stop the frame's
// room solves stay under a 3.5 ms spread cap while the edit's rooms still wait. Sends that must stop a solve (a lamp removed
// from a list) and the game's own list rebuilds are never held.
// The drag is seen from the lamp's own change stream: position steps (LampMarkFilter, LevelLightShare::NoteLampEditing),
// lamp moves seen by the lamp list (LotLightBridge), and a type-11 light removed and added again on the same lot within
// 0.5 m (the same lamp moving).
// Each measure has a switch in the developer Tester tab (tester_toggles.h), all on by default.
//
// Classic: relight only after release (14/10, Tester switch classicAfterRelease, on; Lamp lighting Classic, or Instant on a
// device that cannot run it): while a lamp is dragged no room is solved at all. Every room the drag touches (any urgency
// tier: the lamp's own room and its story's room 0, the neighbours, the rooms lit through openings, the safety net's) and
// every room of the lot being played gets priority 0 in the priority call (HoldUntilRelease), so the scheduler picks none;
// the game's light-list rebuilds and invalidations go on untouched (only the solves wait; holding or replaying those lists
// broke lamps before). The drag is over once the lamp has been still for kReleaseMs (or let go). Then the rooms are solved
// once: the lamp's own story first with the edit budget (Faster room lighting's 12-16 ms), then the rest under the
// kSpreadCapMs spread cap. The terrain's local relight (NightTerrainRelight::EditReady) and the per-pixel lamp list
// (LotLightBridge) wait for the release too; the lamp list is still read at the drag pace for the drag's own detection.
// One log line per drag: rooms held, solves after the release and the time until every room of the edit was solved.
// Instant (GPU) keeps the measures above unchanged.
#include <atomic>
#include <cstdint>
#include <string>

namespace DragLite {

// Why a room is being sent (QueueRoom reads it). Fanout .. Census are the extra sends left out while a lamp is dragged
enum class Why : int { None, Fanout, Retry, Dependents, Openings, Audit, DeferredBounce, Census, RelightLot, LampSwitch, QueueAgain, Walk, RelightAll, Correction };

constexpr unsigned long kStillMs = 750;    // the lamp still this long: the drag is over (the left-out sends go)
constexpr unsigned long kDragMs = 300;     // a step within this long: dragging
constexpr unsigned long kRepickMs = 400;   // while dragged: the same room is solved again at most this often
constexpr unsigned long kSolveGapMs = 120; // while dragged: at most one room solve begins this often
constexpr unsigned long kSettleMs = 15000; // the flush after a drag lasts at most this long
constexpr float kSpreadCapMs = 3.5f;       // the frame's room solves while the flush runs
constexpr unsigned long kReleaseMs = 400;  // Classic, relight only after release: the lamp still this long is let go
constexpr unsigned long kMaxHoldMs = 20000; // Classic: steps that go on longer than this without a pause no longer hold
                                            // the room solves (a lamp that keeps moving never starves the lot's rooms)

// A step of a lamp being dragged (any thread). source: for the log
void NoteLampStep(const char* source);
// Lamp lighting is Classic in effect (Classic chosen, or Instant unable to run); Night Lighting sets it every frame
void SetClassicLighting(bool on);
bool ClassicLighting();
// Classic with its Tester switch "Classic: relight only after release" on (any thread)
bool ClassicReleaseOn();
// ClassicReleaseOn and a lamp is being dragged now (a step within kReleaseMs): no room solve begins (any thread)
bool ClassicHolding();
// The priority call while ClassicHolding: true = this room waits for the release (an edit's room of any tier, or any room
// of the lot being played: onPlayedLot); the rooms held are counted for the drag's log line
bool HoldUntilRelease(const void* room, bool onPlayedLot);
// The latest step of this drag stream is within ms (any thread; LevelLightShare::LampDragging reads it)
bool StepWithin(unsigned long ms);
// A lamp step within kDragMs (any source, also LevelLightShare::LampDragging)
bool Dragging();
// A lamp step within kStillMs (the drag and its settle time; kReleaseMs while ClassicReleaseOn)
bool Engaged();
// The flush after a drag (any thread): from the drag's end while a lamp edit's rooms still wait, at most kSettleMs; the
// frame's room solves stay under kSpreadCapMs meanwhile (LotLightingMotion, RoomLightQueue's drain), with no edit boosts
bool Settling();
// The frame's room solve cap now (ms; 0 = none): kSpreadCapMs while Settling and its Tester switch is on. With
// ClassicReleaseOn, none while the lamp's own story still waits for its solve (the edit budget takes it first)
float SolveCapMs();

// The re-pick throttle (RoomLightQueue's priority hook; LevelLightShare's solve start hook notes the starts): while a lamp
// is dragged a room that takes its light from elsewhere (urgency tier 1 or 2) waits for the stop, a room whose solve began
// less than kRepickMs ago, or any room within kSolveGapMs of the last solve start, is not picked now (priority 0: it keeps
// its state, its invalidations and its gathers, and is picked once the gap or the drag ends)
void NoteSolveStarted(const void* room);
bool HoldRepick(const void* room);

// Why a room is being sent (QueueRoom reads it)
extern thread_local Why t_why;
struct WhyScope {
    explicit WhyScope(Why w) : prev(t_why) { t_why = w; }
    ~WhyScope() { t_why = prev; }
    WhyScope(const WhyScope&) = delete;
    WhyScope& operator=(const WhyScope&) = delete;
    Why prev;
};
// QueueRoom (any thread): true = left out now and sent once the lamp is still (an extra send during a drag)
bool DeferSend(uintptr_t tracker, int story, int id);

// Render thread, every frame (LevelLightShare::OnPresent): the left-out sends once the lamp is still (urgent, tier 2, while
// "Classic: show all floors together" holds the edit, so they change with the other stories), the frame timers
void OnPresent();
std::string StatusText();
// Tester tab readout (render thread values, read anywhere): the last frame's time, and the average and worst frame of the
// last finished drag (0 when none yet)
float FrameMs();
float LastDragAvgMs();
float LastDragMaxMs();
int LastDragFrames();

} // namespace DragLite
