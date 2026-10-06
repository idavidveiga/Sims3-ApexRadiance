# Room to Save

The Sims 3 is a 32-bit game: in a long session a save can fail with Error 12 even though plenty of memory is free in
total, because no single free block is large enough for the save. Room to Save keeps a block of free address space in
reserve and hands it back right before every save, and when memory gets tight it empties the game's cache of files it
is not using. Nothing on screen changes.

## Status

| | |
|---|---|
| Availability | Experimental: Released in 2.7.0 |
| Default | On |
| Menu | System > Performance > Memory handling > *Room to save* (Experimental badge) |
| Configuration | `[patches.MemoryGuard]` in `ApexRadiance.toml` |
| Source | [`features/memory_guard.{h,cpp}`](../../../features/memory_guard.cpp), [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) (`MemoryGuardPatch`) |

## The problem

`TS3W.exe` has a 4 GB address space. Over a long session it fragments, and what runs out first is not memory but one
large contiguous free block. The world save writes large streams; when its allocation fails, the world save
(0x00C6D460) returns false and its caller (0x00AAC110) returns 12, the "Error 12" the player sees. The game also keeps
up to about 200 MB of idle files in its resource cache ("Resources/CacheBudget"), which it frees only on its own
schedule.

## How Apex Radiance solves it

1. **Keep a reserve.** At start, a 128 MB block of address space is reserved (`MEM_RESERVE` only, top-down: no memory
   is committed).
2. **Hand it back before a save.** The world save's call (0x00AAC320) goes through Apex: the reserve is released and,
   when the save runs on the thread of the resource system's update, the game's own shrink of its two resource caches
   runs first (idle entries only). Then the game saves.
3. **Watch the largest free block** every 4 s. Under 320 MB the resource caches are shrunk (at most once a minute, on
   the thread of their per-frame update); under 160 MB the reserve is released too. Once a free block of 640 MB is back
   (the reserve plus 512 MB) and 30 s have passed, the reserve is taken again.

Only idle resources are dropped; the game reads them again when it needs them.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Room to save | `[patches.MemoryGuard] enabled` | bool | on | | Turns the reserve and the cache shrink on. A missing key reads as on |

The row is part of the Overview's Performance group switch. It is not in the Performance part of profiles: applying a
profile leaves it as it is.

## Compatibility and interactions

- **Faster memory handling** ([fast-memory.md](fast-memory.md)) changes the allocator lock and big-block releases; the
  two work together.
- **Lot Streaming** ([lot-streaming.md](lot-streaming.md)) with more detailed lots uses more address space.
- **Vulkan driver guard** ([vulkan-driver-guard.md](vulkan-driver-guard.md)) keeps an unused driver out of the same
  address space.
- **Game build:** Steam 1.67.2 with fixed addresses and byte checks; other builds (the EA app's 1.69) through signatures
  of the same five sites, checking that the calls reach their targets.

## Limitations

- It cannot create memory: a save that needs more than the reserve plus what is free still fails.
- The resource caches are only shrunk from the thread of their own update; a save made on another thread skips the
  shrink and only releases the reserve.
- The size of the idle cache read for the log (`+0x98` of each cache) is inferred from the game's trim, not verified in
  game.

## Technical reference

| Site (Steam 1.67.2) | What | Patch |
|---|---|---|
| 0x007377F7 (in 0x007377F0) | The ResourceSystem's per-frame update call to 0x00737560 | CALL redirected to `UpdateHook`: runs the update, remembers the system and its thread, shrinks when asked |
| 0x00733E70 | ResourceSystem vtable 0x00FFE2F0 slot +0x50: `Trim` on both caches (+0x1E0, +0x1E4), every idle entry | Called by Apex |
| 0x00AAC320 (after `mov ecx,[esp+30h]` at 0x00AAC31C) | The world save call to 0x00C6D460 (thiscall, 2 arguments, `ret 8`, `al` = saved) | CALL redirected to `WorldSaveHook` |

Every call site is checked before it is rewritten and written back when the feature is turned off. A helper thread
(`Watch`) samples the largest free block with `VirtualQuery`. Address group `MemoryGuard` (`ResUpdateCall`,
`ResUpdate`, `ResShrinkBoth`, `WorldSaveCall`, `WorldSave`).

Log lines: `[MemoryGuard] On: a reserve of 128 MB ...`, `[MemoryGuard] World save done / FAILED (Error 12): largest free
block N MB, M MB for the save (...)`, `[MemoryGuard] Largest free block N MB: the reserve of 128 MB let go` and
`... the game's resource cache emptied (N MB of idle files)`. `MemoryGuard::StatusText` (largest free block and its
lowest value, reserve held or let go, world saves and failures, cache shrinks and the idle megabytes they freed) and
`RenderDeveloperUI` (with a button *Empty the game's resource cache now*) exist, but no Developer card calls them in the
current menu.

## Rejected approaches

None recorded.

## See also

- [Performance overview](README.md)
- [Validation](../../validation/performance-room-to-save.md)
- [History](../../history/performance-room-to-save.md)
