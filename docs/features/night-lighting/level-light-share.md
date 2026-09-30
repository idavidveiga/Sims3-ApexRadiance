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
| Indoor light between floors (Experimental; "Through stair openings" until 2026-09-29) | `luzInternaEntreAndares` | bool | `true` | - | Part 4. Lighting > Stories tab (with "Upper floors light the ground" and "Outdoor light between floors" (then "Light passes between floors"), moved there from Ground & Lots on 2026-09-29), right under "Seamless walls between floors" and disabled while "Outdoor light between floors" is off. Applied live: `LevelLightShare::SetIndoor` every frame; a change sends every lot's rooms near openings to gather again. Reset sets it `true`. |
| Seamless walls between floors | `paredesSemEmendaEntreAndares` | bool | `true` | - | Part 5 (2026-09-29). Stories tab, between the two rows above, disabled while "Outdoor light between floors" is off (the part is installed with the module). Applied live: `LevelLightShare::SetWallAlign` every frame; a change sends every room of every loaded lot to light its walls again (`RequeueAllRooms`). Reset sets it `true`. |
| Every floor in full detail | `todosOsAndaresEmDetalhe` | bool | `true` | - | 30/09. Stories tab, disabled while "Outdoor light between floors" is off. `LodChoiceHook` (FUN_0069e710 at its 4 calls) gives every room of the active lot (+0x288 == 1) the top LOD class on every story, so a floor change leaves the light maps as they are instead of re-solving the floors the camera left and reached (the game gives the top class only to the camera story). More solve work once when entering a lot. A change relights every room. |

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

### Part 4: indoor lamps through stair openings

Added 2026-09-29 (user report: a red lamp next to the stairwell on story 2 lit the story-2 wall and stopped at the
moulding; LightProbe `andares2`: the story-1 wall below uses its own room's wall atlas, 256x128, with only the room's
ambient). Not tested in game yet.

Why the game never does it: an indoor room's list comes from `FUN_006c6990(treeLevel, room, 1)`, whose filter
`FUN_006c7820` requires `entry+0x1C == room+0xC`, and room ids are unique in a lot (story 1: rooms 1, 2, 3, 4, 8; story 2:
5, 6, 7, 9, 11, 12 in the test house). So a lamp only ever lights its own room. `FUN_006c6990` cannot be reused.

Data (RE agent 2026-09-29, verified in code unless marked):
- **Lighting tiles** (per story manager): `mgr+0x260` pointers, width `+0x264`, height `+0x268`, index `iz*w + ix`
  (`FUN_006a42d0`); tile `+0x78` floor height in lot space (`FUN_006a9620`, set from `0x00A880C0`), `+0x7C + q*0x14` the
  room of quadrant q (`FUN_006a9760`). The floor and ceiling batches light every quadrant of a room, holes included: the
  lighting side cannot tell a floor from an opening.
- **Floor grid** (world side): the level floor object (0x350 bytes, ctor `0x00A88790`, vtable `0x01062680`): `+0x214` its owner lot, `+0x230` world
  level, byte `+0x234` (lighting level = world level, minus 1 when it is 0), `+0x238` a COPY of the story's lighting manager
  written only when the floor is set up (`0x00A89B60..0x00A89B89`), `+0x264` FloorGrid* (null: no floor on the story). The
  game itself finds the manager every time from the lot (`[owner+0x23C]` = the lot's lighting, then `0x00ADBCC0(level)`,
  a deque of story managers; the world level code does the same at `0x00A9D0DC`): pair through that, never through the
  copy (30/09, see Pitfalls). **Two objects name each story:** the lot's floor renderer (`0x00AA1710`, callers `0x00AA35F4` /
  `0x00AA3690` byte 1, `0x00AA4171` / `0x00AA4398` byte 0) makes the story's own floor (world level L, byte 1) and a layer at
  world level L+1 with byte 0, lit by story L: its ceiling, with the outline of the floor above and none of its holes. Only
  the own floor (`LevelOwnFloor`) says where the story is open. FloorGrid (ctor `0x00A89300`): `+0` data, `+0x10` width, `+0x14`
  height, 40-byte tiles, quadrant key at `+8 + q*8` (two dwords), never built = `0xFFFFFFF8 / 0xFFFFFFFF`.
- **A removed floor is not the empty key** (measured with F8 maps, 2026-09-29): removing a floor leaves a key with bit
  `0x40000000` of its low dword (test tower: `0001E00F` with its floor, `4001E000` without; the ground under the
  house's foundation `4001BFFE`; the house stairwell `4000E000`; the air next to walls `40000000`). Floors players place
  have keys without that bit. An opening is only a removed floor, `RemovedFloorKey`: the bit AND other low bits. The
  bare `40000000` (along walls and around the edges of a story's floor, over rooms that keep their ceiling) is not one:
  counting it (second in-game test) found 200 "openings" on the house's story 2 instead of about 40, and the white lamps
  of the story below washed out the upper wall around the red lamp. The RE assumption "empty key only" found no
  opening anywhere (first in-game test).
- **Quadrants** (`FUN_006aa390`): split along the diagonals; with fx, fz the fractions, `|fx-.5| <= |fz-.5|` gives
  `fz > .5 ? 2 : 0`, else `fx > .5 ? 1 : 3`. The floor grid uses the same q (`FUN_006a61a0` passes it straight on).
- **World to lot**: rows at `mgr+0xE0/+0xF0/+0x100/+0x110` (inverse of the lot matrix, the same on every story).
  `mgr+0x98` = `mgr+0xD4` + the story's lowest tile height (`FUN_006a59a0`; 100000 without tiles).
- **Rigs are not affected**: the object rig's room gather `FUN_006bb2f0` keeps only lights with `light+8` (their room) ==
  `rig+0x1E0`, so a lamp added to another room's list never reaches the objects of that room.

What the module does:
1. **Floor objects.** Nothing found links a manager to its floor object, so the 5 calls that set or remove a floor
   quadrant (`FUN_00a89dd0` from `0x00AA0ADB/0CCC/0E4A/0F72`, `FUN_00a893a0` from `0x00AA05C7`; `ecx` =
   `[worldLevel+0x100]` = the floor object) go through `FloorSetThunk` / `FloorRemoveThunk` (naked: `NoteLevel(ecx)` then
   jump to the game's function). `LevelFor(mgr)` finds the object whose manager, found through its lot like the game does (`LevelManager`:
   owner `+0x214` -> `+0x23C` -> `0x00ADBCC0` translated as `LotStoryManager`), is `mgr` (vtable checked, all under SEH). A lot loads its floors through these calls, so objects are known from the first lot load after the mod started.
2. **Openings** (`ReadOpenings` / `BuildOpeningMask`): quadrants with a removed floor on story B (`RemovedFloorKey`) over a quadrant of an
   indoor room (id > 0) of story B-1. The room is read below: the landing around the test house's stairwell is room 0 on
   story 2 (railings close no room), and the air outside a house has no indoor room under it. The point test uses the
   same rule at the crossing.
3. **Gather** (`ShareIndoorLights`, after the game's gather in `OutdoorGather`, rooms with id > 0 on stories 0..7): for
   U = S+1 and S-1, B = max(S, U); an `OpeningMask` of B (per tile: holds an opening, lies within 8 m = `kOpeningReach`
   of one; a mask, not a list: an atrium had 1356 opening quadrants and a list capped at 256 dropped the openings near
   some lamps); the room must have a tile near an opening; the rooms of U with a tile near one; their registry entries
   whose lamp stands near an opening and within 30 m of the room's tiles (no range test: `+0x130` is the range only
   for some classes; wall lights, type 7, hold 0, 0.1, 1 or garbage there, and testing it dropped the sconces of a
   double-height room), whose class has its evaluation wrapped, that pass the checks of `FUN_006c7820` (entry info
   `+0x90 & 2`, lit `light+0x100 & 0x20`, `FUN_006bc520(light)`, type >= 3, `+0x90 & 4` only for type 11) and are not in
   the list yet: the 64 nearest are added with the game's `FUN_006a2060(room, light)` (AddRef + push_back). The room is recorded in
   `g_rooms` with `indoor = true` and, per lamp, its story, room and the floor object of B.
4. **Per point** (`IndoorShadow` from `LightEvalHook`, `IndoorPass`): the lamp head and the point in lot space; the ray's
   crossing of B's floor (the lowest floor first, then the height of the tile it lands on); the quadrant there must be an
   opening, else the lamp gives nothing (also when anything cannot be read: never light through an unknown floor). Then,
   when the game tests 2D walls in the batch (`flags[0]`), the lamp's own room walls are tested with the game's
   `FUN_0069fc40` from the lamp to the crossing point (the sample copied with its position moved there; `+0x639`
   swapped as in part 3). The receiving room's walls are tested by the game itself.
5. **Updates** (`RoomUpdateHook`): `FUN_006c5e20` pushes `FUN_006c7250` (the per-story room update, fastcall(treeLevel))
   as a function pointer (`push 0x6C7250` at `0x006C5E2A`) that `FUN_006c4b40` calls for levels -4..7 of every lot; the
   immediate is replaced by the hook (`BeforeRoomUpdate` first), and its call that empties the "changed rooms" set
   (`call 0x7F3790` at `0x6C7497`, `ecx = tl+8`, `ret 8`) goes through `ChangedClearHook`:
   - the rooms the game found changed on that story (`tl+0x8` set: buckets `+0xC`, count `+0x10`, node {id, next}) send
     the rooms of the other stories that take their lamps (`g_deps`, written by the gather) to gather again. They are read
     just before the set is emptied: it is filled inside the update itself (a dirty lamp entry's vfunc+8 calls
     `FUN_006c7160`), so a read before the update would miss most lamp changes (review 2026-09-29);
   - on level 0, every 2 s per lot (`BeforeRoomUpdate`, `LotState`): when the number of its stories whose floor object is
     known grows, or its opening count changes, the rooms near its openings gather again. A lot gathers its rooms while it
     loads, before its floors or room ids are ready (in-game test: after a restart the stairwell light was gone until a
     floor was edited), and this catches that without a floor edit;
   - on level 0, once per lot (`BeforeRoomUpdate`): when `g_indoorGen` moved (install, option switched) or the lot's floors changed (the floor
     thunks, then 1.5 s quiet in `OnPresent`), the rooms near its openings (both sides) and the rooms holding lamps of
     another story gather again (`QueueOpeningRooms`).
   Rooms are queued like the game's own refresh (`QueueRoom`: invalidate + insert into `tl+0x28`, skipped when already
   pending), never into the "changed" set, so a queued room never sends others: no loop.
6. **Safety rules from the review (2026-09-29)**: a floor that cannot be read (no grid, outside it, a fault) counts as
   solid; a room's record is written before each lamp goes into its list; clearing the room records on a world change
   keeps the indoor ones (the list `g_indoorList`, not `g_rooms`, says which rooms to send again); the point solve context
   is set and read only on the light tree thread; points within 2 cm of the floor plane are tested at their own place.
7. **No re-gather loop** (`RoomLampSignature`, 2026-09-29): the game marks a room changed for more than a
   lamp change (the light entry update `0x6C7BA0` does it for any lit lamp whose entry is updated, changed or not; the
   exact trigger in the test house is not known; not `treeLevel+0x4C`: those are object rigs, review 2026-09-29), so two
   rooms taking each other's lamps sent each other to gather again without end (atrium house: rooms 19 and 20 dozens of times in a row, every re-
   gather resetting their lighting LOD). A changed room now sends the rooms that take its lamps only when the state of
   its lamps (each lamp: lit flag `+0x100 & 0x20`, lit colour `+0xE0`, intensity `+0x10`, head `+0x120`, range `+0x130`,
   cone `+0x170..+0x1A0` for types 4 and 5) or its walls (`room+0x30`, tested by the point test of the other stories)
   differs from the last one sent.
8. **Lighting detail of rooms seen through an opening** (`LodChoiceHook`, 2026-09-29). `FUN_0069e710(room)` picks a
   room's lighting LOD class: the max class `[0x01158B00]` (2; 1 with the low lighting setting, set at `0x6A238A`) only
   for the rooms of the camera's story (`mgr+0x88 == mgr+0x284`) on the active lot (`mgr+0x288`), 0 for rooms below.
   Class 0 samples walls every 0.75 m (vertical) and ~0.95 m (horizontal), class 2 about every 0.25 m (F8 samples of the
   atrium house). Seen from above through an opening, the lower room's coarse grid showed as a bright step at the floor
   line under a sconce: its top sample, 0.47 m over the lamp, 5.65, stretched up to the line, while the finely sampled
   wall above started at 0.45 (1.22 m over it); switching the camera's story changed the look (user). Its 4 CALLs
   (`0x69E82E`, `0x69EA86`, `0x69EF46`, `0x69F1B3`) go through `LodChoiceHook`, which gives the max class to indoor rooms
   below the camera's story that take lamps through an opening (bit 1, set in the gather) or whose lamps another story
   takes (bit 2; queued once when first seen). The game then raises them itself: `FUN_0069ea70` at the end of a solve
   steps the class 0 -> 1 -> 2 while it is below `FUN_0069e710`, and `FUN_0069e770` gives such solves their priority
   (weights `[0x01158B10]` 10000 / 1000 / 100 by class; 0 when the class is above what `FUN_0069e710` asks, which is why
   the lower room never rose before). Optional: the rest works without it.
9. **One ambient for the rooms stacked through an opening** (`RoomSolveStartHook`, `MergeStackedAmbient`, 2026-09-29;
   RE agent report, VERIFIED in code unless noted). With the lamps shared and the LOD raised, an atrium wall still showed
   a thin seam at the floor line (Light Probe: c4 (0.041 0.045 0.044) above, (0.058 0.058 0.054) below). State 0 of
   the budgeted room solve (`FUN_006a18b0`, its only call `0x6A3D0B` in `FUN_006a3c90`) computes per indoor room:
   - the ambient colour `room+0x110` (the wall shader adds `lightmap.a × c4`; the parameter table binds a pointer to
     `room+0x110`, so a change shows at once) in `FUN_006a0f50`: the room's lights sampled on every 2nd floor tile and
     4 m above it, over its area (floor quadrants × 0.25 + wall sizes × 3), through the curve `FUN_006a00A0`, clamped to
     [0, 0.35]; `+0x120` (objects, clamped to 0.5) likewise;
   - the normalisation `room+0x160` (`FUN_006a0230`: 1/m if m < 1, 3/m if m > 3, m = the strongest light or sample);
   - the ambient weight ramp of the wall samples `(1 − k) + 2k (y − base)/3` (`FUN_006ab210`), base = the story's
     lowest floor (`mgr+0x98`) in the samplers `room+0x640` and `room+0x660` (`FUN_006ab110`: `[0]` base, `[4..7]` a
     colour copy).
   An atrium's upper room has almost no floor, so its ambient differs, and the ramp restarts at `(1 − k)` at the floor
   line. After the original state 0, the rooms joined by removed floors into a real atrium (`ReadStacked`, breadth
   first over the stories, 12 rooms at most; an edge counts with 16 shared quadrants and 30% of the smaller room's floor,
   so a stairwell never joins two rooms) all get the same ambient colour (each room's colour, which already carries its
   normalisation, brought to the group's, then averaged by floor quadrants; into `+0x110` and the sampler copy `+0x650`)
   and the smallest normalisation. The ramp base of the group's lowest story is used for the WALL pass only
   (`WallPassHook` on the CALL `0x6A3D4C` of `FUN_006a3a30`, `thiscall(room, int, float) ret 8`: it swaps `room+0x640`
   around each call and restores it), since the floor, ceiling and object passes share that sampler (review 2026-09-29:
   the lowest base there made the upper room's floors and ceilings much brighter). A member whose last merge differs
   (colour, normalisation or wall base; the ceiling pass bakes the colour into its texels) is sent to solve again from
   the next room update (`g_ambToQueue`, 3 s apart at most), where its own state 0 reaches the same values. Only on the
   light tree thread (the lot impostor's synchronous solve goes through the same CALL). Optional, like the LOD part.
10. **Uninstall**: the rooms holding lamps of another story (`g_indoorList`, any thread) gather again from
   `RefreshAllLots` (only lots still in the tracker hash), so no lamp keeps shining through a floor without the test.

### Part 5: wall light lined up with the wall (every wall, 2026-09-29)

The step that stayed on the atrium walls at the floor line after parts 1-4 (user: "the floor got better, the walls stay
the same"; the same slight step on outside walls, F7 captures 048/049 of 29/09 on two stories' atlases) is the game's own
wall sampling, not the lamps:

- The wall pass `FUN_006a3a30` lights each piece of each wall (room+0xD8 list) with `FUN_006ac070(wall, piece, class,
  batch)` -> `FUN_006abdd0`: sample (i, k) at `origin + (i + 0.5) * run / cols + (0, 3 * k / N, 0)`, texel
  `(x0 + i, y0 + N - 1 - k)`, with `cols = wall+class*0x10+0x28`, `N = +0x2C`, the atlas block at `wall+class*0x20+0x58` =
  `{x0, y0, x1, y1}` (`y1 = y0 + N - 1`), `run = wall+0xF0`, `origin = wall+0x110`, and `3.0` the float at `0x00FF37DC`.
  Rows cover [0, 3): the top row is 3/N under the top of the wall (N = 13 / 7 / 4 for LOD classes 2 / 1 / 0: 0.23 /
  0.43 / 0.75 m). Verified on the F8 of 29/09: the atrium's lower room had rows up to 46.44 / 46.24 / 45.92 for the three
  classes, the upper room's bottom row at the line (46.67).
- The wall meshes take their light UVs from `FUN_006ac200` (through `FUN_006a5600`, called by the straight wall builder
  `FUN_00c38530` and `CurvedWallGeometry::UpdateLightingTexCoords` `FUN_00a5e0a0`): `v = (y0 + 0.5) / H` at the top
  vertices, `(y1 + 0.5) / H` at the bottom ones, linear in between. So row k is drawn at `k * 3 / (N - 1)`.
- Every row is drawn higher than where it was lit, the top row right at the top: the wall below a floor line shows the
  light from 3/N under the line, the wall above shows the light at the line. Next to a sconce 1.2 m under the line that
  is 0.86 against 0.46 (class 2).
- `WallSamplesHook` (the CALL at `0x006A3AF5`): after the game filled the batch with a piece, each sample moves to
  `k * 3 / (N - 1)` (`y += k * (3 / (N - 1) - 3 / N)`), after checking the whole piece against the formula above (else
  the piece stays as the game has it and counts as "left as the game has them"). Both walls then end on the light at the
  line, and every row is lit where it is drawn.
- The class-2 blur `FUN_0069f650` (the CALL at `0x006A3B62`; `[0x01158B1C]` = 2 passes, mode `[0x011D02E4]` = 0: each
  pass `[1 2 1]` along every row of the block, then down every column, clamped to the block) pulls the edge rows back
  towards the inside of each wall (top row -> `(10 e0 + 5 e1 + e2) / 16`). First version (29/09 19:33): `WallBlurHook`
  kept the top and bottom rows out of the vertical blur (blurred along the row only, the game's rounding
  `(a | b) - ((a ^ b) >> 1 & 0x7F7F7F7F)`). Both walls then ended on the same light, but with a sconce just under the line
  the rows next to the edges were raised by the blur and the edges were not: a crease along the line (user: "almost
  perfect", "only a veeery slight difference"; the east sconce column: slope under the line 0.54/m against 0.11/m above,
  4.8x, where a blur that went on across the line gives 0.44/0.19, 2.4x, like any other row).
- Second version (29/09 ~20:15), the rooms of an atrium group only (`GhostRoom`: members of `g_wallBase`, both lit at class
  2): after the game lights a piece (`WallSolveHook`, the CALL of `FUN_006a31d0` at `0x006A3B0A`), copies of its top-row
  samples 1 and 2 rows (0.25 m) higher and of its bottom-row samples 1 and 2 rows lower are lit by the same function into
  a private 4 x n buffer (`g_ghosts`, per wall, reset at piece 0). `BlurWalls` then blurs every class-2 block itself, the
  game's algorithm over the block with those rows around it, and writes the block back: the wall below and the wall above
  both blur across the line as if they were one wall. Walls without those rows (room 0, other rooms, another thread) keep
  the first version's edges. Room 0 is left out on purpose: the outside walls of the stories under the camera are lit at
  class 0 (no blur), so the wall above must end on the exact light at the line.
- Where a floor really separates the two walls (the solid strip along the atrium's north wall, keys `00028019`), the step
  stays: the lamp under the strip cannot light the wall above it.
- The horizontal direction has the same kind of offset (samples at `(i + 0.5) * run / cols`, drawn from texel centre to
  texel centre); it is left alone: samples on a wall's very end sit on the next wall's line, where the 2D wall test is
  ambiguous.

### Rooms still holding a lamp switched off (2026-09-30, not tested in game yet)

The game switches a lamp off through its intensity (+0x10 = 0, so the lit colour +0xE0 = 0); the lit flag +0x100 & 0x20
stays set. A room's light list (`room+0xC8..+0xCC`, light pointers) holds what its last gather took, and the gather's
filter (`GameTakesLight`: flag 0x20 and `LightBright` = `FUN_006bc520`, the sum of +0xE0..+0xE8 >= `[0x010459E4]`) keeps
a switched-off lamp out. F8 30/09 01:26 (house C49C001BCF2DEA20, every lamp off): after the load, room 2 of story 1 still
held two of its lamps switched off for ~25 s (journal: lights 20, 9, 4, then 2 only when the user switched another lamp
on; user: "right after entering the game the background colour was wrong, after a while it fixed itself"). Why the game
did not gather it again is not known. `OnPresent` (render thread) now scans, once a second, every indoor room (id > 0) at
rest (state not 1..3) of the loaded lots (`ForEachRoomImpl` in lazy mode: its tile walk at most every 10 s, lots gone
skipped, no new walk when lots stream in or out) and sends a room whose list still holds a lamp the filter refuses now
to gather again (`QueueRoomSafe`), once per set of such lamps (`g_staleSent`, so a gather that keeps them is not repeated).
Left out: room 0 (the outside), window lights (types 7 and 8, they follow the sky) and street lamps (type 11, lot 0).
Status: "rooms still holding a lamp switched off gathered again N".

### Refresh on install / uninstall

`RefreshAllLots` (render thread only; from `Install`, `Uninstall` and `OnPresent` when requested) walks the lot tracker tree
(`lightMgr+0xD4`) and, per tracker, queues room 0 of levels 0..7 for a new gather exactly like the game's own refresh in
`FUN_006c7250`: `FUN_006a6550(mgr, 0)`, `FUN_0069eed0(room0, 1, 0)`, insert key 0 into `tl+0x28` via `0x00B7AAD0`
(`thiscall(set, out, const int* key, char)`), skipping rooms already pending (`+0xF0 == 1 && +0x168 != 0`). On uninstall
the lists drop the other stories' lamps at that re-gather.

## Cost of the per-point hooks (standalone, 2026-09-29, not tested in game yet)

The point solve runs for every texel of a room light map, so what the hooks do there counts:
- the thread checks (`SolveInfo` per point, `ShareOutdoorLights`, `RefreshSoon`, `OnPresent`) read the thread id from the
  TEB (`__readfsdword(0x24)`, what `GetCurrentThreadId` returns) instead of calling it;
- the F8 sample records (`Diag`, called from `CrossFloorShadow` for every lit cross-story evaluation: the active-lot test
  under SEH and a distance test, then a record near the lamps) exist only in the development build and are collected
  only while armed: Developer > Lighting "Record story light samples for the diagnostics", or the first F8 / "Save light
  diagnostics" of a session, which writes the section with a note and arms it (the next dump has the samples of the
  solves in between; `DiagText` still empties the records). The game's own wall test is wrapped (`GameWallTest`, only to
  record its result) in the development build only; the public build leaves its CALL as the game has it. Light shares,
  wall tests and the lighting itself are unchanged.

## Files and functions

| File | Function | Role |
|---|---|---|
| `level_light_share.cpp` | `Install` / `Uninstall` | byte checks, call redirects, JNZ->JL, 9 vtable slot writes, `RefreshSoon` |
| | `OutdoorGather`, `ShareOutdoorLights`, `RecordRoom`, `FloorOutdoorLights` | part 1 (sharing, cross-story list) |
| | `SolvePoint`, `SolvePointSingle`, `SolvePointBatch`, `SolveInfo`, `BatchCentreFor` | per-point solve context |
| | `LightEvalHook<I>`, `CrossFloorShadow`, `WallPass(Impl)`, `CulledWalls`, `HomeFloor` | part 3 (walls of the lamp's story) |
| | `GameWallTest` | wrapper of the game's own wall test at `0x69FE93`, records its result for F8 (development build only since 2026-09-29: the public build leaves that CALL untouched) |
| | `QueueOutdoorRegather`, `RefreshAllLots`, `OnPresent`, `OnWorldChanged` | refresh |
| | `Diag`, `DiagText`, `SetDiagArmed` / `DiagArmed`, `Status` | F8 section (development build, recorded only while armed since 2026-09-29) and status line |
| | `FloorSetThunk`, `FloorRemoveThunk`, `NoteLevel`, `LevelFor`, `RemovedFloorKey`, `ReadOpenings`, `BuildOpeningMask`, `ReadRoomSpan`, `RoomsNearOpenings` | part 4: floor objects and openings |
| | `ShareIndoorLights`, `GameTakesLight`, `NoteDeps`, `IndoorShadow`, `IndoorPass(Impl)` | part 4: gather and per-point test |
| | `RoomUpdateHook`, `BeforeRoomUpdate`, `ChangedClearHook`, `AfterChangedWalk`, `RoomLampSignature`, `ChangedRooms`, `QueueOpeningRooms`, `QueueRoom`, `SetIndoor`, `InstallIndoor` | part 4: updates, option, install |
| | `BoostRoom`, `Boosted`, `BoostedLod`, `LodChoiceHook` | part 4: lighting detail of the rooms seen through an opening |
| | `IndoorDiagText` | F8 "STORIES INDOORS": per story of the active lot (tiles, floor grid, floors, openings) and the indoor gathers (while armed) |
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
| `0x006C7820` | registry entry filter of the story gather (`LightFilter`) | signature; `LightBright` and `AddRoomLight` are the CALLs inside it |
| `0x006BC520` | light bright enough, `fastcall(light)` (al) | CALL at `0x006C7848` |
| `0x006A2060` | add a light to a room, `thiscall(room, light)` `ret 4` | CALL at `0x006C7874` |
| `0x006C5E2A` | `push 0x6C7250` (68 imm32) in `FUN_006c5e20` | byte `0x68` and imm32 == `RoomUpdate` checked, imm32 replaced by `RoomUpdateHook` |
| `0x006C7250` | per-story room update, `fastcall(treeLevel)`, plain `ret` | the pushed immediate |
| `0x006C7497` / `0x007F3790` | in it: `call` that empties the "changed rooms" set (`ecx = tl+8`), `thiscall(set, buckets, count)` `ret 8` | `E8` validated (within 0x400 of the update) and redirected to `ChangedClearHook` |
| `0x00A89DD0` / `0x00A893A0` | floor set / remove `thiscall(level floor object, ...)` | 4 / 1 callers (`CallersOf`), each `E8` validated and redirected to the thunks; no branch lands inside the calls |
| `0x0069E710` / `0x01158B00` | lighting LOD choice `fastcall(room)`, plain `ret`; the max class it returns (`mov eax,[0x01158B00]` at +0x4B) | 4 callers (`CallersOf`), each `E8` validated and redirected to `LodChoiceHook`; the `A1` + address at +0x4B checked |
| `0x006A3D0B` / `0x006A18B0` | the CALL of state 0 of the room solve / its function `thiscall(room)`, plain `ret` | signature, `E8` validated, redirected to `RoomSolveStartHook` |
| `0x006A3D4C` / `0x006A3A30` | the CALL of the wall texel pass / `thiscall(room, int, float) ret 8`, returns al (done) | signature, `E8` validated, redirected to `WallPassHook` (both or none) |
| `0x006A3AF5` / `0x006AC070` | in it: the CALL of the samples of one wall piece / `thiscall(wall, piece, class, batch)` `ret 0xC` | signature within the wall pass (`InRange` 0x150), `E8` validated, redirected to `WallSamplesHook` (part 5) |
| `0x006A3B62` / `0x0069F650` | in it: the CALL of the class-2 wall blur / `fastcall(room)`, plain `ret` | signature within the wall pass, `E8` validated, redirected to `WallBlurHook` (part 5; all three or none) |
| `0x006A3B0A` / `0x006A31D0` | in it: the CALL of the batch solve for each piece / `thiscall(room, batch {begin, end}, atlas {base, pitch}, char flags[2], sampler, char ambient)` `ret 0x14`, writes each sample's texel at `Y * pitch + X * 4` | signature within the wall pass, `E8` validated, the point solve call `0x006A3336` must lie within 0x300 of it; redirected to `WallSolveHook` (part 5) |
| `0x01158B1C` / `0x011D02E4` | blur passes (dword, 2) / blur mode (byte, 0) | `Deref` of the blur at +0x20 (`8B 0D`) and +0x88 (`80 3D`), both opcodes and the `cmp [room+0xF4],2` at +0x0D checked |
| `0x01062680` | level floor object vtable | `mov [esi],imm32` after the base ctor call in `0x00A88790` |
| `0x00AA179E` / `0x00A88790` | the only CALL of the level floor object ctor (after `new 0x350`) / the ctor `thiscall(object)`, returns it, plain `ret` | signature, `E8` validated, the ctor must write the level vtable at +0x0D (`C7 06` + vtable); redirected to `LevelCtorHook` (optional; every floor object is remembered at its construction) |

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
- Part 4: only stories next to each other (a lamp two stories away through two aligned openings is ignored); only lamps
  and rooms within 8 m of an opening; the floor objects are known only after a floor set / remove call since the mod
  started (a lot loaded before the option was installed gets them at its next load or floor edit); the receiving room's
  walls are tested by the game over the whole ray, so for a lamp BELOW the room a wall of the room may block the part of
  the ray that is still under the floor (the case of the lamp above, the reported one, is exact).

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
- **Pairing a story with its floor through the floor's `+0x238` (until 30/09).** That field is a copy the game writes once,
  when the floor is set up; it goes stale when the lot's lighting is built again, and a new story manager allocated at the
  old address made another floor "belong" to that story. In game (30/09, a double-height room, lot `8C41002E4010A180`): F8
  paired story 2 with a floor of another shape and other floor keys (`0000C007` / `00000006`, 1064 quadrants, no removed
  floor), so no opening was seen and the lamp below never lit the walls above; after leaving and entering the lot, the real
  floor (`0000C041`, `4000C000` removed x224, `0000C010`) was found and the light passed. Now the manager is found through the
  floor's lot like the game does (`LevelManager`); F8 counts the floors whose copy is stale.
  **That was only half of it** (same day, next session: 37 of 37 copies were fresh, and story 2 was again paired with the
  `0000C007` / `00000006` object; Refresh the lighting did not help): that object is story 2's CEILING layer (world level 3,
  byte `+0x234` = 0, the attic's outline), which names story 2 as well; the pairing took whichever of the two objects was
  noted last, so leaving and entering the lot sometimes "fixed" it. `LevelFor` now keeps only the story's own floor
  (`LevelOwnFloor`), and F8 lists every object naming each story ("its floor" / "the ceiling layer").

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
- Part 4: the house with the red lamp next to the stairwell (story 2, room 7). Expected: the story-1 wall under the
  moulding is lit red where the lamp sees it through the opening, and nothing is lit through solid floor. F8 section
  "STORIES INDOORS": a floor object per story, openings > 0 on story 2, and a gather line for story 1 room 4 listing the
  lamp with all four flags 1. Turning the option off and on must bring the light back within about a second.
- Part 5: the atrium's east wall next to the sconce, both stories in view. Expected: no step in the light at the floor
  line (the solid strip along the north wall keeps its step). Status: "seamless walls between floors: on (N wall samples
  moved to their drawn height, 0 wall pieces left as the game has them, N edge rows kept out of the blur)". F8 section
  "SEAM" (latest solves first, with each room's LOD class): the lower room's rows now reach the line (`y` = the upper
  room's base) and pair with the upper room's bottom row at the same height.
- Re-entering a lot (29/09, user: "sometimes I even have to reload the save for the indoor fix to work"): the lot state
  (`LotState`, keyed by the lot tracker's address) kept the counts of the lot as it was; a lot whose lighting was built
  again (the camera left and came back: new story managers) or a tracker address reused by another lot counted the same
  floors and openings, so its rooms were never sent again and were lit without the other story's lamps. The state now
  keeps the lot's 8 story managers and starts again when any of them changed (status "lots rebuilt N"). The level floor
  objects are also remembered at their construction (`LevelCtorHook`, status "floor objects made N"), not only when a
  floor is set or removed, so a lot built again without floor calls still has its floors known (unverified whether the
  game rebuilds them that way; this covers it either way).
- Loading (29/09, user: "moving the lamps makes it perfect; ideally the lot would load already right"): a lot that finds
  its openings (`LotState`) sends its rooms near them once more, the way a lamp moved by hand relights them (status
  "lots settled after loading N"). F8 section "SOLVES" (development build, kept from the world load, not emptied by a
  dump): every ambient step and wall pass of the rooms that take or give light through an opening or are merged, with
  the thread, the LOD class solving / shown, the state, the lamps (of other stories), boosted / merged, the
  normalisation, ambient and ramp base, and the running totals of moved samples, walls blurred across and pieces left
  alone; since 29/09 also `Q` (Apex sent the room to gather) and `H` (Apex held it until its solve ended), and per line
  the lot id, the camera story and the room flag +0x19 (the flag whose change makes the game invalidate a room,
  0x006A5E00 -> 0x0069F160). One dump right after entering the lot and one after moving a lamp show what differs.
- Atrium house, slow to look right (29/09 F8, user: "the lights still take very long to be 100% between the stories"):
  the lot was entered at night with its lamps still off; they switched on one after another over about 1.5 s. Each
  switch changed a lamp's values, so every room taking those lamps was sent again at once and the solve it was in was
  thrown away (room 10 of story 2 solved 10 times in 2 s; room 20 of story 2, the atrium's upper room, restarted twice
  before its first full solve). The first round was over 2.25 s after the lamps came on, but the fixed 6 s settle only
  came 2.5 s later. Now:
  - `RoomLampSignature` gives two hashes: the shape (which lamps, their position, range and cone, the room's walls) and
    all values. A new shape (a lamp added, deleted, moved or turned, a wall changed) sends the taking rooms at once,
    stopping their solve (their lists may hold the deleted lamp; a lamp dragged in build mode must be followed frame by
    frame: the first build had the position among the values and the light lagged behind a dragged lamp), as before. A
    value change (switched, dimmed, recoloured) sends them at once only if they were
    not sent for a value change in the last 1 s, and without stopping a solve in progress (QueueRoom's defer); later
    changes wait until the lamps are quiet for 300 ms, at most 1.5 s after the first wait (`g_depWait`,
    `FlushDepWaits` from the room update; status "lamp changes folded into one update N").
  - The settle fires as soon as the first round is over: none of the rooms the lot sent (`LotState::watch`) gathers,
    waits or is being solved (states 1-3), none is held back and no lamp burst waits, for 400 ms (not before 500 ms
    after arming); at the latest after 6 s as before (status "lots settled after loading N (M as soon as their rooms
    were done)").
  - World load (user: "only when I opened the game some lights failed"; Night Lighting off and on fixed it): a world
    loaded at night gets no lot relight, its lots are solved during the load screen and the early settle could fire
    before a lot was complete. `OnWorldLive` (called when NightTerrainRelight sees the world drawn) bumps
    `g_indoorGen`: every lot's rooms near openings gather once more, then settle.
  - Invalidate callers (29/09 F8 with the new `I` / `F` notes): 465 of 589 invalidations came from 0x006C7404, the
    part 2 cascade in FUN_006c7250 (room 0 of any floor changed -> room 0 of every floor restarted); room 0 of stories
    1-3 of lot 7D6F001A00D62480 was restarted ~140 times each in 70 s and never shown above class 0 (user: "outside lights
    take very long on both floors"; "on load the light is weak; after opening the map it is right"). The cascade is gone
    when the changed-set hook is in (InstallIndoor writes "90 E9" over the "0F 8C" at 0x006C73B0, so every room 0 takes
    the plain path, its own floor only); AfterChangedWalk sends room 0 of the other floors 0..7 with the same rules as
    the indoor rooms (status: "outside of a floor marked changed without a change N").
  - Same F8: 1028 "sent at once" with the camera still. Position and range are no longer hashed as shape: `LampChange`
    keeps each real lamp's position and range at the last send (`LampAt`) and counts a move only beyond 10 cm or a 2%
    range change (animated lamps wobble; a slow drag adds up against the last send). Status: "lamp values changed
    without a move N".
  - Not done yet: an outdoor room (room 0 of story 3, shown class 0) was sent to solve about every 50 ms for 3 s at
    the end of the same dump, never finishing (its shown class stayed 0). Nothing of Apex sends room 0 there; the new
    journal fields (Q/H, lot, camera story, flag +0x19) should tell whether it is the game (and which invalidate).

## Open items

- Measure the cost together with S3SS Lighting Quality at high sample counts (standalone split).
- Balcony slabs as occluders for lamps of the story above (would need the upper story's floor as a blocker).
- Basements are untouched by design; no user report yet.

## Floor switches: rooms keep their light when their lamps did not change (30/09, `features/lamp_mark_filter.cpp`)

Installed 81934361 (dev build), not tested in game yet; the approved floor build is backup 108, the previous asi backup 127.
User: "the indoor light breaks for a moment at every floor switch"; "keep the light cached and change it only when
something changes". F6 092629: every switch restarted room 0 of every story and every room holding a lamp (0x006C7451 in
FUN_006c7250, twice within 16 ms), solved again in 110-140 ms with the same ambient, light counts and normalisation. Two
studies (30/09): Apex added one room of its own (room 14 of story 0, `AfterChangedWalk` deps); the rest is the game. The
lamp entry update FUN_006c7ba0 (entry vtable 0x00FF5984 slot +8; for a lamp light vfunc+0x18 = 0x00620D60 returns false)
finds the lamp's room (FUN_006c7b20), rewrites its lit bit and lit colour (FUN_006bdca0) and marks the room changed
(FUN_006c7160, thiscall(tl, room) ret 4: 0x006C7CCA for the room left, 0x006C7CD6 always) without comparing anything.
Entries are flagged by FUN_006c4cf0 from the light manager's messages (transform 0x3361F6C9 at 0x6B0A8D and colour
0x966EC80A at 0x6B0BFA always; intensity 0x6B0B33, enable 0x6B0C9A, alpha 0x6B0D26 only on a change). Nothing of the room
solve reads the shown story or a light's visibility (a lamp hidden on an upper floor keeps lighting in the unmodded game).

The call at 0x006C7CD6 goes through `MarkThunk` (esi = entry, edi = light, ecx = tl): the lamp's values (its room, lit bit,
object flags entry+0x20 record +0x90 & 6, +0x10 x4, type +0xB0, +0xC0..+0xDC, +0xE0..+0xF0, position +0x120 x3, range
+0x130, cone +0x170 x13 for types 4 and 5) are hashed and compared with its last mark per (tree level, light): the same
values drop the mark (the rooms keep their solve), anything else marks as before. First sight, window lights (types 7 and 8),
unreadable lamps and the filter switched off always mark. Not filtered: 0x006C7CCA, occluder entries (0x006C7939: objects
that fade or hide block light differently), room creation and object removal. Steam 1.67.2 only (fixed addresses, bytes
checked). Developer page > Lighting: a checkbox (on by default, not saved) and the status; the F6 recorder writes
"Rooms keep their light: ..." with the marks kept / let through and, in the dev build, how many light entries each of the
five messages flagged (the floor switch's trigger). If a lamp change is ever missed: "Refresh the lighting".

**A lamp switched or moved lights its lot again (30/09; built 5eba1685, not installed yet).** User: "also refresh the lighting
whenever a lamp is moved, switched off or on, if it costs no performance". `MarkDecide` (lamp_mark_filter.cpp) keeps, per
(tree level, light), whether the lamp is on (lit bit and lit colour sum > 1e-3), where it is and its room; a change of one
of them (a move of more than 10 cm) makes that lot due 0.7 s later (a drag: once, when it stops). `LampMarkFilter::OnPresent`
(Night Lighting's Present) runs `LevelLightShare::RelightLot` (every room of that lot, stories -4..7, room 0 too) and the rig
refresh (now and 1.5 s later); at most once per 2 s per lot; nothing while the night level is between 0.02 and 0.98 (dusk
and dawn switch every lamp at once) or without a world. Left out: first sight, window lights, and a light switching itself
on and off more than 3 times in 10 s (a flickering TV or effect). Counted in the F6 line "Rooms keep their light".

**Pitfall: the 4 basis maps do not see the floor test (30/09, F7 128, F8 10:45).** A TV in room 5 of story 2 (closed from
below: the only opening of that floor is the stairwell in room 7) took the green lamp of story 1. The lamp is in the light
list of rooms 5, 6 and 7 of story 2 (rooms near an opening take lamps of the story below); the room light map of story 2 is
solved point by point with `IndoorShadow` (the F8 samples: 21 points of the lamp on story 2, all blocked; the map black),
but the 4 directional basis maps (64x64, 1 texel per metre) are filled by another routine of the game that never goes through
the hooked `LightPointWithAllLights` calls (no basis sample in the F8; not identified yet): they held the green light. The
game's own furniture shader does not read them; Apex's indoor-object shader (path A, `PatchIndoorBasis`) does. Fix in that
shader: after the 4 directions are summed, `min(basis, 2 x room light map)` per channel (texld of the room light map at its
own uv, `kBasisCap`, def cS+11): where both are right they agree (captures 096-103: light map 0.239, basis 0.157), behind a
floor or a wall the basis light goes with the light map. Checked offline (4 captured indoor-object shaders). Open: the game's
other basis-reading shaders (stairs, instanced objects) still read the maps as they are; the source fix is to find that
routine and give it the floor test.

**Rooms lit only by lamps of another story: no boost (30/09, F6 105204; built 96f17fbe, not installed yet).** User: "a room
did not take the brightness of rooms for a while", mostly right after loading. Rooms 5 and 7 of story 2 held only the green
lamp of story 1 (rooms near an opening take the lamps of the story below) and the floor test blocked every point of it:
ambient (0, 0, 0), normalisation +0x160 = inf, the Brightness slider did nothing to them (room 6, almost all blocked: x98).
FUN_006a0230 (thiscall(room, float brightest[4]); its only call 0x006A13B4 in FUN_006a0f50) sets +0x160 = limit / max(the
lamps' vfunc+0x30, the brightest sample x k) when that is under [0x01158B24] or over [0x01158B20]; all lamps blocked gives
limit / 0 = inf, the ambient (average x +0x160) 0 x inf = NaN, and the clamp (maxps with 0) makes it 0. `RoomNormHook`
(level_light_share.cpp, installed with the indoor part, Steam 1.67.2 only): a value that is not finite becomes 1, and a room
whose lamps are all of another story (`CrossOnly`: every light of its list has a `FindCross` record) gets no boost (+0x160 at
most 1): the light through an opening is not spread over the whole room, and with none coming the room takes the unlit colour
(the top-up) as an empty room does. Status: "rooms lit only by lamps of another story given no boost N (normalisation not
finite M)".

**Refresh after a load (30/09, user: "right after loading, apply that refresh").** `RequestRefreshAfterLoad` (Night Lighting,
when the world is live): 8 s later the lots, every room and the rigs light again once (`RefreshAll("after loading", terrain
false)`; the terrain keeps the load's own rebuild); a setting change pending at that moment takes over (with the terrain).

**The floor test in the 4 basis maps, at the source (30/09, third agent study; built c05df4fe, not installed yet).** The
story's LightBasisMap0..3 (bound from mgr+0x220 by FUN_006a7700; locked at room+0x584 + i*0x28 in FUN_0069fa40, gated by
room+0x629) are filled by FUN_006a09f0 (called at 0x006A3C0A from FUN_006a3b80, state 7 of the room solve FUN_006a3c90; only
indoor rooms of lots with mgr+0x288, room+0x62A set at 0x006A1BB0): one sample per tile centre (lot point x+.5, floor height,
z+.5, world through room+0xF8), and for each light of the solving room's own list FUN_0069f280 (stdcall(pos, light, float
acc[4][4]) ret 0xC, its only call 0x006A0C56 after `mov ecx, edi` = the room; pos = the sample 0.5 m up) adds the light's rig
colour (vfunc+0x10, FUN_006bdb00) weighted towards the 4 directions (+-0.894, 0.447, 0) / (0, 0.447, +-0.894); no threshold,
wall or floor test. `BasisLightHook` (redirect of that call, bytes 8D 94 24 C8 00 00 00 52 8B CF checked, Steam 1.67.2 only):
a lamp of another story (FindCross in the room's RoomInfo) is tested with IndoorShadow at pos (no 2D wall flags: pass or
not); blocked, it adds nothing. Status "directional maps: lamps of another story tested N, behind a floor M". The shader cap
in PatchIndoorBasis stays as a safety net. The maps change only when a room is solved again (a refresh, a lamp change).
