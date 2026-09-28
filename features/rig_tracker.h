#pragma once
// Which light rig lit the object being drawn right now (see rig_tracker.cpp).
namespace RigTracker {
bool Install();
void Uninstall();
bool IsInstalled();
// Mode of the rig bound for the current draw (rig+0x1D4: 0 room, 1 roofless room, 2 outdoor), -1 when unknown.
// Only meaningful inside a SceneModel part draw, on the thread that draws.
int CurrentMode();
}
