#pragma once
// Light diagnostics used by Night Lighting (patches/night_terrain_relight_patch.cpp).
#include <string>
namespace LightDiag {
bool Init();                    // validates game addresses; false if this game version does not match
void RequestDump();             // writes ApexRadiance_LightDiag.txt on the next frame
void OnPresent();               // call every frame from the render thread (handles Ctrl+Shift+F8 too)
const std::string& Status();
}
