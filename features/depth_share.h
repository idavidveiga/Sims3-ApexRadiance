#pragma once
// Access to the readable scene depth (INTZ texture that replaces the game's depth-stencil), owned by the Depth Blur
// module. The swap runs while Depth Blur is on or another effect requested it.
#include <d3d9.h>
#include <string>
namespace DepthShare {
IDirect3DTexture9* Texture();  // null when the swap is off or not ready
IDirect3DSurface9* Surface();  // level 0 of Texture(), the surface bound as depth-stencil while the scene renders
// Mark a draw issued by another patch (render thread). The post-scene effects ignore it: e.g. the lake lamp pass turns
// ZENABLE off and unbinds the depth-stencil, which would otherwise look like the first UI draw and trigger them mid-frame.
void SetInternalPass(bool on);
bool InternalPass();
// Keep the depth swap running for another effect even with Depth Blur off (reference counted).
void Request(bool on);
std::string Status(); // why Texture() is null, for the requesting effect's status line
}
