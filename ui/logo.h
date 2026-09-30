#pragma once
// The mod's logo (ui/apex_logo.png; pixels in ui/logo_data.h) as a D3D9 texture for the menu header and the start note.
// Made on first use on the render thread (managed pool, so it survives a device reset), with its mip levels built from the
// 128x128 image (alpha-weighted box filter: no dark fringe at the round edge), and drawn with linear mip filtering.
#include <d3d9.h>
#include "imgui.h"

namespace ApexUi {

void SetLogoDevice(IDirect3DDevice9* device); // Overlay::Init
void ReleaseLogo();                           // Overlay::Shutdown, before the device goes
// Draws the logo into [min, max] of `dl`, `alpha` 0..1; false when the texture could not be made (draw a fallback)
bool DrawLogo(ImDrawList* dl, ImVec2 min, ImVec2 max, float alpha = 1.0f);

} // namespace ApexUi
