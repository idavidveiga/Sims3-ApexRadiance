#pragma once
// The rig per-light test's luminance cut for a lamp lit through a roofed room's openings (object_light_bridge.cpp KeepOverCut).
// FUN_006bb270 drops a record whose luminance is under its cut ("comiss cut, rec[10]; ja drop", 0x006BB2CB); a lamp through
// an opening has the per-pixel lamps' law as its record, which outdoor objects take with no cut, so a record with at least
// one 8-bit level of light is raised to the cut (kept, last in the rank, its colour untouched). POD, no game reads: the
// offline harness (tools/roofed_openings_test) tests it directly.
#include <cmath>

namespace RigCut {

constexpr float kMinLuma = 1.0f / 255.0f;

// The luminance the record is tested with: `luma` itself, or `cut` when the law's light is visible but under the cut
inline float KeptLuma(float luma, float cut) {
    if (!(luma >= kMinLuma) || !(cut > 0.0f && cut < 1.0f) || luma >= cut) return luma;
    return cut;
}
// The game's test on that luminance: kept unless the cut is above it
inline bool GameKeeps(float luma, float cut) { return !(cut > luma); }

} // namespace RigCut
