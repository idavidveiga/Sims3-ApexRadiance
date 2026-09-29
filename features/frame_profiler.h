#pragma once
// Frame-hitch profiler (Apex Radiance). Measures, on the player's machine, what each frame and each
// hitch is made of: frame time from the Present hook, timed wrappers around the game functions suspected of hitching
// (lot streaming / lot LOD, lot lighting, terrain, script GC, the game's render and present path, the frame limiter),
// D3D9 call counts and the time the mod's own D3D9 registry hooks take per frame. A hitch is a frame longer than
// max(multiplier x median of the last 120 frames, floor). Results: live UI (graph, percentiles, per-category breakdown of
// the last hitches) and ApexRadiance_Hitches.txt in the Apex Radiance folder, appended in batches at most once per second from a
// background thread.
//
// Optional statistical sampling (Advanced): a sampler thread records where the render / simulation thread is (EIP, TS3W
// call sites on its stack) ~2000 times a second, so each hitch also shows which code its untimed time was spent in.
//
// Counters (research\perf2\plan.md section 8): resource lookups (FindProvider), scene pending nodes, RefPack compression,
// DXT encoding, object lookups by ID and lot room solves, each with calls / ms per thread bucket, the longest call and an
// extra count; every hitch also names its dominant cause. Two measurement presets set the recommended options.
//
// Off (the default) = nothing is hooked: no detours, no registry hooks, no thread. See frame_profiler.cpp for the
// verified addresses, the timing rules (exclusive / inclusive, thread buckets) and the known limitations.
#include <cstdint>
#include <string>

namespace toml {
inline namespace v3 {
class table;
}
} // namespace toml

namespace FrameProfiler {

#ifndef S3SS_PUBLIC // development build

// Turns the profiler on (registers the D3D9 hooks, starts the file writer; the timed game functions are resolved and
// attached at the next frame boundary, on the render thread) or off (detaches and unregisters everything; the collected
// statistics stay visible until Clear). Thread-safe.
void SetEnabled(bool on);
bool IsEnabled();

// The profiler section of the menu (checkbox, live line, frame-time graph, percentiles, last hitches, Advanced).
void RenderUI(bool showEnable = true); // showEnable = false: the caller draws the on/off switch

// Settings under [qol.frame_profiler]
void SaveToToml(toml::table& qolTable);
void LoadFromToml(const toml::table& qolTable);

// Detaches everything (call before the D3D9 hooks are cleaned up at shutdown). Same as SetEnabled(false).
void Shutdown();

// ---- per-hook timing in the D3D9 hook registry (framework/d3d9_hooks.cpp) ----
// The registry times each draw-call callback while RegistryHookTimingActive() (a relaxed atomic load, true only while the
// profiler is on and the Advanced option "Per-hook registry timing" is checked) and reports it here, one call at a time
// (the registry serialises AddRegistryHookTime with its own lock).
bool RegistryHookTimingActive();
uint64_t Ticks();
void AddRegistryHookTime(const std::string& hookName, uint64_t ticks);

#else // public build: the profiler is not compiled; [qol.frame_profiler] is left as it is in ApexRadiance.toml

inline void SetEnabled(bool) {}
inline bool IsEnabled() { return false; }
inline void RenderUI(bool = true) {}
inline void SaveToToml(toml::table&) {}
inline void LoadFromToml(const toml::table&) {}
inline void Shutdown() {}
inline bool RegistryHookTimingActive() { return false; }
inline uint64_t Ticks() { return 0; }
inline void AddRegistryHookTime(const std::string&, uint64_t) {}

#endif

} // namespace FrameProfiler
