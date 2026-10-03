# Roads and sidewalks

> Published 2.5.6 adds recognition of alpha-blended sidewalk VS variants packing
> terrain UV in TEXCOORD1.xy and opacity UV in zw. Current scaleConst is used by
> the SDR ground/road brightness path; the earlier HDR-only note is superseded.

> Roads and sidewalks sample their own copy of the chunk light map, which does not contain the lamps the world terrain
> map has, so they stayed dark next to lit ground. Night Lighting recognises every road vertex shader by pattern
> (summer and winter) and pattern-patches any pixel shader drawn with it: `max(road's own map, chunk terrain map)` on a
> free sampler, the smoothed map replacing the road's own copy, and optionally "trodden snow" on sidewalks. Status:
> **working**: winter variants tested 25/09 ~02:10 (4 variants offline + in game), summer variants added 25/09 ~09:10
> (m42/m43). Part of [Night Lighting](README.md).

## Purpose

| Capture | What it showed |
|---|---|
| m01 / m02 (snow, dark vs lit ground) | dark side had a mesh on top (#108, PS_26B051F8, 2667 prims, road/sidewalk) with its own light map in s6 |
| m04 / m05 / m06 (snow formula analysis) | road PS_27A1AD10 (same in m01 and m06) uses a **copy** of the chunk map in s6 (texture T14, not the terrain's T12); uv = local/256 + 0.5 (VS c16); chunk centre = world matrix translation (VS c8.w, c10.w). The copy has no lamp glow |
| m16 / m18 (LightProbe-rua-canto) | 3rd winter variant, sidewalk corner |
| m24 (LightProbe-rua-beirada) | 4th winter variant, road edge blended into the terrain, stencil passes before (#118/#119, no PS) |
| m42 calcada-verao, m43 rua-verao, grama-luz-lote (summer) | a lot lamp lit the grass through the **terrain map** (lot map there ~0, max 0.012) but not the road/sidewalk: the summer road VS was not recognised |
| print 41, 43 | dark strip at the road edge in snow; dark squares at sidewalk corners |

### Variants

| Season | VS | PS | Own map | Notes |
|---|---|---|---|---|
| Winter | VS_27AEBB08 (MD5 38A329C7, 892 bytes) = VS_2779BF40 | PS_27A1AD10 (F6ABF31E/1240) | `texld r3, v1, s6` | road + sidewalk; sidewalk snow blend present |
| Winter | same | PS_27AE6C20 (620938B3/1320) | s4 | lane markings |
| Winter | same | PS_2779A7D0 (99E81A85/1288) | `texld r3, v1, s6`; last sampler s8 -> extra s9; already uses r6 | sidewalk corner; sidewalk blend present (`lrp r6.xyz, r2.w, r2(s8), r3(s7)`; snow `lrp r1.xyz, v2.w, r0, r3`) |
| Winter | same | PS_277978F0 (554AA649/1272, 897 prims) | `texld r3, v1, s4`; last sampler s8 -> extra s9; temps to r5 | road edge into terrain |
| Summer | VS_2CEAD588 (road, no tangent) / C0E919AE/560 | 00E2AC7D/616, 0F907C16/568 | `texld rX, v1, s2` | map mapping in VS **c14** |
| Summer | VS_2CEAE460 (sidewalk, with tangent) / CD462949/604 | 0ABA504A/800 | s2 | |

(MD5-prefix ids from ground_report.md D; `PS_xxxxxxxx` probe names are object pointers and change between sessions.)

Winter VS snow: `c15.z` = snow level; cover = sat(2 c15.z), height = sat(2 c15.z - 1) (notes "Ruas: duas variantes").
In variant 1 with snow, the bright parts of the road texture (sidewalk, markings) always become snow texture.

## User-facing settings

| UI label | TOML key | Type | Default | Range | Notes |
|---|---|---|---|---|---|
| Trodden snow on sidewalks | `calcadaComNevePisada` | float | 0.5 | 0..1 | Adv / Ground and snow; disabled unless "Street lamps light inside lots". 0 = the game (fully snow-covered). `SetSidewalkClear` clamps 0..1 |

Roads also need `luzDoPosteNaGramaDoLote` (dispatch gate) and benefit from `mapaDeLuzSuavizado`.

## How it works

### Recognition (`ShaderPatches::IsRoadVs`, VS class 4)

A vs_3_0 whose TEXCOORD1 output is declared with write mask **.xy**, or **.xyzw**
with the additional validated opacity-UV multiplication into zw. The full-mask
variant must use the recognised constant/input swizzles, unit-scale definition
and texture-coordinate declaration; arbitrary full outputs are not accepted.
Lot .xyz outputs remain excluded. The shader has `dp4` with c8 and c10, and exactly one
`mad oT1.xy, rA.xzzw, cM, cM.zwzw` (terrain uv). M is stored per VS (`g_roadMapConst`): c16 in winter, c14 in summer.
Scan of 118 unique captured shaders (25/09 09:10): 3 road VS, 7 road PS (4 winter + 3 summer), all disassemble.
The later offline captured-VS scan retained the three existing road matches and
added one alpha-blended sidewalk match among 314 shaders. This recognises a shader
variant; it does not certify every sidewalk or colour-correction configuration.

### Pixel patch (`ShaderPatches::PatchRoad`, ps_3_0)

1. Find `texld rX, v1, sL` whose result is next used, within 8 instructions, by the lamp scale
   `mul rY.xyz, rX, cN.x` (winter: right after, rY = rX, c4.x; summer: a few instructions later, rY != rX, c3.x). Give
   up if rX is read by anything else first or rX.xyz is overwritten (writing rX.w is fine).
2. E = highest sampler + 1 (refuse at 15), T0 = highest temp + 1.
3. Insert `dcl_2d sE` after the last sampler dcl and, right after the texld:
   ```
   texld T0, v1, sE
   max   rX.xyz, rX, T0
   ```
4. **Sidewalk snow blend** (only where the shader has it): `mul r1.w, rA.x, rA.y` (brightness of road texture rA) and,
   later, `lrp r1.xyz, v2.w, r0, r3` (the snow mix). Then: cS = highest const + 1 (the amount), cL = cS+1
   `def (0.3, 0.59, 0.11, 0)`, cK = cS+2 `def (4, -1, 0, 0)`; before the albedo mul `mov T1, rA` (save the road texture);
   after the lrp:
   ```
   dp3     T1.w, T1, cL          // luma of the road texture
   mad_sat T1.w, T1.w, cK.x, cK.y // sat(luma*4 - 1): only the bright parts (concrete, markings)
   mul     T1.w, T1.w, cS.x      // x "Trodden snow on sidewalks"
   lrp     T2.xyz, T1.w, T1, r1  // mix the plain road texture back in
   mov     r1.xyz, T2
   ```
   Offline test on m06/m10/m18/m24: the sidewalk part applies to variants 1 and 3 (the ones with the texture in s7/v0).
5. `scaleConst` = `LampScaleAfter(...)`: K of the lamp scale when read once (combined build HDR gain only).

### Draw (`DrawRoad`)

1. `PatchedFor(g_roadPs, "Rua", PatchRoad)` (one patched copy per game PS; refused shaders logged and saved).
2. VS c[M] must be (1/256, 1/256, 0.5, 0.5), else the game draws.
3. Chunk key from VS c8.w / c10.w; the chunk must be registered by a world terrain draw (`g_chunks`), else
   `g_lotMissing++` and the game draws. Roads do **not** use the atlas: road meshes are per chunk, so the chunk key is
   exact.
4. `SamplerBind` of the chunk texture (smoothed if ready: `ChunkTexture`) to sE (CLAMP, LINEAR, mip LINEAR).
5. If the smoothed map is ready, it also **replaces the road's own copy** in sL, so roads get exactly the same clean
   light as the ground next to them.
6. Sidewalk constant cS.x = `g_sidewalkClear` when present.
7. Swap PS, draw, restore everything. Counter "roads: N".

## Files and functions

| File | Function | Role |
|---|---|---|
| shader_patches.cpp | `IsRoadVs`, `PatchRoad`, `RoadPatch {lightSampler, extraSampler, sidewalkConst, scaleConst}` | bytecode |
| lot_light_bridge.cpp | `DrawRoad`, `g_roadPs`, `g_roadMapConst`, `g_curRoadMap`, `SetSidewalkClear`, `SamplerBind`, `ChunkTexture` | draw |
| lightmap_smooth.cpp | `Find` | smoothed chunk map |

## Game addresses and patterns

No game code is patched. Road world matrices were assumed never rotated: c8/c9/c10 = (1,0,0,896), (0,1,0,500),
(0,0,1,1150) in m01, m43 and calcada-verao (only 3 captures checked; ground_report.md section 6).

## Shader details

| Register | Content |
|---|---|
| VS c8.w, c10.w | chunk centre (world matrix translation) |
| VS c14 (summer) / c16 (winter) | (1/256, 1/256, 0.5, 0.5): terrain uv mapping |
| VS c15.z (winter) | snow level |
| PS c4.x (winter) / c3.x (summer) | lamp scale of the road map |
| PS sL (s6, s4, s2) | road's own map copy |
| PS sE (new) | chunk terrain map (smoothed when ready) |
| PS cS, cS+1, cS+2 (new) | sidewalk amount, luma weights, (4, -1) |

## Interactions

- [world-atlas-and-smoothed-maps.md](world-atlas-and-smoothed-maps.md): source maps.
- [terrain-relight.md](terrain-relight.md): lot lamps must be in the terrain bake to reach roads. A full rebuild also
  marks the road partition (`FUN_00B789B0`, WorldManager+0x5C = RoadNetwork) per chunk (smooth_streaming_patch.cpp).
- [snow.md](snow.md): the snowy road analysis.

## Known limitations

- A road chunk whose world terrain draw never ran (e.g. chunk drawn only by the multi-pass terrain) has no registered
  map: the game draws the road.
- Summer roads have no world y in the PS (ground_report 2): any future per-pixel term needs a VS patch.

## Pitfalls and failed approaches

- First version (25/09 ~01:40) matched only the 2 known winter PS by bytes; variants 3 (corner, uses r6 so the temp
  must be configurable) and 4 (edge) stayed dark -> generic recognition by the VS, extra sampler/temp computed.
- The summer VS (TEXCOORD1 mapping in c14, PS scale a few instructions after the texld) was not recognised until
  25/09 09:10; the lamp-scale search was widened to 8 instructions, stopping on any other read or rgb overwrite of rX.
- Sidewalk blend: in variant 1 the snow always covers the bright texture parts; the blend back is by luma, not by a
  mask the game provides.

## Testing in game

- Night, summer and winter: roads and sidewalks next to street lamps and lot lamps as bright as the grass beside them;
  no dark strip at the road edge and no dark squares at sidewalk corners in snow.
- Slide "Trodden snow on sidewalks" 0 -> 1 in snow: sidewalk concrete shows through.
- Dev > Status: "roads: N" growing; "without terrain texture" stable.
- Log: `Rua: corrigido xN` or `Rua: sem o padrao esperado, fica como o jogo` (dev: `S3SS\ShadersRecusados\Rua_PS_*.bin`).
- F7 on a road: patched PS sizes seen: 130C52EF/1284 and E7A8295D/1364 (PatchRoad outputs, ground_report.md).

## Open items

- Summer road captures were all taken before the summer fix (09:07-09:09); no capture shows the patched summer shaders.
- PASSO3 increment 5 (terrain stamp at 4 texels/m) would also sharpen roads (they read the same map).
