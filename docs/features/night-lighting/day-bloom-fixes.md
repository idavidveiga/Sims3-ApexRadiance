# Daytime bloom and wall bloom fixes

By day, Night Lighting no longer makes objects look as if they glowed: in full daylight fences, snow on objects and
outdoor objects keep the game's own lighting. Exterior walls and foundations lit brighter by Apex keep the game's bloom,
so they no longer turn white in the bloom pass, and a cinema/theatre facade of the base game no longer blooms in the
sun. Switching *Smooth ground light* changes only how the ground light is filtered, without relighting anything. Found
and validated by **idavidveiga**. Part of [Night Lighting](README.md).

## Status

| | |
|---|---|
| Availability | Released in 2.7.0 |
| Default | On with Night Lighting (no switch of its own) |
| Menu | None. Diagnostics: Developer > Lighting (*Lighting + Bloom census*, *Capture bloom alpha mask*, developer mode) |
| Configuration | `[patches.NightTerrainRelight]` in `ApexRadiance.toml` (the existing Night Lighting keys) |
| Source | [`features/lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp) (`DrawWallGain`, `DrawCinemaMarqueeDayBloomGuard`, the daylight gates), [`features/shader_patches.cpp`](../../../features/shader_patches.cpp) (`BloomThresholdConst`), [`features/bloom_alpha_probe.{h,cpp}`](../../../features/bloom_alpha_probe.cpp), [`shaders/shader_ids.h`](../../../shaders/shader_ids.h), [`patches/night_terrain_relight_patch.cpp`](../../../patches/night_terrain_relight_patch.cpp) |

## The problem

- Night Lighting's object, fence and snow passes add the ground light atlas, which has no notion of day: in daylight
  those families could look emissive next to the sunlit ground.
- The ExteriorWall pixel shader derives its output alpha, the scene's bloom mask, from the final luminance. Multiplying
  the wall lamp term to brighten walls near lamps also widened that mask, so whole walls and foundations could turn
  white in the bloom pass.
- The base game's cinema/theatre facade writes bloom alpha from its final luminance minus a material threshold; in full
  daylight its marquee and a narrow centre panel bloomed.
- Switching *Smooth ground light* used to refresh the whole lighting (terrain, lots, rooms, walls, rigs), so an A/B of
  the ground filter also changed the lighting solution.

## How Apex Radiance solves it

1. **Full daylight gates.** At a night level of 0.01 or less the fence pass, the snow-on-objects pass and the outdoor
   object per-pixel pass are not drawn (as the moon-shadow and leaf-shadow passes already were not), so the game's
   draw stays. At dusk and dawn the lamp terms fade with `TerrainLightingPolicy::SurfaceLampGain`
   ([fences.md](fences.md), [objects-and-rigs.md](objects-and-rigs.md), [snow.md](snow.md)).
2. **Wall bloom kept.** For opaque ExteriorWall draws with alpha writable, an alpha-only pass with the game's own lamp
   constant (no depth or stencil write) writes the game's bloom alpha, then the RGB pass writes Apex's brighter lamp term
   with alpha writes off. Blended or unusual wall draws keep the single pass ([walls.md](walls.md)).
3. **Cinema/theatre facade.** For the facade's exact vertex shader (`BFFCCC56`, 1060 bytes) with one of its exact pixel
   shaders, in outdoor rig mode 2, the constant that holds the material's bloom threshold is raised to 1000 for the draw
   and put back: RGB, depth and stencil stay the game's, only the bloom alpha goes. The threshold constant must be proven
   by the bytecode (`BloomThresholdConst`: used only by the luminance-derived alpha write) and read between 0.5 and 5;
   the centre panel is accepted only on draws of at most 4 primitives.
4. **Smooth ground light** only selects smoothed or raw maps in the draw paths; it calls no lighting refresh and no rig
   re-gather ([world-atlas-and-smoothed-maps.md](world-atlas-and-smoothed-maps.md)).

There is no global "no bloom by day" rule: an Apex-added term is neutralised by day where Apex is the cause, the game's
bloom alpha is kept where Apex raises only RGB, and a stock material with a confirmed daytime defect is targeted by its
exact shaders.

## Settings

None of its own. *Lamps light walls* and wall *Brightness*, *Fences and stairs catch light*, *Doors and windows stay lit*
and *Smooth ground light* ([README](README.md#settings)) decide which of these paths run.

## Compatibility and interactions

- **Walls:** the two-pass draw is combined with the daytime wall term of [walls.md](walls.md); the counters "wall bloom
  preserved" and "fallback" are in the developer status.
- **Bloom:** the game's bloom composite is untouched; only the alpha written by the facade and by Apex's wall gain
  changes.
- Other mods that replace the facade or wall shaders: the exact identities and bytecode proofs fail and the game's draw
  stays.

## Limitations

- The facade guard has no night-level test and also matches the facade's night pixel shaders (`kCinemaMarqueeNightPs`
  1748 bytes, `kCinemaMarqueePanelNightPs` 1296 bytes), so the facade's bloom alpha is suppressed at night as well.
  Unverified in game.
- The facade shaders were captured on the EA app build 1.69; the shader package is the same on Steam, but the guard has
  not been confirmed in game there.
- Only the one facade with the captured shaders is handled; other emissive materials keep the game's bloom.

## Technical reference

| Item | Identity / site | Notes |
|---|---|---|
| Facade VS | `kCinemaMarqueeDayVs` {1060, `0xBFFCCC56`} | `VsInfo::cinemaMarqueeDay` |
| Main marquee PS | day `kCinemaMarqueeDayPs` {864, `0xD5ED0EF3`}, night `kCinemaMarqueeNightPs` {1748, `0xDD77CDE4`} | Threshold constant from `BloomThresholdConst`, kept per pixel shader |
| Centre panel PS | day `kCinemaMarqueePanelDayPs` {500, `0x4E570819`}, night `kCinemaMarqueePanelNightPs` {1296, `0x36F5E915`} | At most 4 primitives |
| Wall gain | ExteriorWall pixel shaders, the lamp scale constant per variant | Alpha pass with `D3DRS_COLORWRITEENABLE` = alpha only, then RGB |

Diagnostics (developer mode, Developer > Lighting): *Lighting + Bloom census* writes
`ApexRadiance_LightingBloomCensus.txt` (read-only: the visible draw families of several frames by day, twilight and
night, whether and which Apex path claimed each, shader hashes and sizes, rig mode, a representative position and
textures, and possible bloom-mask families); *Capture bloom alpha mask* (`BloomAlphaProbe`) saves the raw scene alpha at
the post-scene boundary, before the bloom composite, as PNG and TXT statistics tagged day, twilight or night; it never
changes constants, states or colours. The object status line counts "cinema day bloom suppressed" draws (and the centre
panel).

## Rejected approaches

- A global bloom threshold, alpha clamp or daytime bloom switch: would change many valid emissive materials.
- The fork's day fade of the object rig boost: its rig re-gathers at every 0.1 of night level slowed indoor lighting.
- Turning *Smooth ground light* off to hide daytime glow: it also removed the ground light quality it exists for.

Details in [history](../../history/night-lighting-day-bloom-fixes.md).

## See also

- [Validation](../../validation/night-lighting-day-bloom-fixes.md)
- [History](../../history/night-lighting-day-bloom-fixes.md) (with the fork's notes)
- [Walls](walls.md), [Objects and rigs](objects-and-rigs.md), [Fences](fences.md)
