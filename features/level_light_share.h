#pragma once
// Outdoor lot lamps light the walls and floors of every floor, not only their own, and indoor lamps light the story above
// or below through stair openings (see level_light_share.cpp).
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
namespace LevelLightShare {
bool Install(std::string& error);
void Uninstall();
bool IsInstalled();
// True only while the validated directional-map floor guard is installed and indoor sharing is ready.
bool BasisFloorGuardReady();
void OnPresent();      // render thread, every frame (runs the refresh asked by Install/Uninstall)
void OnWorldChanged(); // forget the rooms of the previous world
// The world is on screen after its load: every lot's rooms near openings gather once more (then settle)
void OnWorldLive();
// Render thread: how many lots are loaded now (0 at the main menu and during most of a load)
int LoadedLots();
bool LoadedRoomsBusy(); // cached room states plus pending ambient; startup refresh readiness
// Development tools (render thread): the story each loaded lot shows (low half of its id, story); returns the count
int DisplayLevels(uint32_t* lots, int* stories, int max);
// Development tools (F6 recorder, render thread): "[room]" lines for the indoor rooms whose ambient, state, class or light
// count changed since the last call (reset: every room again)
std::vector<std::string> TraceRooms(bool reset);
// Indoor lamps also light the story above or below through stair openings (the rooms near them gather again on a change)
void SetIndoor(bool on);
// Walls are lit at the heights the game draws their light at, so the walls of two stories meet the floor line on the same
// light (every room lights its walls again on a change)
void SetWallAlign(bool on);
// Every room of the active lot solved at the top LOD class on every story, so a floor change re-solves nothing (default on)
void SetAllFloors(bool on);
bool AllFloorsDetailed(); // current full-detail policy, shared with the room scheduler
// Render thread: every room of every loaded lot lights again (options that change how rooms are lit); why = log text
void RelightAllRooms(const char* why);
// Render thread: every room of one loaded lot (every story, room 0 too) lights again; the rooms sent, -1 = the lot is gone
// With switchChangedAt, fresh running/completed solves may be retained; 0 forces the normal refresh.
// Watches this lot's completion for a furniture refresh, with a 1500 ms fallback and bounded 6 s lifetime.
int RelightLot(uintptr_t tracker, const char* why, unsigned long switchChangedAt = 0);
// Rooms at Night: visits every room (id > 0) of every loaded lot (stories -4..7); visit returns true to send the room to
// gather again. Render thread. Returns the rooms visited; queued = how many were sent.
int ForEachRoom(bool (*visit)(unsigned char* room, void* ctx), void* ctx, int* queued,
    void (*ack)(unsigned char* room, bool queued, void* ctx) = nullptr);
// Stage original room colours, then refresh connected groups after the complete traversal.
bool StageAmbientBaseChange(unsigned char* room, const float* oldOwn, const float* newOwn, const float* oldSecond, const float* newSecond);
bool HoldsAmbientBase(unsigned char* room, const float* own, const float* second);
bool StageUnlitAmbientChange(unsigned char* room, const float* target, bool& changed);
void ApplyAmbientBaseChanges();
// Development build: the solve journal's notes since fromTick (GetTickCount), with their ticks (the recorder)
std::vector<std::pair<unsigned long, std::string>> JournalSince(unsigned long fromTick);
std::string Status();
std::string DiagText(); // F8: samples near the active lot's lights, the game's wall test and ours (empties the record; arms the recording)
// Development build: the samples above are recorded only while armed (Developer checkbox, or the first F8 dump arms it)
void SetDiagArmed(bool on);
bool DiagArmed();
}
