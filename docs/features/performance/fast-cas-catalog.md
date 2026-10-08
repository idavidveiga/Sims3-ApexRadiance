# Faster CAS Catalog — native research

**Status:** an experimental, Apex-native C++ metadata cache exists. It is off by default and is
blocked in ordinary builds until the embedded game's native x86 Mono ABI has been verified.
There is **no tested in-game speedup** from this feature yet.

## Source-code analysis

From the user's own `gameplay.package` and `scripts.package`:

| UI subsystem | Managed methods |
|---|---|
| Clothing, shoes and accessories | `CASClothingCategory.SetTypeCategory`, `PopulateTypesGrid`, `PopulateGrid`, `CASClothingRow.CreateGridItems` |
| Hair and hats | `CASHair.SetHairTypeCategory`, `PopulateTypesGrid`, `AddHairTypeGridItem` |
| Makeup and tattoos | `CASMakeup.PopulatePartsGrid`, `CASTattoo.PopulateTattooGrid` |
| Native metadata | `ICASUtils.PartDataNumPresets`, `PartDataGetPresetId`, `PartDataAddDesignPreset`, `PartDataRemoveDesignPreset` |
| Thumbnail request | `IUIManager.GetCASThumbnailImage` |

The clothes/accessories path already uses small batches of `ItemGrid.BeginPopulating`.
Hair/hats can perform many UI/preset operations sequentially and may require different scheduling.
Thumbnail image handles cannot be cached safely without knowing their lifetime and invalidation rules.

## Current native implementation

`features/fast_cas_catalog.cpp` observes and optionally caches repeated native preset counts/IDs
by full ResourceKey and preset index, using the shared native ICall resolver. Entries expire after 30s;
the first repeat is checked against the game, with periodic consistency rechecks. The cache invalidates
on preset changes and fails closed on mismatches. **None of this validates the native calling convention.**

`FastCasCatalog::Start` rejects activation in ordinary builds unless
`APEX_ENABLE_UNVERIFIED_TS3_MONO_ICALLS` was explicitly supplied when compiling.
Do not set that flag for public builds.

## Implemented native scheduling core

- `features/cas_catalog_scheduler.h` — independently authored, **no third-party patching code**.
- `tests/test_cas_catalog_scheduler.cpp` — visible-item priority, prefetch, context generation
  cancellation, logical index stability, error/requeue and bounds.
- `docs/features/performance/cas-native-scheduler.md` — thread, slot and ABI integration contract.

**Important:** The scheduling core is currently independent of the UI. A validated JIT/native method
bridge and a stable ItemGrid cell replacement API are required before this can affect the game's
actual Hair/Hats, clothing or accessory screens.

## Checks before release

1. Compile the pure C++ scheduler tests using the command in `cas-native-scheduler.md`.
2. Verify the EA .NET method metadata with `tests/verify_sims3_cas_signatures.py`.
3. Obtain/inspect the user's exact installed game executable to validate Mono method resolution,
   JIT entry address and native calling conventions without guessing offsets.
4. Use only Apex's native hooks, and do not ship any standalone script mod or a replacement UI.dll.
5. Test with standard content, Store, CC, NRaas MasterController and rapid category changes.
6. Confirm stable thumbnails/selections and measure first opening, warm switching and scrolling.

Neither an independent scheduler passing unit tests nor metadata verification is a completed game hook.
