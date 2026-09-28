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
| Lot grass keeps the lot's own light | `gramaDoLoteUsaLuzDoLote` | bool | false | Dev only, experimental; see "Experimental game patches" |
| High lighting quality on every lot | `qualidadeAltaEmTodosOsLotes` | bool | false | Dev only, experimental |
| Street lamps count as lit in lot light solves | `postesAcesosNoCalculo` | bool | false | Dev only, experimental |

## How it works

Per draw, `OnDrawInner` in `lot_light_bridge.cpp` reaches the lot branch last (after every VS-class handler) when
the bound PS classifies as `PsClass::LotLight` (exact `kLotLightPs` = 568 bytes, FNV-1a 0xFDAD274B, `shader_ids.h`).

1. `EnsureReplacement` compiles `kReplacementHlsl` once (d3dcompiler_47 `D3DCompile`, target ps_3_0) into
   `g_replacementPs`; status becomes "Active" or "Failed: <error>" (log `[LotLightBridge] Active`). It is also
   pre-compiled when the game creates the 568-byte shader (`PrecreatePs`).
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
4. Binds the terrain texture to **s2**, sets s2 to CLAMP/CLAMP, LINEAR min/mag/mip, sRGB off; sets the replacement PS;
   draws; restores texture, 6 sampler states, c14 and the PS. Counter `g_lotDrawn` ("lot light fixed: N draws").

### The replacement shader (`kReplacementHlsl`, ps_3_0)

| Register | Content |
|---|---|
| c0 | sun colour (game) |
| c1.xyz | sun direction (game) |
| c2 | shadow-map offsets (game) |
| c3.x | lamp scale (game) |
| c4.x | sky scale (game) |
| s0 | sky cube (game) |
| s1 | lot light map (game) |
| s2 | terrain light (mod: atlas or chunk map) |
| s5 | sun/moon shadow map (game) |
| TEXCOORD1.xy | terrain uv |
| TEXCOORD2 | shadow projection |
| TEXCOORD4 | normal |
| TEXCOORD5 | lot map uv |

Body, equal to the game's pass except the marked line:
```
sun   = lerp(avg of 4 tex2Dproj shadow taps, 1, edge fade) * saturate(dot(n, c1.xyz))
lamps = max(tex2D(sLot, lotUv).rgb, tex2D(sTerrain, terrainUv.xy).rgb) * c3.x   // was: tex2D(sLot).rgb * c3.x
col   = sun * c0.rgb + lamps
col   = texCUBE(sSky, n).rgb * c4.x + col
return float4(col * 0.5, 0)                                                        // modulate2x
```
Capture id of the compiled replacement: F688FB46/1020 (MD5 prefix / size, ground_report.md).

### Chunk registration (`RecordWorldChunk`)

For every draw whose PS is `WorldCandidate` (declares s6+), VS c15 must be (1/256, 1/256, 0.5, 0.5) (the terrain uv
mapping). The light map is found by scanning s15 down to s1 for a 2D texture of 256x256 with <= 5 levels and format !=
Q8W8V8U8 (the normal map is also 256x256 but has 9 mips and Q8W8V8U8; paint layers are 1024x1024). The key is
`Key(c8.w, c10.w)` rounded; the texture is kept AddRef'd in `g_chunks`, then `LightmapSmooth::Get` registers it for
smoothing. The world draw itself gets the smoothed map swapped into that sampler
([world-atlas-and-smoothed-maps.md](world-atlas-and-smoothed-maps.md)).

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
| | `LotLightBridge::SetEnabled`, `Status`, `OnWorldChanged` (`ClearChunks`) | lifecycle |
| shader_ids.h | `kLotLightPs` {568, 0xFDAD274B} | exact gate |
| lightmap_smooth.cpp | `LightmapSmooth::Atlas`, `Find`, `Get` | terrain light source |
| patches/night_terrain_relight_patch.cpp | `LotPassStub`, `StreetLampColourStub`, `kQualitySites` | experimental game patches |

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

## Testing in game

- At night, stand at a lot border next to a street lamp: no straight cut between lot and world grass.
- Dev > Status > "Street lamps on lots": "Active | terrain chunks seen: N | lot light fixed: N draws ... | without
  terrain texture: M" (M should stay near 0 once the atlas is ready).
- F7 on lot grass: the covering light pass shows PS size 1020 (our replacement; MD5 prefix F688FB46) with s2 bound to
  the 2D atlas render target (2560x2560 or 4096x4096 in the captures) or a 1024x1024 A8R8G8B8 smoothed map.
- Toggle "Street lamps light inside lots" live to compare.

## Open items

- PASSO3 later increment 4: apply a per-pixel lamp term to the lot-map part (`max(lampTerm(lotMap), terrain)`); not
  started.
- Re-render the terrain stamp at 4 texels/m (increment 5) would sharpen both world and lot grass.
