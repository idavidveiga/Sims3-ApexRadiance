# Create-a-Sim category switching and grid scheduling

**Status:** Apex-native C++ scheduling core implemented in `features/cas_catalog_scheduler.h` with standalone tests in `tests/test_cas_catalog_scheduler.cpp`; **not connected to UI.dll or a playable hook yet**.  No external method-patching libraries or additional script packages are permitted. The previous managed experiment was withdrawn. Native CAS metadata caching remains experimental and off by default; incremental Hair/Hats and visible-first loading are not yet implemented in a playable build.

## Verified code paths

The following methods exist in the supplied original TS3 `UI.dll`:

| Subsystem | Managed entry points |
|---|---|
| Clothing / accessories / shoes | `CASClothingCategory.SetTypeCategory`, `PopulateTypesGrid`, `PopulateGrid`, `AddGridItem` |
| Individual clothing row | `CASClothingRow.CreateGridItems`, `PopulateGrid`, `AddClothingItemAndPresets` |
| Hair / hats | `CASHair.SetHairTypeCategory`, `PopulateTypesGrid(bool)`, `AddHairTypeGridItem` |
| Makeup | `CASMakeup.SetCategory`, `PopulatePartsGrid`, `PopulatePresetsGrid` |
| Tattoos | `CASTattoo.PopulateTattooGrid`, `PopulatePresetsGrid` |
| Shared grid | `ItemGrid.BeginPopulating`, `OnPopulateTick`, `VisibleRows`, `VisibleColumns`, `GetFirstVisibleItem`, `OnGridMouseWheel` |
| Thumbnail | `UIUtils.GetUIImageFromThumbnailKey`, `UIManager.GetCASThumbnailImage` |
| Native metadata | `ICASUtils.PartDataNumPresets`, `PartDataGetPresetId`, `PartDataGetPreset` |

Clothing/accessory item grids already use an incremental `ItemGrid.BeginPopulating` path with a small row batch.
Hair presets have a more sequential per-part construction path (including design preset extraction and layout creation).
Consequently the same optimization is **not** appropriate for all categories.

## Implementation order

### 1. Profile the actual bottleneck first

Compare repeated category transitions under the same Sim, filters and CC set:

- Tops → Bottoms → Tops.
- Hair → Hats → Hair.
- Accessories → Shoes → Accessories.
- First opening vs second opening; scroll from first row to last and back.

The `FastCasCatalog` status now records count/ID lookups, repeated requests, real native time and cache discrepancies.
Measure the full elapsed category transition independently in the game's profiler. If native preset metadata is a tiny
fraction of the delay, do **not** keep expanding the native metadata cache as a solution to the UI problem.

### 2. Incremental hair construction (first functional managed patch)

Refactor only `CASHair.PopulateTypesGrid(bool)` into a cancellable task that appends items in the original CAS order,
with a small per-tick budget. The existing UI must remain responsible for `ObjectDesigner.SetCASPart`,
`AddHairTypeGridItem`, filter/selection state and row creation, on its original thread.

- Each new hair/hat category or Sim-context change increments a generation ID.
- A pending task checks the generation before every batch; obsolete tasks stop without touching the new grid.
- Populate enough rows for the initial viewport as a first batch; continue with a small time budget, checking the
  stopwatch after each completed item. Avoid scheduling fixed N items when a single CC part may be expensive.
- Restore selection only after its actual row is present. Do not automatically change active hair/hat presets.
- On completion, send the same terminal grid/filter update as the original method.

This approach limits a long contiguous pause without changing item indices.

### 3. True visible-first loading (separate, more invasive patch)

Simply inserting the currently visible parts *before* earlier rows would shift indices and break scrolling or selection.
Visible-first requires either (a) preallocated stable placeholder slots or (b) a verified sparse-grid insert/update API.

- Keep the complete **logical** ordering and exact item count.
- Prioritize visible rows; prefetch one or two rows ahead and one behind.
- On scroll, refresh priority without reordering or removing an already visible item.
- Replace placeholders atomically on the game/UI thread, preserving selection, filters, tooltip, draggable item data.
- Discard scheduled work from old category/filter/age/gender/outfit generations.

Do not implement visible-first in an append-only `ItemGrid` without verified placeholder support.

### 4. Reuse filtered category metadata, not UI objects

A future managed-side cache should store immutable catalog descriptors / vetted preset metadata and invalidate by:

- Sim age, gender and species; outfit category; hair vs hat; body type;
- custom-content, Store/installed content and other active filters;
- wardrobe or downloaded content changes, and preset creation/deletion;
- a new CAS session or changes to the underlying list.

Never retain `UIImage` handles, `WindowBase` objects or `ItemGridCellItem` across categories. Their native lifetime and
selection/linkage belong to the game's UI.

## Runtime patching boundary

Changing the scheduler requires a **native Apex-owned hook** on a verified game/Mono method entry point (or an Apex-owned, independently developed IL patcher) rather than only the existing InternalCall lookup hook. We will not import or depend on external runtime patchers.
Modifying only `PartDataNumPresets` or `GetCASThumbnailImage` cannot reorder the grid or make its managed loop yield.

Do not ship a whole replacement `UI.dll`; this can conflict with UI core mods and NRaas MasterController CAS changes.
A per-method patch needs identity/signature checks, runtime ownership checks and a safe way to decline installation when
another mod already owns the target.

**The native ICall ABI remains unverified on TS3W.exe.** Both cache switches must stay disabled by default before runtime
verification, and the existing game remains the fallback.

## Acceptance

- No wrong or mixed hair, tops, bottoms, shoes, accessories, makeup or tattoo thumbnails.
- No change to category selection, presets, outfit or purchased/Store items.
- Smooth scrolling, with stable logical item indices, even after fast category changes.
- No work from an obsolete category appears in the new one.
- Feature off reproduces unmodified game behavior.
- Timings for cold category opening, warm return and high-speed scrolling are recorded.
