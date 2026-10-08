# Temporary MonoPatcher Hair/Hats proof of concept (EA App 1.69)

**USER TEST FAILED, 2026-10-08:** the managed full-method
replacement registered successfully (`APPLIED: 1`), but in game the
Hair/Hats catalog displayed no items. This invalidates the hypothesis
that the reconstructed replacement safely preserves the original method.
`research/temporary-hair-hats/TemporaryCasHairExperiment.cs` has been
replaced with a no-patch diagnostic class. Old builds/artefacts MUST NOT
be reused. Remove `ApexHairTemporaryResearch.package` and restart
the game without saving. Do not attempt another full-method replacement
without a faithful comparison and concrete evidence about simulator
task ownership and cancellation.

**Original status (superseded): research protocol only. No MonoPatcher package or dependency has
been added to Apex, and no playable patch has been implemented.**

## Goal

Demonstrate, in a disposable test installation, that
`Sims3.UI.CAS.CASHair.PopulateTypesGrid(bool)` can be changed to append
Hair/Hats rows incrementally across simulation ticks **while preserving the
original method's behavior**. This is a short-lived research aid, **not** the
implementation or a runtime dependency of Apex Radiance.

The final delivery must use only Apex's own native C++ code and already
validated game interfaces. It must **not** require MonoPatcher, include
MonoPatcher.asi/MonoPatcher.dll, load MonoPatcher dynamically, bundle the
temporary managed plugin, copy MonoPatcher implementation code, or ship a
replacement UI.dll.

## Newly authorized temporary tool

The user explicitly allows studying / using MonoPatcher for an isolated
experiment, **provided final releases have no MonoPatcher dependency**.

Upstream project: https://github.com/LazyDuchess/MonoPatcher (0.3.0).
The upstream source reports EA 1.69-oriented signatures for ScriptHost
initialization and Mono JIT generation, and exposes method replacement,
IL replacement and forced recompilation facilities. These are upstream
claims; they have **not** yet been validated against the user's exact
TS3.exe 1.69.47.024017. Do not copy source or assume runtime ABI from
those reported signatures. Upstream does not present a standalone
license file at the inspected main tree; copying its implementation into
Apex is **out of scope**.

## Original game methods now inspected (player-supplied packages)

**Completed 2026-10-08:** The user's `gameplay.package` and
`scripts.package` were successfully parsed in an isolated offline
workspace. The four `gameplay.package` S3SA assemblies and three
`scripts.package` S3SA assemblies were recovered; no source package
was modified and no game DLL is committed to the repository.

- Original `UI.dll`: S3SA Type `0x073FAA07`, Group `0`,
  Instance `0xF7C3ADE896D4E765`, decoded size **3,014,656 bytes**,
  SHA-256
  `c78716f1eb0191f35b12eb8dfa4b47ef1bc1e22edcf88234eb633569074dec10`.
- `Sims3.UI.CAS.CASHair.PopulateTypesGrid(bool)` is **present and
  confirmed** with MethodDef token `0x06001918`,
  RVA `0x000A0464`, `void(bool)` signature
  `20 01 01 02` (instance; one Boolean parameter), **1,621 IL bytes**
  and `maxstack=6`.
- The original routine clears `mHairTypesGrid`, retrieves featured
  Store items (calling `UIManager.LoadLayout` per matching item),
  then synchronously enumerates `mHairParts`/the current Hair/Hats
  part list and their design presets.
- The local work for one part includes calls to
  `CASUtils.PartDataNumPresets`,
  `ObjectDesigner.SetCASPart`,
  `ObjectDesigner.GetDesignPreset`, and
  `CASUtils.PartDataGetPresetId`/`PartDataGetPreset`.
  It calls `CASHair.AddHairTypeGridItem` at **two distinct IL
  positions** (default item `0x03CC`, extra presets `0x0481`).
- The loop changes `ItemGrid.SelectedItem` at `0x0412` and
  `0x04DF`, so incremental population must defer or reconcile
  selection without silently discarding these rules.
- The method contains **two try/finally enumerator regions**:
  `IL_00FD..0291` with handler `IL_0291..029F`,
  and `IL_02AC..0522` with handler `IL_0522..0537`.
  A naïve IL insertion changes branch and exception-handler offsets.
- This particular original method does **not** itself call
  `Simulator.Sleep` or `ItemGrid.BeginPopulating`.
  A simple native hook on the method entry alone cannot suspend and
  resume the stack and enumerator locals across simulation ticks.
- `CASHair.AddHairTypeGridItem` is an instance method with **five
  parameters**, returns `bool`, has token `0x0600191B`, and
  contains 292 IL bytes. The original caller uses its Boolean result
  differently for the default item and preset variants.
- `CASHair.SetHairTypeCategory` uses token `0x060018E4`;
  its role in category switching and re-entry must also be preserved.

**Conclusion:** We now have the exact managed method and its control
flow landmarks, rather than only a guessed resolver signature. A
temporary managed method replacement is feasible to **research** but
must reproduce both enumerators, Store/CC filtering, default/preset
selection and final UI updates before the gameplay prototype is enabled.
No improvement is yet claimed.

## Narrower candidate seam: item-by-item insertion

The original `CASHair.AddHairTypeGridItem` is only **292 IL bytes**
(MethodDef `0x0600191B`), without an exception/finally section.
This makes it a substantially smaller candidate for a short-lived
MonoPatcher-managed replacement than the entire 1,621-byte grid method.

Its actual behavior, recovered from the user's UI.dll, is:

1. `UIManager.LoadLayout` loads a `GenericCasItem` layout.
2. Retrieves exported window #1, updates custom content icon #23,
   then calls `CatalogProductFilter.ObjectMatchesFilter(preset, ref flag)`.
   An excluded item is not appended and the method returns false.
3. Retrieves child window #20, constructs a `ThumbnailKey` from the
   part's key/preset/body/age-gender-species, assigns the resulting
   `UIImage` and invalidates the image window.
4. Optionally shows badge/window #29 for active-wardrobe items and
   sets the debug tooltip through `GetPartName`.
5. Appends `new ItemGridCellItem(window, preset)` to the passed
   `ItemGrid` and returns true.

The signature is an instance method returning bool and accepting
`(ItemGrid, ResourceKey, CASPartPreset, bool, ref bool)`. The
`CASPartPreset.mPart` and `CASPartPreset.mPresetId` fields are
public in the exact user-supplied UI.dll, but the
`CASHair.mContentTypeFilter` field is private. Any temporary
managed replacement must preserve its filter contract exactly.

**Experimental hypothesis, not validated:** inserting a cooperative
`Simulator.Sleep(0)` **only after a successfully appended item**
could yield between Hair/Hats thumbnails while leaving the two
original parent loops and their try/finally clauses intact.
This requires proving that `PopulateTypesGrid` is called from a
simulator task that supports yielding. If not, this idea must be
abandoned; `Sleep(0)` must never be inserted blindly into a UI event
handler. It is a *responsiveness* hypothesis, not a guaranteed
reduction in total loading time. Invalid or filtered items still
need the original return value and must not yield.

**Preflight for a temporary replacement:** validate MethodDef identity,
parameter signature and the exact unmodified IL digest before applying
a replacement; refuse to install if another CAS/core mod changed the
method. The exact original `AddHairTypeGridItem` IL SHA-256 is
`17a3648b46d839af579a406c81367e40cb1822bc0f04723858a7b3e92b32b365`.
Keep the experiment opt-in and fully removable.

## Prerequisites for a functional prototype

1. Extract the original, user-owned `UI.dll` from the user's
   `gameplay.package` matching the installed EA App game. Previous
   decompilation reports show method names, **not the complete original
   Hair/Hats implementation**; they are insufficient for a safe rewrite.
2. Independently decompile and map `CASHair.PopulateTypesGrid(bool)`,
   `SetHairTypeCategory`, `AddHairTypeGridItem` and the interactions
   with `ItemGrid`. Record signature, return type, loop order, control
   flow, ownership, selection and final notifications.
3. In a **separate disposable test environment**, implement a temporary
   MonoPatcher-managed experiment that preserves those exact operations
   and yields between append batches **on the game's simulator thread**.
   Recompile methods only when necessary. This is not permitted as an
   installable Apex dependency.
4. Compare fast Hair→Hats→Hair transitions, empty categories, switching
   while still loading, scrolling, preset persistence, third-party CAS
   overrides and selection. If failures appear, revert experiment without
   changing the base game.
5. Once that experiment proves the behavior, implement an independent
   **native C++** game bridge to the already tested
   `ApexCasSchedule::HairGridSession`; test fail-closed if the native
   JIT/runtime identity, ABI or ownership cannot be verified.
6. Remove the temporary MonoPatcher installation/plugin and test that
   the C++ build launches and performs the optimization with **zero
   MonoPatcher files**. Audit Win32 build inputs, CI artifacts,
   `ApexRadiance.asi` imports, docs, and release ZIP.

## Current actual code

- `features/cas_catalog_scheduler.h`: ordered incremental scheduling.
- `features/cas_hair_grid_session.h`: native simulator-thread lifecycle
  with callbacks for append/selection/completion.
- These are tested **only with a fake grid**. Nothing currently patches
  `CASHair.PopulateTypesGrid(bool)`.
- `FastCasCatalog` / `FastCreateAStyle` remain disabled behind the
  unverified ABI compile gate on EA 1.69.

## Explicit stop conditions

Do not replace a game method based only on a name match; use the exact
method signature and a known-compatible assembly. Do not call
`Simulator.Sleep(0)` from an arbitrary synchronous event handler unless
the game's task/yield semantics for that method have been verified.
Never re-order append-only slots without stable placeholders. Do not
claim a performance improvement until native-only tests compare category
switch time and verify original CAS behavior.

**Next work item:** complete reconstruction of the two original enumerator
loops and UI side effects, then write a *temporary MonoPatcher-managed
proof-of-concept* for isolated testing. Until the method replacement is
implemented, no new game build is ready to install.
