# Changelog

## Unreleased — Lot Streaming production integration

- The validated Lot LOD probe results are now implemented as a normal production feature instead of requiring a diagnostic build.
- Added **Extended Lot Detail** with persistent controls:
  - **Lot detail distance**: validated range 70..300, default 300.
  - **Maximum detailed lots**: range 1..16, default 16.
- Production writes use the same ownership discipline proven by the probes: capture the live WorldManager baseline, guarded writes, reassert only the captured game baseline, yield to unexpected third-party values, and restore only values Apex still owns.
- **Smooth Lot Streaming** is now considered validated and defaults on for new configurations: native transition throttle + camera-speed threshold 5.
- The final controlled A/B reduced Detailed View transitions from 149.3/min to 99.2/min, same-lot reversals within 5 s from 87 to 18, and reversals within 2 s from 48 to 3.
- Removed the misleading **Use LoD active-lot threshold 12** row from the main Performance menu. The diagnostic/internal feature remains available to development code; the live value was already 12 in the tested game before Apex wrote anything.
- Split the Performance menu into **Lot detail streaming** and **Object streaming** so lot eligibility/capacity is no longer mixed with object creation throttles.
- Diagnostic Metric Probe remains compile-time-only and is not included in the normal implementation build.

## Unreleased — Smooth ground light no longer relights walls

**Status:** testing  
**LOD base:** current `feature/lot-lod-streaming` 300x16 refinement branch

### What changed

- Toggling **Smooth ground light** no longer runs the generic F9-style lighting refresh.
- The switch still changes the smoothed ground-light maps live.
- It now refreshes only object rigs, which are the live consumers relevant to the original bloom A/B.
- It no longer forces terrain, lot stories, rooms or exterior walls to be re-solved.
- The daytime object-ground-light guard remains unchanged: full daylight uses the game's normal object lighting; twilight fades the added ground term with `g_night`; full night keeps the original strength.
- No global bloom, wall brightness, Advanced Rendering, RGB lighting or LOD value was changed.

### Why

The night regression log showed that an A/B of **Smooth ground light** triggered `RefreshAll`, which rebuilt terrain, lots and every room at `g_night = 1`. The accompanying census showed every captured `ExteriorWall` draw being claimed at night. The switch itself is only a smoothing/filter choice, so it should not cause wall/room relighting.

## Unreleased — Lot Streaming 300 + 16 validated baseline

**Status:** refinement/testing  
**Detailed technical changelog:** `docs/features/lot-streaming-changelog.md`

### Validated

- Lot LOD distance is a native eligibility radius stored at `WorldManager+0xDC`; the controlled metric follows the
  squared-distance cutoff (`200 -> ~40,000`, `300 -> ~90,000`).
- **300** is the current validated distance baseline.
- `WorldManager+0xE4` is the independent **Max Active Lots** capacity. Raising it **8 -> 16** produced 16 simultaneous
  Detailed View lots in the same dense reference area that previously saturated at eight.
- Active Lot Bias remains 8.0 and is not being tuned yet.
- The 300 + 16 run remained stable in the captured session; further worlds/saves are still part of validation.

### Provenance

- Distance 300, Max Active Lots 16, the metric probe and the squared-distance validation are Apex research.
- The native game fields/functions are EA code.
- S3SS lineage remains explicitly credited where applicable: transition-throttle settings, visibility override,
  map-view blocking concept and the object throttle.
- The object throttle is a port/adaptation of S3SS's LotStreamingOptimizations implementation; the standalone Apex
  framework/integration is rewritten and adds its own validation, ownership and restoration behavior.

### Next refinement

- transition threshold 12 with a real 16-lot capacity;
- camera speed threshold 5 vs native 32;
- visibility override A/B;
- then production/menu integration of the final values.

## Unreleased — Object ground light follows night level

**Status:** testing  
**LOD base:** `e0a954ed7b1451fc9c05a44b25f6bf5fe67cc914` — LOD 300 + Max Active Lots 16 + bloom diagnostics

### What changed

- `Smooth ground light` remains active for terrain, roads, lots and floors during the day.
- Outdoor rig objects that consume the smoothed ground-light atlas no longer enter Apex's replacement path in full daylight (`g_night <= 0.01`).
- During dawn/dusk, only the added ground-atlas term on those objects is multiplied by `g_night`.
- At full night (`g_night = 1`), object ground-light strength is unchanged.
- Snow on objects/stair relief now follows the same day/night rule; fences/stairs already had this guard.
- No global bloom threshold, Advanced Rendering setting, terrain smoothing or RGB lighting was changed.

### Why

The in-game A/B test showed that toggling **Smooth ground light** could remove the unwanted object bloom, but disabling it globally also removes the desired smoothed ground lighting. The fix therefore keeps the smoothed map and stops only its object consumers from replacing normal daytime object lighting.

## Unreleased — Bloom Alpha Probe

**Status:** diagnostic/testing  
**LOD base:** `08a1bac1fe53586207437c42bf0ac08d06ec506e` — controlled Lot LOD 300 + Max Active Lots 16

### What changed

- Added **Capture bloom alpha mask** under Developer → Lighting → Census.
- The probe captures the raw A8R8G8B8 scene alpha at the PostScene boundary, before the game's first depth-disabled draw (normally the bloom composite / UI).
- It is read-only: no shader constants, lighting values, render states or output RGB are changed.
- The capture is tagged automatically as Day, Twilight or Night from the current night level.
- It writes:
  - `ApexRadiance_BloomAlphaProbe_Day.png/.txt`
  - `ApexRadiance_BloomAlphaProbe_Twilight.png/.txt`
  - `ApexRadiance_BloomAlphaProbe_Night.png/.txt`
- The PNG is the actual scene alpha as grayscale: black = alpha 0, white = alpha 255.
- The TXT records alpha min/max, mean, percentiles, threshold counts and a 16-bin histogram.
- One short frame hitch is expected while the GPU render target is copied to system memory.

### Purpose

Use one capture in daylight and one at night, with the same camera, together with `ApexRadiance_LightingBloomCensus.txt`.  
The census identifies which draw families can write bloom; the alpha probe shows how much bloom mask actually exists on screen.

## Unreleased — Lighting + Bloom Census diagnostic

**Status:** diagnostic/testing  
**Base with current LOD work:** `8e59f6fb10219fde0cd6041559a91417c9eb2f74`

### What changed

- Added a read-only developer diagnostic named **Lighting + Bloom census**.
- It records visible draw families for 3 frames and writes `ApexRadiance_LightingBloomCensus.txt`.
- For each captured row it records:
  - draw path and rig mode;
  - whether Apex claimed/replaced the draw;
  - separate day, twilight and night counts using `g_night`;
  - representative world position when available;
  - shader hashes/sizes and light-map textures;
  - known bloom-mask families (walls, objects, roofs) and possible instanced-structure candidates.
- The diagnostic does **not** change RGB lighting, bloom strength, shader constants or the rendered image.

### Files changed

- `features/lot_light_bridge.cpp`
- `features/lot_light_bridge.h`
- `patches/night_terrain_relight_patch.cpp`

### Output interpretation

Rows with `day/fixed > 0` identify draw families that Apex is still modifying in full daylight.  
Rows marked `bloom=YES` or `bloom=POSSIBLE` identify the first families to investigate for the game's bloom mask.

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
