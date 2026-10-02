#include "lot_lod_streaming.h"
#include "apex_log.h"
#include "game_addresses.h"
#include "game_version.h"
#include "memory_patch.h"
#include <cmath>
#include <cstdint>
#include <format>
#include <string>

namespace LotLodStreaming {
namespace {

bool g_started = false;
bool g_worldLogged = false;
uintptr_t g_worldManagerGlobal = 0;

std::string Addr(uintptr_t a) {
    return a ? std::format("{:#010x}", a) : std::string("not found");
}

} // namespace

void StartProbe() {
    if (g_started) return;
    g_started = true;

    const uintptr_t scoring = GameAddr::Get(GameAddr::Id::LotLodScoring);
    const uintptr_t throttleTest = GameAddr::Get(GameAddr::Id::LotLodThrottleTest);
    const uintptr_t throttleFlag = GameAddr::Get(GameAddr::Id::LotLodThrottleFlag);
    g_worldManagerGlobal = GameAddr::Get(GameAddr::Id::WorldManagerPtr);

    LOG_INFO("[LotLod] === Lot LoD Streaming Probe v1 (READ-ONLY) ===");
    LOG_INFO(std::format("[LotLod] Game: {}", GetGameVersionName()));
    LOG_INFO(std::format("[LotLod] LotLodScoring: {}", Addr(scoring)));
    LOG_INFO(std::format("[LotLod] Throttle test: {}", Addr(throttleTest)));
    LOG_INFO(std::format("[LotLod] Throttle flag: {}", Addr(throttleFlag)));
    LOG_INFO(std::format("[LotLod] WorldManager global: {}", Addr(g_worldManagerGlobal)));

    std::string missing;
    if (!GameAddr::GroupAvailable("LotLodStreaming", &missing)) {
        LOG_WARNING("[LotLod] Probe addresses incomplete: " + missing);
        LOG_WARNING("[LotLod] Probe: WAIT/FAIL (read-only; nothing changed)");
        return;
    }

    uint8_t throttle = 0xFF;
    if (!MemPatch::ReadBytes(throttleFlag, &throttle, sizeof throttle)) {
        LOG_WARNING("[LotLod] Throttle value: unreadable");
    } else {
        LOG_INFO(std::format("[LotLod] Throttle value: {}", static_cast<unsigned>(throttle)));
        if (throttle > 1)
            LOG_WARNING("[LotLod] Throttle value is not a plausible bool (expected 0 or 1); nothing will be changed");
    }

    TickProbe();
}

void TickProbe() {
    if (!g_started || g_worldLogged || !g_worldManagerGlobal) return;

    uintptr_t worldManager = 0;
    if (!MemPatch::ReadBytes(g_worldManagerGlobal, &worldManager, sizeof worldManager) || !worldManager) return;

    float cameraThreshold = 0.0f;
    if (!MemPatch::ReadBytes(worldManager + 0xEC, &cameraThreshold, sizeof cameraThreshold)) return;

    LOG_INFO(std::format("[LotLod] WorldManager: {:#010x}", worldManager));
    LOG_INFO(std::format("[LotLod] Camera speed threshold (+0xEC): {:.6f}", cameraThreshold));

    const bool plausible = std::isfinite(cameraThreshold) && cameraThreshold >= 0.0f && cameraThreshold <= 100.0f;
    if (plausible)
        LOG_INFO("[LotLod] Probe: PASS (read-only; nothing changed)");
    else
        LOG_WARNING("[LotLod] Probe: FAIL: WorldManager+0xEC is not a plausible camera speed threshold (read-only; nothing changed)");

    g_worldLogged = true;
}

} // namespace LotLodStreaming
