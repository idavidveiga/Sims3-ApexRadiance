#pragma once
// Screen pixels during a recording (06/10, user: "can we build something to measure better what happens when the lights
// update?"). While a recording (F8) runs, the pixel under the mouse when it started and 8 more in a column above and below
// it are read from the back buffer at every frame, without stalling it (StretchRect into a small render target, read back
// a few frames later once an event query says the copy is done). Pointing the mouse at the line between two floors
// measures both stories at once. The recorder writes the samples ("Screen pixels.csv") and, for every lamp edit, when each
// point began to change, when it settled and whether it went the wrong way first (Recording.txt, "Light updates").
#include <d3d9.h>
#include <string>
#include <vector>

namespace ScreenWatch {
constexpr int kPoints = 9;
struct Sample {
    unsigned long tick; // GetTickCount of the frame the pixels were copied in
    unsigned char rgb[kPoints][3];
};
// Render thread, every Present: starts with a recording (the pixel under the mouse then), samples, stops with it
void OnPresent(IDirect3DDevice9* dev);
// The points (back-buffer pixels, the mouse's is the middle one) and the samples of the last recording (render thread)
std::vector<Sample> Samples();
int PointY(int i);
int PointX();
bool Watching();
// Releases the device objects (device lost / reset)
void Release();
}
