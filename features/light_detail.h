#pragma once
// Light detail (part of Night Lighting, 2026-10-05): how many lighting texels the game gives walls and floors (see
// light_detail.cpp).
#include <string>
namespace LightDetail {
// The detail chosen for this session, applied once before any lot is lit: 0 = the game's own, 1 = twice the texels per
// metre on walls and floors. False when it could not be applied (the status says why); the game's own detail stays.
bool ApplyAtStartup(int level);
// The detail in effect this session (0 = the game's own); a change made later takes effect after a restart
int Active();
std::string Status();
// Once per session, at the end of a room's solve (LevelLightShare's FinalizeHook): whether every tile of its story has its
// class-2 floor and ceiling texels at n x the tile or in the atlas, none over a neighbour's (one log line: OK, or the first
// tile that overlaps)
void CheckLayout(const void* room);
}
