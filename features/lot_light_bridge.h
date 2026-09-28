#pragma once
// Makes lot grass use the same street-lamp light as world grass, and keeps lamp light on outdoor objects inside the
// moon shadow at night (see lot_light_bridge.cpp).
#include <string>
namespace LotLightBridge {
void SetEnabled(bool on);
void SetObjectShadowFix(bool on);
void SetNightLevel(float level); // lightMgr+0xF0, 0 = day, 1 = night
void SetRoofFix(bool on, float strength);
void OnPresent();                  // render thread, every frame (refreshes the nearby lamp list for roofs)
// Counts edits of outdoor lot lamps that already existed (colour, brightness, on/off), e.g. a colour change in build
// mode. The terrain light has to be rebuilt for those; the game does it only when the lot is reloaded.
int LotLampEdits();
std::string RoofStatus();
void SetWaterFix(bool on, float strength, float reflection);
std::string WaterStatus();
// Sidewalks under snow: 0 = like the game (fully snow-covered), 1 = concrete fully visible.
void SetSidewalkClear(float amount);
// Fences, railings and stairs lit per pixel from the ground light (world light atlas) instead of the game's rig.
void SetFenceGroundLight(bool on, float strength);
// Development: paint lamp-lit draws no fix claimed in magenta; record them for ApexRadiance_Censo.txt (3 frames).
void SetFalseColor(bool on);
void RequestCensus();
std::string CensusStatus();
// Outdoor walls: multiplies their baked lamp light (1 = the game).
// Outdoor rig objects (doors, counters, modular pieces): the same world lamps per pixel instead of each piece's rig lamps.
void SetObjectPixelLights(bool on, float strength);
void SetWallGain(float gain);
std::string WallStatus();
// Outdoor objects lit by a rig (doors, windows, counters) also get the ground light per pixel (max with the rig lamps).
void SetObjectPixelLamps(bool on, float strength);
// A new world was loaded (render thread): forget the previous world's chunk light maps.
void OnWorldChanged();
std::string Status();
std::string ObjectStatus();
std::string DescribeDraw(); // for the light probe: how the current draw is classified and what the mod did with it
void Shutdown();
}
