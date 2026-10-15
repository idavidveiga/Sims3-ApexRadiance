# Lamp drags

While a lamp, or the object holding one, is dragged in Build or Buy mode, Apex Radiance keeps the room solves a drag
causes to the lamp's own rooms and spreads the rest over the frames after the lamp stops.

## Status

| | |
|---|---|
| Availability | Released in 2.13.0 |
| Default | On; each measure has a switch on Developer > [Tester](../dev-tools/tester.md) |
| Menu | None (developer switches only) |
| Source | [`features/drag_lite.h`](../../../features/drag_lite.h), [`features/drag_lite.cpp`](../../../features/drag_lite.cpp), [`features/room_light_queue.cpp`](../../../features/room_light_queue.cpp), [`features/lot_lighting_motion.cpp`](../../../features/lot_lighting_motion.cpp), [`patches/night_terrain_relight_patch.cpp`](../../../patches/night_terrain_relight_patch.cpp) |

## The problem

Without these measures, a drag diagnosis (27.9 s, 1,071 frames) showed 26.0 ms frames against 8.46 ms before the drag: 1,173 room
solves over 13 rooms, 2,315 fan-out sends, a 15 ms solve budget in every frame and a 215 ms frame after each drag (the
game's terrain countdown, armed again by every step). The rooms lit through openings and from other stories were sent
again at every step of the lamp.

## How it works

The drag is seen from the lamp's own change stream: position steps of a lamp entry (`LampMarkFilter`), lamp marks
(`LevelLightShare::NoteLampEditing`), lamp moves seen by the lamp list, and a type-11 light removed and added again on the
same lot within 0.5 m (`LotLightBridge::TrackLotLampEdits`). A step within 750 ms is a drag (`DragLite::Engaged`);
`LevelLightShare::LampDragging` counts every step.

| Measure | What it does |
|---|---|
| Deferred extra sends | Apex's extra room sends (the fan-out to rooms behind openings and on other stories, room 0's dependents, the lifted-lamp retries, the openings' door checks, the audits) wait and are sent once each when the lamp is still, with no urgency. Sends that stop a solve (a removed lamp) and the game's own list rebuilds are never held |
| Re-pick throttle | The room priority call gives priority 0 to a room that takes the lamp's light from elsewhere (urgency tier 1 or 2) until the lamp is still, to a room whose solve began less than 400 ms ago, and to any room within 120 ms of the last solve start. The call goes through Apex also while Faster room lighting is off (`RoomLightQueue::EnsureDragThrottle`) |
| Spread cap | After the stop, while the edit's rooms still wait (at most 15 s), the frame's room solves stay under 3.5 ms: Faster room lighting's drain takes what it needs up to the cap, the lot being played the rest, the other lots 0.25 ms; no edit boost |
| Terrain countdown | At night, with the local terrain path available, the countdown a drag arms (cells+0x38) is put back to idle; the moved lamps are relit locally |
| Slower refresh | Lamp list re-reads every 250 ms (33 ms otherwise) and local terrain relights every 250 ms (100 ms) while dragged; the read and relight after the last step are the exact ones |

The log line `[DragLite] Lamp drag ended` gives the solves during the drag, the re-picks held, the sends left out and
sent, and the average and worst frame of the drag.

### Classic: relight only after release

Tester switch *Classic: relight only after release* (`classic_after_release`, on), Lamp lighting Classic (or
Instant on a device that cannot run it); Instant keeps the measures above unchanged. While a lamp is dragged no room
light work happens at all, as in the unmodified game, and every lamp-light update is done once after the release:

- Every room the drag touches (any urgency tier: the lamp's own room, its story's room 0, the neighbours, the rooms lit
  through openings, the safety net's) and every room of the lot being played gets priority 0 in the priority call
  (`DragLite::HoldUntilRelease`): none is picked, so none is solved. Its state, invalidations and gathers stay.
- The game's light-list rebuilds and invalidations are never held or replayed (holding them broke lamps before): only the
  solves wait, and the lists stay correct.
- The drag ends when the lamp has been still for 400 ms (`kReleaseMs`) or is let go. A run of steps with no such pause
  that lasts more than 20 s (`kMaxHoldMs`) no longer holds: the rooms are then solved as the lamp moves, so a lamp that
  keeps moving never leaves the lot's rooms unsolved. After the release the rooms are solved once:
  the lamp's own story first with Faster room lighting's edit budget (12-16 ms a frame, no spread cap while one of its
  rooms still waits), then the rest under the 3.5 ms spread cap. Chosen so the lamp's own light appears within a few
  frames of the release while the other stories do not spike the frame.
- The local terrain relight (`EditReady`) and the per-pixel lamp list (roofs, water, outdoor objects, trees) wait for
  the release too; the lamp list is still read every 250 ms for the drag's own detection (a type-11 light registered
  again), without rebuilding the per-pixel list.
- The sends left out go at the release; with *Classic: show all floors together* they are tagged urgent (tier 2) so they
  change in the same frame as the rest.
- One log line per drag: `[DragLite] Classic drag: N rooms deferred during the drag, M solves at the stop, settled T ms
  after the release` (or "rooms of the edit still waiting (timeout)" after 15 s).
