# Changelog

## Unreleased — Fence/stair ground light follows night level

**Status:** testing  
**Code change:** `d5fac4b500127dc5be39eb879e3473f102ac5429`  
**Previous baseline:** `0f89c788e2ed23c5b498ff67e7ca08f24a6835e0`

### What changed

- File: `features/lot_light_bridge.cpp`
- Function: `DrawInstanced()`
- Scope: fences, railings, posts and stairs that sample the ground-light atlas.
- The special ground-light contribution is skipped while `g_night <= 0.01f`.
- During dusk/dawn, the configured fence/stair strength is multiplied by `g_night`, so the contribution fades in and out with the game's night level.

### Previous behavior

The path only checked whether the fence/stair fix was enabled:

```cpp
if (!g_fenceFix.load(std::memory_order_relaxed)) return false;
```

The strength was always applied at its full configured value:

```cpp
const float s[4] = {g_fenceStrength.load(std::memory_order_relaxed), 0, 0, 0};
```

Effective value:

```text
g_fenceStrength
```

### Current behavior

```cpp
if (!g_fenceFix.load(std::memory_order_relaxed)) return false;
const float night = g_night.load(std::memory_order_relaxed);
if (night <= 0.01f) return false;
```

The strength now follows the night level:

```cpp
const float s[4] = {g_fenceStrength.load(std::memory_order_relaxed) * night, 0, 0, 0};
```

Effective value:

```text
g_fenceStrength * g_night
```

### Exact rollback

To restore the previous behavior:

1. Remove these lines from `DrawInstanced()`:

```cpp
const float night = g_night.load(std::memory_order_relaxed);
if (night <= 0.01f) return false;
```

2. Replace:

```cpp
const float s[4] = {g_fenceStrength.load(std::memory_order_relaxed) * night, 0, 0, 0};
```

with:

```cpp
const float s[4] = {g_fenceStrength.load(std::memory_order_relaxed), 0, 0, 0};
```

Reverting commit `d5fac4b500127dc5be39eb879e3473f102ac5429` also restores only this code change.

### Unchanged by this correction

- `LightmapSmooth`
- terrain/ground smoothing
- lot-light baking
- snow-on-fence/stair paths
- other Night Lighting object paths

## 2.5.4 — 2026-10-02

- Prioritize known visible-lamp colour, intensity and activation changes, including small intensity adjustments and repeated switches.
- Refresh newly visible lots through bounded local terrain updates and clear disabled lot-owned lamp contributions without waiting for fade completion.
- Correct the verified summer multi-pass terrain-light path so lighting remains consistent across lot/world boundaries.
- Coalesce rapid edits and retain bounded terrain scheduling, coordinated floor lighting and Rooms at Night recovery.
- Provide page/whole-mod defaults with confirmation and Undo, preserving saved captures, reports and profiles.
- End object-point capture after one click, return to the Report panel after completion and align routine notices at the top center.
- Translate the updated shared controls into English, Portuguese, Spanish and French. Developer binaries remain private.

The maintainer accepted the latest private lighting-response build and reported improved perceived performance. This report does not provide instrumented latency/FPS figures or validate unverified shader variants. See the feature documentation for scope and testing.
