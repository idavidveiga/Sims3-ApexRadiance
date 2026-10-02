#pragma once
#include <cmath>
#include <cstdint>

namespace RoomAmbientPolicy {
inline bool AfterLoadRefreshReady(std::uint32_t now, std::uint32_t started, bool busy, std::uint32_t& quiet) {
    if (static_cast<std::uint32_t>(now - started) >= 8000) return true;
    if (busy) quiet = 0;
    else if (!quiet) quiet = now;
    return quiet && static_cast<std::uint32_t>(now - quiet) >= 250;
}
inline bool GroupWaitExpired(std::uint32_t now, std::uint32_t armed) {
    return static_cast<std::uint32_t>(now - armed) >= 1500;
}
inline float FloorPriorityFactor(bool priorityLot, bool allFloors, int level, int camera) {
    if (!priorityLot) return 1.0f;
    if (allFloors) return 4000.0f;
    return level > camera ? 1.0f : level == camera ? 4000.0f : 2000.0f;
}
inline bool RetainFreshSolve(int state) { return state == 2 || state == 3 || state == 5; }
inline bool AmbientMapsCompatible(float norm, float nextNorm, float base, float nextBase) {
    return std::isfinite(norm) && std::isfinite(nextNorm) && std::isfinite(base) && std::isfinite(nextBase)
        && norm > 0 && nextNorm > 0 && std::fabs(norm - nextNorm) <= 1e-5f && std::fabs(base - nextBase) <= 1e-5f;
}
inline bool RigFallbackDue(std::uint32_t now, std::uint32_t armed, bool sent) {
    return !sent && static_cast<std::uint32_t>(now - armed) >= 1500;
}
inline std::uint32_t LampRefreshDelay(bool switchOnly) { return switchOnly ? 120u : 700u; }
inline bool GatherAfterChange(std::uint32_t started, std::uint32_t changed) {
    return static_cast<std::int32_t>(started - changed) > 0;
}
inline bool WindowRecheckDue(std::uint32_t now, std::uint32_t armed, unsigned pass) {
    return pass < 3 && static_cast<std::uint32_t>(now - armed) >= (pass == 0 ? 0u : pass == 1 ? 2000u : 6000u);
}
inline float MoveBackground(float own, float oldBase, float newBase) { return own + newBase - oldBase; }
inline void AccumulateAmbient(float* total, const float* own, float ownNorm, float groupNorm, float area) {
    const float scale = ownNorm > 1e-6f ? groupNorm / ownNorm : 1.0f;
    for (int k = 0; k < 4; k++) total[k] += own[k] * (k < 3 ? scale : 1.0f) * area;
}
inline float BackgroundShare(float brightness, float night) {
    return 1.0f + (brightness - 1.0f) * night;
}
inline bool SameRgb(const float* a, const float* b) {
    for (int k = 0; k < 3; k++)
        if (!std::isfinite(a[k]) || !std::isfinite(b[k]) || std::fabs(a[k] - b[k]) > 2e-4f) return false;
    return true;
}
inline bool SolverOwnsAmbient(int state) { return state >= 1 && state <= 3; }
inline bool AmbientUpdateDue(std::uint32_t now, std::uint32_t last) {
    return !last || static_cast<std::uint32_t>(now - last) >= 3000;
}
// Unknown or busy rooms are queued only after a change settles, never every scan.
inline bool NeedsAmbientSolve(int matchState, bool rest) { return rest && (matchState == 0 || matchState == -2); }
inline bool RebuildRoomList(bool empty, bool expired, bool lotsChanged, bool managersChanged, bool lazy) {
    return empty || expired || (!lazy && (lotsChanged || managersChanged));
}
} // namespace RoomAmbientPolicy
