// World lamp lifecycle: literal production functions; engine reads are data fixtures.
// No game process, native light rebuild, file writes or GPU calls.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "features/lot_light_bridge.h"
#include "features/world_lamp_policy.h"
using BYTE = unsigned char;
using DWORD = uint32_t;
using Clock = std::chrono::steady_clock;
/* PRODUCTION_STRUCTS */
using LampSig = std::pair<uintptr_t, LotLampState>;
constexpr uint32_t kLampMemoSize = 512;
constexpr float kMoveTol = .05f, kLightRel = .05f, kLightAbs = .05f;
constexpr int kAnimatedChanges = 3;
constexpr auto kAnimatedWindow = std::chrono::seconds(60), kAnimatedExpire = std::chrono::seconds(120), kQuietLogEvery = std::chrono::seconds(60);
std::vector<std::array<float, 8>> g_allLamps;
float g_lampData[33][4]{};
int g_lampCount = 0, g_lastLampCandidates = 0, g_lampFrame = 0;
uint32_t g_lampMemoGen = 1, g_lampMemoHits = 0, g_lampMemoMisses = 0;
LampMemo g_lampMemo[kLampMemoSize];
std::vector<uintptr_t> g_enumLights;
std::vector<LampSig> g_lotLampCur, g_lotLampSig;
std::vector<uint64_t> g_lotsNow, g_lastUserLots;
std::map<uint64_t, LotSeen> g_lotSeen;
std::map<uint64_t, Clock::time_point> g_quietLogAt;
LotLightBridge::BakeSnapshot g_bakeSnap;
std::unordered_map<uintptr_t, int> g_lampSwitchPrev;
std::vector<int> g_lotRects;
std::unordered_map<uint64_t, DWORD> g_lotDrawSeen;
std::unordered_set<uint64_t> g_lotArrivals;
std::atomic<int> g_lotLampEdits{0}, g_lotLampUserEdits{0};
int g_lotChangesCounted = 0, g_lotChangesIgnored = 0, g_lampChangesOutside = 0, g_lampChangesNoise = 0, g_lampChangesAnimated = 0, g_lampsAnimated = 0, g_lampEnumerations = 0;
std::string g_lastLotChange = "none";
bool g_lampRefreshNow = false, g_lampEditRefresh = false, g_lotRectMiss = false, g_haveLastEdgeRect = false;
DWORD g_lampReadTick = 0, g_lotDrawTick = 0;
std::atomic<bool> g_roofFix{true}, g_waterFix{false}, g_objPixelLamps{false};
int clearChunks = 0, clearPadding = 0, enumerationCalls = 0;
bool enumerationAvailable = false;
std::unordered_map<uintptr_t, std::array<float, 8>> enginePool;
std::unordered_map<uintptr_t, LotLampState> engineStates;
int assertions = 0, failures = 0;
void ClearChunks() { clearChunks++; }
namespace RoomMapPadding { void Clear() { clearPadding++; } }
namespace Recorder { bool Verbose() { return false; } }
namespace ChunkRelight { bool Editing() { return false; } }
namespace FrameProfiler { enum class ModTime { LampRefresh }; struct ModTimeScope { explicit ModTimeScope(ModTime) {} }; }
void TestLog(const std::string&) {}
#define LOG_INFO(...) TestLog(__VA_ARGS__)
#define LOG_DEBUG(...) TestLog(__VA_ARGS__)
bool VisibleLot(uint64_t lot) { return g_lotDrawSeen.count(lot) != 0; }
void TrackLampSwitches() {}
bool ReadLamp(uintptr_t L, float* out) {
    const auto it = enginePool.find(L); if (it == enginePool.end()) return false;
    std::memcpy(out, it->second.data(), sizeof(float) * 8); return true;
}
bool ReadLotLamp(uintptr_t L, LotLampState& out) {
    const auto it = engineStates.find(L); if (it == engineStates.end()) return false;
    out = it->second; return true;
}
bool EnumerateLights() {
    enumerationCalls++;
    if (!enumerationAvailable) return false; // Matches production's unavailable gate: previous vector stays untouched.
    g_enumLights.clear();
    for (const auto& [L, s] : engineStates) { (void)s; g_enumLights.push_back(L); }
    return true;
}
/* PRODUCTION_HELPERS */
int SelectLampsScan(float x, float z, float maxScore);
/* PRODUCTION_FUNCTIONS */
/* PRODUCTION_REFRESH */
/* BASELINE_FUNCTION */
void Assert(bool ok, const char* name) {
    assertions++; if (!ok) { failures++; std::printf("FAIL: %s\n", name); }
}
bool ZeroRows(int n = 33) {
    for (int i = 0; i < n; ++i) for (float v : g_lampData[i]) if (v != 0) return false;
    return true;
}
bool InvalidMemos() { for (const auto& m : g_lampMemo) if (m.gen != 0) return false; return true; }
LotLampState EngineLamp(uint64_t lot, int type, float x, float red = 1) {
    LotLampState s; s.lot = lot; s.type = type; s.flags = 0x65; s.inten = 1; s.range = 10;
    s.col[0] = red; s.col[1] = .5f; s.col[2] = .25f; s.pos[0] = x;
    s.rect[0] = x - 10; s.rect[1] = -10; s.rect[2] = x + 10; s.rect[3] = 10;
    return s;
}
void SeedOldWorld(uint32_t generation = 1) {
    enginePool.clear(); engineStates.clear(); enumerationAvailable = true;
    engineStates[0x100] = EngineLamp(7, 3, 0);
    enginePool[0x100] = {0, 0, 0, 5, 1, .5f, .25f, 0};
    g_enumLights = {0x100}; g_lampMemoGen = generation;
    ReadEnumeratedLamps(true); TrackLotLampEdits();
    Assert(SelectLamps(0, 0, 80) == 1 && !ZeroRows(32), "seed old world has direct lamp rows");
    g_lampData[32][0] = .7f;
    g_lampSwitchPrev[0x100] = 1; g_lastUserLots = {7};
    g_lotRects = {1}; g_lotDrawSeen[7] = 50; g_lotArrivals.insert(7);
    g_lampEditRefresh = true; g_haveLastEdgeRect = true; g_lampRefreshNow = false; g_lampFrame = 3;
    g_lampReadTick = 1234; g_quietLogAt[7] = Clock::now();
    g_lampsAnimated = 9;
    enumerationAvailable = false;
}
void SetNewEngineWorld() {
    enginePool.clear(); engineStates.clear();
    engineStates[0x200] = EngineLamp(9, 3, 3, .25f);
    engineStates[0x201] = EngineLamp(0, 11, 7, .5f);
    enginePool[0x200] = {3, 0, 0, 6, .25f, .5f, .25f, 0};
    enginePool[0x201] = {7, 0, 0, 6, .5f, .5f, .25f, 0};
    enumerationAvailable = true;
}
int main() {
    g_lotLampEdits = 11; g_lotLampUserEdits = 5;
#ifdef HAVE_BASELINE
    SeedOldWorld();
    // Recreate a valid entry tagged 1, precisely the value used by the new world's reset.
    g_lampMemoGen = 1; Assert(SelectLamps(0, 0, 80) == 1, "baseline seed generation one");
    OldOnWorldChanged();
    Assert(g_allLamps.size() == 1 && SelectLamps(0, 0, 80) == 1, "baseline retained old world direct pool and memo");
    Assert(!g_lampRefreshNow && g_lampFrame == 3, "baseline did not request first frame lamp read");
#endif
    SeedOldWorld();
    g_lampMemoGen = 1; SelectLamps(0, 0, 80);
    const auto hitsBefore = g_lampMemoHits, missesBefore = g_lampMemoMisses;
    const int enumBefore = enumerationCalls, countBefore = g_lotLampEdits.load(), userBefore = g_lotLampUserEdits.load();
    const int clearsBefore = clearChunks;
    OnWorldChanged();
    Assert(clearChunks == clearsBefore + 1 && clearPadding == clearChunks, "world reset still clears terrain and room maps once");
    Assert(g_allLamps.empty() && g_lampCount == 0 && g_lastLampCandidates == 0 && ZeroRows(), "world reset clears direct pool, count, candidates and all thirty three constant rows");
    Assert(g_lampsAnimated == 0, "new-world status excludes previous world's animated lamps");
    Assert(g_lampMemoGen == 1 && InvalidMemos(), "all memo slots invalid even when old generation was one");
    Assert(g_enumLights.empty() && g_lotLampCur.empty() && g_lotsNow.empty() && g_lampSwitchPrev.empty(), "old world enumeration and tracking scratch discarded");
    Assert(g_lotLampSig.empty() && g_lotSeen.empty() && g_quietLogAt.empty() && g_lastUserLots.empty(), "old signatures, settle state and old priority requests discarded");
    Assert(g_bakeSnap.lamps.empty() && g_bakeSnap.lots.empty() && g_bakeSnap.settledLots.empty(), "old terrain bake snapshot discarded");
    Assert(g_lotRects.empty() && g_lotDrawSeen.empty() && g_lotArrivals.empty() && !g_haveLastEdgeRect && g_lotRectMiss, "visibility and lot edges are world scoped");
    Assert(g_lampFrame == 0 && g_lampRefreshNow && !g_lampEditRefresh && g_lampReadTick == 1234, "next read forced without changing cumulative time marker");
    Assert(SelectLamps(0, 0, 80) == 0 && ZeroRows(), "same coordinates cannot use old valid memo rows");
    Assert(g_lampMemoHits == hitsBefore && g_lampMemoMisses == missesBefore + 1, "same coordinates recomputed after real memo invalidation");
    RefreshPresentFixture();
    Assert(enumerationCalls == enumBefore + 1 && g_lampFrame == 0 && !g_lampRefreshNow, "first present attempts read immediately despite twenty frame cadence");
    Assert(g_allLamps.empty() && g_enumLights.empty() && SelectLamps(0, 0, 80) == 0 && ZeroRows(), "failed first enumeration leaves new world pool blank");
    Assert(g_lotLampEdits == countBefore && g_lotLampUserEdits == userBefore, "failed first read does not synthesize old lamp removals or edits");
    for (int i = 0; i < 19; ++i) RefreshPresentFixture();
    Assert(enumerationCalls == enumBefore + 1, "ordinary nineteen frames keep cadence");
    RefreshPresentFixture();
    Assert(enumerationCalls == enumBefore + 2 && g_allLamps.empty() && SelectLamps(0, 0, 80) == 0, "late failed enumeration stays blank rather than restoring old rows");
    SetNewEngineWorld(); g_lampRefreshNow = true; RefreshPresentFixture();
    Assert(SelectLamps(0, 0, 80) == 2 && g_lampCount == 2 && g_lampData[0][0] == 3 && g_lampData[1][0] == 7, "successful new world reads only new lamp positions and colors");
    Assert(g_bakeSnap.lamps.size() == 2 && g_lotLampSig.size() == 2 && g_lotLampEdits == countBefore && g_lotLampUserEdits == userBefore, "first lot and world lamp registration creates baseline without edit event");
    for (int i = 0; i < 512; ++i) {
        OnWorldChanged();
        Assert(InvalidMemos() && ZeroRows() && g_allLamps.empty(), "repeated world entry clears render rows and memo");
        RefreshPresentFixture();
        Assert(g_lotLampEdits == countBefore && g_lotLampUserEdits == userBefore && g_lastUserLots.empty(), "repeated world entry never counts registrations as edits");
        Assert(SelectLamps(0, 0, 80) == 2, "new world remains correctly selectable after repeated reset");
    }
    // Simulate wrap before reset. Invalidating every slot makes the reset independent of the generation value.
    for (auto& memo : g_lampMemo) { memo.gen = 1; memo.picked = 16; memo.candidates = 64; }
    g_lampMemoGen = UINT32_MAX; OnWorldChanged();
    Assert(InvalidMemos() && SelectLamps(0, 0, 80) == 0 && ZeroRows(), "reset is independent of prior generation wrap and stale tags");
    std::printf("World lamp lifecycle fixtures: %d scenario assertions, %d failures; native reads and resource clears are stub callbacks.\n", assertions, failures);
    return failures ? 1 : 0;
}
