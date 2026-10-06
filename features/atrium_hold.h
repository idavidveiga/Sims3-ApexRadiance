#pragma once
// An atrium's stories change together (06/10; part of Faster room lighting, not an option).
//
// A double-height room is one room per story (LevelLightShare's stacked-ambient groups), each solved on its own. When a lamp
// of an atrium switched, the lamp's story changed first and its wall showed a step at the floor line against the story above
// for about a second, until that story's solve ended (video 12:00, user: "some corners look right, then wrong again").
//
// The room solves write the story maps (wall atlas, floor and ceiling maps, room light map, basis maps: MANAGED single-level
// A8R8G8B8 textures, docs/engine/room-light-maps.md) through LockRect / UnlockRect; both are detoured. While a lamp edit is
// pending (LevelLightShare::LampEditPending or LampMarkFilter::MassSwitchActive), a map an atrium room's solve writes is kept
// (AddRef, two buffers):
//   - at the game's UnlockRect, while another room of the same atrium is still waiting for or in its solve
//     (LevelLightShare::GroupPending), what was on screen goes back and the new content waits; otherwise it stays as the
//     game wrote it;
//   - at the game's LockRect of a waiting map, the exact content is put back first, so the game never reads or builds on
//     what is shown (the wall blur reads the atlas);
//   - every frame (Present, render thread): the waiting maps of an atrium whose rooms are all solved, or that waited
//     kHoldMs, take their content in the same frame.
// A refinement (a quick-pass room solved again at its own class) writes the maps of another class, not the ones on screen:
// they never wait. A map the game holds locked is never written by Apex; a dragged lamp's rooms never wait. Buffers exist
// only while a map waits (and 3 s after). The 250 ms fade this module began with (06/10, "Smooth light changes indoors") was
// removed the same day: the user preferred the light changing at once.
#include <d3d9.h>
#include <string>

namespace AtriumHold {
// Render thread (Present): installs the texture lock hooks once (false: not available, logged), then releases the maps
// whose atrium is solved
void OnPresent(IDirect3DDevice9* dev);
// Releases every kept map, the waiting ones with their content (device reset, uninstall)
void Clear();
std::string Status();
}
