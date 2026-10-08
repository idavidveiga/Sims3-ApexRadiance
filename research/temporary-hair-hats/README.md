# Temporary managed Hair/Hats responsiveness prototype (RESEARCH ONLY)

**DO NOT PUBLISH OR INCLUDE THIS IN APEX RADIANCE RELEASES.**

This disposable MonoPatcher 0.3.0 experiment was independently reconstructed
from the player's original `gameplay.package` EA 1.69 UI.dll. It is **not**
a native optimization and **has not yet been tested in-game**.

It attempts only one change: after each successfully appended Hair/Hats
`ItemGrid` row, ask `Simulator.Sleep(0)` to yield the simulator task.
The exact original content filtering, thumbnail creation, wardrobe badge,
tooltip and Boolean return behavior are preserved in the source as closely
as possible. A runtime check refuses to apply the replacement unless
`AddHairTypeGridItem` has the expected 5-parameter signature, MethodDef token
`0x0600191B`, 292-byte original method body and expected FNV-1a fingerprint.
The required exact `ThumbnailKey` constructor is resolved dynamically
against the actual loaded 1.69 assembly rather than presumed from public refs.

**Safety limitation:** a synchronous UI event handler may not support
`Simulator.Sleep`; neither functional benefit nor safety is validated.
This could introduce reentrancy/crashes. This is an opt-in experiment for a
*disposable save/testing copy only*. A smoother response is a hypothesis:
total category load duration may even increase. Other mods replacing this
method may be incompatible. Do not enable it until build validation has
passed and the tester can safely undo the changes.

## Build inputs

- Independently owned source: `TemporaryCasHairExperiment.cs`.
- Research-only project: `ApexHairTemporaryResearch.csproj`.
- MonoPatcher 0.3.0 upstream (tag pinned to
  `2fa43bf18e4bbc43620f2d3275ad1ad27109877c`) is only checked out
  into an ephemeral GitHub Actions workspace for this *separate* managed
  project. It is not vendored, linked into or loaded by `ApexRadiance.asi`.
- The research packaging script creates an S3SA-only mod file containing
  just the managed experiment, not any of the user's original game DLLs.

CI workflow:
`.github/workflows/cas-temporary-monopatcher-research.yml`.

## Installation for a future controlled smoke test — NOT YET VALIDATED

1. Back up the game configuration and use a disposable save.
2. Install MonoPatcher 0.3.0 according to the official instructions.
   **Do not blindly overwrite the existing `wininet.dll`** (the tester
   already uses a loader and Apex alongside DXVK); verify whether the
   current loader can load `MonoPatcher.asi`. Stop if it conflicts.
3. Place only `ApexHairTemporaryResearch.package` in Mods/Packages.
4. The test `.package` itself is the explicit opt-in: installing it in
   Mods/Packages enables a guarded replacement attempt. The previous activation
   marker `Game/Bin/MonoPatcher/EnableApexHairResearch.txt` is **no longer
   needed or checked**. You may delete the old marker. This change avoids the
   observed `EntryPointNotFoundException: GetModuleFileNameW` from the game's
   embedded Mono runtime.
   The status command is registered whenever the research script assembly loads.
   Enter `apexhair_status` in the cheat console to see whether the original
   method passed the exact signature/IL checks, was patched, or was rejected.
   The standard MonoPatcher `monopatcher_log` command reports replacements.
   If `apexhair_status` is unknown, verify the experiment `.package` is
   actually installed; MonoPatcher loading is not evidence that this script loaded.
5. Compare Hair→Hats→Hair, empty categories, fast switching, CC thumbnail
   identity, active wardrobe, selected preset and scrolling against an
   unmodified launch. Avoid saving any Sims until comparisons succeed.
6. Remove the research package, marker, MonoPatcher managed package and
   `MonoPatcher.asi`; restore the original loader if installation changed
   it. Restart and confirm the Apex C++ build still works alone.

**Final requirement:** no MonoPatcher source, DLL, package, ASI, dependency
or custom JIT plugin in Apex's deliverables. The permanent native-only
method bridge must be independently developed and validated.
