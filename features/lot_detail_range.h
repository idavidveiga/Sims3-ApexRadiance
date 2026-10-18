#pragma once
// Production lot-detail range/capacity control.
// Applies validated WorldManager values without the diagnostic metric probe:
//   +0xDC Lot LOD distance
//   +0xE4 Max Active Lots
// Values are captured per live WorldManager, guarded before writes, maintained only while Apex still owns them,
// and restored on a clean disable/unload.
//
// EA 1.69 in-game validation baseline: distance 300, max detailed lots 16.
// Experimental branch: default 400 / 32, with optional distance up to 600; unvalidated in game.

#include <string>

namespace LotDetailRange {

inline constexpr int kDefaultDistance = 400;
inline constexpr int kDefaultMaxActiveLots = 32;
inline constexpr int kMinDistance = 70;
inline constexpr int kMaxDistance = 600;
inline constexpr int kDistanceStep = 10;
inline constexpr int kMinActiveLots = 8;
inline constexpr int kMaxActiveLots = 32;

bool Start(int distance, int maxActiveLots, std::string* error = nullptr);
void Stop();
void Tick();
bool Running();
std::string StatusText();

} // namespace LotDetailRange
