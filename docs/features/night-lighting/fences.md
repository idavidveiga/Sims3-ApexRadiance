# Fences, railings, posts and stairs

> **Status in the standalone:** the v0.1.0 baseline (b84d5f1) has the ground term only: `max(atlas x strength, vC)`
> (`IsInstancedStructureVs` without `worldY`, `PatchInstancedLamps` without `pixelLamps`, one cache `g_fencePs`). The
> per-pixel lamps on fences (`worldY`, `g_fenceLampPs`, `AppendPixelLamps`, `RigTracker::CurrentCentre`) are post-0.1.0;
> the user approved them and they are the **first** change to re-add.

> Instanced lot structures (fence rails and posts, railings, outdoor stairs) get almost no lamp light in the game: their
> shader reads only the rig's overflow "vertex light" slots, and one rig serves a whole fence group from its centre.
> Night Lighting patches their pixel shader by pattern so they take `max(game vertex lights, ground light atlas at the
> pixel x strength, per-pixel world lamps x strength)`. Status: working (ground term since 25/09 02:45; per-pixel lamps on
> fences since 28/09, test pending). Both build flavours.

Related: [objects-and-rigs.md](objects-and-rigs.md) (rig background, per-pixel lamp law, RigTracker),
[foliage.md](foliage.md) (the other "fence" family: vs_2_0 per-instance shrub/fence shader and its moon-shadow fix),
[snow.md](snow.md) (snow lying on fences and on stair tops uses this option and strength),
[world-atlas-and-smoothed-maps.md](world-atlas-and-smoothed-maps.md), [../../engine/light-objects-and-rigs.md](../../engine/light-objects-and-rigs.md).

## Purpose

Two shader families draw fences in the game:

1. **vs_2_0 instanced "object rig" family** (first fence capture `LightProbe-cerca` draw #106, alpha test, cull 1; same
   shaders as bushes `LightProbe-arbusto` #103: `VS_2BC7F188` + `PS_2BC82F40`, PS = `kObjectRigPs`, 600 bytes). Each
   instance (index `a0.z`) gets 3 directional lamps (directions `c27/c28/c29[i]`, colours `c54/c55/c56[i]`) + sun
   (`c137/c138`). Fixed by the rig boost and the moon-shadow replacement: see [foliage.md](foliage.md) and
   [objects-and-rigs.md](objects-and-rigs.md).
2. **vs_3_0 instanced structures** (this page): `SceneModelArray` rails, posts, railings and stairs, with `POSITION1`
   (instance translation) and `POSITION2` (instance rotation) streams. Captures: railing m03 (`PS_21846848`, hash
   964b282b, `VS_21849CA0`), stairs `LightProbe-escada` #89 (`PS_215B1440`, `VS_216C8910`), fenced-area fence m15, weak
   fence m32 (`PS_21AA4330` / `VS_21793C00`, 72 prims, textures s8..s10), grids m33/m37 (108 prims). Lamp light comes per
   vertex: `VS c4..c7` directions, `c8..c11` colours, summed into COLOR0. In m03, m15, m33, m37 all were zero; m32 had one
   grey light (direction (0.997, 0.05, 0.06), colour 0.354).

## User-facing settings

Saved in `Documents\Electronic Arts\The Sims 3\S3SS\S3SS.toml`, table `[patches.NightTerrainRelight]`.

| UI label (Advanced > Objects) | TOML key | Type | Default | Range | Notes |
|---|---|---|---|---|---|
| Fences, railings and stairs get the ground light | `cercasComLuzDoChao` | bool | `true` | - | Also enables the lamp light on snow lying on objects and on stair tops (`DrawSnowOnObject`). UI-disabled unless "Street lamps light inside lots" and "Smooth light on the ground" are on. |
| Fence light strength | `forcaNasCercas` | float | `1.0` | 0.25-2.0 | Multiplies the atlas term (`cB.x`); 1 = the same light as the ground. |
| Seamless lamp light on outdoor objects | `luzPorPixelNosObjetos` | bool | `true` | - | Shared with objects: turns on the per-pixel lamp term for fences too. |
| Object lamp light strength | `forcaLuzPorPixelNosObjetos` | float | `1.0` | 0.25-3.0 | Shared with objects: `cS.y`. |

Passed every frame by the Present hook: `LotLightBridge::SetFenceGroundLight(cercasComLuzDoChao, forcaNasCercas)`.
Combined build only: both strengths were also multiplied by the HDR lamp gain; see
[../../removed-features.md](../../removed-features.md).

## How it works

### Root cause analysis (workflow "fence-lamp-rca", 4 agents + synthesis, 25/09 02:45)

- **Cause 1 (certain).** The vs_3_0 fence/railing/post/stair shader reads lamps in `VS c4..c7 / c8..c11` =
  `VertexLightDirections` / `VertexLightColors` (rig `+0x90` / `+0xD0`, parameter ids `DAT_01158c74` / `DAT_01158c78`),
  NOT the 3 main slots (`LightDirections/Colors`, rig `+0x10/+0x50`) that every earlier fix filled. `FUN_006ba340` fills the
  vertex slots only with the EXCESS: `vcount = min(4, n - 4)` from `list[count..]`. With 4 or fewer gathered lights they
  stay zero. Proof in the same capture (window m36): PS c0 = sun, PS c1/c5 = a lamp, VS c4..c11 = 0.
- **Cause 2 (certain).** `FenceRenderManager` (`0x00A6C6F0` AddRailSceneModel, `0x00A6CBE0` AddPostSceneModel) adds every
  rail or post as an instance (0x60-byte record) of a `SceneModelArray` (0x340 bytes, constructor `FUN_006f9a10`, vtable
  `0x00FF99D8`), grouped by {room, rail/post, model, product, compositor}. ONE rig per group, at the CENTRE of the group's
  box (`FUN_006f8930` / `FUN_006f8c70`), and one instanced DIP per group.
- **Cause 3 (probable).** The room is also queried at the group centre; if it falls in a room with a roof the rig is in
  mode 0 (only that room's lights + 4 grey fill lights): matches m32 (one grey 0.354 light with w != 0).
- **Fenced area (m15).** Fences/railings that close an area create a roofless "room" (`room+0x18 = 1`); `FUN_006c7cf0 ->
  FUN_006baa70` puts the rig in mode 1 (`rig+0x1D4`); lamps then come only from the room list (`FUN_006bbde0 ->
  FUN_006bb2f0`) and `FUN_006bb270` accepts only lights with `light+8 == rig+0x1E0`; street lamps are room 0, so never.
  Fixed on the CPU by `RoomGatherThunk` at `0x006BBE70` (see [objects-and-rigs.md](objects-and-rigs.md)).
- **Premise corrected.** Earlier (note m03) the missing bit `model+0x29C & 4` / `rig+0x224 & 0x10` was blamed and
  `RigCtorForce` was added ("Also stairs, railings and columns"). The SceneModel constructor `FUN_006f5b40` already writes
  0x10 and the fence/stair code never calls `FUN_006f4840`; `FUN_006ceb20` is the CAS compositor, not the model
  constructor. RigCtorForce changes nothing for fences and stairs.
- Also found: `FUN_006ba340` drops `list[3]` when n = 4 (off-by-one at `0x006BA386` "add eax, -4"); rooms in mode 0/1 leave
  the cells and are not refreshed at dusk (`FUN_006b58f0` walks only the cells).

### Fix: ground light and per-pixel lamps in the pixel shader

**Vertex shader recognition** (`ShaderPatches::IsInstancedStructureVs`, VS class 7): vs_3_0; no `mova` and no relative
addressing; inputs `POSITION1` and `POSITION2`; outputs TEXCOORD1 and COLOR0; exactly the shape
`mov oT1.zw, rW.xyxz` (world xz into TEXCOORD1.zw) and `mad oC0.xyz, rX.w, c11, rY` (last vertex light into COLOR0). Only 3
of 116 captured VS matched at first (stairs `216C8910`, fence `21793C00`, railing `21849CA0`). Optional `worldY`: the VS
also writes `mov oT2.xyz, rN` (world normal) and `mov oT2.w, rW.y` from the same register as TEXCOORD1.zw; all 9 captured
variants (`VS_216C8910` ... `VS_28228DD8`) do, giving the pixel shader the full world position. Recorded per VS in
`g_instancedWorldY`.

**Pixel shader patch** (`ShaderPatches::PatchInstancedLamps`, ps_3_0): needs COLOR0 input (`.xyz`), TEXCOORD1 input, and
exactly one `add rD.xyz, rS, vC` (vC = COLOR0 in either operand). Inserted before that add (and the add reads the new temp
instead of vC):

```
mad  rT.xy, vT1.zwzw, cA, cA.zwzw        ; atlas uv from world xz
texld rT, rT, sE                         ; ground light atlas
mul  rT.xyz, rT, cB.x                    ; x fence strength
max  rT.xyz, rT, vC                      ; never below the game's vertex lights
; pixelLamps variant only (VS worldY):
mov  rP.xz, vT1.zzww ; mov rP.yw, vT2.w  ; world position (x, y, z)
dp3/rsq/mul -> rN = normalize(vT2.xyz)   ; world normal
<AppendPixelLamps: 8 lamps>  -> rQ
mul  rQ.xyz, rQ, cS.y ; max rT.xyz, rT, rQ
```

The declarations of TEXCOORD1 (and TEXCOORD2 for pixel lamps) are widened to `.xyzw`. Resources: sampler
`E = maxSampler+1`, `cA = maxConst+1` (atlas mapping), `cB = cA+1` (`.x` strength), and for pixel lamps `cS = cA+2`,
`cL = cA+3..cA+18`, temps `T..T+5`; refused if `maxConst + 2 + 18 >= 224`, `maxTemp + 6 >= 32` or sampler 15 is taken.
The per-pixel law is the object one: `colour x min(1, W x sat(N.l)/d^2)`, `W = 0.4 x range` (see
[objects-and-rigs.md](objects-and-rigs.md), "bake-matched falloff").

**Draw** (`DrawInstanced`, `lot_light_bridge.cpp`), reached from `OnDrawInner` when the VS is class 7 and the Night
Lighting bridge (`luzDoPosteNaGramaDoLote`) is on:
1. Off if `cercasComLuzDoChao` is off or the atlas (`LightmapSmooth::Atlas`) is not ready.
2. Per-pixel lamps only when `luzPorPixelNosObjetos` is on, the VS has `worldY`, and RigTracker reports the bound rig in
   mode 2 (outdoor) or 1 (fenced yard) with a finite centre. Lamps are chosen once for the whole group with
   `SelectPixelLamps` around the rig centre (`rig+0x140`), so they do not depend on the camera. Indoor railings/stairs
   (mode 0) get the ground term only. RigTracker is installed only when `objetosDeForaComLuzDoChao` is on, so without it
   fences have no per-pixel lamps. probe7_cerca (28/09): a fence 18 m from the nearest lamp got only the atlas at its foot
   (0.055) although its upright faces look at that lamp; this is why the lamp term was added.
3. The patched PS is cached per game PS pointer in two caches (`g_fenceLampPs` with lamps, `g_fencePs` without). If the
   lamp variant is refused, the ground-only variant is used.
4. Binds the atlas (CLAMP, LINEAR, no mip), sets `cA`, `cB = (forcaNasCercas, 0, 0, 0)`, the lamp block, draws, restores.
   The game's VS is kept.

Whether rails drawn through the instanced batch flush `FUN_006cf920` see a rig at all is unverified: RigTracker clears
the rig inside that flush; the code comment assumes fence groups are drawn through the model draw with their group rig
bound.

## Files and functions

| File | Function | Role |
|---|---|---|
| `shader_patches.cpp/.h` | `IsInstancedStructureVs(t, &worldY)`, `PatchInstancedLamps(t, out, pixelLamps)`, `InstancedPatch`, `AppendPixelLamps` | recognition and patch |
| `lot_light_bridge.cpp` | `ClassifyVsCode` (class 7), `DrawInstanced`, `SelectPixelLamps`, `PatchedFor`, `g_fencePs`, `g_fenceLampPs`, `g_instancedWorldY`, `SetFenceGroundLight` | dispatch |
| `object_light_bridge.cpp` | `RoomGatherThunk` | fenced yards on the CPU (mode 1) |
| `rig_tracker.cpp` | `CurrentMode`, `CurrentCentre` | group rig mode and centre |

## Game addresses and patterns

| Address | What | Source |
|---|---|---|
| `0x00A6C6F0` / `0x00A6CBE0` | FenceRenderManager AddRailSceneModel / AddPostSceneModel | fence-lamp-rca |
| `0x006F9A10`, `0x00FF99D8` | SceneModelArray constructor, vtable | fence-lamp-rca |
| `0x006F8930`, `0x006F8C70` | group box / rig placement at the group centre | fence-lamp-rca |
| `0x006BA340`, `0x006BA386` | rig slot distribution, off-by-one | fence-lamp-rca (not patched) |
| `0x01158C74` / `0x01158C78` | VertexLightDirections / VertexLightColors parameter ids | fence-lamp-rca |
| `0x006BBE70` | room gather call (fenced yards) | patched by ObjectLightBridge |
| `0x006F68C3` | proposed binder thunk site for filtering by rig mode (EBX = model, ECX = rig), not used; RigTracker patches the CALL at `0x006F68C5` | notes |

No byte pattern in the game code is patched for the shader part: recognition is by instruction shape.

## Interactions

- Snow on top of fences and objects (VS class 8, `PatchSnowCover`) and on stair tops (class 9, `PatchSnowRelief`) use the
  same option and strength: [snow.md](snow.md), [roofs.md](roofs.md) (stair snow routing).
- Objects: shared lamp list, law and strength (`forcaLuzPorPixelNosObjetos`).
- `PS_2673D758` (a fence/railing PS of the object family) also matches the object patch; it is drawn by class 7 first
  (notes 25/09 "Passo 2").

## Known limitations

- The atlas is the light of the GROUND (no height, no occlusion): a balcony railing gets the ground light below it, and an
  indoor stair railing near a lamp can glow. The atlas term applies whatever the rig mode; only the per-pixel lamps are
  limited to modes 1/2. If it bothers, filter by rig mode (the notes proposed a binder thunk at `0x6F68C3`; RigTracker now
  offers `CurrentMode()`).
- One lamp selection per fence group (group centre): a very long fence far from its centre may use lamps that are not the
  brightest for its ends.
- Requires the atlas (bridge + smoothed maps) and, for lamps, RigTracker.

## Pitfalls and failed approaches

- Filling the 3 main rig slots (all earlier fixes: rig boost, RigCtorForce) does not reach these shaders; they read only
  the vertex-light slots.
- Forcing `rig+0x224 & 0x10` (RigCtorForce) for fences/stairs: no effect (bit already set).
- Weak fence m32: low intensity (1/d^2 from the lamp head + rig cap) and per-vertex N.L on side faces; solved by the atlas
  and per-pixel terms, not by the rig.

## Testing in game

- Scene: a fence line and a fenced yard between street lamps and garden lamps, stairs and a porch railing outside, at night.
  Rails must be as bright as the ground next to them; faces turned to a lamp brighter.
- F7 on a rail (dev): the "mod:" line reads `CERCA/ESCADA corrigida pelo mod | luz do chao forca 1.00 | luz por pixel: N
  lampadas (forca 1.00, centro do rig)` or `nao (sem y do mundo no VS, sem rig de fora ou opcao desligada)`. Game VS c4..c11
  (vertex lights) are usually zero.
- Status (dev): `Street lamps on lots: ... fences/stairs: N ...` draws.
- Log: `Shaders at their first draw: Cerca/escada: corrigido x N` / `Cerca/escada (lampadas por pixel): corrigido x N` or
  `...: sem o padrao esperado, fica como o jogo`.

## Open items

- Confirm in game the 28/09 per-pixel lamps on fences.
- Optional rig-mode filter for the atlas term (indoor railings).
- Roadmap 1.4: instanced objects (grids, columns, repeated fences drawn with per-instance position) beyond the 9 known VS.
