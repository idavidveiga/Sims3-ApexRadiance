#pragma once
// Faster Create-a-Style: caches the native pattern thumbnails requested by UI.dll's CASt browser.
//
// UI.dll -> CompositorUtil.GetPatternThumbnail -> SimIFace.ObjectDesigner.GetPatternThumbnail ->
// IWorld.ObjectDesigner_GetPatternThumbnail (Mono internal call). The managed CASt grid can request the same pattern
// preview repeatedly while it is populated, scrolled or reopened. This module remembers the finished byte[] result for
// an identical request and copies it back instead of rebuilding the thumbnail.
//
// The hook is installed at mono_lookup_internal_call, not in UI.dll: the official game assemblies stay untouched.
// Existing methods that already resolved to our wrapper remain safe when the switch is turned off; the wrapper then
// passes straight through to the game's native function.
#include <string>

namespace FastCreateAStyle {

// Shared resolver: a second client may monitor additional Mono internal calls without detouring twice.
bool AcquireResolver(std::string* error);
void ReleaseResolver();
bool Start(std::string* error);
void Stop();
bool Running();
std::string StatusText();

} // namespace FastCreateAStyle
