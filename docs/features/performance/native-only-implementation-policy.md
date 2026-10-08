# CAS / Create-a-Style — Apex-native-only implementation policy

## Non-negotiable restrictions

- **Do not use MonoPatcher or copy/reuse its source code.**
- No MonoPatcher DLL, ASI, dependency, script package, build references, initialization attributes, or installation instructions.
- The supported delivery is the existing **ApexRadiance.asi**. No second mod is required.
- Implementation must be written independently in Apex's C++ tree, using the existing native GameAddress / DetourBatch infrastructure and technical facts established by studying the user's own EA game assemblies and executable.
- Do not replace or redistribute the game's entire UI.dll / gameplay.package / scripts.package.
- New hooks must fail closed on unknown game versions, method signatures, or native calling conventions.

## Independent native design

The costly work is in the managed UI.dll methods:
- CASClothingCategory.PopulateTypesGrid / PopulateGrid for clothing, shoes and accessories.
- CASHair.PopulateTypesGrid for Hair / Hats.
- Related CAS preset and thumbnail requests from SimIFace.

The existing native InternalCall interception can reduce some repeated metadata lookups. It **cannot**
make a managed catalog population loop yield, stop obsolete category work, or fill visible cells first.
An actual incremental CAS scheduler requires a separate, independently authored hook on the
**native machine-code entry point of a managed method**, or a game-native scheduling API that
can be demonstrated to have the same effect.

Possible Apex-only implementation route, subject to reverse-engineering validation:

1. Use Apex's own signature scanner to resolve the embedded game's Mono APIs; validate every address
   and relevant method signature on the installed executable. Never assume a stable public Mono DLL.
2. Resolve the target UI.dll method by assembly, namespace, class, name and complete signature.
3. Identify its JIT-compiled native entry point and verify it is compatible with a Detours trampoline
   under the game's x86 ABI. Do not hard-code a guessed JIT function address.
4. Write the scheduling logic independently in C++ and hook only verified entry points. Keep
   the game's original function callable for disabled/unsupported conditions.
5. Ensure all UI/layout/compositor operations remain on the correct game thread. Use a
   generation/cancellation token for context changes and preserve catalog item order,
   selection, filters, presets, Store items and ownership/lifetime.
6. Detect other mods which replace or alter the target method and leave the feature disabled if
   compatibility cannot be demonstrated.
7. Benchmark category changes and scrolling with cold/warm caches, heavy CC folders and
   different Sim contexts. Do not claim a speedup before in-game measurements.

**Important:** The JIT-entry approach is a feasibility path, not an implementation or proof of safety.
The native calling conventions and lifecycle must be confirmed before activating any such hook.

## Current branch status

- Experimental Apex-native CAS metadata/CASt thumbnail hooks exist but remain **off by default**.
- Both are excluded from the global Overview Performance switch.
- A signature test and a detailed CAS grid scheduling plan exist.
- **No incremental Hair/Hats method patch or visible-first implementation is active.**
- No MonoPatcher code or artifacts are part of this branch's tree or new branch history.

Use this branch for independent development and review before submitting any change upstream.
