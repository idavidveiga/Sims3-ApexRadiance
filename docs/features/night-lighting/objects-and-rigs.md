# Objects and rigs (lamp light on outdoor objects)

> **Status in the standalone:** the CPU part (`object_light_bridge.cpp`: 9-class rig boost, cap, `RigCtorForce`, fenced
> yards) and `RigTracker::CurrentMode` are in the v0.1.0 baseline (b84d5f1). The GPU patch is in v0.1.0 in its **older
> form**: per-pixel lamps chosen with `SelectLamps(x, z, 40 m)` (nearest by distance minus radius), falloff
> `sat(N.l) x sat(1 - d^2/R^2)^2` with `R = clamp(1.2 sqrt(range), 2, 25)` (lamp block .w = 1/R^2), the rig **zeroed** for
> the draw (PS c5..c7 and the VS vertex-light colours) and `rD = max(rD + Q x s, ground)` (shape C: `max(vC + Q x s,
> ground)`), ground facing factor `0.5 + 0.5 N.y` (def cH = 0.5, 0.5, 1, 0), ground strength = `forcaNosObjetos` alone.
> **Not in the standalone yet (post-0.1.0):** the bake-matched law `W = 0.4 x range` / `SelectPixelLamps`, the rule
> `max(rig + vertex lights, per-pixel, ground)` (rig kept), `sat(N.y + 1)`, `max(1, forcaNosObjetos)`; they come back one
> by one after user tests. **30/09:** `sat(N.y + 1)` was brought back (F7 120-121: an upright object got half the ground light of the
> fence of the same material beside it; installed 81934361) and undone the same morning at the user's request (`0.5 + 0.5 N.y` again).

> Outdoor objects in The Sims 3 are lit by a per-object "light rig" (sun + the 3 strongest point lights at the object's
> centre, plus 4 overflow "vertex lights"). Night Lighting fixes this at three levels: (1) CPU: the rig gather gets the
> lamps' ground-footprint brightness for all 9 light classes, stairs/railings are opened to lamps, fenced yards also
> gather street lamps (`object_light_bridge.cpp`); (2) tracking: which rig is bound for the current draw
> (`rig_tracker.cpp`); (3) GPU: outdoor Counters/Phong object shaders are patched by pattern to add per-pixel world lamps
> and the ground light atlas, with the rule "never darker than the game" (`shader_patches.cpp` PatchObjectLampVs /
> PatchObjectLampPs, `lot_light_bridge.cpp` DrawObjectLamp). Status: working (doors/windows/counters confirmed 25/09
> ~11:35; the 28/09 "dark gate" fix and the bake-matched falloff are installed, test pending). Both build flavours; F7
> detail lines, the census and the refused-shader dump are dev-only.

Related: [README](README.md), [fences.md](fences.md) (instanced structures share the per-pixel lamp code),
[foliage.md](foliage.md) (bushes/trees and the moon-shadow fix), [lamp-colour.md](lamp-colour.md),
[world-atlas-and-smoothed-maps.md](world-atlas-and-smoothed-maps.md) (the ground light atlas),
[../../engine/light-objects-and-rigs.md](../../engine/light-objects-and-rigs.md), [../../engine/shaders.md](../../engine/shaders.md),
[../dev-tools/light-probe.md](../dev-tools/light-probe.md), [../dev-tools/census.md](../dev-tools/census.md).

## Purpose

Symptoms that led here (NOTAS-ILUMINACAO.md sections 3, 4, 5, "Porta escura", "Counters"):

- Fences, bushes, props next to a street lamp got ~30% of the light the ground gets; dimmer lamps fell under the 0.1
  luminance cut; lot lamps (other light classes) contributed ~0.1 or nothing (captures m32-m41).
- Stairs and railings outside: rig lamp constants all zero (LightProbe-escada, m03).
- Fences closing an area: no street lamp at all (m15).
- Doors and windows darker than the lit wall next to them (m52, m56): the rig lights them with 3 lamps from above at a
  grazing angle.
- Modular pieces outside (counters, shelves, railings): each piece has its own rig evaluated at its own centre, so
  neighbouring pieces get different light and show colour steps ("Counters vs Phong", friend's hint, m55). Inside a lot the
  interior Counters technique reads the room light map instead of the rig, so it is continuous.
- 28/09 "dark gate": with per-pixel lamps replacing the rig, a gate 9-26 m from 8 lamps became darker than vanilla
  (LightProbe probe4_portao).

## User-facing settings

Saved in `Documents\Electronic Arts\The Sims 3\S3SS\S3SS.toml`, table `[patches.NightTerrainRelight]`.

| UI label | TOML key | Type | Default | Range | Notes |
|---|---|---|---|---|---|
| Lamps light nearby objects | `postesNosObjetos` | bool | `true` | - | Main option. Installs `ObjectLightBridge` (rig boost, cap, RigCtorForce, fenced-yard gather) and turns on the moon-shadow/foliage fixes (`LotLightBridge::SetObjectShadowFix`). Live (`ApplyLive`). |
| Object light strength | `forcaNosObjetos` | float | `1.0` | 0.25-3.0 | Advanced > Objects. Scales the rig boost (`ObjectLightBridge::SetStrength`), raises the rig cap to `cap * max(1, s)`, and sets the ground-light strength on patched objects to `max(1, s)`. Changing it re-gathers all rigs (`FUN_006b58f0`). |
| Include stairs, railings and columns | `lampadasEmTodosObjetos` | bool | `true` | - | Advanced > Objects. `RigCtorForce`: applies to rigs created afterwards (i.e. on world load). Also gates the fenced-yard gather (`g_forceAll`). |
| Doors, windows and counters get the ground light | `objetosDeForaComLuzDoChao` | bool | `true` | - | Advanced > Objects. Installs `RigTracker` and enables `DrawObjectLamp`. Disabled in the UI unless "Street lamps light inside lots" and "Smooth light on the ground" are on (the atlas needs both). Live. |
| Seamless lamp light on outdoor objects | `luzPorPixelNosObjetos` | bool | `true` | - | Advanced > Objects. Per-pixel world lamps (Q term) on objects and on fences. Off = the lamp blocks are zero, only the ground term remains. |
| Object lamp light strength | `forcaLuzPorPixelNosObjetos` | float | `1.0` | 0.25-3.0 | Advanced > Objects. `cS.y` of the patched shaders. |

Where each value goes (Present hook in `patches/night_terrain_relight_patch.cpp`, every frame):
`ObjectLightBridge::SetStrength(forcaNosObjetos)`, `SetAllObjects(lampadasEmTodosObjetos)`,
`LotLightBridge::SetObjectPixelLamps(objetosDeForaComLuzDoChao && RigTracker::IsInstalled(), forcaNosObjetos)`,
`LotLightBridge::SetObjectPixelLights(luzPorPixelNosObjetos, forcaLuzPorPixelNosObjetos)`.

Combined build only: the ground strength and the per-pixel lamp strength were also multiplied by the HDR lamp gain
(`HdrOutput::LampGain()`); that path does not exist in the standalone, see [../../removed-features.md](../../removed-features.md).

## How it works

### 1. The game's rig (background, from RE)

- Every SceneModel has its own rig (`FUN_006f7880`); only `SceneModelArray` (fences) shares one rig per group. Rig
  vtable `0x00FF4218` (set by the constructor `FUN_006bb8f0`).
- Rig layout used: `+0x08` HDLight*, `+0x10` LightDirections[4], `+0x50` LightColors[4] (slot 0 sun, 1..3 lamps),
  `+0x90` / `+0xD0` VertexLightDirections/Colors[4], `+0x140` centre (transformed bounding-box centre, `FUN_006b76b0`),
  `+0x1B4` light manager, `+0x1D4` mode, `+0x1E0` room id, `+0x224` bit `0x10` "accepts dynamic lights", `+0x225` bit 1.
- Mode (`FUN_006c7cf0`): room != 0 with roof -> 0 (interior technique), roofless room -> 1, room 0 or outside a lot -> 2.
  Only mode 0 uses the interior technique (`FUN_006f4800` -> ctx+0x34, technique table `0x00FF20F0`); modes 1 and 2 draw
  with the exterior technique.
- Outdoor gather: `FUN_006bbba0` (only if `rig+0x225 & 1` and `rig+0x224 & 0x10`) -> `FUN_006b5af0` walks the 3x3 cells of
  32 m around `rig+0x140` -> `FUN_006bb270` per light: requires `light+8 == rig+0x1E0` (room), calls light `vfunc+0x10`
  (colour record at the point), keeps it if luminance >= 0.1 (`0x01158D60`; HD objects 0.01 at `0x01158D64`).
  Room gather (modes 0/1): `FUN_006bbde0` -> `FUN_006bb2f0` (room light list; rejects world-class lights).
- Sort and distribution: `FUN_006bb1f0` sorts, `FUN_006ba340` copies the 3 strongest to slots 1..3 and only the EXCESS to
  the vertex-light slots (`vcount = min(4, n - 4)`, off-by-one at `0x006BA386` "add eax, -4" also drops `list[3]` when
  n = 4).
- Cap: `FUN_006b92a0` scales the rig down above a cap read at `0x006B9418` (`B9 A8 0B 1D 01` = `mov ecx, 0x011D0BA8`);
  the cap includes the sun.
- Binder: `FUN_006b8b30(rig)` only stores pointers into the rig in the shader parameter table (`*0x011D7530`); the
  effect pass uploads the constants and issues the DrawIndexedPrimitive in the same call tree on the Present thread. If
  `rig+0x224 & 0x10` is clear it sends zeros.
- Street lamp colour for the rig (`vfunc+0x10` of class `0xFF42F8` = `FUN_006c02a0` + record format `FUN_006bdb00`):
  `F0 x (+0x130) x I x fade(+0x20) x k2 x g^2 x k3 / (k1 x d^2)`, d = 3D distance from the lamp head, no x3.333 the ground
  gets: about 30% of the ground light.
- Updates: the rig re-gathers when a lamp turns on/off, through the night level (`FUN_006b58f0` dirties all rigs in the
  cells), or when the object moves. Room-mode rigs are removed from the cells (`FUN_006baa70`) and are not re-gathered at
  dusk.

### 2. CPU fixes (`object_light_bridge.cpp`, namespace `ObjectLightBridge`)

**Rig boost for all 9 light classes.** Each class's `vfunc+0x10` slot (vtable + 0x10) is replaced by `ClassColour<I>`:

| Slot | Original | Class |
|---|---|---|
| `0x00FF42B0` | `0x006C02A0` | vtable `0xFF42A0` |
| `0x00FF4308` | `0x006C02A0` | vtable `0xFF42F8`, street lamps (`kStreetClass`) |
| `0x00FF4360` | `0x006C0690` | vtable `0xFF4350` |
| `0x00FF43B8` | `0x006C0AF0` | vtable `0xFF43A8` |
| `0x00FF44D0` | `0x006C16D0` | vtable `0xFF44C0` |
| `0x00FF4528` | `0x006C1980` | vtable `0xFF4518` |
| `0x00FF4580` | `0x006C1BC0` | vtable `0xFF4570` |
| `0x00FF4418` | `0x006C0FE0` | vtable `0xFF4408` CircleWindowLight |
| `0x00FF4478` | `0x006C1320` | vtable `0xFF4468` TubeLight |

`ClassColour<I>` calls the original, then only when the return address is `0x006BB2B3` (after `call edx` in
`FUN_006bb270`, i.e. the rig gather; bytes `FF D2` at `0x6BB2B1` checked) runs `BoostRec`:

- lamp must be lit (`light+0x100 & 0x20`); non-street classes additionally outdoor (`flags & 0x04` and room `+0x08 == 0`)
  and type (`+0xB0`) 3..6, so interiors are unchanged;
- radius R = half the width of the light bounds rect `+0x134` {minX, minZ, maxX, maxZ}, accepted if 0.5 < R < 100;
- horizontal distance h from the head (`+0x120`), `w = (1 - h^2/R^2)^2` (the terrain-stamp footprint);
- `s = forcaNosObjetos x intensity(+0x10) x fade(+0x20) x w`; record colour `rec[4..6] = max(rec, F0(+0xF0) x s)`;
  luminance `rec[10]` recomputed with the game's weights at `0x011D1140`.

A slot that does not hold the expected function is left alone; status shows "light classes: N/9".

**Cap.** `0x006B9418` operand redirected to `g_capScaled`, updated every frame to `*(0x011D0BA8) x max(1, forcaNosObjetos)`
(original cap unless the user raises the strength). An earlier "3x cap x strength" blew out objects glued to a lamp head
(note 1b, 24/09); reverted.

**RigCtorForce** (stairs, railings, columns). The 3 constructor calls in `FUN_006f7880` (`0x006F7905`, `0x006F795C`,
`0x006F799D`, target `0x006BB8F0` `thiscall(rig, flag, model, kind)`) go through `RigCtorForce`, which sets bit 0 of
`flag` when `lampadasEmTodosObjetos` is on (the constructor turns it into `rig+0x224 & 0x10`, inferred from the RE in
notes m03). Counted as "objects opened to lamps". Correction recorded 25/09 02:45: the SceneModel constructor
`FUN_006f5b40` already sets the model bit (`model+0x29C` bit 4); the game clears it with `FUN_006f4840(0)` only for
terrain, roads, roofs, ceiling, sea, lot skirt, and objects whose script asks (message `0x827917ca` -> `FUN_006ffcb0` ->
`FUN_006f4840`). So RigCtorForce does NOT fix fences and stairs (their cause is the vertex-light slots, see
[fences.md](fences.md)); it only opens objects the game or their script deliberately closed.

**Fenced yards (rig mode 1).** Call at `0x006BBE70` (`FUN_006bb2f0` inside `FUN_006bbde0`) goes through
`RoomGatherThunk`: after the room gather, for a mode-1 rig with `+0x224 & 0x10`, run the world-cell gather
`FUN_006b5af0(cells = *(rig+0x1B4)+0x104, rig)` with `rig+0x1E0` temporarily 0 (restored in `__finally`). The game keeps
the 3 strongest of both lists. Validation: the call's target, and `ret 4` (`C2 04 00`) at `0x006B5AF0 + 0x145`. Rigs served
are remembered (max 8192) and re-gathered on the render thread with `FUN_006bbf90` (unconditional rig update) whenever the
night level (`lightMgr+0xF0`) moves by 0.1, after validating vtable `0xFF4218` and mode 1 under SEH; the set is cleared on
world change (cells pointer changed).

**Refresh.** Install, strength change and uninstall request `DirtyAllRigs` = `FUN_006b58f0(cells)` (13-byte prologue
validated: `83 EC 10 55 8B E9 33 C9 33 C0 39 4D 30`), always on the render thread (`OnPresent`); uninstall off the render
thread only sets a flag (review 25/09 item 3).

### 3. Rig tracking (`rig_tracker.cpp`, namespace `RigTracker`)

- The call at `0x006F68C5` (`CALL FUN_006b8b30`, ECX = rig) is redirected to `BinderThunk`, which stores the rig.
- Detours (Microsoft Detours via `DetourHelper`) on `FUN_006f6250` (SceneModel part draw, `thiscall(model, part, ctx)`
  `ret 8`; vtable `0xFF97F8 +0x18`, also called from `0x6F83B0`, `0x6FA0B0`; prologue `55 8B EC 83 E4 F0 81 EC 94 01 00 00`)
  and `FUN_006cf920` (instanced batch flush, thiscall + 5 args `ret 0x14`; prologue `55 8B EC 83 E4 F0 81 EC A4 0B 00 00`).
  The model-draw hook clears the rig on entry and restores it on exit (binds without a draw, nested draws, pass flag 8
  never leak); the flush hook clears it while it runs (it draws other objects' instances).
- `CurrentMode()` = `rig+0x1D4` when installed, depth > 0, rig set, same thread as the draw, vtable `0xFF4218`; else -1.
  `CurrentCentre()` = `rig+0x140`.
- The Swarm effects path (`FUN_0071cfb0`, binder at `0x71D314`) is not tracked: its draws see no rig.
- Installed only while `objetosDeForaComLuzDoChao` is on.

### 4. Per-pixel patch of outdoor object shaders

Dispatch (`lot_light_bridge.cpp`): a vertex shader is class 10 when `ShaderPatches::PatchObjectLampVs(t, needColor0=true)`
accepts it and no earlier class matched (order: roof, lake, snow lot, floor by exact id; road; instanced structure; snow
cover; snow relief; floor pattern; foliage; object; snow floor). In `OnDrawInner`, after the Night Lighting bridge gate
(`luzDoPosteNaGramaDoLote` must be on) and after road/floor/fence/snow branches, `DrawObjectLamp` runs; when it declines,
the draw falls through to the remaining branches (roof/snow VS can also be class 10). `DrawObjectLamp` requires:
`objetosDeForaComLuzDoChao` (and RigTracker installed), rig mode 2 or 1, the ground atlas ready
(`LightmapSmooth::Atlas`), the PS accepted by `PatchObjectLampPs` (cached per PS pointer, `g_objLampPs`), and the patched VS
created. It swaps VS and PS, sets the constants, draws, restores everything.

**Vertex shader: `PatchObjectLampVs`** (vs_3_0 only):
- Flow control: only `if`, `ifc`, `else`, `endif` (opcodes 0x28..0x2B) and `ret` (0x1C) are allowed; `call`, `loop`,
  `rep`, `break`, `label` refuse the shader. Skinned doors with `mova` and `if b0` (m57) are fine because the world triple
  (c199..c201 there) is outside the `if`.
- POSITIONn inputs only together with NORMALn (morph targets: 176 Phong VS blend POSITION1..3/NORMAL1..3 with c26); POSITION
  1/2 without NORMAL = instancing, refused.
- Needs a COLOR0 output (object family), TEXCOORD8 free, max output register < 11.
- World position = a triple `dp4 rW.x/.y/.z, rP, cK/cK+1/cK+2` outside branches, fixed constants (no `c[a0]`). A triple
  stays "open" until something writes rP or a component of rW it already has (split triples, Phong_VS_3875). Several full
  triples: the one whose source's last writer reads POSITION0; otherwise the unique "root" triple (forward taint: not
  computed from another triple's result, Phong_VS_3810).
- Inserts `dcl_texcoord8 oN.xyz` (N = max output + 1) and `mov oN.xyz, rW.xzy` right after the last dp4 of the triple.
- Reports `worldK` (K; `c[K..K+2].w` = object translation) and `vertexLight` (base of the rig's 4 vertex-light colours:
  4 consecutive `dp3_sat rS.s, cD, rN` + `mul/mad rX.xyz, rS.s, cD+4` whose last step writes or feeds COLOR0; found as
  c4, c8, c184 or c188 in all 588 SM3 VS). Chosen world constants in practice: c12/c15/c16/c19 (unskinned),
  c192/c195/c196/c199 (skinned).

**Pixel shader: `PatchObjectLampPs`** (ps_3_0 only; everything read/inserted must come before the first flow-control
instruction; TEXCOORD8 must not be read already; max input register < 9). Three shapes:

| Shape | Recognised by | Where the code goes |
|---|---|---|
| A | rig lamps chain ending `mad rD.xyz, rS.s, c7, rP`, then sky `mad rA.xyz, rCube, cK.s, rD` (rCube from a cube `texld` at the normal), optionally `add rX.xyz, rA, vC` (COLOR0 vertex lights, destination may differ, e.g. generic objects `add r4.xyz, r0, v0`); also the no-COLOR0 variant (PS_298DF5B8) and the 4-light specular variant (c0..c3 / c4..c7, PS_2D041628) | ground + lamp code before the cube texld; combine before the sky mad |
| B | 4-light chain c0..c3 / c4..c7, no cube, no COLOR0 (8 Counters, Counters_PS_440) | ground + lamp code before the chain end (the chain end may overwrite the normal); combine right after it. Normal = the unique register with `dp3` against c1 and against c0/c9/c13 |
| C | no lamps in the PS: light = `max(vC, per-object light map, sky)` (32 Phong, e.g. Phong_PS_3095): the single `max rX.xyz, vC, rY` and the single cube texld | code at the cube texld if it comes first (Phong_PS_3113 overwrites the normal before the max), else at the max; the max reads the new register instead of vC |

For A/B the normal register must also be used with c1 (first lamp) and with the sun (c9, c0 or c13), and must hold the same
value at the insertion point as at its reference use.

Resources added (A = maxConst+1 etc.; refused if `maxConst + 4 + 16 >= 224` or `maxTemp + 5 >= 32` or sampler 15 taken):

| Register | Content |
|---|---|
| `v(maxIn+1)` TEXCOORD8.xyz | world (x, z, y) from the VS |
| `s(maxSampler+1)` | ground light atlas (CLAMP, LINEAR min/mag, no mip) |
| `cA` | atlas mapping: uv = world.xz * cA.xy + cA.zw |
| `cB` | `.x` = ground strength = `max(1, forcaNosObjetos)` |
| `cH` (def) | (1, 1, 1, 0) |
| `cS` | (0, lamp strength `forcaLuzPorPixelNosObjetos`, 0, 1e-4) |
| `cL .. cL+15` | 8 lamps x {(head x, y, z, W), (colour r, g, b, 0)} |
| temps T, Fr, A, B, Q | T+0..T+4 |

Inserted computation (per pixel):
1. Ground term `G = atlas(world.xz) x sat(N.y + 1) x cB.x` (facing factor: full on upright and upward faces, 0 on faces
   turned down; was `0.5 + 0.5 N.y` until probe6 28/09, which gave an upright gate half of what the fence next to it got).
   Shape A with COLOR0: `G -= vC` (vC is added after the combine).
2. Per-pixel lamps `Q = sum_k colour_k x min(1, W_k x sat(N.l_k) / d_k^2)` (`AppendPixelLamps`), d = 3D distance from the
   pixel to the lamp head, `d^2 >= 1e-4`.
3. Shapes A/B: rig estimate `Rg = c5 x sat(N.c1) + c6 x sat(N.c2) + c7 x sat(N.c3)` (the game's own diffuse sum,
   chain `mov_sat rS, (N.c9, N.c1, N.c2, N.c3)` of PS_30648F80); then
   `rD += max(Q x cS.y - Rg [- vC], 0)` and `rD = max(rD, G)`. With vC added afterwards (shape A), the result is
   `max(sun + max(rig + vC, Q x cS.y), ground)`: never below the game, and Q replaces the rig (no double count) only where it
   is stronger, i.e. near lamps. The rig constants c5..c7 and the VS vertex lights are no longer zeroed (28/09).
4. Shape C: `A = max(Q x cS.y, vC, G)` and the game's `max rX.xyz, vC, rY` reads A: `max(vertex lights, per-pixel lamps,
   ground, light map, sky)`. `rigLamps = false`.

**Bake-matched falloff (28/09).** `W = 0.4 x range` (`light+0x130`), colour = `F0 x intensity x fade`. The law was fitted
on the light atlas: around a street-lamp pair (range 97 at 1.7 m and range 40 at 3.7 m) the ground follows
`2 x w x cos / d^2` with `w = 0.2 x range x intensity` (the bake's per-light value, `0xC29526..0xC29533`), with the same
factor (1.7..2.4) from 5 to 16 m. So `W = 0.4 x range` with the intensity carried by the colour, and the cap 1 is the
atlas' own saturation next to a lamp. An upright face turned to the lamp gets the cosine the ground lacks (a fence 10 m
from a lamp 3 m up: about 3x the ground value), as with real light. The previous kernel `sat(1 - d^2/R^2)^2` with
`R = 1.2 sqrt(range)` (11.8 m for a street lamp) was 0 beyond R while the atlas still shows the lamp there (source: comment
of `AppendPixelLamps`; not in the notes).

**Lamp list and selection.**
- `LotLightBridge::OnPresent`: every 20 frames, if roofs, water or `luzPorPixelNosObjetos` are on, `ReadEnumeratedLamps` (29/09; was `UpdateLampList`, see [roofs.md](roofs.md) for the `SelectLamps` memo)
  enumerates all lights with `FUN_006acf70(visitor)` (stdcall, visitor vtable[0] = thiscall(visitor, Light*); prologue
  checked: `E8 2B 36 00 00 8B 4C 24 04 51 68 40 CF 6A 00`) and keeps in `g_allLamps` every light that is alive (`0x01`) and
  lit (`0x20`) and is a street lamp (type `0xB`) or an outdoor lamp (`flags & 0x04` and room 0): head `+0x120`, visual
  radius `clamp(1.2 sqrt(range), 2, 25)` (roofs/water), colour `F0 x I x fade`, `W = 0.4 x range`.
- `SelectPixelLamps(x, z)` at the object's translation (`c[worldK..+2].w` read from the VS constants): score
  `W / (dx^2 + dz^2 + 1)`, drop below 0.002, keep the best 8 (brightness at the centre, not nearest: a street lamp at 15 m
  beats a small lamp at 6 m). Unused blocks: position (1e6, 0, 1e6), colour 0. The same lamps for every piece near each
  other, independent of the camera.

**Ground strength rule (28/09 dark-gate fix).** `ObjectGroundStrength() = max(1, forcaNosObjetos)`: objects get at least
the ground light around them. It used to be `forcaNosObjetos` alone; lowered to 0.38 to tame objects next to lamps, it left
a gate with 0.19x the ground light while the fence next to it got all of it (probe3_cerca / probe4_portao).

## Files and functions

| File | Function | Role |
|---|---|---|
| `object_light_bridge.cpp/.h` | `Install`, `Uninstall`, `ClassColour<I>`, `BoostRec`, `OnPresent`, `SetStrength`, `SetAllObjects`, `Status` | rig boost, cap, refresh |
| | `RigCtorForce`, `InstallRigCtorPatch` | open closed rigs |
| | `RoomGatherThunk`, `CellGatherForRoomRig`, `RegatherRoomRigs`, `UpdateRoomRigs`, `ReadNight` | fenced yards |
| | `InstallLampColour` etc. | lamp colour, see [lamp-colour.md](lamp-colour.md) |
| `rig_tracker.cpp/.h` | `Install`, `BinderThunk`, `ModelDrawHook`, `InstanceFlushHook`, `CurrentMode`, `CurrentCentre` | current rig per draw |
| `shader_patches.cpp/.h` | `PatchObjectLampVs`, `PatchObjectLampPs`, `AppendPixelLamps`, `kObjectPixelLamps = 8`, `ObjectLampPatch` | bytecode patches |
| `lot_light_bridge.cpp` | `ClassifyVsCode` (class 10), `ObjectVsFor`, `DrawObjectLamp`, `DescribeObjectDraw`, `SelectPixelLamps`, `ReadLamp`, `ReadEnumeratedLamps`, `EnumerateLights`, `PatchedFor`, `SaveRefused`, `DescribeDraw` | dispatch, constants, lamp list, diagnostics |
| `patches/night_terrain_relight_patch.cpp` | settings registration, Present hook, `ApplyLive`, UI | options |

## Game addresses and patterns

| Address | What | Verification |
|---|---|---|
| `0x00FF4308` | street-lamp class `vfunc+0x10` slot = `0x006C02A0` | `Install` fails "Street lamp light function differs at 0xFF4308" otherwise |
| other 8 `+0x10` slots | see table | each compared, skipped if different |
| `0x006BB2B3` | return address in the rig gather `FUN_006bb270` | `FF D2` at `-2` |
| `0x006B9418` | `B9 A8 0B 1D 01` (cap operand) | `ValidateBytes`, rewritten to `B9 <&g_capScaled>` |
| `0x011D0BA8` | cap global | read every frame |
| `0x011D1140` | luminance weights | read |
| `0x006B58F0` | `FUN_006b58f0(cells)` dirty all rigs | 13-byte prologue |
| `0x011D1860` | root; `+0x1C0` light manager; `lightMgr+0xF0` night level, `+0x104` cells | read under SEH |
| `0x006F7905`, `0x006F795C`, `0x006F799D` | rig constructor calls (target `0x006BB8F0`) | `E8` + target |
| `0x006BBE70` | `CALL FUN_006bb2f0` in `FUN_006bbde0` | target + `C2 04 00` at `0x6B5AF0+0x145` |
| `0x006B5AF0`, `0x006BBF90`, `0x00FF4218` | cell gather, rig update, rig vtable | used |
| `0x006F68C5` | binder call (target `0x006B8B30`) | `E8` + target (notes once said `0x6F68C3`; the code patches the CALL at `0x6F68C5`) |
| `0x006F6250`, `0x006CF920` | model part draw, instanced flush | 12-byte prologues, Detours |
| `0x006ACF70` | light enumeration | 15-byte check |
| `0x01158D60` / `0x01158D64` | gather luminance cut 0.1 / 0.01 | from RE (not patched) |

## Shader details

Captured object shaders (LightProbe names = session pointers): door/window `PS_2CE14418` (= `PS_2C72FF40`), sofa
`PS_2CE16998` (normal-mapped), outdoor counter `PS_2947EBF8` / `VS_29586D20`, generic object (mailbox, fountain, some
windows) `PS_26656138` = `PS_26AF02A0` with `VS_2665D348` / `VS_26AF66A0`, animated door `VS_2C744058`, Phong outdoor
`C249A5C0` and `BC7DE009` (census, rig 2), `VS_E3A718D3` (world c19..c21, view c12..c14). Game constants in these PS: sun
direction c9 / colour c8 (or c0/c4, or c13/c12 in Phong), lamps c1..c3 / c5..c7, vertex lights in VS COLOR0.

Coverage (offline "countertest" against the 2139 Counters/Phong variants of `Shaders_Win32.precomp`, v5.6, reproduced by
the perl simulators in scratchpad `counter_sh\`, REPORT.md):

| | before v5.6 | v5.6 (current rules) |
|---|---|---|
| VS (SM3 Counters + Phong) | Counters 118/236, Phong 64/352 | **588/588** |
| PS Counters | 72/184 | **136/184** (A 128, B 8) |
| PS Phong | 292/444 | **426/444** (A 394, C 32) |
| invalid after patch | - | 0 |

Not covered: 34 PS that already use all of v0..v9 (no free input; packing into COLOR1.yzw rejected for precision risk), 32
Counters "group D" (directional baked maps, no lamps: vanilla lamps never affect them either). TEXCOORD8+ is used by none of
the 2139 shaders (Counters uses TEXCOORD7 for the sink/stove cut-out UV, Phong for a projective coordinate); hence the
fixed index 8 in both VS and PS (VS and PS are patched independently, so "7 if free, else 8" could disagree).

Earlier accepted/refused lists (25/09 ~13:50, `S3SS\ShadersRecusados`, test5.cpp): accepted after generalising
`PS_298DF5B8`, `PS_298DFD88` (no COLOR0), `PS_2D03FF80`, `PS_2D041628` (4-light specular); "HD" objects with their own
per-pixel lamps (6 lights, `2D0547A0`, `2D0555B0`, `2D0558D0`, `2D055998`, `2D055BF0`; 4 lights `2D068BD8`: position in
TEXCOORD2, lamps in c0..cN, falloff `saturate(w/d^2)`) need nothing; `2BE88B48` (2 lights, no c7), `2E279A20` (7-tap soft
shadow, inverted chain, COLOR0 in a mad), `2E255A58` (308 instructions, flow control) stay vanilla; `PS_2A74E378` is the
wall family (baked light, see [walls.md](walls.md)).

## Interactions

- **Ground light atlas** ([world-atlas-and-smoothed-maps.md](world-atlas-and-smoothed-maps.md)): required; it exists only
  with "Street lamps light inside lots" and "Smooth light on the ground".
- **Fences** ([fences.md](fences.md)): the per-pixel lamp code (`AppendPixelLamps`, `SelectPixelLamps`, strength
  `forcaLuzPorPixelNosObjetos`) is shared; fences use the rig centre from RigTracker, so their per-pixel lamps also need
  RigTracker (i.e. `objetosDeForaComLuzDoChao`).
- **Foliage / summer shrubs** ([foliage.md](foliage.md)): they get the CPU rig boost but not the per-pixel patch.
- **Lamp colour** ([lamp-colour.md](lamp-colour.md)): `F0` read here is the tinted colour.
- **Level light share** ([level-light-share.md](level-light-share.md)): walls and objects now agree on which lamps light a
  story.
- **Light probe F7**: `LotLightBridge::DescribeDraw()` adds a "mod:" line per draw.

## Known limitations

- A rig and the per-pixel lamps have no wall shadow; objects inside a U of walls can get lamp light through the wall.
- The ground term is the light of the ground below, without height: an upper-story window can pick up ground light (judged
  acceptable 25/09 ~11:35, "the upper windows look fine"; a height correction was left out).
- Specular from rig lamps (c5 read twice in 84 Counters / 300 Phong PS) is not replaced, so per-piece highlights can still
  differ.
- Shape B: unknown whether c0/c4 is the sun or a 4th rig lamp (c4 is not treated as a lamp). Check with F7 if a seam
  appears on a counter without sky reflection (8 variants).
- Only 8 lamps per draw; lamps are chosen at the object centre (a very large object far from its centre could miss one).
- **Summer object HLSL gap:** summer instanced shrubs/fences drawn with the 600-byte PS `kObjectRigPs` go to
  `DrawObjectRig` (moon-shadow HLSL) and get neither the ground light nor per-pixel lamps (roadmap phase 1.5).
- Not covered at all: OutdoorProp `C0C6E0FF` (and `2BE88B48`): 1 lamp (c1 -> c3) + sun (c5 -> c4), `mad rA, sky*c8.x, AO,
  rD` with if/else before, VS already uses TEXCOORD7; instanced SingleObject/InstancedObject `PS_D4FD3CB3` / `VS_9E59FCE3`
  and `8F40BA1C` (POSITION1 instancing is refused); Sims (roadmap phase 7); indoor (mode 0) objects by design.
- Room-mode rigs only re-gather when the night level moves by 0.1 (not on single lamp changes).

## Pitfalls and failed approaches

- Zeroing the rig (PS c5..c7 and the VS vertex-light colours) when per-pixel lamps were on (v5.6, 25/09 ~17:50) made
  objects far from lamps darker than vanilla (probe4_portao 28/09). Do not zero the rig again; the max rule replaces it.
- The per-pixel kernel `sat(1 - d^2/R^2)^2`, `R = 1.2 sqrt(range)`: too short (13% of the nearest lamp at 9.4 m); replaced
  by the bake law.
- Ground strength tied to "Object light strength" alone: gates at 0.19x of the ground (see above).
- Subtracting vC in the PS to remove the vertex lights also removed the Phong ambient term `mad oC0.xyz, vNormal.w, cAmb, r`;
  the vertex-light base is found in the VS instead (now only used for F7 description).
- TEXCOORD7 for objects collided with Counters' cut-out UV and Phong's projective coordinate: fixed TEXCOORD8.
- Treating POSITION1..3 as instancing refused 176 Phong morph VS.
- Boosting only the street-lamp class left lot lamps (other classes) at ~0.1 on objects (m38/m40/m41).
- `RigCtorForce` was believed to fix fences and stairs; it does not (bit already set; see [fences.md](fences.md)).
- A 3x cap blew out objects touching a lamp head (note 1b).
- The class-10 dispatch used to return early and starved roofs/snow drawn with class-10 VS of their branches
  (25/09 ~11:15): it now falls through when the object patch does not apply.
- Shader caches are keyed by pointer: every classified shader is AddRef-pinned until Shutdown (review 25/09) so a freed
  address cannot inherit a stale class.
- `PatchLeafShadow`-style two-constant instructions are invalid on native D3D9 though DXVK accepts them; every patch here
  uses one constant per instruction.

## Testing in game

- Scene: a front door and a gate next to street lamps and lot lamps; an outdoor modular counter; a gate 10-25 m from
  lamps. At night the door must not be darker than its wall; counter pieces must show no colour steps; nothing darker than
  with the option off.
- F7 (Ctrl+Shift+F7, dev) on the object: the draw line "mod:" shows `OBJETO corrigido pelo mod | forma A/B or C | rig
  modo 2 (fora) | luz por pixel ligada (forca ..) | luz do chao forca ..`, the object position (VS `c[wk..wk+2].w`), the 8
  lamps used "of N in reach" with position, weight W, colour and distance, the game's rig PS c0..c13 (kept) and the VS
  vertex lights. For a game draw of class 10 it says why it was not replaced ("PS RECUSADO", "opcao dos objetos
  desligada", "rig de dentro de casa", "sem atlas de luz do chao"). F7 on piece A then piece B lists the registers that
  changed (automatic comparison).
- Status (dev, Developer > Status): `Objects: Active | light classes: 9/9 | lights boosted on objects: N | objects opened
  to lamps (stairs, railings...): N | in fenced areas: N`; `Street lamps on lots: ... outdoor objects: N` (draws).
- `S3SS_LOG.txt`: `[ObjectLightBridge] Classes de luz com reforco: 9 de 9`, `[ObjectLightBridge] Installed`,
  `[RigTracker] Installed`; batched lines `Shaders at their first draw: Objeto de fora (luz do chao): corrigido x N ...` or
  `...: sem o padrao esperado, fica como o jogo`.
- Dev: refused shaders are saved to `Documents\...\S3SS\ShadersRecusados\<fix>_PS_<FNV32>.bin` and
  `<fix>_PS_<FNV32>_VS_<FNV32>.bin` (named by content hash, written once, max 300 per session).
- Census/false colour (dev): magenta = lamp-lit candidate no fix claimed; `S3SS_Censo.txt`.

## Open items

- Calibrate "Object lamp light strength" in game against walls and ground (phase 0/3 of ROADMAP-NIGHT-REMAKE.md).
- Verify shape B's c0/c4.
- Phase 1 of the roadmap: foliage ground light (m79 flower), OutdoorProp recogniser (TEXCOORD8), 4-light foliage VS
  `4375A3EE`/`EAB58655` (PS `936D7C02`), instanced objects, ground light for the summer `kObjectRigHlsl` objects, optional
  COLOR1 packing for the 34 full-input PS.
- One lamp model for all surfaces (roofs and water still use `clamp(1.2 sqrt(range), 2, 25)` and `(1 - d^2/R^2)^2`).
- Sims under street lamps (roadmap phase 7).
