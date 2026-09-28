#pragma once
// Game camera state for the Apex effects: is the map view (M, or zooming all the way out) open?
// Part of Apex Radiance.

namespace MapView {
// True while the game's map view is on. False when unknown (function not found on this game version).
bool IsOpen();
// The game function was found and validated
bool Available();
} // namespace MapView
