# Lot Streaming

Lot Streaming lets nearby lots keep their full detail farther from the camera and lets more of them be detailed at once,
and makes them stream in smoothly while the camera moves: fewer lots switch detail back and forth, the viewing angle
alone no longer loads or unloads lots, and lot streaming pauses while the map view is open. A separate switch builds a
lot's objects a few at a time. Research, measurements and code by **idavidveiga**.

## Status

| | |
|---|---|
| Availability | Experimental: Released in 2.7.0 |
| Default | Off (every switch) |
| Menu | System > Lot Streaming (cards *Lot detail streaming* and *Object streaming*) |
| Configuration | `[patches.LotDetailRange]`, `[patches.LotLodStreaming]`, `[patches.LotVisibilityOverride]`, `[patches.MapViewStreamingBlocker]`, `[patches.LotObjectThrottle]` in `ApexRadiance.toml` |
| Source | [`features/lot_detail_range.{h,cpp}`](../../../features/lot_detail_range.cpp), [`features/lot_lod_streaming.{h,cpp}`](../../../features/lot_lod_streaming.cpp), [`features/lot_visibility_override.{h,cpp}`](../../../features/lot_visibility_override.cpp), [`features/lot_object_throttle.{h,cpp}`](../../../features/lot_object_throttle.cpp), [`features/lot_active_threshold.{h,cpp}`](../../../features/lot_active_threshold.cpp), [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) |

## The problem

The game decides lot detail in several separate steps, each with its own native value: whether a lot is close enough
for Detailed View (Lot LOD distance, 70 by default), how many lots may stay detailed at once (Max Active Lots, 8), how
many detail transitions may start together (a native throttle, off, with a camera-speed threshold of 32), whether the
camera's viewing angle biases the distance test, and how a promoted lot's objects are built (all in one burst). At the
game's values few lots are detailed, and while the camera moves lots switch detail back and forth. See
[engine/lot-loading-and-streaming.md](../../engine/lot-loading-and-streaming.md).

## How Apex Radiance solves it

Each step is a separate switch that sets the game's own value or flag; no lot loader is replaced.

1. **Extended lot detail** sets the Lot LOD distance (`WorldManager+0xDC`) and Max Active Lots (`+0xE4`) of each live
   WorldManager to the chosen values (300 and 16 by default; the game's metric is a squared distance).
2. **Smooth lot streaming** turns on the game's native "Throttle Lot LoD Transitions" flag and sets its camera-speed
   threshold (`+0xEC`) to 5.0.
3. **Keep lot visibility stable** turns off the camera-view bias of the lot visibility metric: its short `JZ` becomes a
   `JMP`.
4. **Pause lot streaming in map view** holds the live "skip lot streaming" gate (`+0x258`) while the map view is open
   and for 1 s after it closes, without detouring WorldManager::Update.
5. **Spread lot objects while loading** replaces `Lot::AddLotObjectsToScene` so regular objects are built a few per
   window, the next window posted through the game's own remote-method call. Building and apartment shells, large
   exterior geometry and flora are built in the first window, because the lot's activation fix-ups need them at once.

Every write is guarded: Apex reads the expected native value first, keeps a value only while it still owns it, yields to
a value another mod set, and restores only what it changed.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Extended lot detail (Experimental) | `[patches.LotDetailRange] enabled` | bool | off | | More lots in full detail, farther away, than the game's 70 / 8 |
| Lot detail distance (shown while on) | `[patches.LotDetailRange] distance` | int | 300 | 70 to 300, steps of 10 | How far lots stay in full detail; 70 is the game's. Applied live |
| Maximum detailed lots (shown while on) | `[patches.LotDetailRange] maxActiveLots` | int | 16 | 8 to 16 | How many lots can be detailed at once; 8 is the game's. More lots use more memory. Applied live |
| Smooth lot streaming (Experimental) | `[patches.LotLodStreaming] enabled` | bool | off | | The native transition throttle and a camera threshold of 5.0 |
| Keep lot visibility stable (Experimental) | `[patches.LotVisibilityOverride] enabled` | bool | off | | The viewing angle alone no longer makes lots load or unload |
| Pause lot streaming in map view (Experimental) | `[patches.MapViewStreamingBlocker] enabled` | bool | off | | No lot detail streaming while the map view is open |
| Spread lot objects while loading (Experimental) | `[patches.LotObjectThrottle] enabled` | bool | off | | Builds a lot's objects a few at a time |
| Objects per lot window (shown while on) | `[patches.LotObjectThrottle] objectsPerLot` | int | 2 | 1 to 64 | Regular objects built per window; 2 matches Sims3SettingsSetter |
| Delay between lot windows (shown while on) | `[patches.LotObjectThrottle] delayMs` | int | 16 ms | 0 to 500 ms | Minimum wait between two windows of the same lot |
| (no menu row) | `[patches.LotActiveThreshold] enabled` | bool | off | | EA app build only: sets the internal transition threshold "Throttle Lot LoD Transitions Max Active Lot Threshold" to 12, as Sims3SettingsSetter does. Not Max Active Lots |

A missing key reads as off. None of these switches is part of a profile part, and the built-in profiles leave them as
the player set them.

## Compatibility and interactions

- **Official Sims3SettingsSetter:** when its LotStreamingOptimizations has the matching setting on
  (`streamingSettings`, `mapViewBlocker`, `visibilityOverride`, `objectThrottle`), that part makes no write and its row
  shows "Handled by Sims3SettingsSetter". A visibility branch already patched by another mod is left alone ("Already
  applied by another patch").
- **Spread new objects over frames** ([scene-node-budget.md](scene-node-budget.md)) works later in the pipeline (the
  scene's pending-node drain); the two are independent and can be on together.
- **Faster room lighting and lamp edits:** lamps are lot objects. With *Spread lot objects while loading* on they arrive
  one by one and every arrival relights the rooms, so the lighting of a lot settles later.
- **Memory:** more detailed lots use more of the 32-bit game's address space; see *Room to save*
  ([room-to-save.md](room-to-save.md)).

## Limitations

- Distance 300 and 16 lots are the tested maximums; the menu does not go higher.
- The Steam addresses of Extended lot detail, the visibility branch and the object throttle were found offline with the
  fork's EA signatures; the research and the controlled tests were made on the EA app build 1.69.
- *Spread lot objects while loading* makes objects, lamps included, appear over a few frames when a lot is promoted.

## Technical reference

| Part | Game side (Steam 1.67.2) | What Apex writes |
|---|---|---|
| Extended lot detail | WorldManager `[0x011ECBC4]`: `+0xDC` Lot LOD distance (70), `+0xE4` Max Active Lots (8); `+0xE0` Active Lot Bias (8.0) and `+0xE8` terrain-height threshold (600) are left alone | The two values per live WorldManager, captured first and maintained by `Tick` while still owned |
| Smooth lot streaming | Throttle test 0x00C6C695 (inside LotLodScoring 0x00C6C290) on the flag byte 0x011ECBC0; `+0xEC` camera-speed threshold (32) | Flag on; `+0xEC` = 5.0 after checking a finite value in 0 to 100 |
| Keep lot visibility stable | `JZ` (0x74) at 0x00C63015 in the lot visibility metric's camera bias | 0x74 -> 0xEB, only when it reads 0x74 |
| Pause in map view | `+0x258` "skip lot streaming"; map view from `Camera_IsMapViewModeEnabled` | The gate while the map is open and 1000 ms after |
| Spread lot objects | LotAddObjectsToScene 0x00AC1130, LotUpdateObjectSceneNode 0x00ABFAC0, ScriptMessageScope 0x007D2DB0 / 0x007D2DF0, PostRemoteMethodCall 0x00ABE9C0, IsObjectLargeOrFlora 0x00B088C0 | `EntryChain` layer `LotObjectThrottle` on AddLotObjectsToScene |

Address groups `LotLodStreaming` (scoring, throttle test and flag, WorldManager), `LotVisibilityOverride` and
`LotObjectThrottle`; on other builds the same parts resolve by signature and stay off when a signature is missing. The
EA app build 1.69.47 addresses of the throttle test and flag (0x00C6BA15 / 0x01246C50) were verified in game by the
fork. The fork's diagnostic metric probe is not part of Apex Radiance.

## Rejected approaches

- Distances above 300 or more than 16 detailed lots: not exposed until tested for stability and memory.
- The threshold-12 control in the main menu: it is not Max Active Lots and misled players; kept registered for reference
  only.
- Spread lot objects while loading on by default: lamps arriving one by one relit the rooms (5.8 to 7.2 s to settle).

Details in [history](../../history/performance-lot-streaming.md).

## See also

- [Performance overview](README.md)
- [Validation](../../validation/performance-lot-streaming.md)
- [History](../../history/performance-lot-streaming.md) (with the fork's research log)
- [Lot loading and streaming](../../engine/lot-loading-and-streaming.md)
