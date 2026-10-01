#pragma once
// GPU light probe used by Night Lighting ("light capture" on the Report a problem page, both builds).
// Its shortcut (F7 with the F-key set) with the mouse over a pixel: finds every draw call that covers that pixel in the
// next frame (occlusion query on a 1x1 scissored copy of each draw), writes Captures\<date time> Light capture\Light
// capture.txt with the textures those draws use as BMP files beside it (features/captures.h), and (Developer page) lets the
// user replace any of them with black or white to see what it does on screen.
#include <d3d9.h>

namespace LightProbe {
void OnPresent(IDirect3DDevice9* device); // call every frame from the Present hook (render thread)
void RenderUI();                          // ImGui section
void Shutdown();                          // unregister hooks and release references
bool Capturing();                        // the draws of this frame are being recorded (other modules add detail only then)
}
