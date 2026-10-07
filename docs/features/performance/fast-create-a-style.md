# Faster Create-a-Style

Reduces repeated work while browsing patterns in Build/Buy Create-a-Style. The stock UI populates the complete pattern
grid in a simulator task and asks the native ObjectDesigner for thumbnails as it goes. Apex keeps a bounded in-memory
copy of finished native pattern thumbnails, so an identical request can reuse the exact bytes instead of rebuilding the
same preview again.

## Status

| | |
|---|---|
| Availability | Development branch; needs in-game validation before release |
| Default | On in the development branch |
| Menu | System > Performance > Create-a-Style > *Faster Create-a-Style* |
| Configuration | `[patches.FastCreateAStyle] enabled` in `ApexRadiance.toml` |
| Source | [`features/fast_create_a_style.{h,cpp}`](../../../features/fast_create_a_style.cpp), [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) |

## Why this exists

The official `UI.dll` controller `Sims3.UI.CAS.CASCompositorController` fills the material grid through
`PopulateMaterialsBinGridTimeslicedTask`. For each pattern it can request a thumbnail, build compositor data and create
a `MaterialsGridItem` layout before yielding with `Simulator.Sleep(0)`. The mouse-wheel handler itself only scrolls the
grid; the visible delay while scrolling is consistent with this full-grid population still running as later rows are
reached.

The thumbnail path is:

```text
CASCompositorController
  -> CompositorUtil.GetPatternThumbnail
  -> SimIFace.ObjectDesigner.GetPatternThumbnail
  -> IWorld.ObjectDesigner_GetPatternThumbnail
```

The last call is a Mono InternalCall into the native game.

## How Apex changes it

Apex hooks `mono_lookup_internal_call` and watches only methods on `Sims3.SimIFace.IWorld`. When Mono resolves the
three pattern-thumbnail calls, Apex keeps the game's native target and substitutes a small wrapper:

- `ObjectDesigner_GetPatternThumbnail`: lookup in the cache first; on a miss call the game and store the finished
  byte array.
- `ObjectDesigner_CreateLargePatternThumbnail`: call the game, then invalidate entries for that compositor/pattern.
- `ObjectDesigner_ClearLargePatternThumbnail`: call the game, then clear Apex's thumbnail cache.

The cache key contains the compositor ID, pattern hash, byte-array length and a hash of the caller-provided bytes before
the native call. A cache hit copies the exact stored bytes back to the Mono `byte[]` and returns the same native result.
The game remains the source of truth for every miss.

The cache is bounded to 2,048 entries and 64 MiB. Reaching either limit clears it rather than allowing unbounded growth.

## Compatibility and safety

- The official `UI.dll`, `SimIFace.dll` and `Sims3Metadata.dll` are not modified.
- Steam 1.67.2 uses the validated `mono_lookup_internal_call` address; other builds use the routine's byte signature and
  validate the exact prologue before Detours writes anything.
- If the resolver is not found or differs, the feature stays off and writes nothing.
- Turning the feature off is safe even if Mono already cached Apex's wrapper: the wrapper checks the live switch and
  passes directly to the original native call.
- The cache is protected by a mutex, but the game's native thumbnail generator is never called while that mutex is held.

## What this stage does not change

This first stage does **not** change `CASCompositorController.PopulateMaterialsBinGridTimeslicedTask`, does not move the
texture compositor to another thread, and does not virtualize the grid. Therefore the first generation of every unique
thumbnail still costs the same as the game.

The decompilation also found `ItemGrid.BeginPopulating`, `VisibleRows`, `VisibleColumns` and
`GetFirstVisibleItem`. A later stage can use those findings to prioritize the visible rows and prefetch one or two rows
ahead, after this cache has been measured separately.

## Validation targets

See [validation](../../validation/performance-fast-create-a-style.md).
