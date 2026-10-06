#pragma once
// Faster room lighting (Apex Radiance, feature "RoomLightQueue"; docs/features/performance.md, "Room lighting queue").
//
// The game relights the rooms of every loaded lot through ONE queue: the scheduler 0x006C5C20 (at the end of the
// per-frame root update, after the rooms' gathers) makes one room current ([lightMgr+0xD4]+0x74) only when none is, the
// lot lighting pass solves it within its lot's budget, and the next room is picked only on the next frame. A room climbs
// LOD class 0 -> 1 -> 2 through three full solves, and every pending first solve of every lot (priority 10000) goes
// before any upgrade of the viewed story (1000 / 100). Measured 29/09: 12-36 s from entering a lot to the last solve,
// with only 1.2-2.8 s of solve work in it. This feature, on the Steam build:
//   1. Priority: the only CALL of the priority function (0x006A81DF -> 0x0069E770) goes through Apex; the rooms of the
//      priority lot (the one the game gives 15 ms, SceneObjectManager +0x10D0 / +0x10E0) get x1000, rooms on the camera's
//      story x4 and rooms below it x2 (they are seen from above), so the viewed lot and story go first.
//   2. No middle step: a finished class-0 solve steps straight to the room's target class (0x0069EAA2: mov edi,1 ->
//      mov edi,eax), so class 2 needs 2 solves instead of 3.
//   3. Requeues keep the class: a room already solved once goes straight to its target class when it is invalidated
//      (the jl at 0x0069EF58 and 0x0069F1C5), instead of starting the ladder from class 0 again.
//   4. More than one room per frame: the scheduler's tail jump (0x006C5E39) goes through Apex, which, on the render
//      thread, solves the new current room at once when it belongs to the priority lot and picks the next one, until a
//      small budget of its own is spent (4 ms, 1 ms while the camera moves).
#include <string>

namespace RoomLightQueue {
bool Start(std::string* error);
void Stop();
bool Running();
std::string StatusText();
// Quick pass when many lamps switch (06/10): while LampMarkFilter::MassSwitchActive, a lamp edit's room still waiting for
// its solve at a class above 0 goes back to class 0 (the game's own fast first solve, ~1/20 of class 2), so every room of
// the switch takes its new light within a few frames, the camera's story first; the game then refines it to its class.
void SetQuickPass(bool on);
bool QuickPass();
// Development build: status lines
void RenderDeveloperUI();
}
