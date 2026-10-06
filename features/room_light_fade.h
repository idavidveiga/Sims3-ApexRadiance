#pragma once
// Smooth light changes indoors (06/10, user: "a blink right after switching all the lights on or off").
//
// The room solves write the story maps (wall atlas, floor and ceiling maps, room light map, basis maps: MANAGED single-level
// A8R8G8B8 textures, docs/engine/room-light-maps.md) through LockRect / UnlockRect, so a room's new light appears from one
// frame to the next, and the quick pass of "Quick update when many lamps switch" shows a coarser solve first. While a lamp
// edit is pending (LevelLightShare::LampEditPending or LampMarkFilter::MassSwitchActive), this module fades each such map
// from what was on screen to its new content over kFadeMs:
//   - the game's LockRect of a map: the exact content (the solve's own result) is put back first, so the game never reads
//     or builds on a blend (the wall blur reads the atlas);
//   - the game's UnlockRect: the new content is kept as the target and what was on screen is put back;
//   - every frame (Present, render thread): the blend of the two is written, until the target is reached exactly.
// A map the game holds locked is never written by Apex. Buffers exist only while a map fades (and 3 s after), each map is
// AddRef'd while kept. Nothing at all happens when the option is off or no lamp edit is pending.
#include <d3d9.h>
#include <string>

namespace RoomLightFade {
// Render thread (Present): installs the texture lock hooks once (false: not available, logged), then advances the fades
void OnPresent(IDirect3DDevice9* dev);
void SetEnabled(bool on);
bool Enabled();
// Releases every kept map (device reset, uninstall)
void Clear();
std::string Status();
}
