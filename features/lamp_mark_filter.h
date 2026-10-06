#pragma once
// Rooms keep their light when their lamps did not change (part of Night Lighting; see lamp_mark_filter.cpp).
#include <string>
namespace LampMarkFilter {
bool Install(std::string& why); // Steam 1.67.2 only (fixed addresses, checked byte by byte); false: the game marks as before
void Uninstall();
bool IsInstalled();
// Render thread, every frame (Night Lighting): the lots whose lamps were switched or moved light again (nightLevel: the game's, -1
// = no world)
void OnPresent(float nightLevel);
// Many lamps switched by the player within a moment (06/10: "all the lights" of a lot): kMassSwitches or more user switches
// within kMassWindowMs, held for kMassHoldMs after the last one. Any thread.
bool MassSwitchActive();
// Render thread: on = a lamp entry update marks its room changed only when the lamp changed (default on; Developer page)
void SetEnabled(bool on);
bool Enabled();
// Development tools (F6 recorder, Developer page): marks let through / kept back, and which light-manager messages flagged
// light entries (dev build)
std::string Status();
}
