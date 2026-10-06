# Faster Room Lighting

Rooms light up much sooner when you enter a lot, change floors or switch lamps. The lot you are on and the floor you look
at go first, rooms reach their final look in fewer steps, and several small rooms are lit per frame. When you switch one
lamp or all the lights of a house, the rooms, the furniture, the ground and the trees change together, in a single
frame, once the new light is ready. The final lighting is the game's own.

## Status

| | |
|---|---|
| Availability | Released in 2.1.0 or earlier (on by default since its introduction). Lamp switches all at once and Quick update for lamp switches (both Experimental), an atrium's stories changing together and the quick return of empty light object removals: Released in 2.7.0 |
| Default | On |
| Menu | System > Performance > Camera and lighting > *Faster room lighting* |
| Configuration | `[patches.RoomLightQueue]` in `ApexRadiance.toml` |
| Source | [`features/room_light_queue.{h,cpp}`](../../../features/room_light_queue.cpp), [`features/atrium_hold.{h,cpp}`](../../../features/atrium_hold.cpp), [`features/room_ambient_policy.h`](../../../features/room_ambient_policy.h), [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) |

## The problem

The game solves room lighting one room at a time for the whole world, and picks the next room only on the next frame.
Its priority rule puts every pending first solve of every loaded lot ahead of any upgrade of the story being viewed, and
each room climbs a three-step detail ladder with one full solve per step. From entering a lot to the last room solve can
take many seconds, of which only a small part is solve work: the cause is the queue, not the solve itself.

## How Apex Radiance solves it

Six changes to the queue, each checked against the Steam bytes and left off when they differ:

1. **Viewed lot first.** Rooms of the priority lot on the camera's story get the game's priority x4000, rooms below it
   x2000. While Night Lighting's full-detail-all-floors policy is on, every floor of the priority lot gets x4000.
2. **No middle step.** A finished class-0 solve steps straight to the room's target detail class.
3. **Requeues keep the class.** A room solved before goes straight to its target class when it is invalidated.
4. **Several rooms per frame.** After the game's scheduler makes a room of the priority lot current, Apex solves it at
   once and picks the next, until 4 ms are used (1 ms while the camera moves) or a room does not finish.
5. **Stranded rooms.** A room waiting at a class above its current target (its story left the camera's view) gets the
   lowest priority instead of zero, so it is solved last rather than never.
6. **Empty light maps.** An object removal from a level's five light maps returns at once when the five maps are
   empty (the game walks them for twelve levels per removal).

Two further parts handle lamp switches, *Lamp switches all at once* and *Quick update for lamp switches* (below), and
the stories of an atrium (a double-height room is one room per story) always change together.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Faster room lighting | `[patches.RoomLightQueue] enabled` | bool | on | | Turns all six changes and the lamp switch parts on. A missing key reads as on |
| Lamp switches all at once (Experimental) | `[patches.RoomLightQueue] switchAllAtOnce` | bool | on | | A player's switch is shown in one frame once the rooms on screen have their final light (see "Lamp switches all at once" below); the quick pass is not used meanwhile |
| Quick update for lamp switches (Experimental; shown only while the row above is off) | `[patches.RoomLightQueue] quickPass` | bool | on | | An approximate light first, refined room by room |

### Lamp switches all at once

An approximate light first can never match the final one: class 0 samples 1 point per tile and class 2 four
(`0xFF36AC`), so rooms shown with a quick light correct themselves afterwards, by up to 17 levels, over about 2 s. This
part shows one change instead, once the final light is ready. With `switchAllAtOnce` on (`AtriumHold`,
`features/atrium_hold.{h,cpp}`):

- A player's switch (`LampMarkFilter::SwitchLastTick`) starts a hold (`AtriumHold::SwitchHolding`, also true in the very
  frame of the switch, before the next Present). The quick pass is off (`QuickPassRoom`); the drain gives the switch's rooms
  16 ms a frame.
- Every map the switch's rooms (lamp-edit urgency) write waits at the game's UnlockRect, as an atrium's maps do. A map is
  taken only when the game locks it in step 1 of the room's solve (`0x0069FA40`, its call `0x006A3D26` hooked by
  `LevelLightShare`, `InMapLockStep`), never another texture the render thread locks while a room is mid-solve; at most
  192 maps and 96 MB of copies, and a map with no memory for its copies is left to the game. The
  furniture of those rooms keeps its rig lights (`lot_light_bridge.cpp`, `HoldRig`); the ground keeps its smoothed chunk
  maps (`LightmapSmooth`); the per-pixel lamps (roofs, water, outdoor objects, trees) keep their list (`LotLightBridge`).
- It ends when every room the switch marked on the camera's story (and the atrium's rooms below it) has ended a solve
  begun after the latest switch (`LevelLightShare::SwitchRoomsPending`, `RoomLightQueue::SolvedSince`, from the solve
  start and end hooks; a solve start is stamped with `LampMarkFilter::SwitchSerial`, so a solve begun in the switch's
  own tick step counts as after it), and the ground's chunks are re-rendered, at least 150 ms after the latest switch;
  at most 2.5 s after it (4 s after the first). Then the maps take their content and the others follow in the same frame.
- The lamp object itself (its lit model) is the game's and changes at once. Recordings note `[switch] lamp switch shown
  all at once, N ms ...`; the status line counts the switches and the last one's time.
- It needs both solve hooks (the end `0x006A3E65`, Steam 1.67.2 only, and the lock step): without them the switches
  change room by room as with the option off (`AtriumHold::AllAtOnce`). Turning Faster Room Lighting off ends any hold and
  stops new ones (`AtriumHold::Clear`).

### Quick update for lamp switches

Used only while *Lamp switches all at once* is off (`quickPass`, once per room and switch). After a player's switch
(`LampMarkFilter::SwitchActive`: a lamp switched on or off where it is, not a light switching itself, not at dusk or
dawn; switches less than 1.5 s apart are one event, `SwitchEventId`), the priority hook puts a lamp edit's room that is
still waiting (state 2) at a class above 0 back to class 0 before the game reads its priority. Class 0 is the game's own
fast first solve and has 100 times the priority of class 2, so every room of the switch takes its new light within a
few frames, the camera's story first; *No middle step* then takes each room straight to its class. Drags and value
edits never take it.

- A quick-pass room takes the wall mode, pass switches and light threshold of the class it will be refined to, so the
  quick light looks like its refinement ([room-light-maps.md](../../engine/room-light-maps.md), per-class switches).
- A room counts as shown at the first solve end after the quick pass set it to 0 (`RoomLightQueue::NoteSolveEnd`, from
  `FinalizeHook`, the solve's step 8, `0x006A0E00` called at `0x006A3E65`; without that hook, when `+0x100` reads 0). A
  room is set to 0 at most 3 times per burst; after 3 s the burst counts as refining.
- Once every room of the burst shows its quick light, the extra solving for the burst drops from 12 ms to 4 ms a frame
  and the lot lighting budget is held at 6 ms ([lot-lighting-motion.md](lot-lighting-motion.md)), so the frame rate
  holds while the rooms are refined.
- Status line: "quick pass for lamp switches" and its room count; each lamp edit's log line ends with the rooms that
  took the quick pass.

### The lamp switch's safety net

About 120 ms after a switch (`LevelLightShare::RelightLampSwitch`, with either option), the rooms the lamp can reach are
sent again: those holding it, its own room (`LampHome`: room id `light+8` on the story whose lowest floor is the highest
at or under the lamp's height), the rooms near the stair openings of its story and the stories next to it, and every
story's outdoor rooms for an outdoor lamp. A lamp moved into another room, moved and switched, or more than 8 lamps at
once still send the whole lot. A room that is queued and has not started its solve is skipped (its gather reads the
lamps as they are when it starts), and when only lamps switched off, a room with an empty light list is left alone.

### An atrium's stories together

A double-height room is one room per story (level light share's stacked-ambient groups), each solved on its own. The
atrium's members of an edit are solved right after the lamp's room whatever their story, and the room maps a member's
solve writes wait at the game's `UnlockRect` while another member is still waiting for or in its solve (at most 1.5 s;
not while a lamp is dragged), then show in the same frame. At the game's `LockRect` of a waiting map the exact content
is put back first, so the game never builds on what is shown. Each member's ambient is taken at the merge with the
game's own step, so an atrium is solved in one round ([level-light-share.md](../night-lighting/level-light-share.md)).

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
6. **Empty light maps:** 0x006C7610 (thiscall(levelLights; idLo, idHi), `ret 8`) takes an object out of a level's five
   light maps (`+0x4C`, `+0x90`, `+0xD4`, `+0x118`, `+0x15C`) with five finds; 0x006C7690 runs it for the twelve levels
   -4..7 of a lot on every object removal, and its callers ignore the result. `EntryChain` site `LightObjectRemove`, layer
   `RoomLightQueue`: when the five element counts (`+0x5C`, `+0xA0`, `+0xE4`, `+0x128`, `+0x16C`) are 0 the hook
   returns at once.

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
`DrainableRoom`, `EmptyRemovalHook`, `SetQuickPass`, `NoteSolveEnd`, `StatusText`, `RenderDeveloperUI`. Group
`RoomLightQueue`. `features/atrium_hold.{h,cpp}`: `SetAllAtOnce`, `AllAtOnce`, `SwitchHolding`, `OnPresent`, `Clear`,
`Status`.

## Rejected approaches

- Smooth light changes indoors (the room maps fading over 250 ms during lamp edits): removed before release; the light
  changes at once. An old `lightFade` key is ignored.
- Holding every story's maps until an edit's first solves end: the lamp's story waited for a cascade of 2 to 4 s;
  replaced by fixing the causes (one round per atrium, the quick pass looking like its refinement) and keeping the hold
  for atriums only.

Details in [history](../../history/performance-room-light-queue.md).

## See also

- [Performance overview](README.md)
- [Validation](../../validation/performance-room-light-queue.md)
- [History](../../history/performance-room-light-queue.md)
- [Room light maps](../../engine/room-light-maps.md)
