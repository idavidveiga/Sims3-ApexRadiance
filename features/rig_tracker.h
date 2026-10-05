#pragma once
// Which light rig lit the object being drawn right now (see rig_tracker.cpp).
#include <cstdint>
namespace RigTracker {
bool Install();
void Uninstall();
bool IsInstalled();
// Mode of the rig bound for the current draw (rig+0x1D4: 0 room, 1 roofless room, 2 outdoor), -1 when unknown.
// Only meaningful inside a SceneModel part draw, on the thread that draws.
int CurrentMode();
// Development tools: the rig bound for the object being drawn now (0 = none): one per object, so it names the object
uintptr_t CurrentRig();
// Color > Filters > Sun rays: the sun of the outdoor rigs (slot 0 of modes 1 and 2 = the game's ExteriorLightData; world
// space, pointing toward the sun). Reference counted and separate from Install / Uninstall, so Night Lighting and the
// filter never remove each other's hooks; the sun is read only while it is wanted.
void WantSun(bool on);
// The sun of the latest outdoor rig binds (dir normalised, colour as bound); false when none was seen in the last 2 s
bool Sun(float dir[3], float colour[3]);
}
