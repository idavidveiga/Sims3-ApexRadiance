# Changelog

## Unreleased — upstream 2.6.0 integration + validated fork work

**Upstream base:** Apex Radiance final `v2.6.0` (`9ca0b102d4ee0f90f5f4fe406d97ab0f3f5dca9a`)  
**Validated integration build:** workflow run `37414470615` — x86 Release success  
**Detailed Lot Streaming handoff for Luís:** `docs/Luis_Lot_Streaming_Implementation_Guide.md`

### What we integrated

- Updated the fork to the **published final Apex Radiance 2.6.0** rather than the post-release experimental `feature/color-filters` branch.
- Where upstream 2.6.0 and the fork had parallel/intermediate EA 1.69 implementations of the same subsystem, the final upstream implementation was preferred. In particular, `features/level_light_share.cpp` now comes from the official 2.6.0 implementation.
- Preserved fork-only validated work that does not exist in upstream 2.6.0:
  - **Extended Lot Detail** research and production module: validated distance 70-300, tested/recommended 300; capacity 8-16, tested/recommended 16.
  - **Smooth Lot Streaming** integration and S3SS cooperation layer.
  - **Lot Object Throttle** port/adaptation and Apex ownership/integration.
  - **Lot Visibility Override** ownership-safe implementation.
  - Lot Streaming probes, validation logs and A/B documentation.
  - the confirmed **wall/cinema bloom** work.
- Kept the confirmed rule that toggling **Smooth ground light** changes the smoothed/raw draw maps only and does not trigger a general room/lot/wall/object-rig relight.
- Kept S3SS coexistence checks: when official Sims3SettingsSetter owns a corresponding LotStreamingOptimizations subfeature, Apex yields instead of installing a second writer/hook.

### Current Lot Streaming defaults vs validated values

The **validated values** remain 300 distance, 16 maximum detailed lots and camera threshold 5.0.  
The current v2.6.0-integrated patch registrations are nevertheless **opt-in**:

- `Extended Lot Detail`: `enabledByDefault = false`;
- `Smooth Lot Streaming`: `enabledByDefault = false`;
- Map View Blocker, Lot Object Throttle and Visibility Override: opt-in.

The 300/16 values are therefore the defaults **inside Extended Lot Detail when the feature is enabled**, not a statement that the feature is automatically on.

### Wall seam investigation cleanup

- Removed the experimental wall-seam work that was tried after the original baseline:
  - experimental EA WallSolve resolver from that investigation;
  - FacadeT2 / ExactSeam probes;
  - four-state S2/S6 test;
  - WallSeamS6 normalization.
- The final S6 normalization test was rejected because it propagated one wall-light sampler across legitimate, different ExteriorWall draws and visibly worsened the facade.
- **Bloom preservation remains.** The cleanup removed the seam experiments, not the separate wall bloom-alpha correction.

### S3SS provenance

The current comparison is pinned in the Luís handoff against public Sims3SettingsSetter main `5eb2c65bb11e21dac423731c9726627f1fb118ac`, file `patches/lot_streaming_optimizations_patch.cpp`.

- Distance 300, Max Active Lots 16, the metric/cutoff probes and the controlled A/B are Apex research.
- Object Throttle has direct source/algorithm lineage from S3SS and is a port/adaptation into Apex's `GameAddr` + `EntryChain` framework.
- Visibility JZ->JMP, map-view blocking behavior, transition throttle, threshold 12 and camera threshold 5 have S3SS behavior lineage.
- Apex does **not** wholesale copy the S3SS patch framework, `OptimizationPatch`, `PatchHelper`, `DetourHelper`, LiveSetting framework or S3SS UI/config system.
- Apex's map-view implementation deliberately does not use S3SS's `WorldManager::Update` detour.

## Unreleased — Lighting/bloom refinement consolidated

**Detailed technical changelog:** `docs/features/night-lighting-changelog.md`

- Added the second full-day cinema/theatre facade material to the surgical bloom guard: VS `BFFCCC56/1060` +
  PS `4E570819/500`, correlated with the captured night panel material `36F5E915/1296`.
- The cinema guard remains full-day only, requires the exact outdoor object shader path, proves the shader's isolated
  luminance-bloom threshold before changing it, and applies an additional tiny-draw guard to the centre-panel variant.
- The captured night cinema shaders remain untouched.
- Added a dedicated Night Lighting refinement changelog covering the current options, defaults, corrections,
  diagnostics, implementation areas and final validation checklist.
- Added/updated EN / PT-BR / ES / FR translations for the validated Lot Streaming controls, tooltips and feature
  descriptions.
- Reconciled the Lot Streaming changelog with the current production implementation: validated 70-300 distance range,
  8-16 detailed-lot capacity, 300/16 defaults, Smooth Lot Streaming threshold 5 validation, and the threshold-12 control
  retained for development/reference rather than the main menu.

## Unreleased — Cinema marquee daytime bloom guard

**Status:** testing  
**LOD base:** validated Extended Lot Detail branch (300 distance / 16 detailed lots)

### What changed

- The exact full-day cinema/theatre marquee shader pair captured on EA 1.69 is now recognised by stable bytecode IDs.
- In full daylight only (`g_night <= 0.01`), that confirmed shader pair keeps its RGB/depth/stencil output but receives a temporarily raised bloom-threshold constant, forcing only its bloom alpha to zero.
- The guard also requires the outdoor object rig mode and the captured threshold register to be in the normal TS3 range before acting.
- At twilight/night the guard does nothing. The cinema selects different lamp-enabled pixel shaders at night, so its night appearance remains entirely vanilla.
- The Lighting + Bloom census now attributes this path as `CinemaMarqueeDayBloomGuard`, and the object status counts suppressed daytime marquee draws.

### Evidence

F7 on the visibly glowing cinema marquee measured the same pixel twice and identified the final object draw as VS `BFFCCC56/1060` + PS `D5ED0EF3/864`. The PS computes bloom alpha as `saturate(luminance - c4.x)`; the capture had `c4.x = 1.3`, and c4 is used only for that final alpha expression. Day census showed that pair only as an unmodified outdoor-rig path, while the same object used different, larger pixel shaders at night.

## Unreleased — Wall lighting keeps vanilla bloom alpha

**Status:** testing  
**LOD base:** validated Extended Lot Detail branch (300 distance / 16 detailed lots)

### What changed

- Exterior wall/foundation RGB keeps the configured Apex wall-light gain.
- For opaque ExteriorWall draws, bloom alpha is now written once with the game's original wall-light scale, then RGB is written with Apex's boosted scale.
- The alpha-only pass cannot write depth or stencil; the RGB pass remains the authoritative geometry draw.
- Blended/unsupported wall draws deliberately fall back to the previous one-pass behaviour rather than changing their compositing semantics.
- Developer wall status now reports how many draws preserved vanilla bloom alpha and how many used the fallback.
- No global bloom threshold, roof lighting, ground lighting, object lighting, Advanced Rendering option or LOD value was changed.

### Why

The day/night bloom probes showed the wall/foundation region saturating the night bloom mask while the attributed census identified `ExteriorWallGain` on every captured wall draw. The game's ExteriorWall shaders derive bloom alpha from final luminance, so multiplying the baked lamp term also multiplied the apparent bloom area. This keeps the RGB correction but decouples Apex's wall gain from bloom.

## Unreleased — Object-light boost follows the real night level

**Status:** testing  
**LOD base:** validated Extended Lot Detail branch (300 distance / 16 detailed lots)

### What changed

- The extra `ObjectLightBridge` ground-footprint boost now follows the live night level.
- At full daylight (`g_night <= 0.01`), Apex leaves the game's object-light record untouched.
- During dawn/dusk, only Apex's added boost is multiplied by the night level.
- At full night (`g_night = 1`), the boost has exactly the same configured strength as before.
- General object rigs are re-gathered when the night blend changes by about 0.1 and at the exact day/night boundaries, rather than every frame.
- Toggling **Smooth ground light** no longer requests any object-rig refresh. The switch changes only the smoothed/raw maps consumed by draw paths.
- No global bloom threshold, wall gain, roof strength, Advanced Rendering setting or LOD value was changed.

### Why

The attributed draw census showed the previously fixed `OutdoorObject` draw path at 0 Apex claims in daylight while bloom was still visible on lamps, signs and props. The object-light bridge modifies the light records before those vanilla draws and previously had no day/night guard, so this change isolates that pre-draw boost without changing night rendering.

## Unreleased — Lot Streaming production integration

- The validated Lot LOD probe results are now implemented as a normal production feature instead of requiring a diagnostic build.
- Added **Extended Lot Detail** with persistent controls:
  - **Lot detail distance**: validated range 70..300, default 300.
  - **Maximum detailed lots**: validated range 8..16, default 16.
- Production writes use the same ownership discipline proven by the probes: capture the live WorldManager baseline, guarded writes, reassert only the captured game baseline, yield to unexpected third-party values, and restore only values Apex still owns.
- **Smooth Lot Streaming** is validated as the recommended companion behavior (native transition throttle + camera-speed threshold 5), but the current v2.6.0-integrated registration remains opt-in (`enabledByDefault = false`).
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
- It no longer requests an object-rig refresh; the switch changes only the smoothed/raw maps consumed by draw paths.
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

### Current production state

- **Extended Lot Detail**, when enabled, exposes the validated distance range 70..300 (control default 300, 10-unit steps) and capacity 8..16
  (control default 16). The feature registration itself is currently opt-in.
- **Smooth Lot Streaming** is validated as the normal companion behaviour: native transition throttle + camera threshold 5.
- The threshold-12 control is retained only for development/reference and is not shown in the main Performance menu.
- Object/flora visual-distance research is intentionally separate from this Lot Streaming pass.

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
