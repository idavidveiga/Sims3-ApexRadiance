# Temporary MonoPatcher Hair/Hats proof of concept (EA App 1.69)

**Status: research protocol only. No MonoPatcher package or dependency has
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

**Next work item:** inspect the matching original UI.dll method body
and write a *temporary managed proof-of-concept*. Until then, there is
no new game build for the user to install.
