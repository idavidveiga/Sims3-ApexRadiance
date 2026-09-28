# Foliage: bushes, trees and plants

> **Status in the standalone:** in the v0.1.0 baseline (b84d5f1) as described, except the early creation of the patched
> foliage vertex shaders at `CreateVertexShader` (`PrecreateVs`, the 64-entry pool and its log line), which is
> post-0.1.0: v0.1.0 creates the copy at the first draw. The HDR gain variant of `PatchFoliageVs` never existed there.

> Bushes, hedges, trees and small plants (summer and winter), and the vs_2_0 instanced fences that share their shader, are
> lit per vertex by the per-instance rig (sun + 3 lamps). Night Lighting fixes three things: the lamp light was multiplied
> by the moon shadow (dark side of a planter), the back side of a bush away from a lamp was black (N.L clamped at 0), and
> lot lamps barely reached them (rig gather). Status: working for the known variants (moon-shadow fix confirmed "ficou
> perfeito" 24/09; wrap lighting and winter variants installed 25/09); summer plants still darker than the lit ground
> (open). Both build flavours.

Related: [objects-and-rigs.md](objects-and-rigs.md) (rig boost that feeds the per-instance lamp colours),
[fences.md](fences.md), [lamp-colour.md](lamp-colour.md), [../../engine/shaders.md](../../engine/shaders.md).

## Purpose

- Plants in the plaza planters looked dark on one side (24/09): `LightProbe-conjunto`, `LightProbe-arbusto2`.
- The back half of winter bushes was black ("arbusto pela metade", m22, print 43).
- Winter bushes stayed dark although lamp colours arrived (m21, m63/m64).
- Plants on lots got almost no lamp light: per-instance lamp colours `c54..c80` zero or 0.10 (m38, m40) while correct
  vegetation had 0.42-0.58 (m41): lot lamp classes were not boosted (fixed on the CPU, see
  [objects-and-rigs.md](objects-and-rigs.md)).

## User-facing settings

Saved in `[patches.NightTerrainRelight]` of `Documents\Electronic Arts\The Sims 3\S3SS\S3SS.toml`.

| UI label | TOML key | Type | Default | Range | Notes |
|---|---|---|---|---|---|
| Lamps light nearby objects | `postesNosObjetos` | bool | `true` | - | Turns on everything on this page: the rig boost (`ObjectLightBridge`), the moon-shadow HLSL (`DrawObjectRig`), the foliage wrap-light VS (`FoliageVsFor`) and the winter leaf-shadow patch (`DrawLeafShadow`) (`g_objectFix`). Live. |
| Object light strength | `forcaNosObjetos` | float | `1.0` | 0.25-3.0 | Advanced > Objects. Scales the rig boost that fills the per-instance lamp colours. |

No foliage-specific strength exists. The fixes act only at night (`g_night > 0.01`, night level = `lightMgr+0xF0`,
passed by `LotLightBridge::SetNightLevel`), except the wrap-light VS which is swapped whenever the option is on.

## How it works

### The shaders

| Capture | VS | PS | Notes |
|---|---|---|---|
| bushes/fences summer (`LightProbe-arbusto` #103, `-cerca` #106) | `VS_2BC7F188` (vs_2_0, instanced, `a0.z`) | `PS_2BC82F40` (ps_2_0) = `kObjectRigPs` {600 bytes, FNV `0x0A2D0BE4`} (notes hash e9be1ab5) | lamps dir `c27/c28/c29[i]`, colour `c54/c55/c56[i]`, sun `c137/c138` |
| planter bushes (`-conjunto`, `-arbusto2`) | `VS_2BEA1650` | same 600-byte PS | lamp colours 0.8-0.99 per instance after the rig boost |
| tree (`-arvore` #252) | `VS_2BC70BD8` | `PS_2BC72CA8` (ps_2_0, no shadow) | sun `c124/c125` |
| tree 2 (`-arvore2` #107) | `VS_2BC6D848` (triangle strip) | `PS_2BC6CD58` | lamp 1 in `oD0`, lamps 2-3 in `oT4`; `max` with `def c117 = 0` |
| winter bush (m21 `-arbusto-neve`, m22) | `VS_2C55DC50` | `PS_2C565BA8` (ps_2_0, 1016 bytes) | lamps in `oT2` = sun*c147 + 3 lamps; PS `lrp_pp r3.w, t5.x, c8.y, r1.w` then `mad_pp r0.xyz, t2, r3.w, r0` |
| winter tree (m28 `-arvore-neve`) | `VS_2A839190` (1448 bytes) | `PS_2A83D588` (ps_2_0, 672 bytes) | no shadow map: light = ambient cube(t0)*c0.w + t2 |
| winter bush (m63/m64) | `VS_32779B38` (vs_2_0) | `PS_3277A240` (ps_2_0, 872 bytes) | `max r0, r0, c131.x` with `def c131 = 0`; shadow `lrp r2.w, t6.x, c6.y, r1.w` |
| summer bush (m78) | (foliage VS, patched) | `PS_32DDB220` (ps_2_0) | lamps `t1.w*v0 + t4` x shadow `lrp r1.w, t6.x, c3.z, r2.w` (`def c3 = 0.5, 0.25, 1, 0`) |
| flower on a log wall (m79) | patched VS/PS (addresses 1201...) | | rig 0.62 / 0.55 / 0.49, still darker than the ground |

In the VS, `r0.x` = sun N.L and `r0.yzw` = the 3 lamps' N.L; `max r0, r0, cK.w` clamps them at 0; each lamp weight then
scales its per-instance colour `cN[a0.c]`. In the PS, lamp light and sun share the moon-shadow multiply.

### Fix 1: moon shadow on summer instanced objects (`DrawObjectRig`)

The 600-byte PS (`PsClass::ObjectRig`, exact id `kObjectRigPs`) computes `light = (t2 x shadow + sky x c1.w) x t1.z`: the
lamp light (inside t2 with the sun) vanishes wherever the moon shadow falls (the side of a hedge or planter wall). It is
replaced by `kObjectRigHlsl` (in `lot_light_bridge.cpp`, compiled as ps_2_0 with `d3dcompiler_47` `D3DCompile`), identical
except `shadow = lerp(shadow, 1, c3.x)`; `DrawObjectRig` sets PS `c3 = (night, 0, 0, 0)` for the draw and restores it. By
day nothing changes. Checked first in `OnDrawInner` (before any VS class), only with `postesNosObjetos` on and night > 0.01.
Precreated when the game creates that PS (`PrecreatePs`).

### Fix 2: wrap lighting for lamps (`ShaderPatches::PatchFoliageVs`, VS class 6)

Recognition: any VS (vs_2_0 or vs_3_0 token) that reads the per-instance lamp array `c27[a0.x]` (relative addressing on
c27), with a `max r0, r0, cK.s` (full mask, no relative) where `cK.w` is a runtime constant, or `cK.s` is a replicated
component of a shader-defined constant equal to 0 (winter bush m63 `c131.x`, tree 2 `c117`). Patch:

```
def c255, 1/1.5, 0.5/1.5, 0, 0          ; inserted at the top
max r0.x, r0, cK.s                      ; the sun keeps the plain clamp (original instruction, mask .x)
mad r0.yzw, r0, c255.x, c255.y          ; lamps: N.L / 1.5 + 1/3
max r0.yzw, r0, cK.s                    ; ... clamped at 0
```

So each lamp weight becomes `max(N.L/1.5 + 1/3, 0)`: light passing through leaves; the back side of a bush now gets a
third of the facing side. Tested offline on `2BC7F188`, `2BEA1650`, `2C55DC50`, `2A839190`, later `32779B38` and
`2BC6D848` (VS count 6 -> 8 after accepting def-zero clamps, test7.cpp on 77 VS / 99 PS). The road VS is correctly refused.

Dispatch: in `OnDrawTracked`, when the VS is class 6 and `postesNosObjetos` is on, the patched copy is bound around the
whole draw handling (`FoliageVsFor`), then the PS side runs (`OnDrawInner`: ObjectRig replacement or leaf-shadow patch);
if no PS branch draws, the game's PS draws with the patched VS; the game's VS is restored afterwards. Patched VS copies are
created early: when the game creates a VS that `PatchFoliageVs` accepts, `PrecreateVs` builds the copy and parks it in a
pool keyed by the patched bytecode (max 64 waiting, `kMaxPendingVs`) until the first draw asks for it. Log line:
`foliage vertex shader copies made at shader load: N new, N total, N used`.

Combined build only: an HDR lamp gain was folded into the same `c255` wrap constants (a second VS copy per gain, only where
the lamp weights just scale the per-instance colours); not in the standalone, see
[../../removed-features.md](../../removed-features.md).

### Fix 3: moon shadow in winter/other foliage PS (`ShaderPatches::PatchLeafShadow`, `DrawLeafShadow`)

For any PS drawn with a foliage VS (class 6) that is not the exact 600-byte one. Recognition: `lrp rD.w, tN.x, cK.s, rS.w`
(the shadow fade; `t5.x` in the 24/09 bushes, `t6.x` in m63/m78), with `cK.s` = runtime `cK.y`, or a replicated
component of a def equal to 1 (m78 `c3.z`). Patch right after it (two instructions, because ps_2_0 allows only one constant
register per instruction):

```
add rT.w, cK.s, -rD.w
mad rD.w, rT.w, cN.x, rD.w              ; rD.w = lerp(rD.w, 1, night)
```

`cN = maxConst + 1` (refused if >= 32 or temp >= 12, ps_2_0 limits). `DrawLeafShadow` sets `cN = (night, 0, 0, 0)` for the
draw; only at night.

### Rig side (CPU)

The per-instance lamp colours `c54..c56[i]` come from the object rigs, so the 9-class rig boost and the cap change of
`ObjectLightBridge` apply (see [objects-and-rigs.md](objects-and-rigs.md)). Before the boost reached lot lamp classes,
lot plants had 0-0.10 per-instance colours.

## Files and functions

| File | Function | Role |
|---|---|---|
| `lot_light_bridge.cpp` | `kObjectRigHlsl`, `EnsureObjectReplacement`, `DrawObjectRig` | fix 1 |
| | `ClassifyVsCode` (class 6), `FoliageVsFor`, `CreateFoliageVs`, `PoolFoliageVs`, `ClearVsPool`, `PrecreateVs`, `OnDrawTracked` | fix 2 dispatch |
| | `DrawLeafShadow`, `g_leafPs` | fix 3 |
| | `ObjectStatus` | status line |
| `shader_patches.cpp/.h` | `PatchFoliageVs(t, lampGain = 1)`, `PatchLeafShadow(t, nightConst)` | bytecode patches |
| `shader_ids.h` | `kObjectRigPs` {600, `0x0A2D0BE4`} | exact id of the summer PS |

## Game addresses and patterns

No game code is patched for foliage; everything is D3D9-level (shader swap by class). Patterns:

| Pattern | Used by |
|---|---|
| relative read of `c27` (`c27[a0.x]`) | foliage VS detection |
| `max r0, r0, cK.w` (runtime) or `max r0, r0, cK.s` with `def cK.s = 0` | wrap light site |
| `lrp rD.w, tN.x, cK.s, rS.w` with runtime `cK.y` or `def cK.s = 1` | leaf shadow site |
| PS size 600 + FNV-1a `0x0A2D0BE4` | summer object-rig PS |

## Interactions

- Fences of the vs_2_0 instanced family get all three fixes too (same shaders as bushes).
- Snow: winter foliage VS/PS are covered by fixes 2 and 3; see [snow.md](snow.md) for ground snow.
- Lamp colour: per-instance colours were pink (0.77/0.58/0.61 in m21) before [lamp-colour.md](lamp-colour.md).
- The `def c255` is inserted unconditionally; the notes planned to check that no relative-addressed per-instance array
  reaches c255, which is not verified in the code (vs_2_0 guarantees 256 constants).

## Known limitations

- No ground light on foliage: summer plants and flowers stay darker than the atlas-lit ground next to them (m79; roadmap
  1.1: export the atlas uv from the vs_2_0 VS through a free constant `c254` and `max(light, atlas)` in the ps_2_0 PS, two
  formats: `add r3, r3, t4` in the bush, `mad r0, t2, shadow, sky` in the flower).
- **Summer object HLSL gap:** the summer 600-byte PS goes to `kObjectRigHlsl` (moon shadow only): no ground light and no
  per-pixel lamps (roadmap 1.5).
- Plant 2 (m65): already drawn with the patched shaders but dark; its PS multiplies all lamp light by `texld s2` (TEXCOORD3,
  a 128x128 DXT1 texture, while plant 1 has an 8x8 white one). Not resolved; a new F7 on that plant was requested (the F7
  capture now decodes DXT1/3/5).
- Foliage with a 4-light matrix (`VS_4375A3EE` / `EAB58655`, `PS_936D7C02`, 46 draws in the census; `max r0, r0, c31.w`;
  directions `c8..c10`, colours `c11..c13`): not recognised (no `c27[a0]`); which light is the sun is unknown.
- Large SpeedTree trees use another path (`TreeLightColors` / `TreeLightDirections`); the captured trees did not use it.

## Pitfalls and failed approaches

- The planter darkness was first attributed to the room-mode gather (objects inside the plaza "room"); the measurement
  (`-conjunto`, `-arbusto2`) showed lamps arriving at 0.8-0.99 and the cause was the moon shadow multiplying lamp light.
- `PatchLeafShadow` first emitted `lrp` with two constant registers: accepted by DXVK, invalid on native D3D9 (review
  25/09 ~03:30 item 5). Now add + mad.
- `PatchFoliageVs` initially accepted only a runtime `cK.w`; the winter bush m63 (`def c131 = 0`) was never classed as
  foliage. `PatchLeafShadow` initially required `t5.x` and runtime `cK.y`; m63 uses `t6.x`, m78 `def c3.z = 1`.
- When `cK` is a def in `PatchLeafShadow`, its value must be exactly 1 (the "no shadow" end), added in the 25/09 review.

## Testing in game

- At night, walk around a planter with hedges and a street lamp: both sides of the planter lit; the back of a bush facing
  away from the lamp should be dim, not black. Winter: same with snowy bushes.
- Status (dev, Developer > Status): `Shadow: moon shadow on objects: fixed | draws fixed: N | foliage (wrap light): N |
  winter foliage without shadow: N`.
- F7 on a bush: VS `c54..c62` per-instance colours should be well above 0.1 near lamps; the VS/PS addresses of the patched
  copies differ from the game's (e.g. m65 "8C8E...", sizes +60 / +36 bytes).
- Log: `[LotLightBridge] Sombra dos objetos: ativo`; batched `Folhagem: shader de vertice corrigido x N`,
  `Folhagem (sombra da lua): corrigido x N`.

## Open items

- Ground light on foliage (roadmap 1.1) and on summer `kObjectRigHlsl` objects (1.5).
- 4-light foliage VS (1.3).
- Plant 2 texture multiply (m65).
