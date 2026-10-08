# Experimental incremental hair/hats catalog for The Sims 3

**Development source only; NOT a release, installable package, or validated replacement for the game's original CAS.**

This is a prototype of an incremental managed implementation of
\`Sims3.UI.CAS.CASHair.PopulateTypesGrid(bool)\`. It belongs to the experimental
Create-a-Sim work in David's fork of Apex Radiance; it does **not** replace \`UI.dll\`, is **not**
shipped by \`ApexRadiance.vcxproj\`, and is **not controlled** by the native
"Faster CAS catalog" switch yet.

## How it works

1. The method patch is offered through [LazyDuchess/MonoPatcher](https://github.com/LazyDuchess/MonoPatcher)
   (a separate third-party dependency). It is registered only after a developer opts in.
2. Before registering, it checks the signature of \`CASHair.PopulateTypesGrid(bool)\`
   and its **exact original IL SHA-256**:
   \`e0866c4327c0eec9ae55485aee7601e6168aa651b0f201e168c440dadc10bd15\`.
   This fingerprint came from the supplied \`gameplay.package\` \`UI.dll\` and is not a promise of
   compatibility with other releases, core mods or the EA App.
3. The replacement snapshots the clothing/hair catalog context and appends items in their original order.
   It creates Store rows first, then regular hair/hat parts; hat presets are handled individually.
4. It runs inside a **simulator task** using \`Simulator.AddObject(new OneShotFunctionTask(...))\`,
   and cooperatively yields through \`Simulator.Sleep(0)\` after at most three items or about 2 ms of
   the running batch. \`Sleep(0)\` is a cooperative yield, **not** a guaranteed frame-time limit.
5. A monotonically increasing generation counter prevents an old task from continuing after a category
   change. The task also checks the CAS singleton and the Sim's age, gender, species and outfit category.
6. Item indices are not reordered; selection is restored at the end. This is **incremental**, not yet
   a sparse **visible-first** catalog.

## Isolation and activation

The managed patch is not enabled by either of the native Apex switches. Compiling this source to a DLL
does not, on its own, patch the game. In a developer-only test installation:

- Supply MonoPatcher and let it load this managed script assembly as a TS3 S3SA script mod.
- Create the marker
  \`Documents\Electronic Arts\The Sims 3\Apex Radiance\enable-experimental-hair-grid.txt\`
  **before game startup**.
- The plugin refuses to patch if the method fingerprint does not match.
- Diagnostic messages go to
  \`Documents\Electronic Arts\The Sims 3\Apex Radiance\cas-hair-experiment.log\`.
- Remove the experimental script package and marker and restart to restore vanilla behavior.

**Do not install the experiment on an important save.** This is a source-level prototype that has not
passed an x86/Mono runtime compile, TS3 build or in-game correctness test. The method replacement
cannot invoke the stock implementation as a fallback after installation.

## Compile prerequisites (Windows x86, developer test only)

Use Visual Studio/MSBuild with .NET Framework 2.0 reference assemblies and the game's own
assemblies extracted locally with a compatible DBPF/S3SA tool. Do not commit EA game DLLs.

The \`VeigaApexCasHairExperimental.csproj\` requires:
- \`Sims3ReferenceDir\`: local folder containing \`mscorlib.dll\`, \`System.dll\`, \`UI.dll\`,
  \`SimIFace.dll\`, \`ScriptCore.dll\`, \`Sims3GameplaySystems.dll\`, \`Sims3GameplayObjects.dll\`,
  \`Sims3StoreObjects.dll\`; obtained legally from your own installation.
- \`MonoPatcherDll\`: path to \`MonoPatcher.dll\` from the installed third-party mod.

Example, after obtaining references:

\`\`\`powershell
msbuild .\VeigaApexCasHairExperimental.csproj /p:Configuration=Release /p:Sims3ReferenceDir="C:\TS3\ReferenceDLLs" /p:MonoPatcherDll="C:\TS3\MonoPatcher.dll"
\`\`\`

A compiled \`.dll\` must be packaged as TS3 S3SA correctly so that MonoPatcher can find the plugin.
This repository **does not include a packaging pipeline** for that test mod yet.

## Release blockers

- Compile using the actual EA/Mono references and fix any framework incompatibilities.
- Verify the MonoPatcher version and patch timing; this is independent of the Apex native ICall resolver.
- Test with ordinary CAS and with NRaas MasterController (especially Compact CAS and its own method patches).
- Check every Store thumbnail/button, outfit/hat selection, filter, hair preset and category-switch transition
  against vanilla; the original boolean argument and all final selection/refresh semantics need runtime validation.
- Confirm no queued task touches a disposed UI controller and no UI thread affinity violation occurs.
- Measure transition and first-row appearance times with/without the patch under the same heavy CC inventory.
- Add an explicit, supported integration path from Apex's configuration/UI only after those checks.
- Implement true **visible-first with stable placeholder cells** only after verifying ItemGrid replacement API and its
  invalidation semantics; this version does not virtualize a grid.

For the verified method inventory and follow-up implementation rationale, see
[CAS grid scheduling plan](../../docs/features/performance/cas-grid-scheduling-plan.md).
