# Faster CAS catalog (experimental)

Development-only optimization of repeated CAS preset metadata requests. It is **off by default** and is **not yet validated in-game**.

## Decompiled official TS3 assemblies

Source packages: user-supplied `gameplay.package`, `scripts.package`.

| Assembly | Type | Relevant methods |
|---|---|---|
| UI.dll | `Sims3.UI.CAS.CASClothingCategory` | `SetTypeCategory`, `PopulateTypesGrid`, `PopulateGrid`, `AddGridItem` |
| UI.dll | `Sims3.UI.CAS.CASClothingRow` | `CreateGridItems`, `PopulateGrid`, `AddClothingItemAndPresets` |
| UI.dll | `Sims3.UI.CAS.CASHair` | `SetHairTypeCategory`, `PopulateTypesGrid`, `AddHairTypeGridItem` |
| UI.dll | `Sims3.UI.CAS.CASMakeup` | `SetCategory`, `PopulatePartsGrid`, `PopulatePresetsGrid` |
| UI.dll | `Sims3.UI.CAS.CASTattoo` | `PopulateTattooGrid`, `PopulatePresetsGrid` |
| SimIFace.dll | `Sims3.SimIFace.CAS.ICASUtils` | `PartDataNumPresets`, `PartDataGetPresetId`, `PartDataAddDesignPreset`, `PartDataRemoveDesignPreset` |
| SimIFace.dll | `Sims3.SimIFace.IUIManager` | `GetCASThumbnailImage` |

### Separate costs

`CASClothingCategory` uses the game's existing `ItemGrid.BeginPopulating` with a three-row batch. `CASHair.PopulateTypesGrid` traverses visible parts, calls `ObjectDesigner.SetCASPart`, looks up presets and creates the UI rows in its loop. The slow first opening of a **new** category is not solved by caching already-seen preset IDs alone.

The native `IUIManager.GetCASThumbnailImage` returns an image/drawable handle. That handle cannot safely be kept in an independent cache without understanding its native lifetime, invalidation and ownership. No handles are cached in this stage.

### Current implementation

`FastCasCatalog` uses the already-existing Mono InternalCall resolver owned by `FastCreateAStyle`, without installing a second Detours hook.

- On `ICASUtils.PartDataNumPresets`, cache the resulting count under the complete `ResourceKey`.
- On `ICASUtils.PartDataGetPresetId`, additionally include preset index in the key.
- On `PartDataAddDesignPreset` / `PartDataRemoveDesignPreset`, invalidate all related entries.
- Limit to 4,096 counts and 16,384 preset IDs; entries expire after 30 seconds.
- Every 32nd hit re-queries the game's native result and invalidates entries if they disagree.
- Turning the feature off clears the cache; previously-resolved wrappers remain pass-through.

### Risks / next steps

**Native ABI remains unverified:** the game uses an old embedded x86 Mono. The `ResourceKey` blittable layout was checked in `SimIFace.dll` (32-bit type ID, 32-bit group ID, 64-bit instance ID), but the exact native ICall calling convention and marshaling must be established on the target executable before enabling this switch.

**No game or CI build has been run.** The feature must stay off by default pending ABI verification.

Before release:
1. Verify call ABI and method dispatch on TS3W.exe Steam 1.67.2 with a debugger, then repeat validation for other builds.
2. Test count/ID equivalence and add/remove invalidation with base game, expansion, Store and CC parts.
3. Record timings/call counts and test category transitions and fast scrolling with feature ON and OFF.
4. Implement visible-first loading for hair/presets separately; avoid modifying grid ordering or selected state until behavior tests pass.
5. Only then decide whether to enable by default or extend native thumbnail caching.
