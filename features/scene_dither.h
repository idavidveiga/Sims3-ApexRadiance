#pragma once
// Banding Fix (scene_dither.cpp): the grain in the scene's pixel shaders. Picture's gradient smoothing (deband) follows
// its switch since 30/09 (the Color page's Banding tab holds both).
namespace SceneDither {
bool On();         // the Banding Fix is on
float Strength();  // its grain in 8-bit steps (1 = +-1 step); the AO composite uses the same
float GrainPhase(); // the phase of its grain pattern (0 = still; a new one each frame with Moving grain on)
}
