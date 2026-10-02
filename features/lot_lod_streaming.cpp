#include "lot_lod_streaming.h"
#include "apex_log.h"
#include "game_addresses.h"
#include "game_version.h"
#include "memory_patch.h"
#include "s3ss_detect.h"
#include <cmath>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace LotLodStreaming {
namespace {

std::mutex g_lock;
bool g_running = false;
bool g_externalOwner = false;

uintptr_t g_throttleFlag = 0;
uintptr_t g_worldManagerGlobal = 0;

bool g_throttleOriginalValid = false;
bool g_throttleWritten = false;
uint8_t g_throttleOriginal = 0;

uintptr_t g_worldManager = 0;
bool g_thresholdOriginalValid = false;
bool g_thresholdWritten = false;
float g_thresholdOriginal = 0.0f;

template <typename T> bool Read(uintptr_t address, T& out) {
    return address && MemPatch::ReadBytes(address, &out, sizeof(T));
}

template <typename T> std::vector<BYTE> Bytes(const T& value) {
    const BYTE* p = reinterpret_cast<const BYTE*>(&value);
    return std::vector<BYTE>(p, p + sizeof(T));
}

template <typename T> bool WriteExpected(uintptr_t address, const T& value, const T& expected) {
    const std::vector<BYTE> bytes = Bytes(value);
    const std::vector<BYTE> old = Bytes(expected);
    return MemPatch::WriteBytes(address, bytes, nullptr, &old);
}

bool S3SSOwnsStreamingSettings() {
    // A stale S3SS.toml may remain in Game\\Bin after the ASI is removed. Configuration alone does not make S3SS an
    // active owner: only defer when the official S3SS module is actually loaded in this process.
    const S3SSDetect::Info info = S3SSDetect::Scan();
    if (!info.s3ssLoaded) return false;
    return S3SSDetect::S3SSPatchBoolSettingEnabled("LotStreamingOptimizations", "streamingSettings", true);
}

void ClearWorldState() {
    g_worldManager = 0;
    g_thresholdOriginalValid = false;
    g_thresholdWritten = false;
    g_thresholdOriginal = 0.0f;
}

bool ApplyThrottle(std::string* error) {
    uint8_t current = 0xFF;
    if (!Read(g_throttleFlag, current)) {
        if (error) *error = "Could not read the native Lot LoD throttle flag";
        return false;
    }
    if (current > 1) {
        if (error) *error = std::format("Native Lot LoD throttle has an unexpected value ({})", static_cast<unsigned>(current));
        return false;
    }

    g_throttleOriginal = current;
    g_throttleOriginalValid = true;
    if (current == 1) {
        LOG_INFO("[LotLod] Native transition throttle was already on; Apex leaves it on without taking ownership of that byte");
        return true;
    }

    const uint8_t enabled = 1;
    if (!WriteExpected(g_throttleFlag, enabled, current)) {
        if (error) *error = "Could not enable the native Lot LoD transition throttle";
        g_throttleOriginalValid = false;
        return false;
    }
    g_throttleWritten = true;
    LOG_INFO(std::format("[LotLod] Native transition throttle: {} -> 1 at {:#010x}", static_cast<unsigned>(current), g_throttleFlag));
    return true;
}

bool ApplyCurrentWorld(std::string* error) {
    uintptr_t world = 0;
    if (!Read(g_worldManagerGlobal, world) || !world) return true; // no world yet: Tick() retries

    if (world == g_worldManager) return true;

    // A different WorldManager means the previous world is gone or being replaced. Never write back through a stale
    // pointer: the new manager gets its own original value and restoration state.
    ClearWorldState();
    g_worldManager = world;

    float current = 0.0f;
    if (!Read(world + 0xEC, current)) {
        if (error) *error = "Could not read WorldManager+0xEC (camera speed threshold)";
        ClearWorldState();
        return false;
    }
    if (!std::isfinite(current) || current < 0.0f || current > 100.0f) {
        if (error) *error = std::format("WorldManager+0xEC is not a plausible camera speed threshold ({})", current);
        ClearWorldState();
        return false;
    }

    g_thresholdOriginal = current;
    g_thresholdOriginalValid = true;
    LOG_INFO(std::format("[LotLod] WorldManager {:#010x}: camera speed threshold is {:.3f}", world, current));

    if (current == kCameraThreshold) {
        LOG_INFO("[LotLod] Camera speed threshold already equals 5.0; no write needed");
        return true;
    }

    if (!WriteExpected(world + 0xEC, kCameraThreshold, current)) {
        if (error) *error = "Could not apply camera speed threshold 5.0";
        ClearWorldState();
        return false;
    }

    g_thresholdWritten = true;
    LOG_INFO(std::format("[LotLod] Camera speed threshold: {:.3f} -> {:.3f}", current, kCameraThreshold));
    return true;
}

void RestoreWorldIfOwned() {
    if (!g_thresholdWritten || !g_thresholdOriginalValid || !g_worldManager) {
        ClearWorldState();
        return;
    }

    uintptr_t liveWorld = 0;
    if (!Read(g_worldManagerGlobal, liveWorld) || liveWorld != g_worldManager) {
        LOG_INFO("[LotLod] Camera threshold restore skipped: the WorldManager changed");
        ClearWorldState();
        return;
    }

    float current = 0.0f;
    if (!Read(g_worldManager + 0xEC, current)) {
        LOG_WARNING("[LotLod] Camera threshold restore skipped: current value is unreadable");
        ClearWorldState();
        return;
    }

    if (current != kCameraThreshold) {
        LOG_INFO(std::format("[LotLod] Camera threshold restore skipped: another owner changed {:.3f} to {:.3f}", kCameraThreshold, current));
        ClearWorldState();
        return;
    }

    if (WriteExpected(g_worldManager + 0xEC, g_thresholdOriginal, current))
        LOG_INFO(std::format("[LotLod] Camera speed threshold restored to {:.3f}", g_thresholdOriginal));
    else
        LOG_WARNING("[LotLod] Camera threshold restore failed; current value was left unchanged");

    ClearWorldState();
}

void RestoreThrottleIfOwned() {
    if (!g_throttleWritten || !g_throttleOriginalValid) {
        g_throttleWritten = false;
        g_throttleOriginalValid = false;
        return;
    }

    uint8_t current = 0xFF;
    if (!Read(g_throttleFlag, current)) {
        LOG_WARNING("[LotLod] Throttle restore skipped: current value is unreadable");
    } else if (current != 1) {
        LOG_INFO(std::format("[LotLod] Throttle restore skipped: another owner changed it to {}", static_cast<unsigned>(current)));
    } else if (WriteExpected(g_throttleFlag, g_throttleOriginal, current)) {
        LOG_INFO(std::format("[LotLod] Native transition throttle restored to {}", static_cast<unsigned>(g_throttleOriginal)));
    } else {
        LOG_WARNING("[LotLod] Throttle restore failed; current value was left unchanged");
    }

    g_throttleWritten = false;
    g_throttleOriginalValid = false;
}

} // namespace

bool Start(std::string* error) {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_running) return true;

    g_throttleFlag = GameAddr::Get(GameAddr::Id::LotLodThrottleFlag);
    g_worldManagerGlobal = GameAddr::Get(GameAddr::Id::WorldManagerPtr);

    std::string missing;
    if (!GameAddr::GroupAvailable("LotLodStreaming", &missing)) {
        if (error) *error = "Lot LoD streaming addresses are incomplete: " + missing;
        return false;
    }

    LOG_INFO(std::format("[LotLod] Starting Smooth Lot Streaming on {}", GetGameVersionName()));
    LOG_INFO(std::format("[LotLod] Throttle flag {:#010x}; WorldManager global {:#010x}", g_throttleFlag, g_worldManagerGlobal));

    if (S3SSOwnsStreamingSettings()) {
        g_externalOwner = true;
        g_running = true;
        LOG_INFO("[LotLod] Official Sims3SettingsSetter owns LotStreamingOptimizations.streamingSettings; Apex makes no streaming-setting writes");
        return true;
    }

    g_externalOwner = false;
    if (!ApplyThrottle(error)) return false;

    std::string worldError;
    if (!ApplyCurrentWorld(&worldError)) {
        // The throttle was already changed: put it back if the second half cannot be validated.
        RestoreThrottleIfOwned();
        if (error) *error = worldError;
        return false;
    }

    g_running = true;
    LOG_INFO("[LotLod] Smooth Lot Streaming active");
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_running) return;

    if (g_externalOwner) {
        LOG_INFO("[LotLod] Smooth Lot Streaming off in Apex; Sims3SettingsSetter remains the owner");
    } else {
        RestoreWorldIfOwned();
        RestoreThrottleIfOwned();
        LOG_INFO("[LotLod] Smooth Lot Streaming stopped");
    }

    g_externalOwner = false;
    g_running = false;
    g_throttleFlag = 0;
    g_worldManagerGlobal = 0;
    ClearWorldState();
}

void Tick() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_running || g_externalOwner) return;

    std::string error;
    if (!ApplyCurrentWorld(&error) && !error.empty()) {
        // Fail closed for this world. Do not keep trying to write an implausible field every 10 ms.
        LOG_WARNING("[LotLod] " + error + "; camera threshold left unchanged for this world");
        uintptr_t world = 0;
        if (Read(g_worldManagerGlobal, world)) g_worldManager = world;
    }
}

bool Running() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_running;
}

bool HandledByS3SS() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_running && g_externalOwner;
}

std::string StatusText() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_running) return "Off";
    if (g_externalOwner) return "Handled by Sims3SettingsSetter";
    if (!g_worldManager) return "On; waiting for a world";
    return "On; native lot transitions throttled, camera threshold 5.0";
}

} // namespace LotLodStreaming
