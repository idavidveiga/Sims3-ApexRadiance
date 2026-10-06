// Read-only fixtures for extracted production queue functions. This executable
// prints only stdout. It does not attach to TS3 or access native game memory.
#include "features/terrain_chunk_relight.h"
#include "features/terrain_lighting_policy.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <format>
#include <map>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>
using Clock = std::chrono::steady_clock;
using ChunkRelight::Done;
using ChunkRelight::FrameResult;
/* PRODUCTION_LIMITS */
/* PRODUCTION_TYPES */
std::deque<Entry> g_queue;
Entry g_flight;
bool g_haveFlight = false, g_flightSetByUs = false, g_offThisWorld = false;
uint32_t g_flightFrames = 0, g_flightIdle = 0, g_frame = 0, g_lastDoneFrame = 0;
std::vector<Batch> g_batches;
uintptr_t g_queueTerrain = 0, g_queueBegin = 0;
std::atomic<uintptr_t> g_flightChunk{0};
std::atomic<bool> g_flightDone{false};
std::atomic<int64_t> g_flightTicks{0};
std::deque<Clock::time_point> g_releases;
int g_nextId = 1, g_timeoutsThisWorld = 0;
int g_statSweeps = 0, g_statChunks = 0, g_statOtherPath = 0, g_statRefused = 0, g_statFailed = 0;
int g_waitRebuildFlags = 0, g_waitGates = 0, g_waitRate = 0, g_timedChunks = 0;
double g_lastMs = -1, g_sumMs = 0, g_maxMs = 0;
std::string g_lastRefusal, g_lastDone, g_offWhy;
View fixtureView;
std::vector<uint32_t> fixtureSlots;
std::vector<ChunkRaw> fixtureChunks;
bool fixtureReady = true, fixtureRead = true, fixtureLayout = true;
int fixtureGate = 0, fixtureBadSlot = -1, fixtureBadMap = -1, fixtureWrites = 0, fixtureNotices = 0;
int cases = 0, failures = 0, handoffs = 0;
void Assert(bool ok, const char* name) {
    cases++;
    if (!ok) { failures++; if (failures < 20) std::printf("FAIL: %s\n", name); }
}
#define LOG_WARNING(...) ((void)0)
namespace LightmapSmooth { void NoteChunkRendered(int, int) { fixtureNotices++; } }
double TicksToMs(int64_t ticks) { return static_cast<double>(ticks) / 1000.0; }
bool Check(View& v, std::string& why) {
    if (!fixtureReady || g_offThisWorld) { why = "fixture render gate closed"; return false; }
    v = fixtureView; return true;
}
bool ReadView(View& v, std::string& why) {
    if (!fixtureRead) { why = "fixture read failed"; return false; }
    v = fixtureView; return true;
}
uintptr_t ReadPtr(uintptr_t at) {
    return *reinterpret_cast<const uint32_t*>(at);
}
bool ReadChunkRaw(uintptr_t chunk, ChunkRaw& c) {
    if (!chunk || chunk > fixtureChunks.size()) return false;
    c = fixtureChunks[chunk - 1]; return true;
}
bool LayoutOk(const View&, uint32_t slot, const ChunkRaw&) {
    return fixtureLayout && static_cast<int>(slot) != fixtureBadSlot;
}
int GatesRaw(const View&) { return fixtureGate; }
void ScanFlagsRaw(uintptr_t, uint32_t n, uintptr_t except, Pending& p) {
    p = Pending{};
    for (uint32_t slot = 0; slot < n; slot++) {
        const auto& c = fixtureChunks[slot];
        p.f55 += c.f55 != 0; p.f56 += c.f56 != 0; p.f50 += c.f50 != 0;
        p.other54 += c.f54 && fixtureSlots[slot] != except;
    }
}
int WriteFlag54(uintptr_t chunk, uint8_t value) {
    if (!chunk || chunk > fixtureChunks.size()) return -1;
    auto& f = fixtureChunks[chunk - 1].f54;
    int previous = f; f = value; fixtureWrites++; return previous;
}
/* PRODUCTION_FUNCTIONS */
Entry E(uintptr_t chunk) { Entry e; e.chunk = chunk; e.slot = static_cast<uint32_t>(chunk - 1); return e; }
void Reset(uint32_t side = 8) {
    ClearQueue(); fixtureChunks.assign(side * side, ChunkRaw{}); fixtureSlots.resize(side * side);
    for (uint32_t i = 0; i < side * side; i++) {
        fixtureSlots[i] = i + 1;
        fixtureChunks[i].cx = (i % side) * 256 + 128;
        fixtureChunks[i].cz = (i / side) * 256 + 128;
        fixtureChunks[i].lightMap = i + 1000;
    }
    fixtureView = View{}; fixtureView.n = side * side; fixtureView.nx = fixtureView.nz = side;
    fixtureView.terrain = 999; fixtureView.begin = reinterpret_cast<uintptr_t>(fixtureSlots.data());
    fixtureReady = fixtureRead = fixtureLayout = true; fixtureGate = 0; fixtureBadSlot = fixtureBadMap = -1;
    g_frame = g_lastDoneFrame = 0; g_releases.clear(); g_lastMs = -1; g_offThisWorld = false;
    g_queueTerrain = fixtureView.terrain; g_queueBegin = fixtureView.begin;
    fixtureWrites = fixtureNotices = 0; g_flightTicks = 0;
}
std::vector<uintptr_t> ExpectedOrder(const float* eye) {
    std::vector<std::pair<float, uintptr_t>> order;
    for (uint32_t i = 0; i < fixtureView.n; i++) {
        float distance = static_cast<float>(i);
        if (eye) { const float dx = fixtureChunks[i].cx - eye[0], dz = fixtureChunks[i].cz - eye[1]; distance = dx * dx + dz * dz; }
        order.emplace_back(distance, i + 1);
    }
    std::stable_sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<uintptr_t> result; for (const auto& item : order) result.push_back(item.second); return result;
}
bool QueueInvariant(int batch, const std::vector<uintptr_t>& expected, size_t priority) {
    if (g_queue.size() != expected.size() || g_batches.size() != 1 || g_batches.front().id != batch ||
        g_batches.front().remaining != static_cast<int>(expected.size())) return false;
    for (size_t i = 0; i < expected.size(); i++) {
        const Entry& e = g_queue[i];
        if (e.chunk != expected[i] || e.batches != std::vector<int>{batch} || e.urgent != (i < priority)) return false;
    }
    return !g_haveFlight || g_flight.batches.empty();
}
std::string Fingerprint() {
    std::string out = std::format("{}:{}:{}:{}:{}:{}:{}:{}", g_queueTerrain, g_queueBegin, g_haveFlight, g_flight.chunk,
                                   g_flightDone.load(), g_nextId, g_statSweeps, fixtureWrites);
    for (int owner : g_flight.batches) out += std::format("F{}", owner);
    for (const auto& e : g_queue) { out += std::format("Q{}:{}", e.chunk, e.urgent); for (int owner : e.batches) out += std::format("B{}", owner); }
    for (const auto& b : g_batches) out += std::format("R{}:{}", b.id, b.remaining);
    return out;
}
int main() {
    static_assert(sizeof(uintptr_t) == 4, "These fixtures represent the x86 production slot vector");
    std::string why, info;
    Reset(); g_haveFlight = true; g_flight = E(7); g_flight.batches = {1};
    Attach(E(7), 2, false, true);
    Assert(g_queue.size() == 1 && g_queue.front().batches == std::vector<int>{2} && g_flight.batches == std::vector<int>{1}, "fresh repeat does not steal old flight owners");
    Attach(E(7), 3, true, true);
    Assert(g_queue.size() == 1 && g_queue.front().batches == std::vector<int>({2, 3}), "urgent promotion preserves queued owners");
    Reset(); Attach(E(1), 10); Attach(E(2), 11); Attach(E(3), 12); Attach(E(2), 13, true);
    Assert(g_queue.size() == 3 && g_queue.front().chunk == 2 && g_queue.front().batches == std::vector<int>({11, 13}), "promotion retains every previous owner");
    std::mt19937 random(731539); const int attempts = 3000;
    for (int trial = 0; trial < attempts; trial++) {
        Reset(1 + random() % 8);
        const bool interactive = random() & 1, haveEye = random() & 1, oldDone = random() & 1;
        const float eye[2] = {static_cast<float>(random() % 2048), static_cast<float>(random() % 2048)};
        const auto expected = ExpectedOrder(haveEye ? eye : nullptr);
        g_haveFlight = true; g_flight = E(1 + random() % fixtureView.n); g_flight.batches = {99};
        g_flightDone = oldDone; g_flightTicks = 2500;
        fixtureChunks[g_flight.chunk - 1].f54 = oldDone ? 0 : 1;
        const auto oldFlight = g_flight.chunk;
        Attach(E(oldFlight), 100, true);
        const int batch = QueueSweep(haveEye ? eye : nullptr, why, info, interactive); handoffs++;
        const auto priority = TerrainLightingPolicy::PreviewPriorityChunks(fixtureView.n, interactive, haveEye);
        Assert(batch > 0 && QueueInvariant(batch, expected, priority), "actual QueueSweep order, fresh ownership and bounded priority");
        FrameResult result;
        ::OnPresent(result);
        Assert(!result.failed && result.done.empty() && g_batches.front().remaining == static_cast<int>(fixtureView.n), "old flight completion never decrements newest phase");
        Assert(fixtureWrites == 0 && !g_flight.batches.size(), "no second flight released during old bake or its free frame");
        if (oldDone) {
            ::OnPresent(result);
            Assert(g_haveFlight && g_flight.chunk == expected.front() && fixtureWrites == 1, "first current-state chunk is nearest after free frame");
            g_flightDone = true; fixtureChunks[g_flight.chunk - 1].f54 = 0; ::OnPresent(result);
            const bool finished = fixtureView.n == 1;
            Assert(finished ? result.done.size() == 1 : g_batches.front().remaining == static_cast<int>(fixtureView.n - 1), "fresh completion decrements exactly once");
        }
    }
    for (int failure = 0; failure < 4; failure++) {
        Reset(); g_haveFlight = true; g_flight = E(7); g_flight.batches = {90}; g_flightDone = true;
        Attach(E(5), 91, true); Batch b; b.id = 91; b.remaining = 1; g_batches.push_back(b);
        if (failure == 0) fixtureReady = false;
        if (failure == 1) fixtureBadSlot = 17;
        if (failure == 2) fixtureChunks[19].lightMap = 0;
        if (failure == 3) g_offThisWorld = true;
        const auto before = Fingerprint();
        Assert(QueueSweep(nullptr, why, info, true) == 0 && Fingerprint() == before, "refused sweep preserves active queue and unconsumed completion");
    }
    Reset(); const float eye[2] = {900, 1200}; QueueSweep(eye, why, info, true);
    fixtureGate = 1; FrameResult result; const auto beforeGate = Fingerprint(); ::OnPresent(result);
    Assert(Fingerprint() == beforeGate && fixtureWrites == 0, "native render gate blocks all release writes");
    fixtureGate = 0; fixtureChunks[10].f55 = 1; ::OnPresent(result);
    Assert(fixtureWrites == 0, "full geometry rebuild excludes local release");
    fixtureChunks[10].f55 = 0; fixtureChunks[10].f56 = 1; ::OnPresent(result);
    Assert(fixtureWrites == 0, "secondary native rebuild excludes local release");
    fixtureChunks[10].f56 = 0; g_lastMs = 2;
    for (int i = 0; i < 12; i++) g_releases.push_back(Clock::now()); ::OnPresent(result);
    Assert(fixtureWrites == 0 && g_releases.size() == 12, "priority cannot reset or exceed twelve-release rolling cap");
    Assert(ReleaseLimit(true, -1) == 8 && ReleaseLimit(true, 12.01) == 8 && ReleaseLimit(false, 2) == 8 && ReleaseLimit(true, 12) == 12, "unchanged measured-cost release reserve");
    Reset(); QueueSweep(nullptr, why, info, false); fixtureView.terrain++; ::OnPresent(result);
    Assert(result.failed && g_offThisWorld && g_queue.empty(), "terrain owner change fails closed and requests fallback");
    std::printf("Terrain queue fixtures: %d scenario assertions, %d failures; %d randomized fresh phase handoffs. Actual production Attach/QueueSweep/OnPresent/FinishFlight extracted; game memory and driver timing are fixtures.\n", cases, failures, handoffs);
    return failures ? 1 : 0;
}
