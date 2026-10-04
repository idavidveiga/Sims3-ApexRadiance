# Snow

> **Status in the standalone:** in the v0.1.0 baseline (b84d5f1) as described (snowy lot pass, snow on floors, fence tops,
> stair tops and the stair routing). `FloorPatch::scaleConst` (`LampScaleAfter`) is post-0.1.0, HDR-only: leave it out.

> In winter the game swaps many surfaces to snow shader variants, and each one misses lamp light in its own way. Night
> Lighting fixes: the snowy lot ground (lot light pass bytecode patch), snowy floor tiles and pool edges, the snow mesh
> lying on floors and door sills, snow on stair tops (routed away from the snowy-roof handler), and snow on fence tops
> and props. All of them take the world light atlas at the pixel's world position. Snowy roads, roofs, foliage, ponds
> and the pink lamp tint have their own docs (links below). Status: **working**; fence tops and stair tops were added
> 25/09 ~10:00-10:40 (stairs fixed after a first failed test), snow on floors made reachable 25/09 ~15:00. Part of
> [Night Lighting](README.md).

## Purpose

### The snow formula analysis (LightProbe-m04 grey lot, m05 bright world, m06 road)

| Surface | Shader | Lamp term |
|---|---|---|
| World snow terrain | PS_28FDBAE0 | albedo x (terrainMap x c7.x (= 1) + 0.65 x ambient cube + specular) |
| Lot snow (light pass, modulate2x over the base pass PS_255BBEC8) | PS_2A13D200 (1852 bytes) | albedo x (lotMap x direction factor x **0.25** (`def c9.x`) + 0.65 x ambient + specular). Simple lots do not bind the direction maps s7-s10, so the factor is about N.y = 1 |
| Snow road | PS_27A1AD10 | own copy of the chunk map in s6, without the lamps ([roads.md](roads.md)) |

So the lot term is 4x weaker than the world term by construction, and the lot map has no street lamps: a strong cut
at the lot border in snow (m01/m02, `LightProbe-neve`). Fixes chosen: lot = `max(lot x factor, terrain x 4)` (the x4
cancels the 0.25, making it equal to the world); road = extra `texld` of the terrain map + `max`.

### Snow surfaces and their captures

| Surface | Capture | Shaders | Game's lamp light | Fix |
|---|---|---|---|---|
| Snowy lot ground | LightProbe-neve, m04 | VS_2A13C7D8 (`kSnowLotVs`, 1400, 0x9256F0DF) / PS_2A13D200 (`kSnowLotPs`, 1852, 0x08DF01E8) | `texld r0, v3, s2` x normal factor x c4.x x 0.25 | `PatchSnowBytecode`, `DrawLotSnow` |
| Snowy floor tiles | m08 | `kFloorVs` / PS_1B3938E8 | lot map x factor x 0.25 | `PatchFloor` ([floors.md](floors.md)) |
| Curved pool edge (winter floor + pool mask) | m66 | VS_29991640 / PS_29F52F90 | same | `IsFloorVs` + `PatchFloor` |
| Snow mesh lying on lot floor (around the pool) | m69 | VS_2FA6DE10 / PS_2FA6D640 (2748 prims) | `texld r0, v1, s6`, `mul r1.xyz, r1.z, r0`, x c3.x; VS writes TEXCOORD7.xy = world xz x 0.5, PS never reads it | class 11 (tc7), `PatchSnowFloor` |
| Door sills with snow | m71, m72 (also in m29) | VS_27CE3AD0 (= VS_215055E8 of m29) / PS_2A044CD0 | `texld r0, v1, s1` (32x64 in m71, 512x128 in m72), `mul r0.xyz, r1.w, r0`; VS TEXCOORD0.zw = world xz x 0.5 (`mul o1.zw, r2.xyxz, c20.x`) | class 11 (tc0), `PatchSnowFloor` |
| Snow on stair tops | m50, m51 | VS_2E036438 / PS_2E036820 (ps_3_0, 4992 bytes, 304 slots; 4 `rep i0` loops of 3D noise, s1 permutation 256x256, s2 gradient 256x1) | none: `dp3_sat r1.w, c1, N` x 4-tap shadow (s5) x c0 + cube s0 x c5.x (`mad_pp r0.xyz, r0, c5.x, r1`), x r6.w, x r6 albedo, fog v4 | class 9, `PatchSnowRelief` |
| Snow on fence tops, rails, props | m48 | VS_2F27C9C0 / PS_2F27C510 (#126/#139, 2138 tris) | none: `r1 = N.L_moon x shadow x c0 + cube(0,1,0) x c4.x`, then `mul oC0.xyz, r1, snowTex(s1, uv = world x 0.125)` | class 8, `PatchSnowCover` |

Other winter items, documented elsewhere: snowy roads and sidewalk trodden snow ([roads.md](roads.md)), snowy roofs
(PS 4992 bytes, additive pass; [roofs.md](roofs.md)), winter foliage and the moon shadow
([foliage.md](foliage.md)), black pond in snow m25 ([water.md](water.md)), pink lamps visible on white snow m12/m14
([lamp-colour.md](lamp-colour.md)).

## User-facing settings

| UI label | TOML key | Type | Default | Range | Used by |
|---|---|---|---|---|---|
| Street lamps light inside lots | `luzDoPosteNaGramaDoLote` | bool | true | | all snow handlers (dispatch gate) |
| Smooth light on the ground | `mapaDeLuzSuavizado` | bool | true | | all atlas readers |
| Fences, railings and stairs get the ground light | `cercasComLuzDoChao` | bool | true | | snow on fence tops (class 8) and stair tops (class 9) |
| Fence light strength | `forcaNasCercas` | float | 1.0 | 0.25..2 | strength of the atlas term on class 8 / 9 snow |
| Sidewalk visibility | `calcadaComNevePisada` | float | 0.5 | 0..1 | how much sidewalk shows where Sims have walked ([roads.md](roads.md)) |

The lot snow pass and snow on floors have no strength slider: the atlas enters at the game's own lamp scale.

## How it works

### Snowy lot ground (`PatchSnowBytecode` + `DrawLotSnow`, lot_light_bridge.cpp)

- Gate: PS class `LotLightSnow` (exact 1852 bytes) **and** VS class 3 (exact `kSnowLotVs`), VS c15.xy = 1/256.
- `PatchSnowBytecode` (made once, from the bound shader or at shader creation via `EnsureLotSnow`):
  - requires the `dcl` of s11 and the first `texld r0, v3, s2`, immediately followed by `mul r0.xyz, r1.w, r0`
    (light-basis factor);
  - inserts after the dcl of s11: `dcl_texcoord1_pp v7.xy` (tokens `0200001F 80010005 90230007`) and `dcl_2d s12`
    (`0200001F 90000000 A00F080C`);
  - inserts after the mul:
    ```
    texld_pp r7, v7, s12          // 03000042 802F0007 90E40007 A0E4080C
    add_pp   r7.xyz, r7, r7       // x2
    add_pp   r7.xyz, r7, r7       // x4: cancels the later "mul r0.xyz, r4, c9.x" (0.25)
    max_pp   r0.xyz, r0, r7
    ```
  - validated with D3DDisassemble at the time; capture id of the output: C61F8B55/1940. Log `[LotLightBridge] Neve:
    ativo` / `falhou`.
- `DrawLotSnow`: terrain source = world atlas when ready; the VS computes the uv as
  `(world.xz - c16.xz) * c15.xy + c15.zw`, so c15 becomes `(a.x, a.y, a.z + c16.x a.x, a.w + c16.z a.y)` for the draw.
  Without the atlas: the home chunk map at key (c16.x, c16.z) (smoothed if ready), else `g_lotMissing++` and the game
  draws. Binds s12 (CLAMP, LINEAR, mip LINEAR, sRGB off), swaps the PS, draws, restores s12 states, c15, PS.
  Counter "snow: N".

### Snow on floors and door sills (VS class 11, `PatchSnowFloor` + `DrawSnowFloor`)

- `IsSnowFloorVs(t, &tc)`: a vs_3_0 with exactly one `mul oTn, rW.swz, cH.s` where either (tc 7) TEXCOORD7 declared
  `.xy` and `mul oT7.xy, rW.xzzw, cH.s`, or (tc 0) TEXCOORD0 declared full and `mul oT0.zw, rW.xyxz, cH.s`; cH.s a
  **replicated** component of a shader `def` equal to exactly 0.5; rW.x / rW.z from `dp4` with c8 / c10. It
  **refuses** any VS containing `mad oN.xy, rX, cK, cK.zwzw` (the terrain-map uv of terrain, lot and water VS, which
  share the TEXCOORD0.zw shape).
- Class 11 is tested **last** in `ClassifyVsCode` (after the object patch), and `DrawSnowFloor` runs only when no PS
  class claimed the draw: from the `WorldCandidate` branch when `RecordWorldChunk` fails (these PS declare s6+), or from
  the "not LotLight" fallback. Runtime guard: the map sampler found by the patch must hold an **A8R8G8B8, non-DEFAULT
  pool, <= 1024x1024** texture (a room map); terrain maps are DXT5.
- `PatchSnowFloor(t, tc, FloorPatch&)` (ps_3_0, no flow control): the single `texld rL, vK, sM` whose next reader is
  `mul rB.xyz, rS.s, rL` with a replicated swizzle on rS (writes to `.w` only between them are ignored: m69 has
  `max r0.w`). tc 7: new input `dcl_texcoord7 vV.xy` (refuse if TEXCOORD7 is already read); tc 0: widen the existing
  TEXCOORD0 declaration to .xyzw. Then after the mul:
  ```
  mad  T.xy, vV.(xyxy | zwzw), cA, cA.zwzw
  texld T, T, sE
  max  rB.xyz, rB, T
  ```
  `mapSampler` = M, used by the runtime guard.
- `DrawSnowFloor`: atlas mapping with **c.xy x 2** (the coordinate is world xz / 2), separate caches per tc
  (`g_snowFloorPs` for 7, `g_snowFloorPs0` for 0). Counter "snow on floors: N".

### Snow on stair tops (VS class 9, `PatchSnowRelief`), routed away from the roof-snow handler

- The stair snow PS is **byte-identical** to the snowy roof PS: PS_2E036820 = PS_2793C3E8 = PS_27D4DAE0 =
  `kRoofSnowPs` (4992 bytes, 0x3CEB025E). The first build tested the PS class `RoofSnow` before the VS classes, so
  stairs went to `DrawRoofSnow`, which computes lamp positions from the roof VS constants and failed on stairs (first
  test 25/09 10:43: not fixed). Now `OnDrawInner` sends `RoofSnow` to `DrawRoofSnow` only when the VS is **not** class 9.
- `IsSnowReliefVs`: TEXCOORD4 output with .zw, TEXCOORD2 **input** (the snow's base position; roofs of the same family
  have none), exactly one `mul oT4.zw, rW.xyxz, cD.x` with `def cD.x = 0.5`, rW.x / rW.z from `dp4` c8 / c10.
  Offline: only VS_2E036438 matches among 161 VS (the PS also matches the roof-snow PS, but entry is by the VS).
- `PatchSnowRelief` (ps_3_0, requires `dcl_cube s0` and TEXCOORD4 with .zw): after the **single**
  `mad rL.xyz, rCube, cK.x, rS` outside any `rep`/`loop` (rCube = the last `texld rCube, rN, s0`), insert
  ```
  mad  T.xy, vT4.zwzw, cA, cA.zwzw
  texld T, T, sE
  mad  rL.xyz, T, cB.x, rL          // + atlas x strength, before the "x r6.w" and "x r6" (albedo)
  ```
- `DrawSnowRelief` = `DrawSnowOnObject(..., posScale = 2)`: atlas c.xy x 2, cB.x = `forcaNasCercas`. Needs
  `cercasComLuzDoChao`. Counter "snow with relief: N".

### Snow on fence tops and props (VS class 8, `PatchSnowCover`)

- `IsSnowCoverVs`: TEXCOORD3 output `.xy`, TEXCOORD1 input; the morph flags `slt rX, cK, vT1.x`; exactly one
  `mov oT3.xy, rW.xzzw` with rW.x / rW.z from `dp4` c8 / c10 (the VS morphs between two positions, v0 and TEXCOORD0).
  Offline over all captured shaders: only VS_2F27C9C0 / PS_2F27C510 match.
- `PatchSnowCover` (ps_3_0, refuses any flow control): the single `mul oC0.xyz, rP, rQ`; of the two operands, the one
  whose last writer is a `texld` is the snow texture, the other (rL) must be written by `mad rL.xyz, rX.w, c0, rY`
  (moon x shadow + sky). Before that mul insert
  ```
  mad  T.xy, vT3.xyxy, cA, cA.zwzw
  texld T, T, sE
  mad  rL.xyz, T, cB.x, rL          // like the ground: light map + ambient
  ```
- `DrawSnowCover` = `DrawSnowOnObject(..., posScale = 1)`. Needs `cercasComLuzDoChao`; strength `forcaNasCercas`.
  Counter "snow on objects: N".

## Files and functions

| File | Function | Role |
|---|---|---|
| lot_light_bridge.cpp | `PatchSnowBytecode`, `EnsureLotSnow`, `DrawLotSnow`, `g_snowPs` | snowy lot ground |
| | `DrawSnowFloor`, `g_snowFloorPs`, `g_snowFloorPs0`, `g_snowFloorTc` | snow on floors / sills |
| | `DrawSnowOnObject`, `DrawSnowCover`, `DrawSnowRelief`, `g_snowCoverPs`, `g_snowReliefPs` | snow on objects / stairs |
| | `OnDrawInner` (`RoofSnow && !g_curVsIsSnowRelief`) | stair routing |
| shader_patches.cpp | `IsSnowFloorVs`, `PatchSnowFloor`, `IsSnowCoverVs`, `PatchSnowCover`, `IsSnowReliefVs`, `PatchSnowRelief`, `IsFloorVs`, `PatchFloor` | bytecode |
| shader_ids.h | `kSnowLotPs`, `kSnowLotVs`, `kRoofSnowPs`, `kFloorVs` | exact ids |

## Game addresses and patterns

No game code is patched for snow. Patterns and token sequences are listed above.

## Shader details

| Handler | New sampler | New constants | Position source | Scale |
|---|---|---|---|---|
| Lot snow | s12 (fixed) | none (VS c15 overwritten per draw) | TEXCOORD1 (v7) | x4, then game's c4.x x 0.25 |
| Snow floor tc7 / tc0 | max + 1 | cA | TEXCOORD7.xy / TEXCOORD0.zw = world xz / 2 | game's lamp scale after the max |
| Stair snow | max + 1 | cA, cB | TEXCOORD4.zw = world xz / 2 | cB.x = fence strength |
| Snow cover | max + 1 | cA, cB | TEXCOORD3.xy = world xz | cB.x = fence strength |

## Interactions

- [floors.md](floors.md): winter floor tiles and the pool edge (class 5).
- [fences.md](fences.md): the fence strength and option drive class 8 / 9 snow too.
- [roofs.md](roofs.md): the roof snow pass shares the PS with stair snow.

## Known limitations

- Winter lot light pass drawn with **VS 436BB272/1348** (seen only in m58, "prefeitura" #378): not handled.
  `DrawLotSnow` requires the exact `kSnowLotVs`, and that VS keeps the chunk centre in c15 and the uv mapping in c14
  (the known one uses c16/c15), so relaxing the byte check alone would read the wrong constants (ground_report 4.2).
- The atlas is ground light without height: snow on a high fence top gets the ground's value.
- `IsSnowFloorVs` shape (TEXCOORD0.zw = world xz / 2) appears in 12 terrain/lot winter pairs; the three guards
  (class 11 last, only when no PS class claimed the draw, room-map texture check) are what keep it off them.

## Pitfalls and failed approaches

- **Stairs to the roof-snow handler** (above): classify by VS before trusting a PS class shared by two families.
- **Snow on floors never ran** (review 25/09 ~14:35-15:00): the m69/m71 PS declare s6+ and fell into `WorldCandidate`,
  which returned the draw to the game -> `DrawSnowFloor` is now called when `RecordWorldChunk` rejects the draw.
- **IsSnowFloorVs caught terrain and water** (same review; the A8R8G8B8 check did not separate lot maps, which are also
  A8R8G8B8) -> refuse VS with the terrain-uv `mad`, require the replicated 0.5.
- **Snow-floor cache**: tc0 and tc7 variants of the same PS need separate patched copies.
- **Winter seam m73 (bright snow lot, PS 33A013F8) / m74 (dark winter world terrain PS_2FA80C68, 2488 bytes, smoothed
  map in s9)**: the dark side read chunk (896, 896), the lot used centre (640, 896), border at x = 768. Hypothesis 1
  (stale smoothed map after an unreadable game map) and hypothesis 2 (lots reading the home chunk with CLAMP, confirmed by
  m76) are both fixed ([world-atlas-and-smoothed-maps.md](world-atlas-and-smoothed-maps.md)).
- `PatchSnowCover` refuses flow control, so it could not be reused for the stair snow (4 `rep` loops): a separate patch
  that inserts outside the loops.

### Snow fix lists (history, 25/09)

Pending list (~01:40), and what happened to each item:

| # | Item | Outcome |
|---|---|---|
| 1 | Pink lamps still pink (56 street lamps type 11, 46 lot lamps type 3 at 1/0.75/0.79; script sets the colour after creation via `FUN_006b0b50` -> `FUN_006bc3e0`) | fixed ~02:10: detour of the call 0x6B0BDE ([lamp-colour.md](lamp-colour.md)) |
| 2 | Dark strip at the road edge (print 41; m24, 4th road variant) | fixed: generic `PatchRoad` ([roads.md](roads.md)) |
| 3 | Dark squares at sidewalk corners (m16/m18, 3rd road variant) | fixed: same |
| 4 | Black pond with white streaks in the snowy plaza (print 42; m25): MSAA on, no INTZ, the 40 m guess sampled the scene copy wrongly | fixed: no screen reflection without depth ([water.md](water.md)) |
| 5 | Winter bushes dark (m21: moon shadow multiplies lamp light; m22: back side zeroed by `max r0, r0, c132.w` in the VS) | fixed: `PatchLeafShadow`, `PatchFoliageVs` ([foliage.md](foliage.md)) |
| 5b | Snowy tree (m28, PS_2A83D588 / VS_2A839190, no shadow map) | wrap light via `PatchFoliageVs` |
| 6 | Snowy roofs "waiting" (m23, PS_2793C3E8 4992 bytes, VS_27944BD8) | fixed: additive pass `roof_snow_lamps_ps.hlsl`; m29 position fix ([roofs.md](roofs.md)) |
| 7 | Snowy floor (m08, PS_1B3938E8, lot map only x 0.25) | fixed: world atlas + `PatchFloor` |

Later additions: snow on fence tops (m48, ~10:01), stair snow (m50/m51, ~10:40), snow on floors around the pool (m69),
door sills (m71/m72), pool edge (m66).

## Testing in game

- Winter night: lot borders next to street lamps show no cut on snow; snow on fence tops, stair tops, door sills and
  around pools is lit like the ground next to it.
- Dev > Status > "Street lamps on lots": counters `snow`, `snow on floors`, `snow on objects`, `snow with relief`.
- Log kinds: `Neve no piso`, `Neve nos objetos`, `Neve com relevo (escada)`: `corrigido` or
  `sem o padrao esperado, fica como o jogo`; `[LotLightBridge] Neve: ativo`.
- F7 on snow: lot pass PS of 1940 bytes (patched) with s12 bound; for class 8/9 draws the `mod:` line.

## Ground brightness across terrain variants

The October 4 capture contains a world terrain lamp-map path using `c4.x * c4.x`
through a scalar temporary, alongside a linear `c7.x` path. Previously only the
linear path received Ground brightness, exposing a boundary when the gain differed
from 100%. The classifier now recognizes the exclusive squared multiplier and the
runtime uses the existing square-root compensation. This classification depends
on bytecode, not snow or lot names; shared constants and unknown layouts remain
unchanged. Native GPU checks verify gain equivalence across day/night weights.
Visual validation in the game, on snow and grass, remains required.

## Open items

- VS 436BB272 lot snow variant (needs its own constant mapping: c15 centre, c14 uv).
- PASSO3 later increment 4 includes winter floors (`PatchFloor`) and VS 82e79a9e / PS 68113aad (m29) for a per-pixel
  term; not started.
