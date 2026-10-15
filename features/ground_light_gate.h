#pragma once
// The ground light on outdoor objects and fences (lot_light_bridge.cpp PickObjectLamps, DrawInstanced): how much of the world
// atlas a surface takes. The atlas is the bake lamps' light lying on the ground, with no wall, slab or height, so a surface
// takes it only for lamps that are in it and that it sees, and none above the heads of those lamps. POD and the standard
// library only, no game access: the offline harness (tools/roofed_openings_test) tests it directly.
#include <algorithm>

namespace GroundLightGate {

constexpr float kFadeAbove = 1.5f; // m over the highest head: the gate fades from 1 to 0

// Notes the head height of a lamp that can light the ground pool near the surface (horizontal offset dx, dz within reach).
// A lamp shining out of a house (inBake false) is not in the atlas and does not count.
inline void NoteHead(float& top, float headY, float dx, float dz, float reach, bool inBake) {
    if (inBake && dx * dx + dz * dz <= reach * reach) top = std::max(top, headY);
}
// The height gate at surface height y: 1 at or under the highest head, 0 from kFadeAbove over it; 1 with no head noted
inline float HeightGate(float y, float top) {
    return top > -1e8f ? std::clamp(1.0f - (y - top) / kFadeAbove, 0.0f, 1.0f) : 1.0f;
}

// The share of the atlas lamps' light the surface sees: sum w v / sum w over the lamps in the atlas, w the lamp's weight at
// the surface, v the share of it past walls and slabs (a lamp left out: 0). 1 with no atlas lamp near.
struct Share {
    float seen = 0.0f, all = 0.0f;
    void Add(float w, float v, bool inBake) {
        if (!inBake || !(w > 0.0f)) return;
        all += w;
        seen += w * std::clamp(v, 0.0f, 1.0f);
    }
    float Value() const { return all > 1e-5f ? std::clamp(seen / all, 0.0f, 1.0f) : 1.0f; }
};

} // namespace GroundLightGate
