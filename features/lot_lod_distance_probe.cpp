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
#include <vector>

namespace {

using FnScoring = uint64_t(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t);
using FnDetailRequest = uint64_t(__fastcall*)(void*, void*, uint32_t);

void* g_origScoring = nullptr;
void* g_origDetail = nullptr;
uintptr_t g_scoringAddr = 0;
uintptr_t g_detailAddr = 0;
std::atomic<bool> g_running{false};
std::atomic<uint64_t> g_scoringCalls{0};
std::atomic<uint64_t> g_promotions{0};
std::atomic<uint64_t> g_demotions{0};

constexpr float kBaselineLodDist = 70.0f;
constexpr float kTestLodDist = 100.0f;
std::atomic<bool> g_overrideApplied{false};
std::atomic<bool> g_overrideAbandoned{false};
void* g_overrideWorld = nullptr;
float g_originalLodDist = 0.0f;
bool g_originalLodValid = false;
bool g_driftLogged = false;

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

void TryApplyOrMaintainTestValue(void* world, float observed) {
    if (!world || g_overrideAbandoned.load(std::memory_order_acquire)) return;

    if (!g_overrideApplied.load(std::memory_order_acquire)) {
        if (!FloatNear(observed, kBaselineLodDist)) {
            g_overrideAbandoned.store(true, std::memory_order_release);
            LOG_WARNING(std::format("[LotLodDistProbe] Controlled 100 test NOT applied: initial Lot LOD dist. was {:.3f}, expected baseline {:.3f}", observed, kBaselineLodDist));
            return;
        }
        if (!WriteExpectedLodDistance(world, kTestLodDist, observed)) {
            g_overrideAbandoned.store(true, std::memory_order_release);
            LOG_WARNING("[LotLodDistProbe] Controlled 100 test NOT applied: guarded write of WorldManager+0xDC failed");
            return;
        }
        g_overrideWorld = world;
        g_originalLodDist = observed;
        g_originalLodValid = true;
        g_overrideApplied.store(true, std::memory_order_release);
        LOG_INFO(std::format("[LotLodDistProbe] Controlled test applied: Lot LOD dist. {:.3f} -> {:.3f} at WorldManager+0xDC ({:#010x})",
                             observed, kTestLodDist, reinterpret_cast<uintptr_t>(world) + 0xDC));
        return;
    }

    if (world != g_overrideWorld) {
        g_overrideAbandoned.store(true, std::memory_order_release);
        LOG_WARNING("[LotLodDistProbe] WorldManager changed while the 100 test was active; Apex stopped maintaining the test value");
        return;
    }

    if (FloatNear(observed, kTestLodDist)) return;

    if (FloatNear(observed, g_originalLodDist)) {
        if (WriteExpectedLodDistance(world, kTestLodDist, observed)) {
            if (!g_driftLogged) {
                LOG_INFO(std::format("[LotLodDistProbe] Lot LOD dist. drifted {:.3f} -> {:.3f}; maintained at test value", observed, kTestLodDist));
                g_driftLogged = true;
            }
            return;
        }
    }

    g_overrideAbandoned.store(true, std::memory_order_release);
    LOG_WARNING(std::format("[LotLodDistProbe] Another owner changed Lot LOD dist. to {:.3f}; Apex stopped maintaining the 100 test", observed));
}

void RestoreTestValueIfOwned() {
    if (!g_overrideApplied.load(std::memory_order_acquire) || !g_originalLodValid || !g_overrideWorld) return;

    float current = 0.0f;
    if (!ReadLodDistance(g_overrideWorld, current)) {
        LOG_WARNING("[LotLodDistProbe] Restore skipped: WorldManager+0xDC is unreadable");
        return;
    }
    if (!FloatNear(current, kTestLodDist)) {
        LOG_INFO(std::format("[LotLodDistProbe] Restore skipped: current Lot LOD dist. is {:.3f}, so Apex no longer owns the value", current));
        return;
    }
    if (WriteExpectedLodDistance(g_overrideWorld, g_originalLodDist, current)) {
        LOG_INFO(std::format("[LotLodDistProbe] Restored Lot LOD dist. {:.3f} -> {:.3f}", current, g_originalLodDist));
    } else {
        LOG_WARNING("[LotLodDistProbe] Restore failed; WorldManager+0xDC was left unchanged");
    }
}

bool MatchBytes(uintptr_t addr, const uint8_t* expected, size_t n) {
    uint8_t buf[16] = {};
    return n <= sizeof(buf) && MemPatch::ReadBytes(addr, buf, n) && std::memcmp(buf, expected, n) == 0;
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
        // Re-read so every diagnostic line reflects the value actually used by the test.
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
        LOG_INFO(std::format("[LotLodDistProbe] WorldManager {:#010x}: Lot LOD dist.={:.3f}, Active Lot Bias={:.3f}, Max Active Lots={}, Terrain Height Thresh={:.3f}, Camera speed threshold={:.3f}",
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
        float lod = 0.0f, bias = 0.0f, terrain = 0.0f, cameraThreshold = 0.0f;
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
            terrain = g_terrainHeight;
            cameraThreshold = g_cameraThreshold;
        }

        const uint64_t lotId = (static_cast<uint64_t>(idHi) << 32) | idLo;
        if (requested) g_promotions.fetch_add(1, std::memory_order_relaxed);
        else g_demotions.fetch_add(1, std::memory_order_relaxed);

        if (cameraValid && settingsValid) {
            LOG_INFO(std::format("[LotLodDistProbe] Detailed View {}: lot=0x{:016X} ptr={:#010x}, camera=({:.3f},{:.3f},{:.3f}), Lot LOD dist.={:.3f}, Active Bias={:.3f}, Max Active Lots={}",
                                 requested ? "ON" : "OFF", lotId, reinterpret_cast<uintptr_t>(lot),
                                 camera[0], camera[1], camera[2], lod, bias, maxActive));
        } else {
            LOG_INFO(std::format("[LotLodDistProbe] Detailed View {}: lot=0x{:016X} ptr={:#010x}; camera/settings snapshot not ready",
                                 requested ? "ON" : "OFF", lotId, reinterpret_cast<uintptr_t>(lot)));
        }
    }

    return reinterpret_cast<FnDetailRequest>(g_origDetail)(lot, edx, want);
}

void ResetSnapshot() {
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

} // namespace

namespace LotLodDistanceProbe {

bool Start(std::string* error) {
    if (g_running.load(std::memory_order_acquire)) return true;

    std::string missing;
    if (!GameAddr::GroupAvailable("LotLodDistanceProbe", &missing)) {
        if (error) *error = GameAddr::NotAvailable(missing);
        return false;
    }

    g_scoringAddr = GameAddr::Get(GameAddr::Id::LotLodScoring);
    g_detailAddr = GameAddr::Get(GameAddr::Id::LotDetailRequest);
    if (!g_scoringAddr || !g_detailAddr) {
        if (error) *error = "Lot LOD probe addresses are unavailable";
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

    g_origScoring = reinterpret_cast<void*>(g_scoringAddr);
    g_origDetail = reinterpret_cast<void*>(g_detailAddr);

    LONG rc = DetourTransactionBegin();
    if (rc != NO_ERROR) {
        if (error) *error = std::format("DetourTransactionBegin failed ({})", rc);
        return false;
    }
    DetourUpdateThread(GetCurrentThread());
    rc = DetourAttach(&g_origScoring, reinterpret_cast<void*>(&HookScoring));
    if (rc == NO_ERROR) rc = DetourAttach(&g_origDetail, reinterpret_cast<void*>(&HookDetailRequest));
    if (rc != NO_ERROR) {
        DetourTransactionAbort();
        g_origScoring = reinterpret_cast<void*>(g_scoringAddr);
        g_origDetail = reinterpret_cast<void*>(g_detailAddr);
        if (error) *error = std::format("Could not attach Lot LOD distance probe ({})", rc);
        return false;
    }
    rc = DetourTransactionCommit();
    if (rc != NO_ERROR) {
        g_origScoring = reinterpret_cast<void*>(g_scoringAddr);
        g_origDetail = reinterpret_cast<void*>(g_detailAddr);
        if (error) *error = std::format("Could not commit Lot LOD distance probe ({})", rc);
        return false;
    }

    ResetSnapshot();
    g_scoringCalls.store(0, std::memory_order_relaxed);
    g_promotions.store(0, std::memory_order_relaxed);
    g_demotions.store(0, std::memory_order_relaxed);
    g_overrideApplied.store(false, std::memory_order_relaxed);
    g_overrideAbandoned.store(false, std::memory_order_relaxed);
    g_overrideWorld = nullptr;
    g_originalLodDist = 0.0f;
    g_originalLodValid = false;
    g_driftLogged = false;
    g_running.store(true, std::memory_order_release);
    LOG_INFO(std::format("[LotLodDistProbe] CONTROLLED probe active on {}: scoring={:#010x}, detailRequest={:#010x}; will change ONLY Lot LOD dist. {:.3f} -> {:.3f} after validating the baseline",
                         GetGameVersionName(), g_scoringAddr, g_detailAddr, kBaselineLodDist, kTestLodDist));
    return true;
}

void Stop() {
    if (!g_running.exchange(false, std::memory_order_acq_rel)) return;

    RestoreTestValueIfOwned();

    if (g_origScoring && g_origDetail) {
        if (DetourTransactionBegin() == NO_ERROR) {
            DetourUpdateThread(GetCurrentThread());
            LONG rc1 = DetourDetach(&g_origScoring, reinterpret_cast<void*>(&HookScoring));
            LONG rc2 = DetourDetach(&g_origDetail, reinterpret_cast<void*>(&HookDetailRequest));
            LONG rc3 = (rc1 == NO_ERROR && rc2 == NO_ERROR) ? DetourTransactionCommit() : ERROR_INVALID_FUNCTION;
            if (rc1 != NO_ERROR || rc2 != NO_ERROR || rc3 != NO_ERROR) {
                DetourTransactionAbort();
                LOG_WARNING(std::format("[LotLodDistProbe] Probe detach was not clean ({}/{}/{})", rc1, rc2, rc3));
            }
        }
    }

    LOG_INFO(std::format("[LotLodDistProbe] Stopped: {} scoring calls, {} Detailed View ON, {} Detailed View OFF; controlled Lot LOD dist. test {}",
                         g_scoringCalls.load(std::memory_order_relaxed), g_promotions.load(std::memory_order_relaxed),
                         g_demotions.load(std::memory_order_relaxed),
                         g_overrideApplied.load(std::memory_order_relaxed) ? "was applied" : "was not applied"));
    g_origScoring = nullptr;
    g_origDetail = nullptr;
    g_scoringAddr = 0;
    g_detailAddr = 0;
    ResetSnapshot();
}

bool Running() { return g_running.load(std::memory_order_acquire); }

} // namespace LotLodDistanceProbe
