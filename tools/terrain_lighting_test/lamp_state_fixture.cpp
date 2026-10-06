// Extracted terrain-edit state functions. No game process, file writes or GPU calls.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <format>
#include <string>
#include <vector>
using Clock = std::chrono::steady_clock;
enum class EditWait { None };
constexpr auto kEditQuiet = std::chrono::milliseconds(250);
bool g_editLocalRefused = false, g_editKickPending = false, g_editUser = false, g_editForce = false;
bool g_worldRigRefreshPending = false;
Clock::time_point g_editFirstAt{}, g_editLastAt{};
EditWait g_editWait = EditWait::None;
std::vector<uint64_t> g_editUserLots;
std::string g_editReason, g_lastEditOutcome;
int refreshes = 0, assertions = 0, failures = 0;
namespace ObjectLightBridge { void RequestRigRefresh() { refreshes++; } }
namespace Recorder { bool Verbose() { return true; } }
#define LOG_INFO(message) ((void)(message))
/* PRODUCTION_FUNCTIONS */
/* BASELINE_FUNCTION */
void Assert(bool ok, const char* name) {
    assertions++; if (!ok) { failures++; std::printf("FAIL: %s\n", name); }
}
void Reset() {
    g_editLocalRefused = g_editKickPending = g_editUser = g_editForce = g_worldRigRefreshPending = false;
    g_editUserLots.clear(); refreshes = 0; g_editReason.clear(); g_lastEditOutcome.clear();
}
void ObservedWorldEdit(Clock::time_point now) {
    NoteEdit(now, true, false, "observed world lamp");
    g_editUserLots.push_back(0); g_worldRigRefreshPending = true;
}
int main() {
    const auto now = Clock::time_point{} + std::chrono::seconds(1);
    Reset(); ObservedWorldEdit(now);
    Assert(!EditReady(now + std::chrono::milliseconds(20), g_editFirstAt, g_editLastAt, true), "rebuild precedes priority debounce");
    FinishEdit("game rebuild covered the terrain edit");
    Assert(refreshes == 1 && !g_editKickPending && !g_worldRigRefreshPending, "early terrain completion flushes independent rig request");
    RefreshWorldRigs("after debounce"); FinishEdit("again");
    Assert(refreshes == 1, "covered edit is never flushed twice");
    Reset(); ObservedWorldEdit(now);
    Assert(EditReady(now + std::chrono::milliseconds(80), g_editFirstAt, g_editLastAt, true), "normal priority debounce still accepts at eighty milliseconds");
    RefreshWorldRigs("after debounce"); FinishEdit("local terrain edit accepted");
    Assert(refreshes == 1 && !g_worldRigRefreshPending, "normal debounce and finalization share one request");
    Reset();
    for (int i = 0; i < 30; i++) ObservedWorldEdit(now + std::chrono::milliseconds(i * 10));
    Assert(g_editFirstAt == now && g_editLastAt == now + std::chrono::milliseconds(290), "successive world edits preserve first and latest debounce times");
    Assert(EditReady(now + std::chrono::milliseconds(500), g_editFirstAt, g_editLastAt, true), "continuous edit reaches bounded priority readiness");
    FinishEdit("coalesced edit covered");
    Assert(refreshes == 1, "thirty coalesced edits dirty rigs once");
    ObservedWorldEdit(now + std::chrono::seconds(1)); FinishEdit("next separate edit");
    Assert(refreshes == 2, "later distinct edit has its own refresh");
    Reset(); NoteEdit(now, true, false, "ordinary lot edit"); FinishEdit("ordinary local edit");
    Assert(refreshes == 0, "ordinary edits do not force a world rig refresh");
    Reset(); NoteEdit(now, false, false, "automatic"); FinishEdit("day defer");
    Assert(refreshes == 0, "automatic deferred edits do not force a world rig refresh");
    Reset(); ObservedWorldEdit(now);
    // Mirrors the verified production world-reset assignments; no old-world completion is consumed.
    g_editKickPending = false; g_worldRigRefreshPending = false;
    RefreshWorldRigs("after new world"); FinishEdit("new world idle");
    Assert(refreshes == 0, "world reset discards old-world rig work");
#ifdef HAVE_BASELINE
    Reset(); ObservedWorldEdit(now); OldFinishEdit("game rebuilt before debounce");
    Assert(!g_editKickPending && g_worldRigRefreshPending && refreshes == 0, "unchanged baseline reproduces lost independent refresh");
    NoteEdit(now + std::chrono::seconds(1), true, false, "later ordinary edit");
    Assert(!g_worldRigRefreshPending && refreshes == 0, "baseline next edit discarded the stranded request");
#endif
    std::printf("Lamp edit state fixtures: %d scenario assertions, %d failures; native rig dirtying is a counted callback fixture.\n", assertions, failures);
    return failures ? 1 : 0;
}
