#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Only the baked lamp RGB is adjusted. Native solar/shadow terms keep their scale.
namespace TerrainLightingPolicy {
inline float DayLampScale(float night, float gain) {
    if (!std::isfinite(night) || !std::isfinite(gain) || gain < 0) return 0;
    return (1.0f - std::clamp(night, 0.0f, 1.0f)) * gain;
}
// Surface atlases/point lamps otherwise retain their full nighttime response in
// daylight. Use a common subdued daytime response, then smoothly recover the
// exact configured night strength. This is visual tuning, not a bake correction.
inline constexpr float kDaySurfaceResponse = 0.08f;
inline float SurfaceLampGain(float night, float gain) {
    if (!std::isfinite(night) || !std::isfinite(gain) || night < 0 || gain < 0) return gain;
    if (night >= 1.0f) return gain;
    const float day = std::min(gain, 1.0f) * kDaySurfaceResponse;
    return day + (gain - day) * night;
}
// ExteriorWall cK.x scales only baked lamp RGB. Keep the existing night gain;
// supplement the daytime term that the native wall shader otherwise discards.
inline float WallLampScale(float native, float night, float gain) {
    if (!std::isfinite(native) || native < 0 || !std::isfinite(night) || night < 0 || !std::isfinite(gain) || gain < 0) return native;
    const float previous = native * gain;
    if (night >= 1.0f) return previous;
    return previous + DayLampScale(night, std::min(gain, 1.0f) * kDaySurfaceResponse);
}
inline float LampScale(float native, float night, float gain, bool squared) {
    if (!std::isfinite(native) || native < 0 || !std::isfinite(gain) || gain < 0 || !std::isfinite(night)) return native;
    const float n = std::clamp(night, 0.0f, 1.0f);
    const float weighted = 1.0f + (gain - 1.0f) * n;
    // Preserve the previous night formula, including its floating point rounding.
    if (n == 1) return native * (squared ? std::sqrt(weighted) : weighted);
    const float day = DayLampScale(n, gain);
    return squared ? std::sqrt(native * native * weighted + day) : native * weighted + day;
}
inline bool DeferDayEdit(bool night, bool automaticDusk, bool user, bool force, bool worldLamp) {
    return !night && automaticDusk && !user && !force && !worldLamp;
}
inline int64_t PhaseDelay(int64_t configuredMs, bool editing) {
    return editing ? 0 : std::max<int64_t>(0, configuredMs);
}
inline std::size_t PreviewPriorityChunks(std::size_t total, bool editing, bool haveEye) {
    return editing && haveEye ? std::min<std::size_t>(total, 4) : 0;
}

enum class Phase { Day, Twilight, Night };
inline Phase LevelPhase(float level) {
    return level > .99f ? Phase::Night : level < .01f ? Phase::Day : Phase::Twilight;
}
// Settled endpoints only. Reversing direction cancels a stale delayed rebuild.
struct Cycle {
    Phase last = Phase::Day, target = Phase::Day;
    bool pending = false;
    int64_t due = 0;
    void Reset(float level) { last = target = LevelPhase(level); pending = false; due = 0; }
    void Observe(float level, int64_t now, int64_t delay, bool enabled, bool loading) {
        const Phase phase = LevelPhase(level);
        if (!enabled || loading || (pending && phase != target)) pending = false;
        if (enabled && !loading && phase != last && phase != Phase::Twilight) {
            target = phase;
            due = now + std::max<int64_t>(0, delay);
            pending = true;
        }
        last = phase;
    }
    bool Consume(int64_t now, Phase& phase) {
        if (!pending || now < due) return false;
        phase = target;
        pending = false;
        return true;
    }
};
}
