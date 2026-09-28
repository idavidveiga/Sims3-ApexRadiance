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
// Counts changes of outdoor lot lamps (edits of colour, brightness, on/off, position; additions; removals), e.g. in Build
// mode, on lots that were already loaded (lots streaming in or out are never counted, see TrackLotLampEdits). The
// terrain light has to be rebuilt for those; the game does it only when the lot is reloaded.
int LotLampEdits();
std::string LotLampStatus(); // developer status of the lamp change tracking
int ChunkCount();            // world terrain chunks whose light map was seen in a draw (0 until the world is drawn)
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
// Soft lot edges: within 3 m of a lot edge the lot grass lamp term blends to the terrain term the world grass shows
// outside (no step where a lamp stands near a lot edge). Default on; developer A/B toggle.
void SetSoftLotEdges(bool on);
std::string LotEdgeStatus();
// A new world was loaded (render thread): forget the previous world's chunk light maps.
void OnWorldChanged();
std::string Status();
std::string ObjectStatus();
std::string DescribeDraw(); // for the light probe: how the current draw is classified and what the mod did with it
// keepChunkMaps: a reinstall in the same world keeps the chunk maps, smoothed maps and world atlas (render thread only).
void Shutdown(bool keepChunkMaps = false);
}
