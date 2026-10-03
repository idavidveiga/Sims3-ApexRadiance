#pragma once
// Smooth lot streaming: uses the game's own native Lot LoD transition throttle and camera-speed threshold.
// No detours and no replacement of the lot loader in this stage.
//
// The EA 1.69.47 addresses were verified in-game by the read-only probe on 2026-10-02. Other non-Steam builds
// still use GameAddr signatures and remain fail-closed when the group cannot be resolved.

#include <string>

namespace LotLodStreaming {

inline constexpr float kCameraThreshold = 5.0f;

// Enables the native "Throttle Lot LoD Transitions" byte and applies WorldManager+0xEC = 5.0 when a world exists.
// If official Sims3SettingsSetter already owns LotStreamingOptimizations.streamingSettings, Apex makes no writes
// and reports the feature as handled by S3SS.
bool Start(std::string* error = nullptr);

// Restores only values Apex actually changed, and only while the current value still equals Apex's applied value.
// This avoids overwriting a later change made by the game or another mod.
void Stop();

// Called by Apex's existing pump. Detects a newly-created WorldManager and applies the threshold to that world.
void Tick();

bool Running();
bool HandledByS3SS();
std::string StatusText();

// Separate map-view blocker stage. While map view is open (plus a 1 s exit grace), Apex sets the live
// WorldManager+0x258 "skip lot streaming" gate without detouring WorldManager::Update. The previous byte is restored
// safely when the grace ends or the feature is switched off. If S3SS owns LotStreamingOptimizations.mapViewBlocker,
// Apex makes no writes.
bool StartMapViewBlocker(std::string* error = nullptr);
void StopMapViewBlocker();
void TickMapViewBlocker();
bool MapViewBlockerRunning();
bool MapViewBlockerHandledByS3SS();
std::string MapViewBlockerStatusText();

} // namespace LotLodStreaming
