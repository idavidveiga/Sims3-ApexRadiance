#pragma once
// Outdoor lot lamps light the walls and floors of every floor, not only their own (see level_light_share.cpp).
#include <string>
namespace LevelLightShare {
bool Install(std::string& error);
void Uninstall();
bool IsInstalled();
void OnPresent();      // render thread, every frame (runs the refresh asked by Install/Uninstall)
void OnWorldChanged(); // forget the rooms of the previous world
std::string Status();
std::string DiagText(); // F8: samples near the active lot's lights, the game's wall test and ours (empties the record; arms the recording)
// Development build: the samples above are recorded only while armed (Developer checkbox, or the first F8 dump arms it)
void SetDiagArmed(bool on);
bool DiagArmed();
}
