#pragma once
// Per-lot object streaming throttle, ported from Sims3SettingsSetter's LotStreamingOptimizations objectThrottle.
//
// This is earlier in the pipeline than SceneBudget ("Spread New Objects Over Frames"): it intercepts
// Lot::AddLotObjectsToScene and builds only a small number of regular lot objects per continuation window.
// Large/flora objects (building/apartment shells and exterior geometry) are always built in the first window because
// Lot::SetActiveImpl has one-shot fixups immediately after AddLotObjectsToScene returns that require those objects to
// already have scene presence.

#include <string>

namespace LotObjectThrottle {

inline constexpr int kDefaultObjectsPerWindow = 2;
inline constexpr int kDefaultDelayMs = 16;

bool Start(std::string* error = nullptr);
void Stop();
void Tick();

bool Running();
bool HandledByS3SS();
std::string StatusText();

void SetObjectsPerWindow(int value);
void SetDelayMs(int value);
int ObjectsPerWindow();
int DelayMs();

} // namespace LotObjectThrottle
