#pragma once
// One trigger for the effects that work on the finished 3D scene, before the game draws any UI: the first backbuffer
// draw with ZENABLE = FALSE after at least 20 depth-tested backbuffer draws (the bloom composite, then the UI). The
// effects run there in a fixed order (edge smoothing, then Depth Blur); each saves and restores what it touches.
// Effects draw with DrawPrimitiveUP, which is not hooked, so they never re-trigger it.
// Its draw hooks run at Priority::First; Picture's scene copy runs after them (picture.cpp) so it contains the effects.
//
// TODO(post-0.1.0 review): interiors have depth-off back buffer draws in the middle of the scene, so this FIRST
// depth-off draw can come before the scene is finished (Picture re-copies at every depth-on -> depth-off transition for
// that reason). Kept exactly as v0.1.0 for now so the image does not change; revisit with in-game tests.
#include <d3d9.h>

namespace PostScene {
using Effect = void (*)(IDirect3DDevice9*);
enum Order : int { kEdgeSmoothing = 20, kDepthBlur = 30 }; // 10 was the removed SSAO / Ambient Occlusion
void Add(int order, Effect fn); // registers the draw hooks with the first effect
void Remove(Effect fn);         // and unregisters them with the last one
}
