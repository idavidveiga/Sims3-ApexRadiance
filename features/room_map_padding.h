#pragma once
// The game's room light maps and their directional maps, as the indoor draws bind them (part of Night Lighting). See
// room_map_padding.cpp.
#include <d3d9.h>
#include <cstddef>
#include <string>

namespace RoomMapPadding {
void SetEnabled(bool on);
// The pixel shader reads the 4 directional room light maps ("LightBasisMap0..3": a def of (0.8944, 0.4472, 0, -0.8944)
// used by dp2add): its draws bind a room light map and its 4 directional maps.
bool IsBasisPs(const DWORD* code, size_t tokens);
// A draw with such a pixel shader (render thread): remember which directional maps go with its room light map.
void NoteDraw(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps);
// Once per frame (render thread): releases the sets no draw bound for a while.
void OnPresent();
// The 4 basis maps (+X, -X, +Z, -Z) of a room light map, as the last basis-reading draw bound them together. False when
// not known (or off).
bool BasisFor(IDirect3DTexture9* lightMap, IDirect3DTexture9* out[4]);
// Releases every remembered map (world change, uninstall).
void Clear();
std::string Status();
}
