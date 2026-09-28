# Level light share (outdoor lamps on every story)

> **Status in the standalone:** in the v0.1.0 baseline (b84d5f1) exactly as described; `level_light_share.cpp` changed
> after v0.1.0 only in its error and status strings (Portuguese in v0.1.0, e.g. "Luz entre andares nao confere",
> "Ativo | luzes externas levadas a outros andares ...").

> Outdoor lot lamps light the walls and floors of every story of a house, not only the story they are registered on, and a
> lamp from another story is tested against the walls of its own story (and the stories in between) exactly the way the
> game tests a lamp against the walls of the story being solved, including the game's per-batch wall culling. Removes
> the straight cut of lamp light at the floor line. Status: working (confirmed by the user 25/09 ~10:00 as "almost
> perfect"; the second round with the wall test was "much better"). Both build flavours (public and dev); the F8 section
> and the "Stories" status line are dev-only.

Module: `level_light_share.cpp` / `level_light_share.h` (namespace `LevelLightShare`), driven by
`patches/night_terrain_relight_patch.cpp` (the Night Lighting patch, `NightTerrainRelight`).

Related docs: [README](README.md) (all Night Lighting settings), [walls.md](walls.md) (outside wall lamp gain, the wall
light-map atlas), [lot-light-pass.md](lot-light-pass.md), [../../engine/room-light-maps.md](../../engine/room-light-maps.md)
(room light maps, lot light solve), [../../engine/light-objects-and-rigs.md](../../engine/light-objects-and-rigs.md).

## Purpose

Walls and floors of a lot are lit by a baked light map per room and per story (the room light solve). The wall shader
(e.g. `PS_2669DA40`, LightProbe-andar2-b/andar1-b) reads it in `s2` through per-vertex UVs (`o2.xy = TEXCOORD1 * 1/4096`,
then `texld r1, v1, s2; mad r5.xyz, r1, c3.x, r2`). The outdoor room of each story is room 0, and the game builds room
0's light list separately per story. The result in the vanilla game:

- A sconce on the outside wall of the upper story lit the upper wall and nothing below the floor line (straight cut,
  LightProbe-andar2-b m44 / andar1-b m45).
- Garden lamps of the ground story lit the upper stories only until the first update of that story's room 0.
- Objects (windows, doors) are lit by rigs that compare only the room id (room 0 has id 0 on every story), so a lamp could
  light the window of one story and not the wall around it ("the street lamp lights the wall of one story and the window
  of another", friend's hint 2, 25/09 ~10:30).

The fix puts the outdoor lamps of all stories 0..7 into every story's room 0 list with the same weight the game gives
them on their own story, and adds a wall-occlusion test on the lamp's own story so upper lamps do not leak around corners
of the lower story.

## User-facing settings

Saved in `Documents\Electronic Arts\The Sims 3\S3SS\S3SS.toml`, table `[patches.NightTerrainRelight]`.

| UI label (Apex tab > Night Lighting) | TOML key | Type | Default | Range | Notes |
|---|---|---|---|---|---|
| Outdoor lights reach every story | `luzExternaEntreAndares` | bool | `true` | - | Main options (not under Advanced). Applied live: `ApplyLive` calls `LevelLightShare::Install` / `Uninstall` immediately. Reset to defaults sets it `true`. |

The option has no strength. It is independent of the other Night Lighting options except that the whole module is
installed only while the Night Lighting patch is enabled (`NightTerrainRelightPatch::Install`, "if (g_levelShare &&
!LevelLightShare::IsInstalled())").

## How it works

### The game's gather (what is patched)

Room light lists live at `room+0xC8..+0xCC` (vector of light pointers). They are built by
`FUN_006c7010 -> FUN_006c6ab0(treeLevel, room)` (thiscall, `ret 4`):

1. `FUN_006c6990(treeLevel, room, 1)` at `0x006C6B08`: the lot lights registered on that story for that room.
2. Only for room 0 (`cmp [esi+0xC], ebx; jnz` at `0x006C6B0D`) and only when the tree level is level 0
   (`cmp dword [edi+0x1A0], ebx; jnz` at `0x006C6B16`): `FUN_006c6990(level0, room, 0)` once more at `0x006C6B2D`
   (argument `lea ecx, [eax+0x6A0]` = the tracker's level 0). So level-0 outdoor lamps count twice (confirmed in
   `S3SS_LightDiag.txt` 01:39: outdoor lot lamps of the own lot appear 2x (type 3) or 3x (type 11) in room 0 of level 0).
   Verified in `re/out/dump/asm/006c6ab0.asm`. (The code comment in `level_light_share.cpp` cites `0x6C6B25`, the
   first `push` of that call sequence; same place.)
3. World lights around the lot from the light cells (`FUN_006b66b0`). With S3SS's Split-Level Lighting Fix
   (`BaseLight::GetLotID` forced to 0 at `0x6BC020`) lot lights of type 11 also come in here, on every story.

Data layout used (all from RE of these functions; see NOTAS-ILUMINACAO.md "Andares"):

| Structure | Offset | Meaning |
|---|---|---|
| lot "tracker" | `+0x6A0 + L*0x1A4` | tree level L (levels -4..7); `TreeLevel(tracker, L)` in code |
| tree level | `+0x00` | room manager of that story (null = story absent) |
| tree level | `+0x04` | back pointer to the tracker |
| tree level | `+0x28` | set "rooms pending restart" (room ids) |
| tree level | `+0x90` | hash of lights registered on the story: buckets `+0x98`, count `+0x9C`; node `+8` -> {begin, end} of entries, next `+0x10`; entry `+0x1C` = room id, `+0x24` = light |
| tree level | `+0x1A0` | the level number (`FUN_006c70c0`) |
| room | `+0x00` | its manager; the manager's `+0x88` = the room's real story |
| room | `+0x0C` | room id (0 = outside) |
| room | `+0x30/+0x34` | 2D occluders (walls) vector, used by the wall test |
| room | `+0xC8/+0xCC` | light list |
| room | `+0xF0`, `+0x168` | pending flag / countdown (a room with `+0xF0 == 1 && +0x168 != 0` is already waiting for its gather) |
| room | `+0x639` | wall mode byte read by `FUN_0069fc40` (wall height test / soft shadows of this pass) |
| room manager | `+0x288` | non-zero = active lot (high lighting quality); used only by the F8 diagnostic |
| light manager | `+0xD4` | tree of lot trackers: buckets `+0x58`, count `+0x5C`, node `+8` = tracker |

Lot load (`FUN_006c54e0`, `0x6C5525`) gathers room 0 of EVERY story through level 0's tree level; later updates
(`FUN_006c7250`) gather it through the room's own story. `FUN_006c7250` (dirty rooms per story) already refreshes room 0 of
all levels 0..7 when room 0 of level 0 changes (`0x6C73B6..0x6C7426`: `FUN_006a6550(mgr, 0)`, `FUN_0069eed0(room, 1, 0)`,
insert 0 into `+0x28`), which is why the analysis concluded EA intended upper stories to see the ground lamps and the test
in `FUN_006c6ab0` looks inverted.

### Part 1: sharing (OutdoorGather)

- Both callers of `FUN_006c6ab0` (`0x006C5816` room creation, `0x006C7094` room update) are redirected (rel32 rewrite of the
  `E8` call) to `OutdoorGather(treeLevel, room)`.
- `OutdoorGather` calls the original, then (under `__try`) `ShareOutdoorLights`:
  1. Only room 0 (`room+0xC == 0`), real story `roomLevel = [room[0] + 0x88]` in 0..7 (basements stay vanilla), and only if
     the tree level passed is either the room's own story or level 0 (the lot-load path). Sanity: `TreeLevel(tracker,
     level) == treeLevel` and the tracker's tree level of `roomLevel` holds the same manager.
  2. For every other story `other` in 0..7 that exists: call the game's own `FUN_006c6990(tl_other, room, 0)` twice when
     `other == 0`, else once (the weight each lamp has on its own story). The list therefore gets each outdoor lamp of each
     story with the same multiplicity everywhere; the ground story's list does not change.
  3. `RecordRoom`: remember per room pointer `{mgr, tracker, level, cross[]}` where `cross` = (light, home story) of every
     outdoor light registered on the OTHER stories (walk of `+0x90`, entries with room id 0), sorted by light pointer.
     `g_rooms` is cleared on world change (`OnWorldChanged`) or above 8192 rooms. The thread that ran the gather is
     remembered (`g_gatherThread`): the light tree thread.

### Part 2: refresh cascade (JNZ -> JL)

- `0x006C73AA`: `cmp [esi+0x1A0], eax` / `jnz 0x6C7432` (bytes `39 86 A0 01 00 00 0F 85 7C 00 00 00`, validated).
- The Jcc opcode byte at `0x006C73B1` changes from `0x85` (jnz) to `0x8C` (jl). Now a change of room 0 of any level 0..7
  (not only level 0) refreshes room 0 of all stories, so turning on, recolouring or moving an upper-story lamp also updates
  the stories below. Levels < 0 still skip.

### Part 3: walls of the lamp's story (cross-floor occlusion)

Why: the light map of a point sums every light of the room's list in `LightPointWithAllLights` (`0x0069FD60`, thiscall
`(room, out, list2D, list3D, flags, sample)`, `ret 0x14`) and tests each light with `FUN_0069fc40` -> `FUN_0069d4c0`
against the room's 2D occluders (`room+0x30`). Those are only the walls of the room's own story (list built by
`FUN_006a1de0` from `room+0xD8..+0xDC` (LightingWall+4) during the room rebuild `FUN_006a2740`), and `FUN_0069aa90`
blocks a ray only if it passes BELOW the top of a wall (no base test). So a sconce upstairs near a corner passed above the
lower story's walls and lit the lower story's side wall around the corner (LightProbe-andar1-c m47: side face lighter
below the floor line). 3D blob occluders (`FUN_006c6a20` + `FUN_0069e5e0`, `room+0x10`) are also per story.

Mechanics:

1. The 3 calls of `LightPointWithAllLights` (`0x006A1187`, `0x006A126F` in `FUN_006a0f50`, `0x006A3336` in
   `FUN_006a31d0`) are redirected to `SolvePointSingle` / `SolvePointBatch` (the last one). They set a context `g_ctx` =
   {room info from `g_rooms` if this room is a recorded room 0 and still the same manager, `list2D`, `flags`, batch flag,
   `room[0x639]`} and call the original. Only on the gather thread; otherwise `g_otherThread` is counted and nothing
   happens.
2. The light evaluation `vfunc+0x4C` of all 9 light classes is wrapped (vtable slot write, only if the slot holds the
   expected function and `vfunc+0x24` is `0x009691E0` = light position getter):

   | Class vtable | original `+0x4C` |
   |---|---|
   | `0x00FF42A0` | `0x006BDE90` |
   | `0x00FF42F8` (street lamp class) | `0x006BE020` |
   | `0x00FF4350` | `0x006BE1C0` |
   | `0x00FF43A8` | `0x006BEFD0` |
   | `0x00FF44C0` | `0x006BFBA0` |
   | `0x00FF4518` | `0x006BFDC0` |
   | `0x00FF4570` (spot, type 4 per LightDiag-passo3-holofote) | `0x006BFFB0` |
   | `0x00FF4408` CircleWindowLight | `0x006BF880` |
   | `0x00FF4468` TubeLight | `0x006BFA70` |

   The last two were missing from `re/out/dump/light_vtables.txt` (it merged `0xFF4408` with `0xFF43A8` and omitted
   `0xFF4468`); the factory `FUN_006ac590` creates 9 classes (review 25/09 ~10:40).
3. `LightEvalHook<I>` calls the original, then acts only if `g_ctx.info` is set and the return address is `0x0069FE19`
   (right after `call edx` in `LightPointWithAllLights`). `CrossFloorShadow`: if the light is in the room's `cross` list
   (it came from another story), the colour is non-zero and `flags[0]` is set (the game tests 2D walls in this batch),
   `WallPass` runs:
   - light position from `vfunc+0x24` (`0x009691E0`, `thiscall(light, float out[4])`);
   - for each story from `min(home, roomLevel)` to `max(home, roomLevel)` except the room's own story (the game tests that
     one right afterwards): room 0 of that story (`FUN_006a6550(mgr, 0)`), then the game's own wall test
     `FUN_0069fc40(room0, indexVec, lightPos, sample, &t)` (thiscall, `ret 0x10`) with the room's `+0x639` byte temporarily
     replaced by the solving room's value (restored after, also on an SEH fault through `g_swapAt`);
   - failed test -> factor 0; passed -> multiply by the transmission `t`. The colour (4 floats) is multiplied by the
     product.
4. **Mirroring the game's per-batch wall culling.** For a batch of samples (`FUN_006a31d0`, the 4 callers push the global
   batch vector `0x01158AC8` = {begin, end}, 0x30 bytes per sample; pushes at `0x006A3B03`, `0x006A3687`, `0x006A37CD`,
   `0x006A3956`, each validated as `68 C8 8A 15 01`), the game does not test every wall: `FUN_006a30b0` builds, per light,
   the walls whose culling edge crosses the segment from the batch centre (mean of the samples, `FUN_0069f1e0`) to the
   light, with `FUN_0069dff0(walls, int-vector* out, from, lightPos)` (call at `0x006A311F`), and each sample is tested
   only against that list (`list2D`). Level light share does the same for the other story: `BatchCentreFor` recomputes the
   mean of the batch samples when the batch changes; `CulledWalls` calls `FUN_0069dff0` on `room0+0x30` of the lamp's
   story with that centre and caches the list per (light, story) for the batch (up to 256 entries). The output vector is
   pre-sized to `walls + 1` so the game never reallocates it. When the game passes no per-light lists (`list2D` null, or
   not a batch), the test uses all walls (index vector null), as the game does.
5. The room's own lights are never touched: a porch lamp under an upper overhang is not blocked by the upper story's walls
   (same as vanilla).

### Refresh on install / uninstall

`RefreshAllLots` (render thread only; from `Install`, `Uninstall` and `OnPresent` when requested) walks the lot tracker tree
(`lightMgr+0xD4`) and, per tracker, queues room 0 of levels 0..7 for a new gather exactly like the game's own refresh in
`FUN_006c7250`: `FUN_006a6550(mgr, 0)`, `FUN_0069eed0(room0, 1, 0)`, insert key 0 into `tl+0x28` via `0x00B7AAD0`
(`thiscall(set, out, const int* key, char)`), skipping rooms already pending (`+0xF0 == 1 && +0x168 != 0`). On uninstall
the lists drop the other stories' lamps at that re-gather.

## Files and functions

| File | Function | Role |
|---|---|---|
| `level_light_share.cpp` | `Install` / `Uninstall` | byte checks, call redirects, JNZ->JL, 9 vtable slot writes, `RefreshSoon` |
| | `OutdoorGather`, `ShareOutdoorLights`, `RecordRoom`, `FloorOutdoorLights` | part 1 (sharing, cross-story list) |
| | `SolvePoint`, `SolvePointSingle`, `SolvePointBatch`, `SolveInfo`, `BatchCentreFor` | per-point solve context |
| | `LightEvalHook<I>`, `CrossFloorShadow`, `WallPass(Impl)`, `CulledWalls`, `HomeFloor` | part 3 (walls of the lamp's story) |
| | `GameWallTest` | wrapper of the game's own wall test at `0x69FE93`, records its result for F8 |
| | `QueueOutdoorRegather`, `RefreshAllLots`, `OnPresent`, `OnWorldChanged` | refresh |
| | `Diag`, `DiagText`, `Status` | F8 section and status line |
| `patches/night_terrain_relight_patch.cpp` | `Install`, `ApplyLive`, `ReinstallNow`, Present hook | install/uninstall, calls `LevelLightShare::OnPresent()` every frame |
| `light_diag.cpp` (F8) and `patches/light_diag_patch.cpp` | dump | append `LevelLightShare::DiagText()` (both: the F8 key uses `light_diag.cpp`, namespace `LightDiag`) |

## Game addresses and patterns

All TS3W.exe 1.67.2 Steam, image base `0x00400000`. Every site is validated before writing; any mismatch makes `Install`
fail with "Light between stories code differs (different game version?)" (the wall part is optional: if its checks fail,
parts 1-2 still install and the log says "Calculo por ponto nao confere; sem sombra das paredes de outros andares").

| Address | What | How found / verified at runtime |
|---|---|---|
| `0x006C6AB0` | `FUN_006c6ab0` AddWorldLights(treeLevel, room) | RE (m44/m45 analysis); call targets checked |
| `0x006C5816`, `0x006C7094` | its two callers (room creation, room update) | `E8` + rel32 == target, then rel32 rewritten to `OutdoorGather` |
| `0x006C6990` | `FUN_006c6990(treeLevel, room, char ownFloor)` `ret 8` | calls at `0x006C6B08`, `0x006C6B2D` validated |
| `0x006C6B16` | `cmp [edi+0x1A0], ebx; jnz` (level-0-only double gather) | not patched; `re/out/dump/asm/006c6ab0.asm` |
| `0x006A6550` | room by id `thiscall(manager, id)` `ret 4` | call at `0x006C73F0` validated |
| `0x0069EED0` | invalidate room `thiscall(room, char full, char keep)` `ret 8` | call at `0x006C73FF` validated |
| `0x00B7AAD0` | set insert `thiscall(set, out, const int*, char)` `ret 0xC` | call at `0x006C741B` validated |
| `0x006C73AA` | `39 86 A0 01 00 00 0F 85 7C 00 00 00` (cmp/jnz of the cascade) | `ValidateBytes` |
| `0x006C73B1` | Jcc byte `0x85` -> `0x8C` | `WriteBytes` with original `0x85` |
| `0x0069FD60` | `LightPointWithAllLights` | calls at `0x006A1187`, `0x006A126F`, `0x006A3336` validated and redirected |
| `0x0069FE19` | return address after `call edx` (light `vfunc+0x4C`) | bytes `FF D2` at `0x69FE17` checked |
| `0x0069FC40` | wall test `thiscall(room, int* idx, lightPos, sample, float* t)` `ret 0x10` | its call at `0x0069FE93` validated and redirected to `GameWallTest` |
| `0x0069DFF0` | wall culling `thiscall(walls, int-vector* out, from, lightPos)` `ret 0xC` | call at `0x006A311F` (in `FUN_006a30b0`) validated |
| `0x01158AC8` | global batch sample vector (0x30/sample) | `push 0x1158AC8` at `0x006A3B03/3687/37CD/3956` validated |
| `0x009691E0` | light position `vfunc+0x24` of all 9 classes | checked per class before wrapping |
| class vtables `+0x4C` | see table above | slot value compared with the expected function |
| `0x011D1860` | root pointer; `root+0x1C0` = light manager | used by `RefreshAllLots` |

## Interactions

- **Walls / floors** ([walls.md](walls.md), [floors.md](floors.md)): the wall and floor shaders read the room maps this
  module changes. The outside wall gain (`forcaNasParedes`) multiplies the result.
- **Objects** ([objects-and-rigs.md](objects-and-rigs.md)): rigs already gather outdoor lamps of every story (room id 0
  matches everywhere); now walls agree with windows/doors on the lamp list. Remaining differences: a rig uses the 3
  strongest lamps at the object centre without wall shadow; the map sums all lamps per point with wall occlusion.
- **Split-Level Lighting Fix** (S3SS patch, `0x6BC020`): type-11 lot lights also come through the world-cell part of the
  gather on every story. Compatible; accounted for in the design.
- **S3SS Lighting Quality** (`lighting_quality_patch.cpp`): detours the entry of `LightPointWithAllLights` (`0x69FD60`)
  and calls the original N times on jittered sample positions. No byte overlap (our redirects are at the three call
  sites, and the return address `0x69FE19` still matches inside the re-entered body), but the cross-story wall tests run
  N times (cost). PLANO-SEPARACAO.md policy: cooperate; measure the cost with Lighting Quality at 16/32 samples.
- **F8 (light_diag)**: the "ANDARES" section is produced here.

## Known limitations

- The floor slab of an upper balcony does not block light to the story below (the blockers are per story), the same as
  what Split-Level already does for type-11 lights.
- The wall test only runs on the thread that did the gather (the light tree thread). If the game ever solves points on
  another thread the status shows "on another thread: N" and those points get no cross-story occlusion.
- Basements (levels < 0) are left exactly as the game has them (no sharing, no cascade).
- Stories above 7 are ignored (the tracker holds levels -4..7).
- `g_rooms` is keyed by room pointer; a reused pointer is detected by comparing the manager (`RoomStillSame`).

## Pitfalls and failed approaches

- First version (25/09 ~09:20) shared lights but had no wall test: the side face of the ground story near a corner became
  lighter than the upper one (m46/m47). Fixed by part 3.
- The lot-load path gathers room 0 of every story through level 0 (`FUN_006c54e0`, `0x6C5525`): the real story must be read
  from the manager (`room[0] + 0x88`), not from the tree level passed in (review finding).
- A "cascade by signature" (re-queue other stories whenever the set of lights of room 0 changed) was in the first design
  and was removed in review (25/09 ~10:00): it could re-enter rooms whose `+0x28` set was being walked. Only the game's own
  cascade (JNZ -> JL) and the explicit refresh on install/uninstall remain. Never queue the story whose set is being
  iterated.
- `FUN_0069fc40` reads the wall mode `+0x639` of the room it is given, not of the room being solved; without swapping the
  byte the other story's test used the wrong mode (review 25/09 ~10:40, item 2).
- Two light classes (CircleWindowLight `0xFF4408`, TubeLight `0xFF4468`) were initially not wrapped because of the
  incomplete `light_vtables.txt`.
- Option changes of other Night Lighting settings used to reinstall this module too; `ReinstallNow` now leaves it in place
  (`reinstalling` flag) and removes it only if the rest cannot come back.

## Testing in game

- Scene: a two-story house with a wall sconce on the upper outside wall near a corner, garden lamps on the ground, at night.
  Expected: the wall below the sconce is lit continuously across the floor line; the side wall around the corner of the
  lower story is not brighter than the upper one.
- Dev build status line (Apex > Night Lighting > Developer > Status): `Stories: Active | outdoor lights carried to other
  stories: N | stories updated: N | walls on the light's story: 9/9 classes, N tests, N blocked` (plus "on another thread"
  or "faults" when non-zero).
- `S3SS_LOG.txt`: `[LevelLightShare] Installed (paredes do andar da luz: 9 de 9 classes)`.
- F8 (Ctrl+Shift+F8, dev build): `S3SS_LightDiag.txt` has the per-story room 0 lists (the same lamps must appear on every
  story, level-0 lamps twice) and the section `==== ANDARES (luz externa entre andares) ====` with, for the active lot,
  samples within 3.5 m of each light: story of the point, light, type, home story (-1 = own/world light), point, normal,
  colour sum, the game's wall test (1 passed / 0 blocked / -1 not run) and factor, our factor, batch flag, culled-list flag,
  plus a per-light/per-story summary.
- F7 (Ctrl+Shift+F7) on the two walls: the light map in `s2` of both stories should show the lamp.

## Open items

- Measure the cost together with S3SS Lighting Quality at high sample counts (standalone split).
- Balcony slabs as occluders for lamps of the story above (would need the upper story's floor as a blocker).
- Basements are untouched by design; no user report yet.
