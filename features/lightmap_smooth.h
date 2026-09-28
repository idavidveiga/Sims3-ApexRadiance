#pragma once
// Smoothed terrain light maps (part of the "Night Remake" patch, see lightmap_smooth.cpp).
#include <d3d9.h>
#include <string>
#include <utility>

namespace LightmapSmooth {
using Key = std::pair<int, int>; // chunk centre (x, z), same key as the lot light bridge

void SetEnabled(bool on);
bool Enabled();
// Render thread. Registers the game's light map of a chunk (seen in a world terrain draw) and returns its smoothed
// version, or nullptr while it is not ready.
IDirect3DTexture9* Get(const Key& key, IDirect3DTexture9* original);
// Smoothed version of an already registered chunk, or nullptr.
IDirect3DTexture9* Find(const Key& key);
// World light atlas (all smoothed chunk maps, 2 texels/m) and its mapping: uv = world xz * c.xy + c.zw. nullptr if not ready.
IDirect3DTexture9* Atlas(float c[4]);
void OnPresent(IDirect3DDevice9* dev); // render thread, every frame: detects changed maps, uploads finished ones
void OnPreReset(IDirect3DDevice9* dev);
void Clear();
std::string Status();
}
