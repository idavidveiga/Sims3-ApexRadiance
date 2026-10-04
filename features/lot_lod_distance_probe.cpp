#include "lot_lod_distance_probe.h"
#include "apex_log.h"
#include "game_addresses.h"
#include "game_version.h"
#include "memory_patch.h"
#include <Windows.h>
#include <detours/detours.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifndef APEX_LOT_LOD_TEST_VALUE
#define APEX_LOT_LOD_TEST_VALUE 70
#endif

#ifndef APEX_LOT_LOD_TEST_MAX_ACTIVE
#define APEX_LOT_LOD_TEST_MAX_ACTIVE 0
#endif

namespace {

using FnScoring = uint64_t(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t);
using FnDetailRequest = uint64_t(__fastcall*)(void*, void*, uint32_t);
// Native function is thiscall(WorldManager*, arg1, arg2, arg3), RET 0x0C, returning a float in ST0.
// A fastcall shim preserves ECX=this and the three stack arguments; EDX is ignored by the original.
using FnMetric = float(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t);

void* g_origScoring = nullptr;
void* g_origDetail = nullptr;
void* g_origMetric = nullptr;
uintptr_t g_scoringAddr = 0;
uintptr_t g_detailAddr = 0;
uintptr_t g_metricAddr = 0;
uintptr_t g_metricCallSite = 0;
std::atomic<bool> g_running{false};
std::atomic<uint64_t> g_scoringCalls{0};
std::atomic<uint64_t> g_metricCalls{0};
std::atomic<uint64_t> g_metricIdentified{0};
std::atomic<uint64_t> g_promotions{0};
std::atomic<uint64_t> g_demotions{0};

constexpr float kBaselineLodDist = 70.0f;
constexpr float kTestLodDist = static_cast<float>(APEX_LOT_LOD_TEST_VALUE);
constexpr bool kChangesLodDist = APEX_LOT_LOD_TEST_VALUE != 70;
constexpr int kBaselineMaxActive = 8;
constexpr int kTestMaxActive = static_cast<int>(APEX_LOT_LOD_TEST_MAX_ACTIVE);
constexpr bool kChangesMaxActive = APEX_LOT_LOD_TEST_MAX_ACTIVE > 0;
static_assert(APEX_LOT_LOD_TEST_MAX_ACTIVE >= 0 && APEX_LOT_LOD_TEST_MAX_ACTIVE <= 128,
              "APEX_LOT_LOD_TEST_MAX_ACTIVE must be 0 (disabled) or 1..128");
// Steam: metric 0x00C62D80, camera-bias JZ 0x00C63015. S3SS has the same JZ pattern on Retail/Steam/EA.
// We only accept this relation when LotLodScoring also contains exactly one CALL to the derived function.
constexpr uintptr_t kMetricToBiasJzDelta = 0x295;
constexpr size_t kScoringCallScanBytes = 0x600;

std::atomic<bool> g_overrideApplied{false};
std::atomic<bool> g_overrideAbandoned{false};
std::atomic<bool> g_baselineValidated{false};
void* g_overrideWorld = nullptr;
float g_originalLodDist = 0.0f;
bool g_originalLodValid = false;
bool g_driftLogged = false;

std::atomic<bool> g_maxActiveOverrideApplied{false};
std::atomic<bool> g_maxActiveOverrideAbandoned{false};
void* g_maxActiveOverrideWorld = nullptr;
int g_originalMaxActive = 0;
bool g_originalMaxActiveValid = false;
bool g_maxActiveDriftLogged = false;

std::mutex g_snapshotMtx;
void* g_world = nullptr;
float g_camera[3] = {};
bool g_cameraValid = false;
float g_lodDist = 0.0f;
float g_activeBias = 0.0f;
int g_maxActiveLots = 0;
float g_terrainHeight = 0.0f;
float g_cameraThreshold = 0.0f;
bool g_settingsValid = false;

struct MetricSample {
    float value = 0.0f;
    uint64_t serial = 0;
    uint64_t lotId = 0;
    uint8_t detailed = 0;
};
std::mutex g_metricMtx;
std::unordered_map<void*, MetricSample> g_metricByLot;
std::atomic<int> g_metricLotArgSlot{0}; // 1..3 once discovered; 0 until then
std::atomic<bool> g_metricSlotConflictLogged{false};

#pragma warning(push)
#pragma warning(disable : 4733)
bool ReadWorldSnapshot(void* self, float& lodDist, float& activeBias, int& maxActive, float& terrainHeight, float& cameraThreshold) {
    if (!self) return false;
    __try {
        const auto* p = static_cast<const uint8_t*>(self);
        lodDist = *reinterpret_cast<const float*>(p + 0xDC);
        activeBias = *reinterpret_cast<const float*>(p + 0xE0);
        maxActive = *reinterpret_cast<const int*>(p + 0xE4);
        terrainHeight = *reinterpret_cast<const float*>(p + 0xE8);
        cameraThreshold = *reinterpret_cast<const float*>(p + 0xEC);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadCameraPoint(uint32_t cameraPoint, float out[3]) {
    if (!cameraPoint) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(static_cast<uintptr_t>(cameraPoint)), sizeof(float) * 3);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadLotState(void* lot, uint8_t& detailed, uint8_t& bulldozing, uint32_t& idLo, uint32_t& idHi) {
    if (!lot) return false;
    __try {
        const auto* p = static_cast<const uint8_t*>(lot);
        detailed = p[0xC1];
        bulldozing = p[0xC9];
        idLo = *reinterpret_cast<const uint32_t*>(p + 0x48);
        idHi = *reinterpret_cast<const uint32_t*>(p + 0x4C);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#pragma warning(pop)

bool PlausibleLot(void* p, uint8_t& detailed, uint8_t& bulldozing, uint32_t& idLo, uint32_t& idHi) {
    const uintptr_t u = reinterpret_cast<uintptr_t>(p);
    if (u < 0x10000 || (u & 3) != 0) return false;
    if (!ReadLotState(p, detailed, bulldozing, idLo, idHi)) return false;
    if (detailed > 1 || bulldozing > 1) return false;
    const uint64_t id = (static_cast<uint64_t>(idHi) << 32) | idLo;
    return id != 0 && id != UINT64_MAX;
}

void* MetricLotArg(uint32_t a, uint32_t b, uint32_t c, uint8_t& detailed, uint32_t& idLo, uint32_t& idHi) {
    const uint32_t raw[3] = {a, b, c};
    int slot = g_metricLotArgSlot.load(std::memory_order_acquire);
    if (slot >= 1 && slot <= 3) {
        uint8_t bulldozing = 0;
        void* p = reinterpret_cast<void*>(static_cast<uintptr_t>(raw[slot - 1]));
        if (PlausibleLot(p, detailed, bulldozing, idLo, idHi)) return p;
        return nullptr;
    }

    void* found = nullptr;
    int foundSlot = 0;
    uint8_t foundDetailed = 0;
    uint32_t foundLo = 0, foundHi = 0;
    for (int i = 0; i < 3; ++i) {
        uint8_t d = 0, bulldozing = 0;
        uint32_t lo = 0, hi = 0;
        void* p = reinterpret_cast<void*>(static_cast<uintptr_t>(raw[i]));
        if (!PlausibleLot(p, d, bulldozing, lo, hi)) continue;
        if (found) return nullptr; // ambiguous: do not guess
        found = p;
        foundSlot = i + 1;
        foundDetailed = d;
        foundLo = lo;
        foundHi = hi;
    }
    if (!found) return nullptr;

    int expected = 0;
    if (g_metricLotArgSlot.compare_exchange_strong(expected, foundSlot, std::memory_order_acq_rel)) {
        LOG_INFO(std::format("[LotLodMetricProbe] Lot pointer identified as metric argument #{}", foundSlot));
    } else if (expected != foundSlot && !g_metricSlotConflictLogged.exchange(true, std::memory_order_acq_rel)) {
        LOG_WARNING(std::format("[LotLodMetricProbe] Metric lot argument changed from #{} to #{}; ambiguous samples will be ignored", expected, foundSlot));
    }
    detailed = foundDetailed;
    idLo = foundLo;
    idHi = foundHi;
    return found;
}

bool PlausibleWorldSettings(float lodDist, float activeBias, int maxActive, float terrainHeight, float cameraThreshold) {
    return std::isfinite(lodDist) && lodDist >= 0.0f && lodDist <= 10000.0f &&
           std::isfinite(activeBias) && std::fabs(activeBias) <= 10000.0f &&
           maxActive >= 0 && maxActive <= 128 &&
           std::isfinite(terrainHeight) && terrainHeight >= 0.0f && terrainHeight <= 100000.0f &&
           std::isfinite(cameraThreshold) && cameraThreshold >= 0.0f && cameraThreshold <= 10000.0f;
}

bool FloatNear(float a, float b) {
    return std::isfinite(a) && std::isfinite(b) && std::fabs(a - b) <= 0.0001f;
}

bool ReadLodDistance(void* world, float& value) {
    if (!world) return false;
    __try {
        value = *reinterpret_cast<const float*>(static_cast<const uint8_t*>(world) + 0xDC);
        return std::isfinite(value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteExpectedLodDistance(void* world, float desired, float expected) {
    if (!world) return false;
    const uintptr_t addr = reinterpret_cast<uintptr_t>(world) + 0xDC;
    std::vector<BYTE> desiredBytes(sizeof(float));
    std::vector<BYTE> expectedBytes(sizeof(float));
    std::memcpy(desiredBytes.data(), &desired, sizeof(float));
    std::memcpy(expectedBytes.data(), &expected, sizeof(float));
    return MemPatch::WriteBytes(addr, desiredBytes, nullptr, &expectedBytes);
}

bool ReadMaxActive(void* world, int& value) {
    if (!world) return false;
    __try {
        value = *reinterpret_cast<const int*>(static_cast<const uint8_t*>(world) + 0xE4);
        return value >= 0 && value <= 128;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool WriteExpectedMaxActive(void* world, int desired, int expected) {
    if (!world) return false;
    const uintptr_t addr = reinterpret_cast<uintptr_t>(world) + 0xE4;
    const DWORD d = static_cast<DWORD>(desired);
    const DWORD e = static_cast<DWORD>(expected);
    return MemPatch::WriteDWORD(addr, d, nullptr, &e);
}

void TryApplyOrMaintainTestValue(void* world, float observed) {
    if (!world || g_overrideAbandoned.load(std::memory_order_acquire)) return;

    if constexpr (!kChangesLodDist) {
        if (g_baselineValidated.load(std::memory_order_acquire)) return;
        if (!FloatNear(observed, kBaselineLodDist)) {
            g_overrideAbandoned.store(true, std::memory_order_release);
            LOG_WARNING(std::format("[LotLodMetricProbe] Baseline probe expected Lot LOD dist. {:.3f}, found {:.3f}; no write was made",
                                    kBaselineLodDist, observed));
            return;
        }
        g_baselineValidated.store(true, std::memory_order_release);
        LOG_INFO(std::format("[LotLodMetricProbe] Baseline validated: Lot LOD dist.={:.3f}; READ-ONLY metric test", observed));
        return;
    }

    if (!g_overrideApplied.load(std::memory_order_acquire)) {
        if (!FloatNear(observed, kBaselineLodDist)) {
            g_overrideAbandoned.store(true, std::memory_order_release);
            LOG_WARNING(std::format("[LotLodMetricProbe] Controlled test NOT applied: initial Lot LOD dist. was {:.3f}, expected baseline {:.3f}",
                                    observed, kBaselineLodDist));
            return;
        }
        if (!WriteExpectedLodDistance(world, kTestLodDist, observed)) {
            g_overrideAbandoned.store(true, std::memory_order_release);
            LOG_WARNING("[LotLodMetricProbe] Controlled test NOT applied: guarded write of WorldManager+0xDC failed");
            return;
        }
        g_overrideWorld = world;
        g_originalLodDist = observed;
        g_originalLodValid = true;
        g_overrideApplied.store(true, std::memory_order_release);
        g_baselineValidated.store(true, std::memory_order_release);
        LOG_INFO(std::format("[LotLodMetricProbe] Controlled test applied: Lot LOD dist. {:.3f} -> {:.3f} at WorldManager+0xDC ({:#010x})",
                             observed, kTestLodDist, reinterpret_cast<uintptr_t>(world) + 0xDC));
        return;
    }

    if (world != g_overrideWorld) {
        g_overrideAbandoned.store(true, std::memory_order_release);
        LOG_WARNING("[LotLodMetricProbe] WorldManager changed while the controlled test was active; Apex stopped maintaining the test value");
        return;
    }
    if (FloatNear(observed, kTestLodDist)) return;

    if (FloatNear(observed, g_originalLodDist)) {
        if (WriteExpectedLodDistance(world, kTestLodDist, observed)) {
            if (!g_driftLogged) {
                LOG_INFO(std::format("[LotLodMetricProbe] Lot LOD dist. drifted {:.3f} -> {:.3f}; maintained at test value", observed, kTestLodDist));
                g_driftLogged = true;
            }
            return;
        }
    }

    g_overrideAbandoned.store(true, std::memory_order_release);
    LOG_WARNING(std::format("[LotLodMetricProbe] Another owner changed Lot LOD dist. to {:.3f}; Apex stopped maintaining the test", observed));
}

void TryApplyOrMaintainMaxActive(void* world, int observed) {
    if constexpr (!kChangesMaxActive) return;
    if (!world || g_maxActiveOverrideAbandoned.load(std::memory_order_acquire)) return;

    if (!g_maxActiveOverrideApplied.load(std::memory_order_acquire)) {
        if (observed != kBaselineMaxActive) {
            g_maxActiveOverrideAbandoned.store(true, std::memory_order_release);
            LOG_WARNING(std::format("[LotLodMetricProbe] Controlled Max Active Lots test NOT applied: initial value was {}, expected baseline {}",
                                    observed, kBaselineMaxActive));
            return;
        }
        if (!WriteExpectedMaxActive(world, kTestMaxActive, observed)) {
            g_maxActiveOverrideAbandoned.store(true, std::memory_order_release);
            LOG_WARNING("[LotLodMetricProbe] Controlled Max Active Lots test NOT applied: guarded write of WorldManager+0xE4 failed");
            return;
        }
        g_maxActiveOverrideWorld = world;
        g_originalMaxActive = observed;
        g_originalMaxActiveValid = true;
        g_maxActiveOverrideApplied.store(true, std::memory_order_release);
        LOG_INFO(std::format("[LotLodMetricProbe] Controlled test applied: Max Active Lots {} -> {} at WorldManager+0xE4 ({:#010x})",
                             observed, kTestMaxActive, reinterpret_cast<uintptr_t>(world) + 0xE4));
        return;
    }

    if (world != g_maxActiveOverrideWorld) {
        g_maxActiveOverrideAbandoned.store(true, std::memory_order_release);
        LOG_WARNING("[LotLodMetricProbe] WorldManager changed while the Max Active Lots test was active; Apex stopped maintaining it");
        return;
    }
    if (observed == kTestMaxActive) return;

    if (observed == g_originalMaxActive) {
        if (WriteExpectedMaxActive(world, kTestMaxActive, observed)) {
            if (!g_maxActiveDriftLogged) {
                LOG_INFO(std::format("[LotLodMetricProbe] Max Active Lots drifted {} -> {}; maintained at test value",
                                     observed, kTestMaxActive));
                g_maxActiveDriftLogged = true;
            }
            return;
        }
    }

    g_maxActiveOverrideAbandoned.store(true, std::memory_order_release);
    LOG_WARNING(std::format("[LotLodMetricProbe] Another owner changed Max Active Lots to {}; Apex stopped maintaining the test", observed));
}

void RestoreMaxActiveIfOwned() {
    if constexpr (!kChangesMaxActive) return;
    if (!g_maxActiveOverrideApplied.load(std::memory_order_acquire) || !g_originalMaxActiveValid || !g_maxActiveOverrideWorld) return;

    int current = 0;
    if (!ReadMaxActive(g_maxActiveOverrideWorld, current)) {
        LOG_WARNING("[LotLodMetricProbe] Max Active Lots restore skipped: WorldManager+0xE4 is unreadable");
        return;
    }
    if (current != kTestMaxActive) {
        LOG_INFO(std::format("[LotLodMetricProbe] Max Active Lots restore skipped: current value is {}, so Apex no longer owns it", current));
        return;
    }
    if (WriteExpectedMaxActive(g_maxActiveOverrideWorld, g_originalMaxActive, current)) {
        LOG_INFO(std::format("[LotLodMetricProbe] Restored Max Active Lots {} -> {}", current, g_originalMaxActive));
    } else {
        LOG_WARNING("[LotLodMetricProbe] Max Active Lots restore failed; WorldManager+0xE4 was left unchanged");
    }
}

void RestoreTestValueIfOwned() {
    if constexpr (!kChangesLodDist) return;
    if (!g_overrideApplied.load(std::memory_order_acquire) || !g_originalLodValid || !g_overrideWorld) return;

    float current = 0.0f;
    if (!ReadLodDistance(g_overrideWorld, current)) {
        LOG_WARNING("[LotLodMetricProbe] Restore skipped: WorldManager+0xDC is unreadable");
        return;
    }
    if (!FloatNear(current, kTestLodDist)) {
        LOG_INFO(std::format("[LotLodMetricProbe] Restore skipped: current Lot LOD dist. is {:.3f}, so Apex no longer owns the value", current));
        return;
    }
    if (WriteExpectedLodDistance(g_overrideWorld, g_originalLodDist, current)) {
        LOG_INFO(std::format("[LotLodMetricProbe] Restored Lot LOD dist. {:.3f} -> {:.3f}", current, g_originalLodDist));
    } else {
        LOG_WARNING("[LotLodMetricProbe] Restore failed; WorldManager+0xDC was left unchanged");
    }
}

bool MatchBytes(uintptr_t addr, const uint8_t* expected, size_t n) {
    uint8_t buf[64] = {};
    return n <= sizeof(buf) && MemPatch::ReadBytes(addr, buf, n) && std::memcmp(buf, expected, n) == 0;
}

bool VerifyMetricBiasTail(uintptr_t jz) {
    // S3SS pattern:
    // 74 ?? F3 0F 10 44 24 08 F3 0F 5C 87 E0 00 00 00 F3 0F 11 44 24 08
    // D9 44 24 08 5F 5E 8B E5 5D C2 0C 00
    constexpr uint8_t expected[] = {
        0x74,0x00,0xF3,0x0F,0x10,0x44,0x24,0x08,0xF3,0x0F,0x5C,0x87,0xE0,0x00,0x00,0x00,
        0xF3,0x0F,0x11,0x44,0x24,0x08,0xD9,0x44,0x24,0x08,0x5F,0x5E,0x8B,0xE5,0x5D,0xC2,0x0C,0x00
    };
    uint8_t got[sizeof expected] = {};
    if (!MemPatch::ReadBytes(jz, got, sizeof got)) return false;
    // Allow 0xEB too: another owner may already have applied the documented visibility override.
    if (got[0] != 0x74 && got[0] != 0xEB) return false;
    for (size_t i = 2; i < sizeof expected; ++i)
        if (got[i] != expected[i]) return false;
    return true;
}

bool FindUniqueCallTo(uintptr_t from, size_t bytes, uintptr_t target, uintptr_t& callSite) {
    std::vector<uint8_t> code(bytes);
    if (!MemPatch::ReadBytes(from, code.data(), code.size())) return false;
    size_t count = 0;
    uintptr_t found = 0;
    for (size_t i = 0; i + 5 <= code.size(); ++i) {
        if (code[i] != 0xE8) continue;
        int32_t rel = 0;
        std::memcpy(&rel, code.data() + i + 1, sizeof rel);
        const uintptr_t site = from + i;
        const uintptr_t dest = site + 5 + static_cast<intptr_t>(rel);
        if (dest == target) {
            ++count;
            found = site;
        }
    }
    if (count != 1) {
        LOG_WARNING(std::format("[LotLodMetricProbe] Expected exactly one LotLodScoring CALL to metric {:#010x}, found {}", target, count));
        return false;
    }
    callSite = found;
    return true;
}

bool ResolveMetricAddress(std::string* error) {
    const uintptr_t biasJz = GameAddr::Get(GameAddr::Id::LotVisibilityCameraBiasJZ);
    if (!biasJz || biasJz <= kMetricToBiasJzDelta) {
        if (error) *error = "Lot visibility metric JZ is unavailable";
        return false;
    }
    if (!VerifyMetricBiasTail(biasJz)) {
        if (error) *error = std::format("Lot visibility metric tail changed at {:#010x}; refusing to derive the metric function", biasJz);
        return false;
    }

    const uintptr_t candidate = biasJz - kMetricToBiasJzDelta;
    uintptr_t callSite = 0;
    if (!FindUniqueCallTo(g_scoringAddr, kScoringCallScanBytes, candidate, callSite)) {
        if (error) *error = std::format("Derived metric {:#010x} is not uniquely called by LotLodScoring", candidate);
        return false;
    }

    g_metricAddr = candidate;
    g_metricCallSite = callSite;
    LOG_INFO(std::format("[LotLodMetricProbe] Metric resolved safely: function={:#010x}, scoring CALL={:#010x}, camera-bias JZ={:#010x} (delta 0x{:X})",
                         g_metricAddr, g_metricCallSite, biasJz, static_cast<unsigned>(kMetricToBiasJzDelta)));
    return true;
}

float __fastcall HookMetric(void* self, void* edx, uint32_t a, uint32_t b, uint32_t c) {
    const float result = reinterpret_cast<FnMetric>(g_origMetric)(self, edx, a, b, c);
    const uint64_t serial = g_metricCalls.fetch_add(1, std::memory_order_relaxed) + 1;

    uint8_t detailed = 0;
    uint32_t idLo = 0, idHi = 0;
    void* lot = MetricLotArg(a, b, c, detailed, idLo, idHi);
    if (!lot) return result;

    g_metricIdentified.fetch_add(1, std::memory_order_relaxed);
    const uint64_t lotId = (static_cast<uint64_t>(idHi) << 32) | idLo;
    {
        std::lock_guard<std::mutex> lk(g_metricMtx);
        g_metricByLot[lot] = MetricSample{result, serial, lotId, detailed};
    }
    return result;
}

uint64_t __fastcall HookScoring(void* self, void* edx, uint32_t dt, uint32_t cameraPoint, uint32_t c, uint32_t d) {
    g_scoringCalls.fetch_add(1, std::memory_order_relaxed);

    float camera[3] = {};
    const bool haveCamera = ReadCameraPoint(cameraPoint, camera);

    float lod = 0.0f, bias = 0.0f, terrain = 0.0f, cameraThreshold = 0.0f;
    int maxActive = 0;
    bool haveSettings = ReadWorldSnapshot(self, lod, bias, maxActive, terrain, cameraThreshold) &&
                        PlausibleWorldSettings(lod, bias, maxActive, terrain, cameraThreshold);

    if (haveSettings) {
        TryApplyOrMaintainTestValue(self, lod);
        TryApplyOrMaintainMaxActive(self, maxActive);
        haveSettings = ReadWorldSnapshot(self, lod, bias, maxActive, terrain, cameraThreshold) &&
                       PlausibleWorldSettings(lod, bias, maxActive, terrain, cameraThreshold);
    }

    bool logSettings = false;
    {
        std::lock_guard<std::mutex> lk(g_snapshotMtx);
        if (haveCamera) {
            std::memcpy(g_camera, camera, sizeof camera);
            g_cameraValid = true;
        }
        if (haveSettings) {
            if (!g_settingsValid || g_world != self || std::fabs(g_lodDist - lod) > 0.0001f ||
                std::fabs(g_activeBias - bias) > 0.0001f || g_maxActiveLots != maxActive ||
                std::fabs(g_terrainHeight - terrain) > 0.0001f || std::fabs(g_cameraThreshold - cameraThreshold) > 0.0001f) {
                logSettings = true;
            }
            g_world = self;
            g_lodDist = lod;
            g_activeBias = bias;
            g_maxActiveLots = maxActive;
            g_terrainHeight = terrain;
            g_cameraThreshold = cameraThreshold;
            g_settingsValid = true;
        }
    }

    if (logSettings) {
        LOG_INFO(std::format("[LotLodMetricProbe] WorldManager {:#010x}: Lot LOD dist.={:.3f}, Active Lot Bias={:.3f}, Max Active Lots={}, Terrain Height Thresh={:.3f}, Camera speed threshold={:.3f}",
                             reinterpret_cast<uintptr_t>(self), lod, bias, maxActive, terrain, cameraThreshold));
    }

    return reinterpret_cast<FnScoring>(g_origScoring)(self, edx, dt, cameraPoint, c, d);
}

uint64_t __fastcall HookDetailRequest(void* lot, void* edx, uint32_t want) {
    uint8_t detailed = 0, bulldozing = 0;
    uint32_t idLo = 0, idHi = 0;
    const uint8_t requested = static_cast<uint8_t>(want & 0xFF);
    const bool readable = ReadLotState(lot, detailed, bulldozing, idLo, idHi);
    const bool transition = readable && bulldozing == 0 && detailed != requested;

    if (transition) {
        float camera[3] = {};
        bool cameraValid = false;
        float lod = 0.0f, bias = 0.0f;
        int maxActive = 0;
        bool settingsValid = false;
        {
            std::lock_guard<std::mutex> lk(g_snapshotMtx);
            cameraValid = g_cameraValid;
            if (cameraValid) std::memcpy(camera, g_camera, sizeof camera);
            settingsValid = g_settingsValid;
            lod = g_lodDist;
            bias = g_activeBias;
            maxActive = g_maxActiveLots;
        }

        MetricSample sample{};
        bool haveMetric = false;
        {
            std::lock_guard<std::mutex> lk(g_metricMtx);
            const auto it = g_metricByLot.find(lot);
            if (it != g_metricByLot.end()) {
                sample = it->second;
                haveMetric = true;
            }
        }

        const uint64_t lotId = (static_cast<uint64_t>(idHi) << 32) | idLo;
        if (requested) g_promotions.fetch_add(1, std::memory_order_relaxed);
        else g_demotions.fetch_add(1, std::memory_order_relaxed);

        const std::string metricText = haveMetric && std::isfinite(sample.value)
            ? std::format("{:.6f}", sample.value)
            : std::string("n/a");
        const uint64_t metricSerial = haveMetric ? sample.serial : 0;

        if (cameraValid && settingsValid) {
            LOG_INFO(std::format("[LotLodMetricProbe] Detailed View {}: lot=0x{:016X} ptr={:#010x}, camera=({:.3f},{:.3f},{:.3f}), metric={}, metricCall={}, metricDetailed={}, Lot LOD dist.={:.3f}, Active Bias={:.3f}, Max Active Lots={}",
                                 requested ? "ON" : "OFF", lotId, reinterpret_cast<uintptr_t>(lot),
                                 camera[0], camera[1], camera[2], metricText, metricSerial,
                                 haveMetric ? static_cast<int>(sample.detailed) : -1, lod, bias, maxActive));
        } else {
            LOG_INFO(std::format("[LotLodMetricProbe] Detailed View {}: lot=0x{:016X} ptr={:#010x}, metric={}, metricCall={}; camera/settings snapshot not ready",
                                 requested ? "ON" : "OFF", lotId, reinterpret_cast<uintptr_t>(lot), metricText, metricSerial));
        }
    }

    return reinterpret_cast<FnDetailRequest>(g_origDetail)(lot, edx, want);
}

void ResetState() {
    {
        std::lock_guard<std::mutex> lk(g_snapshotMtx);
        g_world = nullptr;
        g_cameraValid = false;
        g_settingsValid = false;
        g_lodDist = 0.0f;
        g_activeBias = 0.0f;
        g_maxActiveLots = 0;
        g_terrainHeight = 0.0f;
        g_cameraThreshold = 0.0f;
        std::memset(g_camera, 0, sizeof g_camera);
    }
    {
        std::lock_guard<std::mutex> lk(g_metricMtx);
        g_metricByLot.clear();
        g_metricByLot.reserve(256);
    }
    g_metricLotArgSlot.store(0, std::memory_order_relaxed);
    g_metricSlotConflictLogged.store(false, std::memory_order_relaxed);
}

} // namespace

namespace LotLodDistanceProbe {

bool Start(std::string* error) {
    if (g_running.load(std::memory_order_acquire)) return true;

    std::string missing;
    if (!GameAddr::Have({GameAddr::Id::LotLodScoring, GameAddr::Id::LotDetailRequest, GameAddr::Id::LotVisibilityCameraBiasJZ}, &missing)) {
        if (error) *error = GameAddr::NotAvailable(missing);
        return false;
    }

    g_scoringAddr = GameAddr::Get(GameAddr::Id::LotLodScoring);
    g_detailAddr = GameAddr::Get(GameAddr::Id::LotDetailRequest);
    if (!g_scoringAddr || !g_detailAddr) {
        if (error) *error = "Lot LOD metric probe addresses are unavailable";
        return false;
    }

    const uint8_t scoringPrologue[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0};
    const uint8_t detailPrologue[] = {0x53, 0x8A, 0x5C, 0x24, 0x08, 0x56, 0x8B, 0xF1};
    if (!MatchBytes(g_scoringAddr, scoringPrologue, sizeof scoringPrologue)) {
        if (error) *error = std::format("LotLodScoring prologue changed at {:#010x}; refusing to hook", g_scoringAddr);
        return false;
    }
    if (!MatchBytes(g_detailAddr, detailPrologue, sizeof detailPrologue)) {
        if (error) *error = std::format("LotDetailRequest prologue changed at {:#010x}; refusing to hook", g_detailAddr);
        return false;
    }
    if (!ResolveMetricAddress(error)) return false;

    g_origScoring = reinterpret_cast<void*>(g_scoringAddr);
    g_origDetail = reinterpret_cast<void*>(g_detailAddr);
    g_origMetric = reinterpret_cast<void*>(g_metricAddr);

    LONG rc = DetourTransactionBegin();
    if (rc != NO_ERROR) {
        if (error) *error = std::format("DetourTransactionBegin failed ({})", rc);
        return false;
    }
    DetourUpdateThread(GetCurrentThread());
    rc = DetourAttach(&g_origScoring, reinterpret_cast<void*>(&HookScoring));
    if (rc == NO_ERROR) rc = DetourAttach(&g_origDetail, reinterpret_cast<void*>(&HookDetailRequest));
    if (rc == NO_ERROR) rc = DetourAttach(&g_origMetric, reinterpret_cast<void*>(&HookMetric));
    if (rc != NO_ERROR) {
        DetourTransactionAbort();
        g_origScoring = reinterpret_cast<void*>(g_scoringAddr);
        g_origDetail = reinterpret_cast<void*>(g_detailAddr);
        g_origMetric = reinterpret_cast<void*>(g_metricAddr);
        if (error) *error = std::format("Could not attach Lot LOD metric probe ({})", rc);
        return false;
    }
    rc = DetourTransactionCommit();
    if (rc != NO_ERROR) {
        g_origScoring = reinterpret_cast<void*>(g_scoringAddr);
        g_origDetail = reinterpret_cast<void*>(g_detailAddr);
        g_origMetric = reinterpret_cast<void*>(g_metricAddr);
        if (error) *error = std::format("Could not commit Lot LOD metric probe ({})", rc);
        return false;
    }

    ResetState();
    g_scoringCalls.store(0, std::memory_order_relaxed);
    g_metricCalls.store(0, std::memory_order_relaxed);
    g_metricIdentified.store(0, std::memory_order_relaxed);
    g_promotions.store(0, std::memory_order_relaxed);
    g_demotions.store(0, std::memory_order_relaxed);
    g_overrideApplied.store(false, std::memory_order_relaxed);
    g_overrideAbandoned.store(false, std::memory_order_relaxed);
    g_baselineValidated.store(false, std::memory_order_relaxed);
    g_overrideWorld = nullptr;
    g_originalLodDist = 0.0f;
    g_originalLodValid = false;
    g_driftLogged = false;
    g_maxActiveOverrideApplied.store(false, std::memory_order_relaxed);
    g_maxActiveOverrideAbandoned.store(false, std::memory_order_relaxed);
    g_maxActiveOverrideWorld = nullptr;
    g_originalMaxActive = 0;
    g_originalMaxActiveValid = false;
    g_maxActiveDriftLogged = false;
    g_running.store(true, std::memory_order_release);

    if constexpr (kChangesLodDist) {
        LOG_INFO(std::format("[LotLodMetricProbe] ACTIVE on {}: scoring={:#010x}, metric={:#010x}, detailRequest={:#010x}; controlled Lot LOD dist. {:.3f} -> {:.3f}",
                             GetGameVersionName(), g_scoringAddr, g_metricAddr, g_detailAddr, kBaselineLodDist, kTestLodDist));
    } else {
        LOG_INFO(std::format("[LotLodMetricProbe] ACTIVE on {}: scoring={:#010x}, metric={:#010x}, detailRequest={:#010x}; READ-ONLY Lot LOD dist. baseline {:.3f}",
                             GetGameVersionName(), g_scoringAddr, g_metricAddr, g_detailAddr, kBaselineLodDist));
    }
    if constexpr (kChangesMaxActive) {
        LOG_INFO(std::format("[LotLodMetricProbe] Controlled Max Active Lots test configured: {} -> {} at WorldManager+0xE4",
                             kBaselineMaxActive, kTestMaxActive));
    }
    return true;
}

void Stop() {
    if (!g_running.exchange(false, std::memory_order_acq_rel)) return;

    RestoreMaxActiveIfOwned();
    RestoreTestValueIfOwned();

    if (g_origScoring && g_origDetail && g_origMetric) {
        if (DetourTransactionBegin() == NO_ERROR) {
            DetourUpdateThread(GetCurrentThread());
            LONG rc1 = DetourDetach(&g_origMetric, reinterpret_cast<void*>(&HookMetric));
            LONG rc2 = DetourDetach(&g_origDetail, reinterpret_cast<void*>(&HookDetailRequest));
            LONG rc3 = DetourDetach(&g_origScoring, reinterpret_cast<void*>(&HookScoring));
            LONG rc4 = (rc1 == NO_ERROR && rc2 == NO_ERROR && rc3 == NO_ERROR) ? DetourTransactionCommit() : ERROR_INVALID_FUNCTION;
            if (rc1 != NO_ERROR || rc2 != NO_ERROR || rc3 != NO_ERROR || rc4 != NO_ERROR) {
                DetourTransactionAbort();
                LOG_WARNING(std::format("[LotLodMetricProbe] Probe detach was not clean ({}/{}/{}/{})", rc1, rc2, rc3, rc4));
            }
        }
    }

    LOG_INFO(std::format("[LotLodMetricProbe] Stopped: {} scoring calls, {} metric calls ({} lot-identified), {} Detailed View ON, {} OFF; Lot LOD test value {:.3f}; Max Active Lots test {}",
                         g_scoringCalls.load(std::memory_order_relaxed), g_metricCalls.load(std::memory_order_relaxed),
                         g_metricIdentified.load(std::memory_order_relaxed), g_promotions.load(std::memory_order_relaxed),
                         g_demotions.load(std::memory_order_relaxed), kTestLodDist,
                         kChangesMaxActive ? std::to_string(kTestMaxActive) : std::string("disabled")));
    g_origScoring = nullptr;
    g_origDetail = nullptr;
    g_origMetric = nullptr;
    g_scoringAddr = 0;
    g_detailAddr = 0;
    g_metricAddr = 0;
    g_metricCallSite = 0;
    ResetState();
}

bool Running() { return g_running.load(std::memory_order_acquire); }

} // namespace LotLodDistanceProbe
