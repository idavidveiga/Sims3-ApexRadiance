#pragma once
// One trigger for the effects that work on the finished 3D scene, before the game draws any UI: the first backbuffer
// draw with ZENABLE = FALSE after at least 20 depth-tested backbuffer draws (the bloom composite, then the UI). The
// effects run there in a fixed order (ambient occlusion, edge smoothing, then Depth Blur); each saves and restores what
// it touches.
// Effects draw with DrawPrimitiveUP, which is not hooked, so they never re-trigger it.
// Its draw hooks run at Priority::First; Picture's scene copy runs after them (picture.cpp) so it contains the effects.
//
// TODO(post-0.1.0 review): interiors have depth-off back buffer draws in the middle of the scene, so this FIRST
// depth-off draw can come before the scene is finished (Picture re-copies at every depth-on -> depth-off transition for
// that reason). Kept exactly as v0.1.0 for now so the image does not change; revisit with in-game tests.
#include <d3d9.h>

namespace PostScene {
using Effect = void (*)(IDirect3DDevice9*);
enum Order : int { kAmbientOcclusion = 10, kEdgeSmoothing = 20, kDepthBlur = 30 };
void Add(int order, Effect fn); // registers the draw hooks with the first effect
void Remove(Effect fn);         // and unregisters them with the last one
// The camera, read from the scene draws' vertex constants while an effect asks for it (reference counted)
void WantCamera(bool on);
// The camera's near plane of the current frame (metres). Device depth d = A - near * A / z (A = 1.00008 measured, far
// plane ~3 km); near changes with the camera's zoom and height (0.2 .. 0.3). 0 until a frame was seen.
float CameraNear();
// The camera's view-projection of the current frame (world -> clip, rows = c40..c43), false when not seen this frame
bool CameraViewProj(float vp[4][4]);
// A of the projection (d = A - near * A / z, so view z / near = A / (A - d)); 1.00008 when unknown
float CameraDepthA();
}
