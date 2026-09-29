#pragma once
// Makes lot grass use the same street-lamp light as world grass, and keeps lamp light on outdoor objects inside the
// moon shadow at night (see lot_light_bridge.cpp).
#include <cstdint>
#include <string>
#include <vector>
namespace LotLightBridge {
void SetEnabled(bool on);
void SetObjectShadowFix(bool on);
void SetNightLevel(float level); // lightMgr+0xF0, 0 = day, 1 = night
void SetRoofFix(bool on, float strength);
void OnPresent();                  // render thread, every frame (refreshes the nearby lamp list for roofs)
// Counts changes of outdoor lot lamps that change the terrain bake (a lamp of the bake added, removed or moved; a lamp
// entering or leaving it: lit / enabled / intensity 0; its colour x intensity x range changed by more than 5 %), e.g. in
// Build mode, on lots that were already loaded (lots streaming in or out, lamps outside the bake and "animated" lamps are
// never counted, see TrackLotLampEdits). The terrain light has to be rebuilt for those; the game does it only when the
// lot is reloaded.
int LotLampEdits();
// Of those, the enumerations whose counted changes include a user-driven one (a lamp of the bake added, removed or moved
// by more than 5 cm; the rest are automatic: switched on / off, dimmed, recoloured). The terrain relight rebuilds
// user-driven changes fast and rate-limits automatic ones (terrain-relight.md "Lamp change decisions").
int LotLampUserEdits();
std::string LotLampStatus(); // developer status of the lamp change tracking

// Snapshot of the lot lamps the terrain bake can take, refreshed with every light enumeration (every 20 frames, render
// thread). The terrain relight keeps the snapshot of the last rebuild and compares it with the current one.
struct BakeLamp {
    uint64_t lot = 0;
    int type = 0;           // +0xB0: 3..6 lot lamp classes, 0xB street-lamp class
    float pos[3] = {};      // +0x120
    float light[3] = {};    // what the bake draws: base colour +0xF0 x intensity +0x10 x range +0x130 (the bake also x 0.2)
    bool baked = false;     // the bake takes it now: lit, (types 3..6) enabled, and its light is not zero
    bool animated = false;  // switches / dims by itself (3+ automatic changes within 60 s): its changes never trigger
};
struct BakeSnapshot {
    std::vector<BakeLamp> lamps;       // lamps of type 3..6 or 0xB on lots (outdoors, alive), sorted by lot
    std::vector<uint64_t> settledLots; // lots seen for 10 s without uncounted changes for 5 s (sorted)
    std::vector<uint64_t> lots;        // every lot with a tracked lamp (sorted)
};
const BakeSnapshot& CurrentBakeLamps();
// What differs in the bake between two snapshots: only on lots settled in `now` that were already in `baked` (lots that
// streamed in later were never baked, and streaming never rebuilds). Lamps are matched by type and place (5 cm), not by
// pointer. Light changes under 5 % per channel are noise; animated lamps are counted apart and never make Any() true.
// plainLamps false: "Lot lamps light the street" is off, lamps of type 3..6 are not in the bake.
struct BakeDiff {
    int added = 0, removed = 0, switchedOn = 0, switchedOff = 0, light = 0, lots = 0, animated = 0;
    bool Any() const { return added + removed + switchedOn + switchedOff + light > 0; }
    bool Structural() const { return added + removed > 0; } // placed, deleted or moved (Build mode)
    std::string Text() const;
};
BakeDiff DiffBake(const BakeSnapshot& baked, const BakeSnapshot& now, bool plainLamps);
int LampEnumerations(); // light enumerations done (the snapshot changes only when this does)
// Lots of the latest counted user-driven change (LotLampUserEdits went up with it)
const std::vector<uint64_t>& LastUserChangeLots();
// The next OnPresent enumerates the lights at once (a rebuild was just consumed: its snapshot one frame later)
void RequestLampRefresh();
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
