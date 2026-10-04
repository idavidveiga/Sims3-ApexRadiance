# Walls

## Latest daytime balance candidate (2026-10-04)

After the player reported that the 0.25 daytime response was still too strong,
the shared daytime surface factor is now 0.08. Wall scale is
`native * gain + (1-night) * min(gain,1) * 0.08`; full night retains the exact
previous multiplication. This is visual tuning awaiting gameplay approval.
The 11-32-34 session stayed at night; it cannot establish a new daytime shader
failure. Solar constants, materials and terrain intensity remain unchanged.

> **Status in the standalone:** in the v0.1.0 baseline (b84d5f1) as described: level light share (with the cross-storey
> wall test) and the wall gain table. The per-pixel wall plan (PASSO3) was never implemented in any build.

> Outside walls get lamp light from two Night Lighting parts: (1) **level light share**
> (`level_light_share.cpp`) puts the outdoor lamps of every storey into every storey's room-0 light list and tests
> them against the walls of the lamp's own storey, so the light no longer stops at the floor line; (2) **wall gain**
> ("Lamp light on outside walls", `forcaNasParedes`) multiplies the baked lamp term of the 58 ExteriorWall pixel shaders
> for each draw, because the game lights outside walls much dimmer than the objects in front of them. No wall shader is
> rewritten. Status: **working** (user: "bem melhor" / "quase perfeito" after the second round). Per-pixel lamp light
> on walls (PASSO3 plan) was designed but **never implemented**. Part of [Night Lighting](README.md).

## Purpose

How a wall is lit (`LightProbe-andar2-b` m44, `andar1-b` m45, `passo3-parede` m59, m75):
- Wall VS/PS pair seen in m44/m45: VS_2669D978 / PS_2669DA40 (1372 bytes). m59: VS_2734A4D8 / PS_2734BB80, same
  bytes as PS_2669DA40 (verified: both are 1372 bytes, FNV-1a 0x04956FE9 = `ExteriorWall_PS_1119` in
  `wall_lamp_table.h`, K = 3). m75's PS_29548148 is the same shader.
- Lamp light comes from **s2, one light map per storey** (256x128 in m44/m45: T9 upper storey, T3 ground; 1024x512 in
  m59): an atlas of small strips, one per wall piece. uv from the vertex: `o2.xy = TEXCOORD1 x 1/4096`.
- PS: `texld r1, v1, s2` then `mad r5.xyz, r1, c3.x(=1), r2`.
- m59 PS details: normal map s7, sun shadow `texldp s5` (4096^2), cubes s0/s1, texkill, about 70 instructions; camera
  in VS c11.

Two defects:
1. **Cut at the floor line** (m44/m45, user print): a sconce on the upper storey lit its own wall; the ground-storey
   wall just below stayed dark with a straight cut. Cause: each storey has its own lot lighting manager; room 0's
   light list of a storey only gathers that storey's lamps (details below).
2. **Walls darker than objects** (m75, 25/09 16:25): the wall's lamp light is only its baked map x cK.x, a room solve
   with k2 = 0.075 (F8 16:09: k1 = 1, k2 = 0.075, Cmax = 2, street factor 3.333, type-5 s = 5, 2 blur passes, mode 0),
   much dimmer than the rig lamps objects get.

## User-facing settings

| UI label | TOML key | Type | Default | Range | Notes |
|---|---|---|---|---|---|
| Outdoor lights reach every story | `luzExternaEntreAndares` | bool | true | | Main. Live: `LevelLightShare::Install/Uninstall` (re-gathers all lots). Full detail in [level-light-share.md](level-light-share.md) |
| Lamp light on outside walls | `forcaNasParedes` | float | 2.0 | UI 0.25..4; `LotLightBridge::SetWallGain` clamps 0.25..8 | Buildings / Walls. Lamp RGB multiplier by day and night. Off (`paredesComLuz`) preserves native walls. Live (pushed every frame) |

The wall gain works even with "Street lamps light inside lots" off: `WallGain` is dispatched before the bridge-enabled
check in `OnDrawInner`, and `UpdateHooks` keeps the draw hooks registered while the wall feature is enabled, including gain 1.

## How it works

### Part 1: lamps of every storey (summary; see [level-light-share.md](level-light-share.md))

Room lists are built on the light-tree thread by `FUN_006c7010` -> `FUN_006c6ab0(treeLevel, room)`:
- `FUN_006c6990(level, room, 1)` adds the lot lights of that storey in that room (filter `FUN_006c7820`: lit, type > 2);
- only for room 0 of **level 0**, it is called again with ownFloor = 0 (0x6C6B16 `cmp [edi+0x1a0],0 / jnz`), so ground
  lamps count twice (F8 01:39: type 3 twice, type 11 three times);
- then world lights from the light cells (`FUN_006b66b0`).
The level tracker keeps levels -4..7 at `tracker + 0x6A0 + L*0x1A4`, the level number at +0x1A0 (`FUN_006c70c0`).
`FUN_006c7250` refreshes room 0 of levels 0..7 only when room 0 of **level 0** changes (0x6C73B6..0x6C7426), which
suggests EA intended upper storeys to see ground lamps and the test in `FUN_006c6ab0` is inverted.

The fix (all in `level_light_share.cpp`):
1. `OutdoorGather` on the two calls of `FUN_006c6ab0` (0x6C5816 room creation, 0x6C7094 update): after the game's
   gather, room 0 of levels 0..7 also takes the outdoor lights of the other levels 0..7, with the weight each has on its
   own storey (level 0 twice, others once). Basements (< 0) untouched.
2. 0x6C73B1 `JNZ -> JL`: room 0 of any level 0..7 changing refreshes all storeys.
3. Cross-storey wall occlusion, the "floor line" second round (m46/m47, F8 09:50, lot C49C001BCF2DEA20): the 19 lights
   of room 0 were right on every storey, but a sconce near a corner on the upper storey lit the lower storey's side
   wall around the corner. `LightPointWithAllLights` (0x69FD60) tests each light with `FUN_0069fc40` against
   **room+0x30**, the 2D wall list of the room's own storey only (built by `FUN_006a1de0` from room+0xD8..+0xDC in
   `FUN_006a2740`), and `FUN_0069aa90` blocks a ray only below the wall top (no base test). The mod wraps vfunc+0x4C of
   the 9 light classes; when called from that loop (return address 0x69FE19) for a light of another storey, it also runs
   the game's `FUN_0069fc40` against room 0 of the lamp's storey and every storey in between and scales the colour.
   The room's own lights stay exactly as the game computes them (a porch lamp under an overhang is not blocked by the
   upper storey's walls).

**Per-batch wall culling mirror.** For batches of more than 3 wall samples the game does not test every wall:
`FUN_006a31d0` (the batch solve, call 0x6A3336) first builds, per light, the walls whose culling edge crosses the
segment from the batch centre (mean of the samples, `FUN_0069f1e0`) to the light (`FUN_006a30b0` -> `FUN_0069dff0`,
call site 0x6A311F), and each sample is tested only against those. The mod mirrors this for cross-storey lights:
`SolvePointBatch` recognises samples of the global batch vector at 0x01158AC8 (0x30 bytes per sample; the 4 callers of
`FUN_006a31d0` push it at 0x6A3B03, 0x6A3687, 0x6A37CD, 0x6A3956, byte-checked `68 C8 8A 15 01`), computes the same
centre, and `CulledWalls` calls the game's own `FUN_0069dff0(room0 + 0x30, &list, centre, lightPos)` per (light,
storey). It also copies the solving room's wall mode byte room+0x639 onto the tested room for the duration of the test
(`FUN_0069fc40` reads it from the room it is given; review 25/09 10:40 item 2). Only on the gather thread; otherwise
counted as "on another thread".

### Part 2: wall gain (`DrawWallGain`, lot_light_bridge.cpp)

- `wall_lamp_table.h` lists the 58 pixel shaders of the game's ExteriorWall technique (`Shaders_Win32.precomp`) with
  size in bytes, FNV-1a 32 over DWORDs, and **K**, the constant whose .x scales the baked map: c2 in 26 entries, c3 in
  32. Generator: scratchpad `snowcover/test11.cpp`. The baked map is the texld at TEXCOORD1 (s2 in the full variants,
  s1 in the simple ps_2_0 ones) or, in 4 variants, the single 2D texld (TEXCOORD3) read by `mad ..., cK.x`; K is read
  nowhere else. None of the other families (InteriorWall, ExteriorWallAOSI, UnlitExteriorWall; 27 variants) matches.
- K is per variant because in 24 variants c3.x is the **bloom threshold**, not the lamp scale (notes 25/09 16:25).
- `ClassifyPsCode` -> `WallLampConst(code, size)`: FNV-1a match -> `PsClass::WallGain`, K stored in `g_wallConst[ps]`.
- Per draw: if disabled, the game draws. Otherwise read `cK` and use
  `TerrainLightingPolicy::WallLampScale`: `native * gain + (1 - night) * min(gain, 1) * 0.25`.
  At full night the exact previous multiplication is returned without adding zero.
  The daytime complement is limited to a quarter of the unboosted lamp scale:
  the 11:13 follow-up confirmed full-day c3.x=1 with the latest installed build,
  but the user still found the wall too saturated. This is visual tuning; the
  quarter response requires gameplay approval, not a claimed physical ratio.
  Saved strengths below 1 remain lower; no saved setting is rewritten.
  Only `.x` changes; draw once and restore the full constant. Invalid inputs,
  failed reads, unknown shaders and unchanged scales preserve the native draw.
  Counter `g_wallDrawn`. The shader itself is never changed.

Daytime F7, 2026-10-04 10:28:03: `PS_265EBE18.bin` is 1372 bytes,
FNV-over-DWORDs `04956FE9` (ExteriorWall_PS_1119, K=3). The captured
`c3=(0,0.188235313,0,0)` makes `mad r5.xyz,r1,c3.x,r2` discard all
lamp RGB sampled from s2. This confirms a rendering-scale defect, independent
of whether the particular lamp has reached the sampled wall strip. The map
contains nonzero RGB elsewhere; it is not proof of illumination at that pixel.
The new daytime term exposes the existing baked lamp RGB without changing
sun, sky, wall-map alpha, textures or occlusion. A separate enabled state keeps
Off native even at daytime, while 100% enabled supplies the day term.

Validation uses the actual captured pixel shader on native D3D9 with controlled
materials/maps: the old zero-factor failure is reproduced, corrected lamp RGB
is read back by day and twilight, full-night pixels remain identical, and empty
lamp maps remain unchanged. Extracted production draw tests cover toggles,
failed reads, constant restoration and invalid factors. This does not replace
visual validation of the player's lot or establish instant wall-map baking.

## Files and functions

| File | Function | Role |
|---|---|---|
| lot_light_bridge.cpp | `WallLampConst`, `g_wallConst`, `DrawWallGain`, `SetWallGain`, `WallStatus` | wall gain |
| wall_lamp_table.h | `kWallLampTable[58]` {size, hash, constant} | the 58 ExteriorWall PS |
| level_light_share.cpp | `OutdoorGather`, cascade patch, `SolvePointBatch/Single`, `LightEvalHook<I>`, `CrossFloorShadow`, `WallPass`, `CulledWalls`, `BatchCentreFor`, `GameWallTest`, `DiagText` | lamps of every storey |
| patches/night_terrain_relight_patch.cpp | `RegisterFloatSetting(&g_wallStrength, "forcaNasParedes", ...)`, Present: `SetWallGain(g_wallStrength)` | settings |

## Game addresses and patterns

| Address | What | Evidence / check |
|---|---|---|
| 0x006C6AB0 | `FUN_006c6ab0` room gather, thiscall(treeLevel, room) ret 4; calls at 0x6C5816, 0x6C7094 | notes m44/m45; level_light_share.cpp |
| 0x006C6990 | per-storey gather thiscall(treeLevel, room, char ownFloor) ret 8; calls 0x6C6B08, 0x6C6B2D | same |
| 0x006C73AA / 0x006C73B1 | cascade test `cmp [esi+0x1a0], eax / jnz` -> patched to `jl` (0x85 -> 0x8C) | same |
| 0x006C54E0 (0x6C5525) | lot load: re-gathers room 0 of all storeys with level 0's treeLevel; the room's real storey is room[0]+0x88 | review 25/09 |
| 0x0069FD60 | `LightPointWithAllLights` thiscall(room, out, l2D, l3D, flags, sample) ret 0x14; calls 0x6A1187, 0x6A126F (`FUN_006a0f50`), 0x6A3336 (`FUN_006a31d0`) | RE |
| 0x0069FE19 | return address after `call edx` (light vfunc+0x4C) inside it | RE; the hook's filter |
| 0x0069FC40 | 2D wall test thiscall(room, int* indexVec, lightPos, sample, float* t) ret 0x10; game call 0x69FE93 | RE |
| 0x0069DFF0 | per-light wall culling thiscall(walls, out, from, lightPos) ret 0xC; call 0x6A311F in `FUN_006a30b0` | RE |
| 0x01158AC8 | batch sample vector {begin, end}, 0x30 per sample | pushes byte-checked |
| room+0x30 | 2D wall occluders of the room's storey | RE |
| room+0x639 | wall mode byte read by `FUN_0069fc40` | review 25/09 10:40 |
| room+0x63C | per-light threshold in the solve (0x69FE19-0x69FE4D) | PASSO3 F-J |
| room+0xF4, `DAT_01158b1c`, `DAT_011d02e4` | class 2 wall blur passes over the wall atlas ([1 2 1]/4 separable default, 2x2 box with half-texel shift otherwise) | PASSO3 F-J1 (`fn_0069f650.c`) |
| light vfunc+0x4C of 9 classes | 0x6BDE90, 0x6BE020, 0x6BE1C0, 0x6BEFD0, 0x6BFBA0, 0x6BFDC0, 0x6BFFB0, 0x6BF880 (CircleWindowLight), 0x6BFA70 (TubeLight) | level_light_share.cpp `kClasses` |

## Shader details

- Wall gain only changes one constant's .x for one draw: c2.x or c3.x, per `wall_lamp_table.h`.
- The wall atlas alpha: room-0 texels always have alpha 0; indoor texels have alpha up to 1, used as sky term by
  InteriorWall families (PASSO3 critique F2). Relevant for any future per-pixel wall work.
- Wall VS facts for future per-pixel work (PASSO3 critique F1): `max r1.xy, c12.xzzw, v7.xzzw`,
  `min r1.w, r1.x, c12.y`, `mad r2.y, r1.w, r0.w, r0.z` lower walls in cutaway / walls-down mode
  (y = lerp(v0.w, v0.y, clamp(v7.x))), while the atlas uv (`mul o2.xy, c20.y, v3`) is not lowered.

## Interactions

- Walls and objects: before the storey share, the rig of a window picked outdoor lamps of any storey (room id 0 is 0
  on every storey) while the wall around it did not, giving "the lamp lights the wall of one storey and the window of
  another" (friend's tip 25/09 10:30). With the share both see the same lamp list; remaining differences: the rig uses
  the 3 strongest lamps at the object centre without wall shadow; the map sums every lamp per point with wall
  occlusion. See [objects-and-rigs.md](objects-and-rigs.md).
- Split-Level Lighting Fix (S3SS): type-11 lot lights enter every storey through the world gather; the share handles
  types 3..6 too.
- Floors of each storey use the same per-storey maps; see [floors.md](floors.md).

## Known limitations

- A balcony slab on the upper storey does not block light going down: blockers of a room are its own storey's walls
  (same as the Split-Level fix does for type 11).
- Wall gain scales everything baked in the wall map, including sky-independent window light if any (inferred; the map
  is a lamp/room solve).
- Wall gain does not add light where the game baked none (no wall occlusion change, no new lamps).

## Pitfalls and failed approaches

- **PASSO3 per-pixel wall plan (not implemented).** Design A (bounded: lamp = B + s*V*(P - R), clamped to [B/k, k*B])
  was chosen over Design B; the critique found MUST-FIX items before any implementation:
  1. cutaway / walls-down lowers the wall in the VS but not the atlas uv: P and R at the wrong height, sliding pools;
     export the full-height lot-local position (`mul o10.xyz, v0, c18.x`, tokens `03000005 E007000A 90E40000 A0000012`);
  2. "SM2 wall families B and C" (6B4833BB, 8826CD07) are **InteriorWall**; putting outdoor lamps there would leak;
     guard "atlas alpha < 1/255";
  3. hooks would never register for a new option unless added to `UpdateHooks`;
  4. room lists are written on the light-tree thread: sanitize lamp blocks or snapshot on the gather thread;
  5. no AddRef'd texture cache (managed atlases per storey per lot);
  6. k = 2, not 4 (ghost cones up to 4x);
  7. count refused vs replaced ExteriorWall draws before any public default.
  Revisit PASSO3-PLANO.md before starting it; the decision (notes 25/09 ~15:10) was to use pattern patches tested
  against the whole precomp instead of exact-byte HLSL replicas.
- The first storey-share version cascaded refreshes by signature and touched rooms whose +0x28 set might be iterating;
  review fixes: real storey from the manager, basements out, no signature cascade, skip rooms already pending, never
  refresh the room's own storey.
- The first cross-storey test ignored the batch culling and the room+0x639 mode; both mirrored now.
- `light_vtables.txt` merged 0xFF4408 with 0xFF43A8 and missed 0xFF4468: there are **9** light classes (factory
  `FUN_006ac590`), not 7.
- F8 initially had no storey section: the hotkey uses `light_diag.cpp` (namespace `LightDiag`), not
  `patches/light_diag_patch.cpp`; the section was put in both.

## Testing in game

- House with a sconce on the upper outside wall, at night: the wall below must not show a straight cut at the floor
  line; the side wall around a corner must not be brighter below than above.
- Dev > Status > "Stories": "Active | outdoor lights carried to other stories: N | stories updated: M | walls on the
  light's story: 9/9 classes, T tests, B blocked [| on another thread: X] [| faults: F]".
- Dev > Status > "Walls": "outside walls: strength 2.00 | draws: N | variants seen: M". N grows while walls are on
  screen at night; move the slider to 1 and the walls return to the game's brightness.
- Ctrl+Shift+F8 after 10 s still: the storey section lists room 0 of each level with the same outdoor lights and
  samples near each light with the game's wall test and ours.
- F7 on a wall: PS of 1372 bytes (or another table entry); with the gain on, the draw's c3.x (or c2.x) shows the
  multiplied value.

## Open items

- Per-pixel wall lamps (roadmap phase 2, increment 1) with the MUST-FIX list above.
- One lamp model for walls, objects and roofs (phase 3): today the gain is a manual correction.
- PS_2A74E378 (wall family, baked `texld s2 x c3.x`, refused by the object patch 25/09 13:50) is not verified to be in
  `wall_lamp_table.h`.
