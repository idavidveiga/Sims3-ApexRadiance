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
// "High quality on every lot" is on (night_terrain_relight): SetAllFloors then applies to the priority lot only
void SetAllLotsHighQuality(bool on);
// Walls block lamp light on outdoor floors (open rooms, decks): the floor map's alpha carries the blocked share of the
// outdoor lamps (relights every room when it changes); Active = installed, the game code matched and on
void SetFloorWalls(bool on);
bool FloorWallsActive();
// Walls block light on objects: true when an outside wall stands between the lamp and the point at the height the ray
// crosses it (render thread; a copy of each story's outside walls taken when room 0 is solved)
bool WallBlocks(const float lamp[3], const float point[3]);
void SetObjectWalls(bool on);
bool AllFloorsDetailed(); // current full-detail policy, shared with the room scheduler
// Render thread: every room of every loaded lot lights again (options that change how rooms are lit); why = log text
void RelightAllRooms(const char* why);
// Render thread: every room of one loaded lot (every story, room 0 too) lights again; the rooms sent, -1 = the lot is gone
// With changedAt, fresh running/completed solves (gathered after it) are retained; 0 forces the normal refresh. With lamps,
// only the rooms whose light list holds one of them (lamps that only moved).
// Watches this lot's completion for a furniture refresh, with a 1500 ms fallback and bounded 6 s lifetime.
int RelightLot(uintptr_t tracker, const char* why, unsigned long changedAt = 0, const uintptr_t* lamps = nullptr, int lampCount = 0);
// A lamp switched where it was (same room, not moved): its tree level (story) and room
struct LampSwitch {
    uintptr_t light, tl;
    int room;
};
// Render thread: the lamp switch's safety net for those rooms only: the rooms holding a switched (or moved) lamp, the
// switched lamps' own rooms, the rooms near the stair openings of their story and the stories next to it, and the outdoor
// rooms of every story for an outdoor lamp; fresh solves retained as RelightLot does. -1 = the lot is gone.
int RelightLampSwitch(uintptr_t tracker, const char* why, unsigned long changedAt, const LampSwitch* switched, int count,
                      const uintptr_t* moved, int movedCount);
// Lamp edits first (light tree thread, from LampMarkFilter's lamp entry update): the room this tree level's update marks for
// a lamp edit (user = colour, intensity or on / off by a player or a Sim, not a flicker) is solved before any other room, and
// so are the rooms of other stories that take its light; pure = the lamp stayed in that room (moved, switched or
// changed a value): those rooms gather in the same update (a lamp added or moved into the room waits for the game)
void NoteLampMark(uintptr_t treeLevel, int room, bool user, bool pure);
// A lamp that only moved or changed a value while its room is being solved: true = the mark is held and given back by the
// room update once that solve is over (the light follows a dragged lamp instead of restarting every step)
bool HoldLampMark(uintptr_t treeLevel, int room, bool user);
// Scheduler factor of a room (RoomLightQueue's priority hook): 1, or above any other room's for a lamp edit's rooms
float LampUrgency(const void* room);
// A lamp edit's room that is a member of an atrium (rooms joined through removed floors): the scheduler solves it right
// after the lamp's own room whatever its story, so the atrium's stories change together
bool StackedWithEdit(const void* room);
// The first seconds after the world went live, while the loaded lots' rooms correct themselves (LotLightingMotion and
// RoomLightQueue give the room solves a larger share of the frame meanwhile)
bool SettlingAfterLoad();
// Render thread: the room belongs to an atrium (a stacked-ambient group of 2 rooms or more)
bool InAtrium(const void* room);
// Light tree thread: the share (0..1) of a lamp of another story that reaches a point of an indoor room, tested as the room's
// directional maps test it (the floor crossed through a stair opening, the walls on the way); -1 when the lamp is not one
// the light between stories took for that room (ObjectLightBridge gives such lamps to the room's object rigs)
float CrossLampReach(const void* room, const void* light, const float* point);
// Render thread: another member of the room's atrium group is waiting for its gather or its solve, or being solved
// (AtriumHold keeps the room's new maps waiting meanwhile, so the atrium's stories change together)
bool GroupPending(const void* room);
// A lamp edit's rooms are waiting for or in their solve (RoomLightQueue: a larger budget per frame meanwhile)
bool LampEditPending();
// Lamp switches all at once (AtriumHold, render thread): of the rooms a lamp edit marked (the urgent list) on stories their
// lot shows (its story, and an atrium's rooms below it), how many are still gathered or solved or ended no solve begun after
// the switch whose LampMarkFilter::SwitchSerial is `since`;
// `visible` gets how many there are
int SwitchRoomsPending(unsigned long since, int* visible = nullptr);
// This thread is in step 1 of a room's solve, where the game locks the room's maps (AtriumHold keeps only those)
bool InMapLockStep();
// The solve's end and its map lock step are hooked: lamp switches can be shown all at once (else they change room by room)
bool SolveHooksReady();
// Apex's own share of the room solves so far, ms (-1 until calibrated); render thread
double ApexSolveMs();
// From LampMarkFilter: a lamp edit was noted; continuous = it moved or a value changed (a drag), not switched on or off
void NoteLampEditing(bool continuous);
// A lamp is being dragged (moved or a value changed within the last 200 ms). The one-go solve once an edit ended
// (05/10, option A) was taken out the same day: 300-1300 ms frames while moving lamps in Build mode (log 20:33-20:34)
bool LampDragging();
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
// The same notes as data (the recorder's light update summary). event: S ambient step done, W wall pass done, E solve ended
// (its new maps show from the next frame), Q sent by Apex, H held, I / F invalidated. While a recording runs every room is
// noted; otherwise only the rooms taking or giving light through an opening. lot = the story manager's lot id (low half).
struct SolveEvent {
    unsigned long tick;
    char event;
    uint32_t lot;
    int level, id, cls, shown, state;
    bool merged; // a member of an atrium (stacked-ambient group)
};
std::vector<SolveEvent> JournalEventsSince(unsigned long fromTick);
// A tree level's lot id (low half, as SolveEvent::lot) and story; false when unreadable
bool TreeLevelLot(uintptr_t treeLevel, uint32_t& lot, int& story);
std::string Status();
std::string DiagText(); // F8: samples near the active lot's lights, the game's wall test and ours (empties the record; arms the recording)
// Development build: the samples above are recorded only while armed (Developer checkbox, or the first F8 dump arms it)
void SetDiagArmed(bool on);
bool DiagArmed();
// On-demand recorder only: bounded raw wall-edge measurements, no lighting changes.
void BeginSeamRecording();
std::string EndSeamRecording(bool save);
}
