#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

// Only the baked lamp RGB is adjusted. Native solar/shadow terms keep their scale.
namespace TerrainLightingPolicy {
// One daytime curve for outdoor lamp light: the share of its night strength a lamp keeps at night level n (0 day, 1 night),
// 3% in full daylight rising as smoothstep(0, 1, n) to exactly 1 at night. Every outdoor lamp term (walls, ground, lot
// pass, objects, fences, windows, roofs, water, outdoor lamps in roofed rooms' maps) takes this factor, so a lamp left on
// by day fades the same way on every surface. Visual tuning, not a measured ratio.
inline constexpr float kLampDayFloor = 0.03f;
inline float LampDay(float night) {
    if (!std::isfinite(night) || night <= 0.0f) return kLampDayFloor;
    if (night >= 1.0f) return 1.0f;
    const float s = night * night * (3.0f - 2.0f * night);
    return kLampDayFloor + (1.0f - kLampDayFloor) * s;
}
// The lamp term of the ground (terrain, lot pass) before full night: gain x LampDay(n), 0 for invalid input
inline float GroundLampTotal(float night, float gain) {
    if (!std::isfinite(night) || !std::isfinite(gain) || gain < 0) return 0;
    return gain * LampDay(night);
}
// Surface atlases/point lamps (objects, fences, windows, snow on objects): the night strength x LampDay, bit exact at night.
inline float SurfaceLampGain(float night, float gain) {
    if (!std::isfinite(night) || !std::isfinite(gain) || night < 0 || gain < 0) return gain;
    if (night >= 1.0f) return gain;
    return gain * LampDay(night);
}
// ExteriorWall cK.x scales only baked lamp RGB. At full night the previous native x gain, bit exact; before it the wall
// takes gain x LampDay(n) in place of the game's own daytime factor (0 by day on the captured variant, F7 10:28:03).
inline float WallLampScale(float native, float night, float gain) {
    if (!std::isfinite(native) || native < 0 || !std::isfinite(night) || night < 0 || !std::isfinite(gain) || gain < 0) return native;
    if (night >= 1.0f) return native * gain;
    return gain * LampDay(night);
}
// Realistic lamp reach (12/10): the per-pixel lamps of objects, fences and windows then hold an outdoor wall texel's light
// (lamp_reach.h), so they take the outdoor walls' scale: the native wall lamp scale with the walls' gain as DrawWallGain
// applies it (walls on), or the native scale alone (walls off), times trim (an object slider over its default: 1 at the
// defaults, the same light as the wall behind).
inline float ReachLampScale(float native, float night, bool wallsOn, float wallGain, float trim) {
    if (!std::isfinite(trim) || trim < 0) trim = 1.0f;
    return (wallsOn ? WallLampScale(native, night, wallGain) : native) * trim;
}
inline float LampScale(float native, float night, float gain, bool squared) {
    if (!std::isfinite(native) || native < 0 || !std::isfinite(gain) || gain < 0 || !std::isfinite(night)) return native;
    const float n = std::clamp(night, 0.0f, 1.0f);
    const float weighted = 1.0f + (gain - 1.0f) * n;
    // Preserve the previous night formula, including its floating point rounding.
    if (n == 1) return native * (squared ? std::sqrt(weighted) : weighted);
    // Before full night the lamp term is gain x LampDay(n) whatever the game's own factor (0 by day on the captured
    // variants); the squared (multi-pass) variants square the constant.
    const float total = GroundLampTotal(n, gain);
    return squared ? std::sqrt(total) : total;
}
// The lot light pass (kReplacementHlsl): c3.x scales the terrain term and the lot's own map. Before full night it is set to
// gain x LampDay(n) and c31.x to the lot map's night gain, so the lot ground follows the same curve as the terrain around
// it; at full night the previous constants stay (night = false).
struct LotPassConstants {
    bool day = false;   // replace c3.x and c31.x (else the previous night constants)
    float c3x = 0;      // the lamp scale for the terrain and the lot map
    float lotGain = 1;  // c31.x
};
inline LotPassConstants LotPass(float night, float groundGain, float lotMapGain) {
    LotPassConstants c;
    if (!std::isfinite(night) || night >= 1.0f || !std::isfinite(groundGain) || groundGain < 0 || !std::isfinite(lotMapGain) || lotMapGain < 0) return c;
    c.day = true;
    c.c3x = GroundLampTotal(night, groundGain);
    c.lotGain = lotMapGain;
    return c;
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
// The factor a room solve gives outdoor lamps in roofed rooms (baked maps): exactly LampDay(0) or 1 at the settled
// endpoints, LampDay(level) at twilight. Rooms solved at another factor are sent again once an endpoint settles; nothing is
// solved again per frame.
inline float SolveLampDay(float level) {
    const Phase p = LevelPhase(level);
    return p == Phase::Night ? 1.0f : p == Phase::Day ? LampDay(0.0f) : LampDay(level);
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
