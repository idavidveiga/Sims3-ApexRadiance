#pragma once
// Where the game draws its walls (06/10, the whole-scene wall survey of the F7 capture: on a house on a foundation the
// outside walls are drawn about 2 m under the base their light is laid out from, wall +0x114, and not all of them: a
// facade on a story stood at that base). The wall mesh draws (the game's wall vertex shader) are read once per mesh: the
// foot and top of every vertical edge, in world space. The room solve (level_light_share.cpp) lays a wall's light rows
// out from the measured foot and tests walls against light at their drawn heights; a room solved before its walls were
// drawn is solved again once they are, when the measure differs from its base.
#include <cstdint>
#include <string>
#include <vector>

namespace WallHeights {
void Install();
void Uninstall();
// The drawn foot of the wall along the line a -> b (world xz; ext = how far past the ends the wall reaches) whose drawn span
// best covers [base - 3, base + 3]; false when no drawn wall is known there. Any thread.
bool DrawnFoot(float ax, float az, float bx, float bz, float ext, float base, float& foot);
// A room solved while its wall was not measured yet (light tree thread): solved again once a measure differs from base.
void NotePending(uintptr_t tracker, int level, int id, float ax, float az, float bx, float bz, float ext, float base);
struct RoomKey {
    uintptr_t tracker;
    int level, id;
};
// Render thread: the pending rooms whose walls are now measured away from their base (each once)
std::vector<RoomKey> TakeRequeue();
std::string Status();
}
