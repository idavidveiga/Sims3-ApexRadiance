# Water: ponds and lakes (lamp glow and shore reflection)

> **Status in the standalone:** in the v0.1.0 baseline (b84d5f1) as described. v0.1.0's `water_lamps_ps.hlsl` has no
> `params.z` factor (the HDR line was added later), so no port issue there; the pass PS is compiled at the first lake draw
> (no `PrecreatePs`). Since 2026-09-28 it is compiled at start-up on a background thread and only created at the first
> lake draw (`framework/shader_cache.h`, [architecture 4.6](../../architecture.md#shader-precompile)); not tested in game yet.

> The game's pond/lake water gets no lamp light and reflects only a fixed sky cube, so at night ponds are dark and dead.
> Night Lighting draws a second pass on the same water geometry right after the game's water: glints and glow of nearby
> lamps, and (when the scene depth is available) a screen-space reflection of the shore, blended premultiplied. The
> ocean and swimming pools are not touched. Status: working (lamp glow and reflection installed 24/09-25/09; rotated-lake
> and no-depth fixes 25/09). Both build flavours. User-facing controls are in the Apex tab's **Reflections** section,
> see [../reflections.md](../reflections.md).

Related: [roofs.md](roofs.md) (shared lamp list), [../depth-blur.md](../depth-blur.md) (INTZ depth provider),
[../reflections.md](../reflections.md), [../../engine/shaders.md](../../engine/shaders.md).

## Purpose

- `LightProbe-lago` (draw #70): `PS_253F0B20` (ps_3_0) + `VS_253F0CB0`. Samplers: `s0`, `s1` wave normal maps (256x256,
  format 63), `s2`, `s3` sky cubes for the reflection, `s4` colour ramp by depth (512x4 DXT1), `s5` sun shadow, `s6` scene
  copy for refraction (2048x1024, the image before the water). No lamp light map and no rig: brightness comes only from the
  sun (specular + shadow) and the fixed sky reflection. Under it, the lot terrain passes (#2, #7, #14, #16) are drawn with
  the lot bridge active.
- User: water very dark at night, does not reflect the lights; later "reflections of the ocean on the lake".
- Ocean (`LightProbe-oceano` #176): `PS_1AC2FBF0` / `VS_1B0FB6F0`, stencil on; `s0`/`s1` waves, `s2` foam (256x256 DXT1),
  `s3` ramp (512x4), `s4` refraction (2048x1024), `s6` a **real-time planar reflection** (1024x1024 render target): the ocean
  reflects the actual scene every frame, the lake only the sky cube. Neither gets lamp light on the surface.

## User-facing settings

The keys belong to the Night Lighting patch, saved in `[patches.NightTerrainRelight]` of
`Documents\Electronic Arts\The Sims 3\S3SS\S3SS.toml`, but the UI is in the **Reflections** section of the Apex tab
(`ApexRenderReflectionsUI`, `patches/night_terrain_relight_patch.cpp`); it shows "Needs Night Lighting enabled." when the
patch is off.

| UI label (Apex Radiance menu) | TOML key | Type | Default | Range | Notes |
|---|---|---|---|---|---|
| Night Lights > Water > Lamps glow on ponds | `lagosRefletemLampadas` | bool | `true` | - | The lamp glow and glints (the pass runs for it; with it off the pass runs only for the shore reflection, see below). |
| Night Lights > Water > Glow | `brilhoNaAgua` | float | `1.0` | 0.1-3.0 | Lamp glint/glow strength (`params.x`, PS c52.x); 0 is sent while the glow is off. |
| Effects > Water Reflections | `reflexoNoLago` | float | `1.0` | 0.0-3.0 | Reflection strength (`reflParams.x`, PS c58.x); the card's switch sets 0 / restores the last value. Needs Night Lighting on, Depth Blur on and the game's Edge Smoothing (MSAA) off. |

"Reset Night Lights" restores these with every other Night Lighting option (`ResetDefaults`). Values are read live every
frame (`LotLightBridge::SetWaterFix` in the Present hook); no reinstall. Since 2026-09-28 the pass also runs with the glow
off when the shore reflection is above 0 and the scene depth exists (`DepthShare::Texture()`), with lamp strength 0: the
reflection no longer depends on the glow switch (see [../reflections.md](../reflections.md)).

## How it works

### Recognition and order

- Exact ids (`shader_ids.h`): PS `kLakePs` {1344 bytes, FNV-1a `0x4F52846A`} (`PsClass::Lake`) and VS `kLakeVs` {1088,
  `0x23CCB61B`} (VS class 2). They replaced the byte arrays of the old `lake_ref.h`.
- In `OnDrawInner` the lake branch runs before the Night Lighting bridge gate, so water works even with "Street lamps light
  inside lots" off. The replacement PS is precreated when the game creates the lake PS (`PrecreatePs`).

### `DrawLake` (lot_light_bridge.cpp)

1. Draw the game's water unchanged.
2. `SelectLamps(worldT.x, worldT.z, 150)`: the 16 lamps with the lowest (horizontal distance - radius) within 150 m of the
   water mesh's translation (VS `c8.w`, `c10.w`), from the shared list (see [roofs.md](roofs.md): lit street lamps and
   outdoor lamps, radius `clamp(1.2 sqrt(range), 2, 25)`, colour `F0 x I x fade`, refreshed every 20 frames).
3. Build PS constants c20..c59 (below). The VS's world-view-projection (VS c4..c7) is turned into world -> clip:
   `WVP x inverse(World)` (3x3 inverse of the world rows c8..c10 plus translation), because the water mesh of a rotated lot
   has a rotated world matrix (e.g. 0.5 / 0 / 0.866) and subtracting the translation alone projected wrongly ("lago
   girado"). `c57` (worldT) stays 0 in that case; if the matrix is singular, fall back to the raw WVP with `c57` = translation.
4. Depth: `useDepth` only when Depth Blur's INTZ exists (`DepthShare::Texture()`) AND the currently bound depth-stencil is
   Depth Blur's surface (`DepthShare::Surface()`). Then `c59 = (A, B, 1)` with device `z = A + B / w`,
   `A = dot(row2.xyz, row3.xyz) / dot(row3.xyz, row3.xyz)`, `B = row2.w - A x row3.w` from the projection rows; the pass
   unbinds the depth-stencil (`ExtraHooks::RawSetDepthStencilSurface(nullptr)`), sets `ZENABLE = FALSE`, binds the INTZ
   to `s7` (CLAMP, POINT, no mip, no sRGB) and does its own depth test in the shader. Everything is restored afterwards.
5. States: blend ONE / INVSRCALPHA, BLENDOP ADD (premultiplied: `reflection x a + lamps`), separate alpha off, ZWRITE off,
   colour write RGB only, alpha test off. `DepthShare::SetInternalPass(true)` around the draw so Depth Blur (and the
   PostScene trigger) do not take this ZENABLE=FALSE draw for the first UI draw.
6. Draw with `water_lamps_ps.hlsl`; restore constants, samplers, states and the game's PS.

### The pass shader (`water_lamps_ps.hlsl`, `kWaterLampsHlsl`, ps_3_0)

Inputs: TEXCOORD0 (wave uv 0 in `.xy`, wave uv 1 in `.zw`), TEXCOORD1.xyz world position, TEXCOORD3.w fog. Game constants
reused: `c1` camera position, `c5` wave normal scales.

```
if depth && clip.w > sceneW(uv) + 0.3 + 0.01 clip.w : return 0          ; own depth test
n    = normalize(a.x c5.x + b.x c5.y, a.z + b.z, a.y c5.x + b.y c5.y)     ; a, b = wave maps s0, s1
v    = normalize(c1 - pos)
fres = 0.25 + 0.75 (1 - sat(n.v))^5
fogKeep = 1 - sat(fog.w)
; reflection (only with depth): smoothed normal nr = normalize(0.5 n.x, 1, 0.5 n.z), r = reflect(-v, nr)
;   march t = 0.4, then t = 1.15 t + 0.3, up to 48 steps; stop off screen or behind the camera
;   hit when 0 < clip.w - sceneW < 1.5 + 0.3 t; 5 bisection steps; colour = scene copy s6 at the hit
;   cover = edgeFade(uv) x match x sat(1.5 - t_hit/60); a ray that finds nothing keeps the game's sky reflection
alpha = sat(fres x c58.x) x cover x fogKeep
; lamps (16): l = lampPos - pos, R = radius
;   spec += colour x pow(sat(n.h), 250) x 2 / (1 + d^2 / (16 R^2))
;   glow += colour x sat(1 - d^2/R^2)^2
lamps = min((spec x fres + glow x 0.08) x c52.x x fogKeep, 0.8)
return (refl x alpha + lamps, alpha)
```

(Combined build only: `lamps` was also multiplied by `params.z` = HDR lamp gain after the 0.8 clamp; not in the
standalone, see [../../removed-features.md](../../removed-features.md).)

### Constants

| Register | Content |
|---|---|
| c1, c5 | game: camera, wave normal scales |
| c20..c35 | lampPos: head xyz, w = visual radius |
| c36..c51 | lampCol: colour x intensity x fade |
| c52 | x = lamp strength (`brilhoNaAgua`), y = lamp count (unused by the HLSL), z = HDR lamp gain in the combined build (1 without HDR; the combined shader multiplies `lamps` by it, so a port that keeps that line must set z = 1) |
| c53..c56 | world -> clip matrix |
| c57 | world translation (0 when the matrix was inverted) |
| c58 | x = reflection strength (`reflexoNoLago`) |
| c59 | x = A, y = B, z = 1 when s7 holds the scene depth |
| s0, s1 | game wave maps |
| s6 | game scene copy (refraction source, image before the water) |
| s7 | Depth Blur INTZ (only with depth) |

## Files and functions

| File | Function | Role |
|---|---|---|
| `lot_light_bridge.cpp` | `EnsureWater`, `DrawLake`, `SelectLamps`, `SetWaterFix`, `WaterStatus`, `PrecreatePs` | pass |
| `water_lamps_ps.hlsl`, `water_lamps_hlsl.h` (`kWaterLampsHlsl`) | `main`, `Project`, `ClipToUv`, `SceneW`, `EdgeFade` | shader (header is what the build uses; identical content) |
| `depth_share.h` | `DepthShare::Texture()`, `Surface()`, `SetInternalPass()` | depth from Depth Blur |
| `d3d9_extra_hooks.cpp` | `ExtraHooks::RawGetDepthStencilSurface`, `RawSetDepthStencilSurface` | unhooked depth-stencil calls |
| `patches/night_terrain_relight_patch.cpp` | `ApexRenderReflectionsUI`, `ResetReflectionDefaults`, settings | UI and TOML |
| `shader_ids.h` | `kLakePs`, `kLakePs2`, `kLakeVs` | ids |

## Game addresses and patterns

No game code is patched; recognition by exact shader id; lamps from `FUN_006acf70` (see [roofs.md](roofs.md)).

## Interactions

- **Depth Blur** ([../depth-blur.md](../depth-blur.md)): provides the INTZ depth. The shore reflection works only while
  Depth Blur's INTZ swap is active and the game's MSAA is off (with MSAA, captures from m16 on show "samples=8" and Depth
  Blur does not share the depth). `DepthShare::Request` (kept generic after the SSAO removal) lets another effect ask for
  the swap.
- **PostScene / Depth Blur trigger**: the pass sets `ZENABLE` off; without `SetInternalPass` Depth Blur treated it as the
  first UI draw and blurred mid-frame (review 25/09 ~03:30 item 1).
- **Roofs**: same lamp list; `UpdateLampList` runs when roofs, water or object pixel lamps are on.
- **Reflections section / MirrorSettings**: see [../reflections.md](../reflections.md). The S3SS Mirror Reflection Settings
  patch tunes mirror objects, not water.

## Known limitations

- Only the exact lake PS/VS pair. Not handled: the ocean (its planar reflection already shows lit lamps; no lamp term on
  the surface), swimming-pool water (`PS_3140B910`, ps_2_0, reflection/refraction only, m67), and any other water shader
  (the plaza fountain water, draw #161 in `LightProbe-chafariz`, was not identified as the lake shader).
- Without depth (Depth Blur off, MSAA on): lamp glints/glow only, no shore reflection.
- The reflection is screen space: things off screen or hidden are not reflected (the game's sky reflection remains there).
- Lamp glints use the visual radius rule, not the bake-matched law of objects.

## Pitfalls and failed approaches

- First test (24/09): the whole lake pinkish white. Cause: radius from the light bounds (`+0x134`, ~50 m) and two lights
  per street lamp at the same place (`+0x130` = 97 and 40), so 16 lamps covered the lake. Fix: visual radius
  `clamp(sqrt(range) x 1.2, 2, 25)` (7-12 m), glow x 0.08, spec `x2 / (1 + d^2/(16 R^2))`, sum clamped to 0.8. (Real data
  from F8: intensity 1.0, colour (1, 0.75, 0.79), `+0x130` = 40, 70, 97 or 100.)
- The ocean's planar reflection cannot be reused for lakes: it is a fixed mirror at sea level, only rendered when the ocean
  is visible; making lakes use it would be expensive and touch the reflection pipeline.
- First reflection: the reflected ray (smoothed wave normal x 0.5) sampled at fixed distances 2, 6 and 14 m, projected with
  the water VS WVP (VS c4..c7 -> PS c53..c56, translation c57), read from the scene copy s6, 500 instructions. Wrong
  position at times (limits of 3 fixed distances). Replaced by the depth ray march (25/09).
- The first ray march used a fallback "guess at 40 m with weight 0.6" when nothing was hit; with MSAA on (no depth) every
  pixel used the guess, sampled the wrong part of the scene copy and painted black patches with white streaks on plaza
  ponds at night (m25, `LightProbe-lago-preto`). Now: no depth -> no screen reflection at all; no hit -> no reflection.
- Rotated lots: projecting `(world - translation)` with local->clip was wrong; world->clip is now computed on the CPU.

## Testing in game

- At night by a pond with lamps around: glints and soft glow near lamps; with Depth Blur on and game MSAA off, the shore
  (trees, houses, lamps) reflected; nothing black or streaky; move the camera: no jumps.
- Status (dev): `Water: water: active | draws with reflections: N` ("waiting" before the first lake draw).
- Log: `[LotLightBridge] Agua: ativo` or the compile error.
- F7 on the water (dev): the extra draw after the game's #70-like draw is the mod's pass; check whether `s7` holds the INTZ
  (no `s7` = no depth, e.g. MSAA on).

## Open items

- Depth for water with MSAA on (resolve the MSAA depth or draw depth a second time), noted after m25.
- Lamp term for the ocean surface and pools.
