# Faster Room Lighting

Rooms light up much sooner when you enter a lot, change floors or switch lamps. The lot you are on and the floor you look
at go first, rooms reach their final look in fewer steps, and several small rooms are lit per frame. The final lighting
is the game's own.

## Status

| | |
|---|---|
| Availability | Released in 2.1.0 or earlier (on by default since its introduction) |
| Default | On |
| Menu | System > Performance > Camera and lighting > *Faster room lighting* |
| Configuration | `[patches.RoomLightQueue]` in `ApexRadiance.toml` |
| Source | [`features/room_light_queue.{h,cpp}`](../../../features/room_light_queue.cpp), [`features/room_ambient_policy.h`](../../../features/room_ambient_policy.h), [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) |

## The problem

The game solves room lighting one room at a time for the whole world, and picks the next room only on the next frame.
Its priority rule puts every pending first solve of every loaded lot ahead of any upgrade of the story being viewed, and
each room climbs a three-step detail ladder with one full solve per step. From entering a lot to the last room solve can
take many seconds, of which only a small part is solve work: the cause is the queue, not the solve itself.

## How Apex Radiance solves it

Five changes, each checked against the Steam bytes and left off when they differ:

1. **Viewed lot first.** Rooms of the priority lot on the camera's story get the game's priority x4000, rooms below it
   x2000. While Night Lighting's full-detail-all-floors policy is on, every floor of the priority lot gets x4000.
2. **No middle step.** A finished class-0 solve steps straight to the room's target detail class.
3. **Requeues keep the class.** A room solved before goes straight to its target class when it is invalidated.
4. **Several rooms per frame.** After the game's scheduler makes a room of the priority lot current, Apex solves it at
   once and picks the next, until 4 ms are used (1 ms while the camera moves) or a room does not finish.
5. **Stranded rooms.** A room waiting at a class above its current target (its story left the camera's view) gets the
   lowest priority instead of zero, so it is solved last rather than never.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Faster room lighting | `[patches.RoomLightQueue] enabled` | bool | on | | Turns all five changes on. A missing key reads as on |

## Compatibility and interactions

- **Night Lighting:** its requeues (`QueueRoom`) and the full-detail-all-floors policy of level light share
  ([level-light-share.md](../night-lighting/level-light-share.md)) feed this scheduler. See *Apex's own requeues* below.
- **Spread lot lighting while moving:** independent; both use the shared camera motion signal.
- **Official Sims3SettingsSetter:** its lighting quality patch hooks 0x006A0E00 / 0x006A4480 / 0x0069FD60 / 0x006A8D20 /
  0x006A8C88 / 0x006A8DE0; none of these sites is used here.
- Not part of any profile part ([README](README.md#compatibility-and-interactions)). No Developer card calls its
  `RenderDeveloperUI`, so its status line (below) is not shown in the current menu.

## Limitations

- Native priority differences still apply within the boosted floors; other lots and rooms with zero native priority are
  unchanged.
- The extra solves run only on the render thread and only for the priority lot.

## Technical reference

### The game side (Steam 1.67.2)

- **One room at a time for the whole world.** The scheduler 0x006C5C20 (fastcall(tree = lightMgr+0xD4), plain `ret`),
  reached by the tail `jmp` at 0x006C5E39 of the per-frame light tree update 0x006C5E20 (after the rooms' gathers),
  returns at once while `[tree+0x74]` (the current room) is set. Otherwise it asks 0x006A8190 for (room, priority) of
  every room of every level of every lot, sorts them, and makes the best one current (above 0.001; 0x0069E860: state 3,
  the manager's polling set). The current room is cleared only by FinalizePrime (0x006A0E00 -> 0x006C4870) or an
  invalidate. The lot pass (0x00ADB8F0 -> 0x006A8BA0 -> 0x006A88B0 -> 0x006A3F80) solves it within its lot's budget
  (5 / 10 / 15 / 30 ms), and the next room is picked only on the next frame.
- **The priority** (0x0069E770, fastcall(room), float in ST0; its only call is 0x006A81DF): 0 unless state 2. Class 0:
  10000 x {1 camera story or outdoor below, 0.8 indoor below, 0.5 above} x (0.5 on a lot not in high quality); class 1:
  1000; class 2: 100; 0 when the class is above LodChoice. So every pending first solve of every loaded lot goes before
  any upgrade of the viewed story.
- **The LOD ladder** (0x0069EA70, at the end of every solve): class 0 -> 1 -> 2, one full solve per step (wall rows 4 / 7
  / 13). An invalidate (0x0069EED0, 0x0069F160) restarts at class 0 unless the room was solved before (`+0x100 != 4`)
  and its shown class is at least LodChoice (the `jl` at 0x0069EF58 / 0x0069F1C5).
- An invalidate of the room being solved throws its work away (0x006C4870, then 0x0069E950(0): no commit).
- The game's own solve time per class is kept at 0x011D1200 / 04 / 08 (ms, added by 0x006C2380).

### The patch

Every write goes through `MemPatch::WriteCodeSuspended`, and Stop puts the bytes back.

1. **Viewed lot first:** the CALL at 0x006A81DF -> `PriorityHook`. The factor comes from
   `RoomAmbientPolicy::FloorPriorityFactor`: 4000 on the camera's story, 2000 below it, 1 above it; 4000 on every floor
   while `LevelLightShare::AllFloorsDetailed()`; 1 for other lots. Zero priorities are unchanged. The priority lot is the
   one the game gives 15 ms (0x006FDE10 SceneObjectManager, 0x006FDC80 against its +0x10D0 / +0x10E0), with the story
   manager's lot id `mgr+0x90` / `+0x94`; the camera story is `mgr+0x284`, the room's level `mgr+0x88`.
2. **No middle step:** 0x0069EAA2 `BF 01 00 00 00 8D 5F 01` -> `8B F8 BB 02 00 00 00 90` (mov edi,eax; mov ebx,2).
3. **Requeues keep the class:** the two `jl` (`7C 0B`, `7C 02`) -> `90 90`.
4. **Several rooms per frame:** the `jmp` at 0x006C5E39 -> `PickHook`. It runs the scheduler, then, only on the render
   thread (id taken at Present) and only when the pick made a new room current that belongs to the priority lot (story
   built, `mgr+0x280`), solves the room at once (0x006A3F80 with a game stopwatch of its own: 0x004F35B0 kind 4 = ms,
   0x00408700, 0x004F33C0) and picks the next, until 4 ms (1 ms while `LotLightingMotion::SampleCameraMoving()`) or a
   room that did not finish (the lot pass goes on with it next frame). The drain runs only when the room current at the
   previous pick is done and the new one is of that same lot (so that lot's pass ran: not paused by +0x18 / +0x4E).
5. **Stranded rooms:** a room in state 2 (`room+0xF0`) with a pending class above its LodChoice (`room+0xF4 > 0`) gets
   priority 1 instead of 0.

### Apex's own requeues

In [`features/level_light_share.cpp`](../../../features/level_light_share.cpp):

- `QueueRoom` never invalidates the room being solved (state 3); it is sent again when that solve is over
  (`FlushDeferred`, from the room update).
- It holds back only the requeues after a setting or ambient change (`defer`), never a requeue caused by a lamp list
  change (its list may point to a lamp being deleted), and only while the room update can send it later. A whole-world
  relight asked while the story share is off runs at once.
- Whole-world relights asked in a burst run once, 250 ms after the last ask; the settle requeue is armed once per lot
  state.
- The per-point hooks return at once for a light the game is about to drop (colour sum under room+0x63C, the game's own
  sums in the same order).
- Night Lighting does not relight every lot 3 s after a world loads at night (the game has just solved them).

### Status line

"viewed lot first on (N of M priorities raised), no middle step, requeues keep the class, several rooms per frame
(frames, extra solves, finished, ms)" and the game's own solve time per class.

### Source

`features/room_light_queue.{h,cpp}`: `Start`, `Stop`, `Running`, `PriorityHook`, `Factor`, `Stranded`, `PickHook`,
`DrainableRoom`, `StatusText`, `RenderDeveloperUI`. Group `RoomLightQueue`.

## Rejected approaches

None recorded.

## See also

- [Performance overview](README.md)
- [Validation](../../validation/performance-room-light-queue.md)
- [History](../../history/performance-room-light-queue.md)
- [Room light maps](../../engine/room-light-maps.md)

## Many lamps at once (06/10)

User report: switching all the lights of a big lot (5 stories, rooms with 35-98 lights) took 3-7 s before every room
showed the new light (the same house took up to 10 s on 05/10). Measured: the game's class-2 solve cost 24.8 s of the session
against 1.0 s for class 0.

- **Quick update when many lamps switch** (`[patches.RoomLightQueue] quickPass`, default on, Experimental; once per room and burst): while
  LampMarkFilter sees 3 or more player switches within 1.5 s (`MassSwitchActive`, held 1.5 s after the last; not at dusk or
  dawn), the priority hook puts a lamp edit's room that is still waiting (state 2) at a class above 0 back to class 0 before
  the game reads its priority. Class 0 is the game's own fast first solve and has 100x the priority of class 2, so every
  room of the switch takes its new light within a few frames, the camera's story first; "No middle step" then takes each
  room straight to its class. Status line: "quick pass for many lamps" and its room count; each lamp edit's log line ends
  with the rooms that took the quick pass.
- **No second send for waiting rooms**: the lamp switch's safety net (LevelLightShare::RelightLot, about 120 ms after the
  switch) skips a room that is queued (state 2) and has not started its solve, as it would skip a fresh solve: its gather
  reads the lamps as they are when it starts.
- **Refinement in the background**: once every room of the burst shows its quick solve (logged: "Many lamps: the N rooms
  of the burst showed their new light (quick pass) after X ms"), the extra solving a frame for the burst drops from 12 ms
  to 4 ms. Measured before (06/10): 35-39 ms of solving a frame for 2-3 s while the rooms already showed the right light;
  Apex's own light tests were 21-33% of those solves (the rest is the game's).
  Fixed the same day: the quick pass is seen as done when the room's class rose again (the end of its class-0 solve), not by
  "shown" (the step writes the new class there too, so the first version only saw it at the very end); and while refining,
  the lot lighting budget (LotLightingMotion's hook, which raised it to 25 ms for a lamp edit) is held at 6 ms.

## Smooth light changes indoors (06/10, removed the same day)

Removed after the fixes below (user: "much better without"): the light changes at once; the module became AtriumHold
(`features/atrium_hold.{h,cpp}`), which only makes an atrium's stories wait for each other. History:
`[patches.RoomLightQueue] lightFade` (default on, Experimental), `features/room_light_fade.{h,cpp}`. The user saw the quick
pass as a blink (video 11:01: the lower walls went darker for ~1 s, the class-0 solve has no wall blur, then the refinement
brightened them). The room solves write the story maps (MANAGED single-level A8R8G8B8: wall atlas, floor, ceiling, room light
map, basis maps) through LockRect / UnlockRect; both are detoured (vtable 19 / 20 of a probe texture). While a lamp edit is
pending, a map first locked during a room solve (the queue's current room in state 3, render thread: never the UI) is kept
(AddRef, two buffers): at the game's lock the exact content is put back (the game never reads a blend: the wall blur reads the
atlas); at its unlock the new content is the target and what was on screen goes back; every frame the smoothstep blend over
250 ms is written, ending on the exact content. Maps are released 3 s after their last change. Status: Developer page and the
Faster room lighting status line ("smooth light changes").

Also fixed the quick pass: a room sent back to its class by the switch's safety net before its class-0 solve ran is set
to 0 again until "shown" (+0x100) reads 0 ("class 2/0" in the recorder), and never after.

Every story together (06/10, user: the light's story changed first and the others up to 0.5 s later): while the edit's
first solves run (the quick pass of a burst, or every room of a smaller edit; not its refinement, not a dragged lamp; at most
2.5 s) a changed map holds what was on screen, then every held map starts its fade in the same frame. Maps are kept only
for the edit's own rooms (the solved room must have lamp-edit urgency: other rooms, even of other lots, had filled the 128
slots). A burst is now 3 different lights switching within 1.5 s, even when each switched more than 3 times in 10 s (the
self-switching rule had turned repeated tests of "all the lights" into no edit at all).
That hold was turned off the same day (user: worse; the light's story waited for a cascade of 2-4 s). The causes were
found and fixed instead (below); only an atrium's maps wait now.

## Every story at once: what fixed it (06/10, user: "ficou ótimo")

Recordings 11:01-12:01 of a 4-story house with an atrium (rooms 23, 19 + 3, 20 on stories 0-2, 35-98 lights each):

1. **One round per atrium** (level_light_share, "One round for an atrium"). The stacked-ambient merge used the other
   members' values from their last solve, so the first member solved after an edit took a mixed target, the next another,
   and every normalisation change (compared bit-exact) sent every member to solve again: 2-3 rounds of the atrium's
   biggest rooms. Each member's ambient is now taken at the merge with the game's own step (`0x006A0F50`, fields put back)
   at the top class's light threshold (read from the game's table), cached per gather. A waiting member is no longer sent
   back to its gather by the ambient pass or by the lamp's safety net (that also threw its quick pass away).
2. **The quick pass looks like its refinement.** Class 0 tests no wall and no object and drops more faint lights
   ([room-light-maps.md](../../engine/room-light-maps.md), "per-class switches"): with the lamps off, window lights lit
   walls through them, and the refinement then took that light away ("right, then wrong"). A quick-pass room takes the
   wall mode, pass switches and threshold of the class it will be refined to (the wall pass's through its table bytes
   around each step).
3. **A refinement's maps show at once.** They are the other class's maps, not on screen and still holding the light from
   before the edit; fading from them brought the old light back for a quarter of a second, room after room.
4. **An atrium's stories together** (AtriumHold). The atrium's members of an edit (any member urgent) are solved right
   after the lamp's room whatever their story; their new maps wait while another member is still waiting for or in its
   solve (at most 1.5 s; not while a lamp is dragged), then show in the same frame. Only atrium rooms' maps are kept. The switched lamp's own room is no longer solved twice
   when its gather fell in the change's tick (a gather serial now orders gathers and lamp marks).
5. **Loads** ("when entering the lot it takes long to correct"; log 12:18: 17 s after the world went live). The
   after-load refresh keeps every room gathered again since the world went live (it re-sent 75 rooms of 16 lots after the
   world-live round had settled them), and for 15 s after the world goes live the lot being played gets 25 ms of solving a
   frame with the camera still (the game's own budget while it moves; it averaged 4.4 ms) and the queue drains 12 ms.

Measuring tools that found these: the recorder's [solve] / [room] journal (F8), the "Wall seams.csv" of a recording (the
same wall points at class 0 and class 2), and frames extracted from the user's video with VLC's scene filter.

Then (same day, user: "do the 3 improvements"):
- **The switch's safety net sends only the rooms a lamp can reach** (`LevelLightShare::RelightLampSwitch`): those holding
  the lamp, its own room, the rooms near the stair openings of its story and the stories next to it, and every story's
  outdoor rooms for an outdoor lamp (fresh solves kept). It sent the whole lot before (17 rooms for one sconce). A lamp
  moved into another room, moved and switched, or more than 8 lamps at once ("all the lights") still send the lot.
- **Windows the game takes back are left alone**: every load, the window activation recheck changed the same 50 windows of
  the atrium house 2-3 times within half a second (the game set them back in between), and each change solved their rooms
  again. An entry found back in the state it had before Apex's last update of it (within 10 s) is now left as the game
  keeps it. The log line also shows the real lot id (it printed the tracker's +0x90, a float 1.0).
- **Slow terrain chunk renders are spread** (terrain_chunk_relight `ReleaseLimit`): after a chunk took more than 20 ms
  (44 ms in one session whose game frames took 72-164 ms; 4.7-6 ms in the others, same lamps and chunks), at most 3 chunks a
  second instead of 8.

Left as it is: the refinement of a burst ends room by room (now only a change of resolution).

The "Smooth light changes indoors" option was then removed (user: "the fade is not needed any more, is it?" and, with it
off, "much better without"): its first reason, the quick pass's blink, was gone. An old `lightFade` key is ignored.
