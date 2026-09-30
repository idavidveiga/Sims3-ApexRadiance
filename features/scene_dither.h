#pragma once
// Banding Fix (scene_dither.cpp): the grain in the scene's pixel shaders. Picture's gradient smoothing (deband) follows
// its switch since 30/09 (the Color page's Banding tab holds both).
namespace SceneDither {
bool On(); // the Banding Fix is on
}
