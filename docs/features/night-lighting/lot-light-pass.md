# Lot light pass (street lamps inside lots)

> **Status in the standalone:** in the v0.1.0 baseline (b84d5f1) exactly as described (replacement HLSL, atlas mapping of
> c14, chunk fallback, the three experimental switches). Only the status text differs (Portuguese in v0.1.0) and the
> pre-creation of the replacement at `CreatePixelShader` (`PrecreatePs`) is post-0.1.0 (v0.1.0 compiles it at the first
> draw).

> Removes the classic Sims 3 straight cut of street-lamp light at lot borders. The lot grass light pass is redrawn
> with a replacement pixel shader that takes `max(lot light map, terrain light)`, where the terrain light is the world
> light atlas (or, before the atlas exists, the chunk light map of the lot's home chunk). Status: **working, confirmed
> by the user** (notes section 1). Both build flavours. Setting `luzDoPosteNaGramaDoLote` ("Street lamps light inside
> lots"). Part of [Night Lighting](README.md).

## Purpose

Measured with `LightProbe-lote` (dark lot grass) and `LightProbe-mundo` (lit world grass), notes section 1:
- **World grass** is drawn by the terrain chunk shader, which adds `tex2D(s8 terrainLightMap, uv) * c7.x` to sun and
  sky. The map is 256x256 per 256 m chunk: the pre-baked "stamp" of the lamps, wide soft circles. uv =
  `(position - chunk centre) / 256 + 0.5`; the chunk centre is in VS c8.w / c10.w.
- **Lot grass** is drawn by several lot terrain passes. Its light pass is a 568-byte pixel shader, blended modulate2x
  (DESTCOLOR / SRCCOLOR), that adds `tex2D(s1 lotLightMap) * c3.x`. The lot light map is solved on the CPU by
  `FUN_006be020`: colour x light+0x130 x intensity x N.L / d^2, with d measured from the lamp head. Street lamps arrive
  faint.
- The two formulas meet at the lot border: a straight cut.

The lot pass vertex shader already outputs the terrain-map uv in TEXCOORD1 (mapping in VS c14, chunk centre in
c15.xz), so the fix only needs the pixel shader.

## User-facing settings

| UI label | TOML `[patches.NightTerrainRelight]` key | Type | Default | Notes |
|---|---|---|---|---|
| Street lamps light inside lots | `luzDoPosteNaGramaDoLote` | bool | true | Main section. Live: `ApplyLive` -> `LotLightBridge::SetEnabled`. Off also disables roads, floors, fences, snow, objects (everything after order 6 in the dispatch, see [README](README.md)) |
| Smooth light on the ground | `mapaDeLuzSuavizado` | bool | true | Needed for the atlas; without it the pass falls back to the home chunk's game map |
| Soft lot edges (A/B) | `bordaSuaveLote` | bool | true | Dev only (registered in the dev build; the public build always has it on). Checkbox under "Street lamps in lots" in Developer > Lighting. Pushed every frame (`SetSoftLotEdges`), live. See "Soft lot edges" |
| Lot grass keeps the lot's own light | `gramaDoLoteUsaLuzDoLote` | bool | false | Dev only, experimental; see "Experimental game patches" |
| High lighting quality on every lot | `qualidadeAltaEmTodosOsLotes` | bool | false | Dev only, experimental |
| Street lamps count as lit in lot light solves | `postesAcesosNoCalculo` | bool | false | Dev only, experimental |

## How it works

Per draw, `OnDrawInner` in `lot_light_bridge.cpp` reaches the lot branch last (after every VS-class handler) when
the bound PS classifies as `PsClass::LotLight` (exact `kLotLightPs` = 568 bytes, FNV-1a 0xFDAD274B, `shader_ids.h`).

1. `EnsureReplacement` compiles `kReplacementHlsl` once (d3dcompiler_47 `D3DCompile`, target ps_3_0) into
   `g_replacementPs`; status becomes "Active" or "Failed: <error>" (log `[LotLightBridge] Active`). It is also
   pre-compiled when the game creates the 568-byte shader (`PrecreatePs`, combined build).
   **Standalone, 2026-09-28:** the five replacement shaders of `lot_light_bridge.cpp` (this lot pass, the object-rig
   moon-shadow fix ps_2_0, roofs, lake water, snowy roofs) are compiled at start-up on a background thread
   (`framework/shader_cache.h`, [architecture 4.6](../../architecture.md#shader-precompile); same source, entry `main`,
   flags 0). `CompilePs` now only creates the shader object from that bytecode at the first draw that needs it, so the
   first lot pass / lamp / roof / water no longer runs `D3DCompile` on the render thread. Not tested in game yet.
2. Reads VS c14..c15 (`GetVertexShaderConstantF(14, v, 2)`). Requires c14.xy == 1/256 (tolerance 1e-5), otherwise
   the game draws.
3. Terrain source:
   - **World atlas** when `LightmapSmooth::Atlas(a)` is ready. The summer lot VS computes the map uv as
     `(world.xz - c15.xz) * c14.xy + c14.zw`; for the atlas (`uv = world.xz * a.xy + a.zw`) c14 is replaced for this
     draw by `(a.x, a.y, a.z + c15.x * a.x, a.w + c15.z * a.y)`. c15 also feeds another uv (c13), so only c14 is
     touched.
   - **No atlas**: the chunk texture recorded from the world chunk draw with key `Key(c15.x, c15.z)` (the home
     chunk), or its smoothed version (`ChunkTexture` -> `LightmapSmooth::Find`). If no chunk was recorded,
     `g_lotMissing++` ("without terrain texture" in the status) and the game draws.
4. Soft lot edges (28/09): reads VS c8..c10 (lot matrix), finds the lot's rectangle (`FindLotRect`) and builds PS
   c28..c30 (`LotEdgeConstants`); see "Soft lot edges" below. Without a rectangle, or with the option off, c30 = (0, 1)
   and the pass is the plain max().
5. Binds the terrain texture to **s2**, sets s2 to CLAMP/CLAMP, LINEAR min/mag/mip, sRGB off; sets the replacement PS
   and PS c28..c30; draws; restores texture, 6 sampler states, c14, PS c28..c30 and the PS. Counter `g_lotDrawn` ("lot
   light fixed: N draws").

### The replacement shader (`kReplacementHlsl`, ps_3_0)

| Register | Content |
|---|---|
| c0 | sun colour (game) |
| c1.xyz | sun direction (game) |
| c2 | shadow-map offsets (game) |
| c3.x | lamp scale (game) |
| c4.x | sky scale (game) |
| c28 | soft edges (mod): `(dLx/du, dLx/dv, Lx0, W)`: lot-local x in metres = `dot(float3(terrainUv, 1), c28.xyz)`; W = lot width |
| c29 | soft edges (mod): `(dLz/du, dLz/dv, Lz0, D)`: lot-local z; D = lot depth |
| c30 | soft edges (mod): `(1 / band, bias, 0, 0)`: `(1/3, 0)` on, `(0, 1)` off (w = 1 everywhere) |
| s0 | sky cube (game) |
| s1 | lot light map (game) |
| s2 | terrain light (mod: atlas or chunk map) |
| s5 | sun/moon shadow map (game) |
| TEXCOORD1.xy | terrain uv |
| TEXCOORD2 | shadow projection |
| TEXCOORD4 | normal |
| TEXCOORD5 | lot map uv |

Body, equal to the game's pass except the marked lines:
```
sun   = lerp(avg of 4 tex2Dproj shadow taps, 1, edge fade) * saturate(dot(n, c1.xyz))
t     = tex2D(sTerrain, terrainUv.xy).rgb
lp    = (dot(float3(terrainUv.xy, 1), c28.xyz), dot(float3(terrainUv.xy, 1), c29.xyz))    // lot-local metres
e     = min(lp, (c28.w, c29.w) - lp);  w = smoothstep01(saturate(min(e.x, e.y) * c30.x + c30.y))
lamps = lerp(t, max(tex2D(sLot, lotUv).rgb, t), w) * c3.x         // was: tex2D(sLot).rgb * c3.x
col   = sun * c0.rgb + lamps
col   = texCUBE(sSky, n).rgb * c4.x + col
return float4(col * 0.5, 0)                                        // modulate2x
```
Capture id of the compiled replacement before the soft edges: F688FB46/1020 (MD5 prefix / size, ground_report.md). The
soft-edge version is larger (new id: read it from the next F7 capture). fxc check: `fxc /T ps_3_0 /E main` on the
string's contents (no macros; entry point `main`).

### Soft lot edges (`bordaSuaveLote`, 28/09)

**Problem** (`research\borda2`, `research\borda3`): a street lamp (#1929, type 11, head (958.4, 61.8, 1199)) stands
0.7 m inside the edge of lot 09080020A1D28860 (lot-local (29.3, 11.5); the edge is lot-local x = 30). The lot map
(s1, 256x128 A8R8G8B8, 3.94 texels/m) saturates at 1.0 within ~2.5 m of the lamp; the world atlas (terrain stamp baked
at 1 texel/m, smoothed) peaks at R 0.89 / G 0.69 there. So `max(lot, atlas)` is 1.0 just inside the edge and the world
grass shows the atlas just outside: a step at the edge. Beyond ~3.5 m inside, the atlas already wins the max (the two
terms cross at ~4 m). It happens whenever a lamp stands within ~2.5 m of a lot edge, also right after loading.

**Fix.** Within `kEdgeBand` = 3 m of the lot rectangle, the lamp term blends from `max(lot, atlas)` to the atlas term:
`w = smoothstep(0, 3, distance to the nearest edge)`; `lamps = lerp(atlas, max(lot, atlas), w)`. At the edge w = 0,
so the lot grass shows exactly the terrain term the world grass shows on the other side; the smoothstep has zero slope
at the edge, so the lot side continues the terrain's own gradient; 3 m inside and beyond nothing changes. It can only
lower the lamp term (never below the atlas), so lot edges without a lamp are unchanged. Two lots that share an edge both
fade to the same atlas at it. Only this pass (lot ground) is touched; floors, walls and indoor passes are other shaders.

**Where the lot rectangle comes from** (option (c) of the design; (a) and (b) were rejected, see Pitfalls):
- Lot-local position: the PS already receives the terrain uv `(world.xz - c15.xz) * k.xy + k.zw` (k = the c14 the draw
  runs with: the atlas mapping, or the game's 1/256 chunk mapping). The lot pass VS has the lot matrix in c8 / c10
  (`world.x = c8.x lx + c8.z lz + c8.w`, `world.z = c10.x lx + c10.z lz + c10.w`). `LotEdgeConstants` inverts both on
  the CPU (double precision) into `lotLocal = A * uv + b`, so the shader does two dot products and works for any lot
  rotation. No new dependency on the VS internals: the terrain uv formula is the one the bridge already relies on.
- Lot size: room 0 of the lot, `+0xC0` / `+0xC4` (tiles along lot-local x / z; 1 tile = 1 m). Verified in the decompile
  (28/09): `FUN_006a2740` (room rebuild) sets room 0's +0xC0 / +0xC4 from the manager's tile grid size +0x264 / +0x268
  (and +0x20 / +0x28 = size - 1, +0x1C / +0x24 = 0: the tile bounds); `FUN_0069efc0` walks tiles `[0, C0) x [0, C4)`;
  the manager's room-id grid +0x260 is bounds-checked with +0x264 / +0x268 in `FUN_006a4300`, `006a4890`, `006a4a00`...;
  `FUN_006a4c10` publishes "LotSizeParameters" = (+0x264 / 64, +0x268 / 64); `FUN_006c6ab0` gathers world lights at the
  lot centre `(C0 / 2, 0, C4 / 2)` through +0xF8. The runtime value itself has not been printed yet (see Testing).
- Matching the draw to its lot: `RefreshLotRects` (Present, every 20 frames, or 5 frames after a lot pass found no
  rectangle) walks the light update tree like LightDiag (tracker + 0x6A0 + level * 0x1A4, level 0 first, manager room
  hash +0x234 / +0x238, room 0) and keeps `{origin m12/m14, m0, m8, W, D, lot id}` from room 0's +0xF8 matrix and
  +0xC0 / +0xC4. `FindLotRect` matches VS c8.w / c10.w within 5 cm and c8.x / c8.z within 2e-3. Verified: LightDiag's
  `matriz[+0xF8]` of lot 09080020A1D28860, (-0.3746 0 -0.9272 0 | 0 1 0 0 | 0.9272 0 -0.3746 0 | 958.7 60.52 1230 1),
  is the lot pass VS c8 = (-0.3746, 0, 0.9272, 958.74), c10 = (-0.9272, 0, -0.3746, 1230.44) (this answers PASSO3
  question 3 for rotation and translation).

**Numbers from the capture** (borda3/clara, lot map T6 and atlas T7 2048x1536 with c14 = (1/1024, 1/768, 0.375, 0.5),
bilinear, along lot-local z = 11.46 through the lamp, assuming W = 30 as the lot map content ends at texel 119 =
3.9375 x 30 + 1):

| lot-local x | from edge | lot R | atlas R | before (max) | soft edges |
|---|---|---|---|---|---|
| 27.0 | 3.0 in | 0.974 | 0.644 | 0.974 | 0.974 |
| 28.0 | 2.0 in | 1.000 | 0.777 | 1.000 | 0.942 |
| 29.0 | 1.0 in | 1.000 | 0.879 | 1.000 | 0.911 |
| 29.75 | 0.25 in | 1.000 | 0.886 | 1.000 | 0.889 |
| 30.0 | edge | 1.000 | 0.876 | **1.000** | **0.876** |
| 30.25 | 0.25 out | - | 0.862 | 0.862 | 0.862 |
| 31.0 | 1.0 out | - | 0.803 | 0.803 | 0.803 |

G channel at the edge: 1.000 before, 0.683 after, 0.672 at 0.25 m outside. So the step at the edge was +14% (R) and
+46% (G) of the lamp term; after the fix the two sides of the edge are the same sample of the same atlas (0 % by
construction; 0.25 m apart they differ by 1.6 % R / 1.6 % G, the atlas' own slope). The two
probe pixels map (through the lot pass VS c4..c7, on the ground plane) to lot-local (29.68, 10.86) = 0.32 m inside
(borda3/clara, screen (0.275, 0.333, 0.039)) and (30.48, 11.22) = 0.48 m outside (borda3/escura, (0.259, 0.267,
0.027)). The affine map was checked on those points: uv (0.435249, 0.561012) -> (29.6800, 10.8600) in float32.

**Limits:** the snow lot pass (`LotLightSnow`, bytecode patch) has no feather yet; a porch lamp of the lot within 3 m of
the edge fades to the atlas near the edge (with "Lot lamps light the street" on the atlas contains it, so the change is
small); a lot the tree walk does not list (no lighting manager, room 0 not rebuilt: +0xC0 == 0) draws the plain max
(counted as "without a lot rectangle").

### Chunk registration (`RecordWorldChunk`)

For every draw whose PS is `WorldCandidate` (declares s6+), VS c15 must be (1/256, 1/256, 0.5, 0.5) (the terrain uv
mapping). The light map is found by scanning s15 down to s1 for a 2D texture of 256x256 with <= 5 levels and format !=
Q8W8V8U8 (the normal map is also 256x256 but has 9 mips and Q8W8V8U8; paint layers are 1024x1024). The key is
`Key(c8.w, c10.w)` rounded; the texture is kept AddRef'd in `g_chunks`, then `LightmapSmooth::Get` registers it for
smoothing. The world draw itself gets the smoothed map swapped into that sampler
([world-atlas-and-smoothed-maps.md](world-atlas-and-smoothed-maps.md)).

Standalone, 2026-09-29 (not tested in game yet): `RecordWorldChunk` returns the `g_chunks` entry, so the world draw no
longer looks the key up again. **Fixed 29/09 (after a player video):** the scan used to look at samplers ABOVE the ones the pixel shader declares, so a 256x256 map left bound there by an earlier draw (e.g. the neighbour chunk's map in s8 while a 3-layer chunk reads s7) was taken as this chunk's map, depending on the draw order, i.e. on the camera. The road, fence and lot grass fixes then used a map without the lamps and switched off and on together as the camera moved (and each flip re-smoothed the chunk). Now `Classify` records the samplers every `WorldCandidate` shader declares (both builds) and the scan looks only there; a texture that is already another chunk's registered map (`g_chunkOfTexture`; g_chunks holds a reference, so its address cannot be reused) is skipped too. The development build's "Street lamps in lots" line counts those skips ("leftover textures skipped when looking for chunk light maps").

### Per-channel (sampler) variants

The world terrain PS reads the light map from a sampler that depends on the number of paint layers: s8 with 4 layers,
s7 with 3 (`PS_294418E0`, notes 1b), s6 (summer 86B88B85/1420), winter s10..s12 (ground_report.md A). The first bridge
only looked at s8, so lots on 3-layer chunks were not fixed. Since 24/09 the scan covers s1..s15 and any PS declaring
s6+ is a candidate. `LightProbe-grama2` / `grama2-claro` established this.

### Chunk seams

`LightProbe-grama2`: both sides of a cut were world grass, chunks centred (1408, 1152) and (1152, 1152), cut exactly at
x = 1280. The lamp circle existed only in the chunk that owns the lamp; the neighbour did not get the overflow. That is
a defect of the map baked into the world file. The game's own rebuild includes every light whose range touches the
chunk, so a rebuild after loading fixes it (now the "world loaded" full rebuild of the reconciliation,
[terrain-relight.md](terrain-relight.md)). The smoothed maps read 6 texels from each neighbour, and lots read the atlas,
so no seam comes from the mod's side.

### Terrain source while a chunk map changes (28/09)

The smoothed maps are "correct first" ([world-atlas-and-smoothed-maps.md](world-atlas-and-smoothed-maps.md) "Update
path"): `LightmapSmooth::Find` (used by `ChunkTexture` for the no-atlas fallback and by roads) and the world draw's `Get`
return nullptr while the smoothed map is older than the game's current map, so the draw uses the game's map; the atlas
cell holds a plain 2x copy of the current map until the smoothed one replaces it. The lot pass therefore never reads a
stale or black terrain term after a rebuild or while the atlas grows.

### Lot lamp change tracking (`TrackLotLampEdits`, 28/09; bake-relevance rules 29/09)

Lives here (it uses the bridge's light enumeration, every 20 frames in `LotLightBridge::OnPresent`, or at once after a
consumed terrain rebuild via `RequestLampRefresh`) and drives the lamp-change rebuild of
[terrain-relight.md](terrain-relight.md) ("Lamp change decisions"). It tracks every lot light of a type the terrain bake
can take (lot id +0xC0/+0xC4 != 0, type +0xB0 in 3..6 or 0xB, alive 0x01, room known 0x04, room 0) with its raw fields:
flags +0x100, base colour +0xF0, intensity +0x10, range +0x130, position +0x120. A lamp is **in the bake** when it is lit
(0x20), for types 3..6 also enabled (0x40, as `TerrainLightTest`), and its light is not zero (range x intensity > 0,
colour not black); the street-lamp class is assumed to need the lit flag too (unverified). Its **light** is colour x
intensity x range per channel (the bake draws colour with weight range x intensity x 0.2). It compares the whole SET with
the previous enumeration:
- added (new pointer, in the bake), removed (pointer gone, was in the bake), moved (both in the bake, more than 5 cm):
  **user-driven**;
- entered or left the bake (lit / enabled flag, intensity to or from 0), or light changed by more than 5 % in a channel
  (floor 0.05): **automatic**; a lamp with 3 automatic changes within 60 s becomes **animated** (log `Lamp L... on lot
  ... switches or dims by itself`) and its automatic changes no longer count;
- anything else is not counted: lamps outside the bake (disabled, unlit, window lights 7/8 and type 9 are not even
  tracked), changes below the threshold (counters "outside the bake", "below the threshold", "animated");
- per lot: counted only if the lot is **settled**: present in every enumeration for 10 s and without an uncounted change
  for 5 s. An uncounted change restarts the 5 s, so a lot that is still loading (lamps trickling in) never counts;
- more than 8 counted changes in one enumeration = bulk (lamps switching together at dusk / dawn, or streaming): none
  counted, their lots restart the 5 s;
- removals are confirmed at the next enumeration: the lot must still be there and have lost no further lamp. A lot that
  vanishes (streaming out) cancels them; so the removal of a lot's only lamp is never counted. Since 29/09 the
  stuck-countdown fallback no longer catches it either (it only rebuilds for differences on lots of the last rebuild
  that still have lamps): it reaches the ground at the next rebuild.
Counted changes increment `LotLampEdits()`, and `LotLampUserEdits()` when one of them is user-driven (with its lots in
`LastUserChangeLots()`). Dev log: `[LotLightBridge] Lot lamp change: lot <id>: A added, E edited (M moved), R removed
(user-driven|automatic): L<ptr> type T: lit 1->0, enabled 1->0, intensity 1.000->0.000, colour (..)->(..), range a->b,
moved d m [enters|leaves the bake]; ...` (up to 6 lamps) or `lamp removed on lot <id> (user-driven)`; and at most once a
minute per lot `Lamp changes that do not rebuild the terrain, lot <id>: <same detail> (not in the bake | below the
threshold | animated)`. This is the diagnosis of lamps that keep changing: read which field moves.

After each enumeration the bridge keeps `CurrentBakeLamps()`: every tracked lamp (lot, type, position, light, light rect
+0x134 since 29/09, in the bake, animated), sorted by lot, plus the lots and the settled lots. Since 29/09 `DiffBake`
also lists each counted difference (`BakeDiff::changes`: lot, type, position, user-driven or not, the rect the lamp had
in the bake and the one it has now) for the local terrain relight of [terrain-relight.md](terrain-relight.md), with
`LampsOfLots` / `CoverLots` to mark those lots' lamps as baked once their chunks were re-rendered. A rect change alone
never counts as a change; it only refreshes the snapshot. The terrain relight stores the one of the enumeration
right after each consumed rebuild and compares with `DiffBake(baked, now, plainLamps)`: only lots settled now that were
in `baked`, lamps matched by type and place (5 cm, not by pointer: a lot streamed out and back in has new light objects),
counts added / removed / switched on / switched off / relit, animated lamps apart. `LotLampStatus()` feeds the Developer
line "Lamp changes". A world change clears the tracking and the snapshot. v0.1.0 compared only lamps that existed in both
enumerations, so additions and removals waited for the 15 s stuck-countdown fallback. 29/09 evidence for the rules: the
repeated "7 edited" of lot 7D6F0019FAF78910 were its 7 disabled type-3 lamps (flags 0x35 / 0xB5), never baked (see
terrain-relight.md "Lamp change decisions").

**Cost (standalone, 2026-09-29, not tested in game yet; same counts, logs and snapshot):** the refresh reads the
enumeration once (`ReadEnumeratedLamps`: the lit lamp list for roofs / water / objects when one of them is on, and the
tracked lot lamps), keeps the tracked lamps in two sorted vectors that are swapped and reused (`g_lotLampSig`,
`g_lotLampCur`; same ascending pointer order as the std::map they replace, the first reading of a pointer kept as
`emplace` did) and walks them side by side for additions / edits and then removals (the same two passes, so the dev log
details keep their order). The bake snapshot's lamps and lots are rebuilt only when a lamp was added, removed, changed a
raw field or its lot (otherwise the rebuild would give the same vectors); the settled lots, which depend on time, every
refresh. The enumeration still runs when those three options are off (the tracking needs it), and a failed enumeration
still skips the tracking only then, as before. Frame Profiler: "Lamp refresh (mod)".

### Snow variant

In snow the lot light pass is another shader (`PsClass::LotLightSnow`, 1852 bytes): it is bytecode-patched rather than
replaced, see [snow.md](snow.md) section "Snowy lot ground".

## Files and functions

| File | Function | Role |
|---|---|---|
| lot_light_bridge.cpp | `kReplacementHlsl` | replacement PS source |
| | `EnsureReplacement`, `CompilePs` | compile once (d3dcompiler_47) |
| | `OnDrawInner` (last block) | the lot pass redraw |
| | `RecordWorldChunk`, `g_chunks`, `Key`, `ChunkTexture` | chunk light map registry (key = chunk centre) |
| | `ClassifyPsCode` | `LotLight` / `WorldCandidate` classes |
| | `LotLightBridge::SetEnabled`, `Status`, `OnWorldChanged` (`ClearChunks`), `Shutdown(keepChunkMaps)`, `ChunkCount` | lifecycle |
| | `ReadLotLamp`, `InBake`, `LampChangeText`, `ReadEnumeratedLamps` (29/09), `TrackLotLampEdits`, `LotLampEdits`, `LotLampUserEdits`, `LastUserChangeLots`, `LotLampStatus`; `CurrentBakeLamps`, `LampEnumerations`, `RequestLampRefresh`, `DiffBake` (29/09) | lot lamp change tracking and the bake snapshot |
| | `LotRect`, `ReadLotRects` (SEH walk), `RefreshLotRects`, `FindLotRect`, `LotEdgeConstants`, `g_softEdges`, `kEdgeBand`; `LotLightBridge::SetSoftLotEdges`, `LotEdgeStatus` | soft lot edges |
| shader_ids.h | `kLotLightPs` {568, 0xFDAD274B} | exact gate |
| lightmap_smooth.cpp | `LightmapSmooth::Atlas`, `Find`, `Get` | terrain light source |
| patches/night_terrain_relight_patch.cpp | `LotPassStub`, `StreetLampColourStub`, `kQualitySites` | experimental game patches |
| | `g_softLotEdges` (`bordaSuaveLote`, dev-only registration), `SetSoftLotEdges` in the Present hook, checkbox + "Soft lot edges" line in `RenderDeveloperUI` | soft lot edges switch |

## Game addresses and patterns

| Address | What | How verified |
|---|---|---|
| `FUN_006be020` | street-lamp class light evaluation (vfunc+0x4C) in the lot room solve; reads effective colour +0xE0 | RE (notes section 1, header of night_terrain_relight_patch.cpp) |
| 0x006BE18C | `movaps xmm0,[esi+0E0h]` (`0F 28 86 E0 00 00 00`) inside it; experimental `StreetLampColourStub` | `ValidateBytes` before patch |
| `FUN_00c7f750` | builds the per-chunk light/fog pass; for the lot pass (`[ebp+0Ch]` = 1) binds the rebuilt terrain lightmap chunk+0xD8 | RE comment in the patch file |
| 0x00C7F87D | `mov eax,[edi+0D8h]; test eax,eax` (`8B 87 D8 00 00 00 85 C0`); context `F3 0F 10 05 38 A5 07 01 F3 0F 11 44 24 18 74 13` after it; null-bind path at 0x00C7F8B7 (`A1 80 CE 1E 01 6A 00 6A 00`) | three byte checks |
| 0x00ADB66B, 0x00ADB884 | `mov byte [esp+0Ch],0` in `FUN_00adb5a0` / `FUN_00adb850`: quality flag passed to `FUN_006a5ef0` (active lot or Build mode) | `C6 44 24 0C 00` checked |

### Experimental game patches (dev menu only)

These are kept as switches because each was one of the failed attempts listed below; they are off by default.
- `postesAcesosNoCalculo`: `StreetLampColourStub` at 0x6BE18C. For a light with lit flag (+0x100 & 0x20) clear, type
  +0xB0 == 0xB and lot id (+0xC0|+0xC4) == 0, it returns colour +0xF0 x intensity +0x10 instead of +0xE0 (0 while the
  lamp is off), so a lot solved by day still gets the street lamps. Counter "Street lamps counted as lit".
  PASSO3-PLANO F-J6 notes any future lamp packer must copy this rule.
- `gramaDoLoteUsaLuzDoLote`: `LotPassStub` at 0xC7F87D keeps the lot pass on "no terrain lightmap" (jumps to the null
  bind 0xC7F8B7) after a full terrain rebuild, because the rebuilt chunk+0xD8 has no street-lamp light inside lot
  footprints; the world pass is untouched.
- `qualidadeAltaEmTodosOsLotes`: 0xADB66B / 0xADB884 `... 00` -> `... 01`, every lot solved at the active lot's quality.
  Applies to lots loaded afterwards.

## Interactions

- [world-atlas-and-smoothed-maps.md](world-atlas-and-smoothed-maps.md): source of the terrain term.
- [terrain-relight.md](terrain-relight.md): the terrain map must contain the lamps (dusk rebuild, lot lamps in the bake,
  story gate) for the max to help.
- Roads, floors, snow, fences and objects all require this setting on (dispatch order).
- The combined build also multiplied c3.x by the HDR lamp gain (`ConstGain`) and had `DrawLampGainOnly` for the game's
  own pass when the bridge was off; that code path is not part of the standalone
  ([../../removed-features.md](../../removed-features.md)).

## Known limitations

- The terrain stamp has no wall occlusion (inferred, ground_report.md section 5): near lamps the max can show terrain
  light inside fenced or walled lot areas.
- Winter lot pass with VS 436BB272 (m58, "prefeitura") is not handled: see [snow.md](snow.md).
- Lots on chunks drawn only by the summer multi-pass terrain (light pass 475E594D/756, s0-s2/s5 only) never get a chunk
  registered; with no atlas either, they count as "without terrain texture". Note 4e's "beach lot variant" PS_29C97D28
  is this world multi-pass pass, not a lot shader (ground_report.md B).

## Pitfalls and failed approaches

From notes section 1 ("do not repeat"):
- Changing the world light collection radius: no effect.
- Re-solving room 0: no effect (kept as the dev button "Recalculate lot light now" and `recalcularLotesAoAnoitecer`).
- Rebuilding the type-5 terrain textures: no effect.
- High quality on every lot: no fix, and FPS dropped to 63 (kept as `qualidadeAltaEmTodosOsLotes`, off).
- Turning off the terrain texture in the lot layer (stub at 0xC7F87D): no fix (kept as `gramaDoLoteUsaLuzDoLote`, off).
- Counting street lamps as lit in the solve: no fix alone (kept as `postesAcesosNoCalculo`, off).
- The bridge can keep an older DXT5 chunk map in `g_chunks` if the world draw still binds it (1c); not visible in
  practice.
- `LightProbe-grama3-escura` (1c): the "dark" lot grass was only a different terrain paint; not a lighting bug. Check
  albedo before blaming light.
- m76 (25/09 16:25): a lot at z 1290 on the chunk that ends at z 1280, sampling its home chunk map with CLAMP,
  stretched the chunk's last row: dark lot with a straight edge next to a lit sidewalk. Fixed by reading the atlas; do
  not go back to home-chunk sampling when the atlas is available.
- Soft lot edges, rejected designs (28/09): (a) the lot map UV rect cannot give the lot size: the texture is sized
  `nextPow2(4 x size)` by `FUN_006a8de0` and holds more than the ground (256 wide for a 30 m lot, content ends at
  texel 119 of 256), and the VS lot-map formula `(v0 x 63/128 + 0.25) x c12` uses `def` constants the CPU cannot
  read; (b) the lot map has no coverage signal: room-0 texels have alpha 0 and outside texels are black, like any
  unlit texel (T6 dump); a content bounding box would fade porch-lamp pools in the middle of a lot. Using the lot
  pass geometry (vertex buffer bounds) was also rejected: tiles under floors may be missing, and the VB pool is unknown.

## Testing in game

- At night, stand at a lot border next to a street lamp: no straight cut between lot and world grass.
- Dev > Status > "Street lamps on lots": "Active | terrain chunks seen: N | lot light fixed: N draws ... | without
  terrain texture: M" (M should stay near 0 once the atlas is ready).
- F7 on lot grass: the covering light pass shows PS size 1020 (our replacement; MD5 prefix F688FB46) with s2 bound to
  the 2D atlas render target (2560x2560 or 4096x4096 in the captures) or a 1024x1024 A8R8G8B8 smoothed map.
- Toggle "Street lamps light inside lots" live to compare.
- Soft lot edges: F7 on the lot grass just inside the edge next to a lamp (0.2-0.5 m), then F7 on the world grass
  just outside at the same spot. The lot pass line says `mod draw: lot light pass | soft edges: lot <id>, W x D m,
  origin (x, z), band 3.0 m (PS c28..c30)`; W x D must be the real lot size (e.g. 30 x 30) and PS c28..c30 are
  listed with it. The lamp term just inside should match the terrain term just outside within ~2-3 % (the whole
  pixel within the albedo difference of the two paints). Toggle Developer > "Soft lot edges" for A/B; the line
  "Soft lot edges: on, band 3.0 m | lots known: N | lot passes feathered: M | without a lot rectangle: K" should show
  K staying near 0 once lots are loaded.

## Open items

- PASSO3 later increment 4: apply a per-pixel lamp term to the lot-map part (`max(lampTerm(lotMap), terrain)`); not
  started.
- Re-render the terrain stamp at 4 texels/m (increment 5) would sharpen both world and lot grass.

## Private investigation: 2026-10-02 continuity regression
The reported issue is continuity of street-lamp illumination across the lot/world boundary, as described in Purpose, not an unrelated object or texture edge. The 01:08:51 and 01:08:54 probes address the world and lot paths respectively. Reconstruction on the lot ground plane puts the lot probe about 0.46 m inside x=30 and the world probe about 0.48 m outside, at different positions along that edge. The lot replacement is present, with the 30x40 rotated lot transform, 3 m feather band and world atlas.
Offline comparison of the dumped atlas and world smoothed texture at matching coordinates along that edge found approximately one 8-bit level or less difference in the red lamp term. This rejects an obvious black/missing atlas cell at the tested coordinates, but does not validate final rendered continuity; paint layers, illumination composition and ground reconstruction assumptions still matter. The snapshot also has the tested type-11 lamp disabled (flags 0x35) while both maps retain its red stamp. Validate the captured switch-off correction first, then compare a settled switched-on lamp on both sides of the same border. Do not label the continuity issue fixed based on queue tests or the presence of the feather shader.

### Captures 01:25:58 and 01:26:01: both outside the lot
The user confirmed both probe pixels are WORLD terrain; 01:25:58 is the correct appearance and 01:26:01 is the wrong, brighter appearance. The latter uses the known unsupported summer multi-pass world light shader (756 bytes, FNV EC3141AB) with VS (656 bytes, FNV 5882F972). It reads the rebuilt map from s2, mapping c13, and multiplies lamp RGB by c3.x squared. The single-pass terrain already receives smoothing and Ground brightness (captured effective scale 0.75); the multi-pass light draw was left unmodified at scale 1.0. This supplies a concrete inconsistent-lighting path, independently of lot feathering.
The private follow-up recognizes that exact PS/VS pair, validates the 1/256 mapping, registers only the declared lamp sampler s2 using the existing chunk ownership checks, and uses the existing current-map smoothing cache. Ground brightness applies via c3.x multiplied by sqrt(gain), yielding the same gain after the shader squares the constant. Constants and texture bindings are restored after the draw. Other unverified multi-pass variants retain their original behavior. This supersedes the summer multi-pass limitation above for this exact captured pair; winter variants remain unsupported. Gameplay validation is still required.

### User validation, 2026-10-02
The user confirmed that installing the correct latest private terrain-variant build resolved the reported cutoff. During investigation of the 01:38:56 session, the installed ASI hash matched the earlier lamp-off package (D593AECA5885682FEA5D6EDCDAB3F038512CC6B08D8CDC1371A4F456041DD88D), which predates the multi-pass correction. The latest terrain-variant package hash is C1257AF49C613DF227B5EBAD639D795665D77E378A0FB3744DB948AFA0883B39. This is user-reported gameplay validation of the visible issue in that scenario, not a new captured GPU comparison after installation. Keep the current terrain pacing and smoothing optimizations; this report does not justify reverting them. Winter variants and other untested scenes remain outside this validation.

### Follow-up response delay, session 02:03:26
The same terrain-variant binary still classified ordinary types 3..6 enable changes as automatic and then animated after three changes. This delayed or suppressed terrain response despite the corrected rendering path. Confirmed enable changes of an already observed lamp on the same lot now take the bounded priority reconciliation used by type-11 edits. Entry settling and unrelated bulk streaming no longer hide such value-only edits; pure additions/removals retain their loading guards. The first lamp-state baseline can adopt unrelated newly observed lots even during another pending edit, excluding lots already being edited. See [terrain-relight.md](terrain-relight.md#session-020326-ordinary-switches-and-entry-responsiveness-private) for timings, safeguards and validation limits. The previously validated multi-pass shader correction is unchanged.

### Visible-lot response follow-up (private, 2026-10-02)

The verified regular lot-light pass now records the matched lot's recent visibility even when soft-edge feathering is disabled. It uses the existing manager lot id and verified vertex-matrix rectangle, without changing shader output. First draws/reappearance and late visible-lamp registration request bounded old/new footprint reconciliation instead of relying only on adopting an assumed snapshot baseline. Known visible ordinary lamp value changes (colour/intensity/range) take the priority route, including adjustments below the automatic noise threshold. Native lamp-entry events anticipate the next read, and disabled lot-owned lamps leave the direct lamp pool before fade completion. See [terrain-relight.md](terrain-relight.md#session-starting-022600-colour-intensity-and-visible-lot-arrival-private) for the intervals, receipt rules, measured-cost reserve and validation limits. Arrival detection is currently tied to the verified regular lot-light draw; unverified variants keep their prior fallback. The multi-pass world-light correction, atlas mapping, gains and edge feather remain unchanged.

## Compact single-layer world terrain (private RC, 2026-10-02)

F7 19:55:51/55 exposed a world-terrain variant with a single diffuse layer, using lamp sampler s3 rather than s6+. The existing broad WorldCandidate classifier required s6+, so this draw remained unmodified while the adjacent lot already used the shared atlas and 0.75 ground gain. The identical PS/VS pair also occurs in 19:44:08 and 19:47:36. Its rig-mode field does not make it an object; the bytecode computes chunk terrain geometry and world lamp-map UVs.

The compact PS is 1296 bytes, DWORD FNV-1a 73376C6A; paired VS is 744 bytes, 34E1F1B7. Both must match. Only s3 is eligible for lamp texture acquisition. VS c15 must still match (1/256,1/256,0.5,0.5), and the existing texture type/size/mip/ownership checks remain in force. It reuses the existing world smoothing path and the validated lamp scale lookup (PS c7.x), preserving normal, material, fog and shadow computations. Multi-pass and s6+ paths are unchanged. No additional per-frame scan or replacement shader is introduced.

This fixes a concrete omitted presentation path, not every possible terrain/material seam. 19:44:11 and 19:44:14 also contain different lot painting layers. Their same-atlas observation remains valid, but the prior interpretation of 19:44:08 as an object was incorrect; it is this omitted world variant. Preserve this distinction when investigating remaining boundaries. Compilation and captured-bytecode checks do not establish game validation; compare both newly failing and previously accepted borders, lamp on/off, rotations and terrain paint after installing the RC.
