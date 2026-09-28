# Floors

> **Status in the standalone:** in the v0.1.0 baseline (b84d5f1) as described (`PatchFloor`, `PatchBakedAtlasPs` with the
> 261-entry table). The `scaleConst` / `LampScaleAfter` detection is post-0.1.0 and only fed the removed HDR gain: leave it
> out.

> Outdoor floor tiles (decks, patio tiles, the park fountain plaza, pool edges) are lit in the game only by a baked
> per-lot floor map, which misses street lamps and in summer is nearly black. Night Lighting takes
> `max(floor map, world light atlas)` in patched copies of the floor shaders: winter/snow floors through
> `PatchFloor` (exact VS + pattern), summer outdoor floors through `PatchBakedAtlasPs` on the 261 ExteriorFloors pixel
> shaders of `floor_atlas_table.h`. Floors of upper storeys also gain the outdoor lamps of every storey through
> [level-light-share.md](level-light-share.md). Interior floors are not touched. Status: **working** (winter floors
> m62 confirmed patched; summer floors added 25/09 ~17:25 in v5.4). Part of [Night Lighting](README.md).

## Purpose

| Case | Capture | Shaders | Game's lamp light |
|---|---|---|---|
| Snowy lot floor tiles | m08 | VS `kFloorVs` (1492 bytes, FNV-1a 0x2BC34FA8, MD5 9B6DB72A) / PS_1B3938E8 (1724, MD5 A9849757) | only the lot map (256x128) x direction factor x 0.25, no terrain map |
| Curved pool edge, winter | m66 | VS_29991640 (1064) / PS_29F52F90 (1620): winter floor with a pool mask (texkill, s11 at TEXCOORD7) | same family |
| Summer outdoor floor | m61 (pixel 1763,849) | VS_281C2AE8 / PS_281C2318 (1044 bytes; MD5 1788C5B6) | `texld r0, v2, s2` then `mad r1.xyz, r0, c3.x, r1`. Floor map 256x256 covering 64 m (uv = pos x 0.5 / 64), nearly black: mean 0.002, max 0.18, one lamp spot with steps |
| Park fountain plaza | chafariz #8 | PS_214701C0 (same 1044-byte shader) | room light map 512x256 A8R8G8B8 in s2; the fenced plaza is a separate room with 0 lights |
| Flat floor variant | telhado #116, telhado2 #51 | 812-byte PS (MD5 9BF0A41E): `mov_sat r0.w, c1.y`, cube read at a constant up vector | as above |
| passo3 test ground | m60 | VS_29F3E810 / PS_29F3EA68 (same 1044 bytes) | s2 map alpha 0 everywhere, RGB max 0.176: room 0 with almost no light under the game's own law (PASSO3 F-J4) |
| Snow lying on floors, door sills | m69, m71/m72 | see [snow.md](snow.md) | room map only |

Verified for this doc by hashing the captured `.bin` files with the FNV-1a used by the tables: the m60 / m61 /
fountain PS (1044 bytes) hashes to 0x9CFC614F and the flat telhado PS (812 bytes) to 0x802006F6; **both are entries
of `floor_atlas_table.h`**.

## User-facing settings

No setting of its own. Floors need:

| Setting | Key | Why |
|---|---|---|
| Street lamps light inside lots | `luzDoPosteNaGramaDoLote` | floor handlers sit after the bridge-enabled check in the dispatch |
| Smooth light on the ground | `mapaDeLuzSuavizado` | the atlas only exists with it |
| Outdoor lights reach every story | `luzExternaEntreAndares` | floors of other storeys get the outdoor lamps (floor-line cut) |

## How it works

### Winter floors and the pool edge (`DrawFloor`, VS class 5)

- VS class 5 = exact `kFloorVs`, or `ShaderPatches::IsFloorVs`: a vs_3_0 whose full TEXCOORD0 output gets
  `mov oT0.zw, rW.xyxz` exactly once, where rW.x = `dp4 ..., c8` and rW.z = `dp4 ..., c10` (world xz). Offline test
  (notes m66): accepts only `kFloorVs` and the pool-edge VS.
- `PatchFloor(t, FloorPatch&)` (ps_3_0 only):
  1. find the first `texld rX, v2, s2` (lot map) and after it `mul rB.xyz, rS.w, rX` (map x bump/light-basis factor);
  2. widen `dcl_texcoord0 v0` to .xyzw (the shader only read .xy; .zw = world xz);
  3. add `dcl_2d sE` (E = highest sampler + 1), constant cA (highest + 1), temp T (highest + 1);
  4. after the mul insert:
     ```
     mad  T.xy, v0.zwzw, cA, cA.zwzw   // atlas uv
     texld T, T, sE
     max  rB.xyz, rB, T
     ```
  Refuses if a piece is missing, samplers >= 15 or constants >= 224.
- `DrawFloor` binds the atlas to sE (CLAMP, LINEAR, mip NONE), sets cA = atlas mapping, swaps the PS, draws, restores.
  The game's lamp scale after the max (m08: `mul r0.xyz, r2, c2.x`, read once) is found by `LampScaleAfter`
  (`scaleConst`); it was used only by the combined build's HDR gain.
- Net winter formula: `max(lotMap x dir x 0.25, atlas)` then x c2.x (ground_report E). Note the lot term keeps the
  game's 0.25 and the atlas enters at full scale.

### Summer outdoor floors (`DrawFloorAtlas`, PS class `FloorAtlas`)

- `floor_atlas_table.h`: 261 ExteriorFloors ps_3_0 pixel shaders (size + FNV-1a) that `PatchBakedAtlasPs` accepts and
  that pass `D3DDisassemble` after patching. The precomp has 571 ExteriorFloors PS, 235 of them ps_2_0 (not
  patchable this way); none of the 58 InteriorFloor PS matches. Generator: scratchpad `snowcover/test13.cpp`,
  families from `passo3/ground/techps.pl` into `scratchpad/floorfam`.
- Dispatch: `g_curClass == FloorAtlas && !g_curVsIsSnowFloor`, after the class-5 floor branch.
- Vertex shader: a copy made once per game VS with `PatchObjectLampVs(t, needColor0 = false, &tc)`: finds the world
  position dp4 triple (c8/c9/c10) and writes world xz to the **first free TEXCOORD from 7 up** (many floor VS already
  use TEXCOORD7). All 96 vs_3_0 ExteriorFloors VS export. Log kind: `Piso de fora: shader de vertice corrigido /
  sem o padrao esperado (TEXCOORDn)`.
- Pixel shader: `PatchBakedAtlasPs(t, tc, FloorPatch&)`, cached per TEXCOORD index (`g_floorAtlasPs[tc]`):
  1. before the first flow-control instruction, the **single** `texld rL, vK, sM` (2D sampler, input coordinate) whose
     next rgb reader is `mad rX.xyz, rL, cK.x, rY`;
  2. cK must be read by no other instruction;
  3. refuse if TEXCOORDn is already read or inputs >= 9;
  4. insert `dcl_texcoordN vV.xy`, `dcl_2d sE`, and right before that mad:
     ```
     mad  T.xy, vV.xyxy, cA, cA.zwzw
     texld T, T, sE
     max  rL.xyz, rL, T
     ```
- `DrawFloorAtlas` binds atlas + cA, sets **both** the VS copy and the PS copy, draws, restores both.

### Floors of upper storeys: the floor-line cut (m44-m47)

Floors of a storey are lit by the same per-storey room maps as its walls. The cut at the floor line (lamps of one
storey absent from the others) and its fix are described in [walls.md](walls.md) part 1 and
[level-light-share.md](level-light-share.md); floors benefit identically.

### Interior floors

Not changed by Night Lighting: InteriorFloor shaders are never in `floor_atlas_table.h`, and rig-mode checks keep the
object path off indoors. The census (25/09 17:05) classed InteriorWall/Floor, Ceiling, Rug, FloorThickness as "need
nothing". The combined build's Native HDR had an interior-floor tanh-curve rule (LightingTweaks) that is removed from
the standalone ([../../removed-features.md](../../removed-features.md)).

## Files and functions

| File | Function | Role |
|---|---|---|
| lot_light_bridge.cpp | `DrawFloor`, `g_floorPs` | winter floors |
| | `DrawFloorAtlas`, `FloorVs`, `g_floorVs`, `g_floorAtlasPs`, `IsFloorAtlasPs` | summer outdoor floors |
| | `ClassifyVsCode` (class 5), `ClassifyPsCode` (`FloorAtlas`) | classification |
| shader_patches.cpp | `IsFloorVs`, `PatchFloor`, `PatchBakedAtlasPs`, `PatchObjectLampVs` (floor mode), `LampScaleAfter` | bytecode |
| shader_ids.h | `kFloorVs` {1492, 0x2BC34FA8} | exact VS |
| floor_atlas_table.h | `kFloorAtlasTable[261]` | ExteriorFloors PS list |

## Game addresses and patterns

No game code is patched for floors (the storey sharing is in [level-light-share.md](level-light-share.md)).

| Fact | Evidence |
|---|---|
| Floor VS 898DEAF3 (864 bytes, summer, the one of 1788C5B6): `r1 = (0.5 v0.x, y, 0.5 v0.y, 1)`, `mul r1.xy, c18.x, v0`, `mul o3.xy, r1, c12` -> TEXCOORD2.xy = (lx, lz) x c12; `mov o2.w, r0.y` -> world y in TEXCOORD1.w | PASSO3 F-J3, confirmed by the critique |
| room+0xF8 of the floor's room: pointer to a 4x4 float matrix, row-vector convention; VS c8 = (m0, m4, m8, m12), c10 = (m2, m6, m10, m14) | PASSO3 F-J2 (`fn_006aabe0.c`); runtime values unverified |
| Outdoor floor maps (desc+0x44) do not go through the class-2 wall blur | PASSO3 F-J1 |
| The game does not run the wall test for ground/floor samples (1179 floor samples, none tested) | PASSO3 critique F9 |

## Shader details

| Patch | Registers added | Coordinates |
|---|---|---|
| `PatchFloor` | sE = max sampler + 1, cA = max const + 1, T = max temp + 1 | v0.zw (widened dcl) |
| `PatchBakedAtlasPs` | new input vV = max input + 1 declared as TEXCOORDn.xy, sE, cA, T | TEXCOORDn from the VS copy, n >= 7 |

The constant registers differ between the two 1044/812 floor variants (PASSO3 critique F3): 1788C5B6 has
`def c10, 100, 0.75, -0, 0` and `def c11`, normal-map scale c8.x, final `mul oC0.xyz, r0, c9.x`; 9BF0A41E has `def c9`,
`def c10`, final `mul oC0.xyz, r0, c8.x`. The pattern patch does not care (it appends after the highest constant), but
any HLSL replica must.

## Interactions

- [snow.md](snow.md): snow meshes lying on floors, door sills and pool edges use `DrawSnowFloor` (VS class 11).
- [objects-and-rigs.md](objects-and-rigs.md): the summer floor VS 898DEAF3 also classifies as class 10 (object VS);
  `DrawObjectLamp` needs rig mode 1/2 and falls through for floors, then the `FloorAtlas` PS class handles them (the
  dispatch checks `FloorAtlas` before the object branch).
- Summer floor PS 1044 declares s6+ and would be a `WorldCandidate`, but `RecordWorldChunk` fails (VS c15 is
  (-0.0005, 1.5, 5, 1)) -> before `FloorAtlas` existed these floors kept only the faint lot map (ground_report 4.3).

## Known limitations

- The atlas crosses walls: a crisp or bright pool from a lamp beside a garden wall reaches the tiles behind it (the game
  never occlusion-tests floor samples either, PASSO3 F9).
- 235 ps_2_0 ExteriorFloors PS and any ps_3_0 variant the pattern refuses keep the game's look.
- The passo3 ground under an up-pointing type-4 spot 0.1 m above the tile is dark under the game's own law; it is the
  wrong place to judge floor light (PASSO3 F-J4).

## Pitfalls and failed approaches

- Exact-byte class for the floor VS missed the pool edge (m66) -> pattern `IsFloorVs`.
- PASSO3 increment 2 (per-pixel floor lamps with HLSL replicas of 1788C5B6/9BF0A41E) was planned, not implemented;
  replaced by the pattern approach. Its critique items (per-variant constant lists, `c12.x`/`c12.y` separately in the
  Jacobian, per-tile offsets in `fn_006aabe0.c:111-126`) apply if it is ever revived.
- Using TEXCOORD7 blindly for the floor VS export: many ExteriorFloors VS already use 7 -> first free from 7.

## Testing in game

- Summer, night: a deck or tiled patio next to a street lamp is as bright as the grass beside it; toggle "Smooth light
  on the ground" (removes the atlas) to compare.
- Dev > Status > "Street lamps on lots": counters `floors: N` (winter, `DrawFloor`) and `outdoor floors (summer): N`
  (`DrawFloorAtlas`).
- F7 on the floor: `mod:` line / PS size. Summer floor copy: VS gains one output, PS gains an input and s(E) bound to the
  atlas render target; m62 (winter) showed the `PatchFloor` output (MD5 40D087B0, 1788 bytes) with s13 = atlas.
- Log: `Piso: corrigido`, `Piso de fora: corrigido` kinds in `[LotLightBridge] Shaders at their first draw`; a
  refused one reads `sem o padrao esperado, fica como o jogo` and (dev) lands in `S3SS\ShadersRecusados`.

## Open items

- Floors seen only in winter via VS 9B6DB72A: the per-pixel plan's floor path would have done nothing in winter (critique
  F7).
- Per-pixel floor lamps (roadmap phase 2, increment 2).
- ps_2_0 ExteriorFloors coverage.
