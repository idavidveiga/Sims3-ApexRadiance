# CAS Hair/Hats native Mono bridge — validation boundary (2026-10-08)

**Conclusion: source contract VERIFIED, C++ plan tests PASSED, Windows x86
build PASSED; native JIT bridge NOT VALIDATED, NOT ENABLED.**

Do not claim a playable Hair/Hats improvement. This report separates
offline facts from runtime assumptions. Input binaries remain user-owned,
are never committed, and the original game files have not been modified.

## Inputs verified offline

- Original player-supplied `gameplay.package`: extracted S3SA
  `UI.dll`, Instance `0xF7C3ADE896D4E765`.
- `UI.dll` SHA-256:
  `c78716f1eb0191f35b12eb8dfa4b47ef1bc1e22edcf88234eb633569074dec10`.
- Original player-supplied `TS3.exe` SHA-256:
  `7352dd6e599f4475f812bbca19bbc7e5d8e84bbd5b6d21acaac9b1c5236adfe4`.
- `TS3.exe` x86 PE32 `.text`: RVA `0x1000`, virtual length
  **12,441,402 bytes**, on-disk length **12,443,648 bytes**,
  on-disk Shannon entropy approximately **8.0000 bits/byte**.
- Loaded-memory evidence from a **previous** game session:
  extended `.text` scan completed with no unreadable sections,
  one historical signature candidate at `RVA 0xA826A0`.
  Candidate bytes begin `81 EC 08 08 00 00 53 55 8B AC 24 14 08`.
  That prologue does **not** appear in the on-disk `.text`.
  These facts do not establish what native function it is.

## Original UI.dll method validation: **8 of 8 pass**

All eight names, MethodDef indexes, IL lengths, exact SHA-256 digests
and known CALL/CALLVIRT edges were checked independently against the
actual extracted `UI.dll` using the private offline metadata parser.
The equivalent public repeatable check is
`tools/verify_hair_ui_contract.py` applied to the user's original
decoded DLL. No EA game assembly is bundled with the verifier.

| Original CASHair method | Token | IL length | Original call edge |
|---|---|---:|---|
| SetHairTypeCategory | `0x060018E4` | 215 | 1× PopulateTypesGrid |
| RefreshHairGrid | `0x060018EA` | 14 | 1× PopulateTypesGrid |
| OnTrashButtonClick | `0x060018ED` | 127 | 1× PopulateTypesGrid |
| OnSaveButtonClick | `0x060018F0` | 353 | 1× PopulateTypesGrid |
| OnUndo | `0x060018F9` | 14 | 1× PopulateTypesGrid |
| OnRedo | `0x060018FA` | 14 | 1× PopulateTypesGrid |
| PopulateTypesGrid | `0x06001918` | 1,621 | 2× AddHairTypeGridItem |
| AddHairTypeGridItem | `0x0600191B` | 292 | original ItemGrid append |

This shows the *managed method identity and calling graph*, **not**
a native method address or per-method JIT trampoline. In the original
method, the two `AddHairTypeGridItem` calls belong to default/extra
preset branches; each part group must remain atomic. The 2026-10-08
MonoPatcher full-method prototype was **withdrawn** after game hair
and hats vanished; removing it restored the catalogue. No replacement
is active.

## Native code readiness checks

- `features/cas_hair_population_plan.h` uses original-order
  `FeaturedStore → PartGroups → Finalize` logic and keeps entire
  default/extra-preset groups together.
- A recursive mutex now protects plan state from cross-thread access
  and permits game-thread reentrant category changes; foreign-thread
  `Begin` is rejected without altering the active generation.
- Regression tests:
  `tests/test_cas_hair_population_plan.cpp` (8 groups).
- CAS native CI successful:
  `https://github.com/idavidveiga/Sims3-ApexRadiance/actions/runs/37734353459`.
- Win32 Apex build successful on the immediately previous project head:
  `https://github.com/idavidveiga/Sims3-ApexRadiance/actions/runs/37733737832`.
  The later mutex change touches only the header, which is registered
  as a project include; the **new head's Win32 build has not been
  independently completed by this report**.
- The game bridge and runtime callbacks to `CASHair.PopulateTypesGrid`
  remain **unimplemented**. The header's successful tests cannot
  measure Hair/Hats performance.
- The experimental native Mono ICall caches still explicitly return
  `false` unless `APEX_ENABLE_UNVERIFIED_TS3_MONO_ICALLS` is
  deliberately enabled at compile time. Leave this macro **OFF**.
- The production `ApexRadiance.vcxproj` does not compile or link the
  withdrawn MonoPatcher-managed test project.

## Remaining blockers before enabling a native live hook

1. Observe the **loaded** native Mono runtime code in the actual game
   process and identify a specific `mono_generate_code`/JIT entry by
   actual caller/callee and `MonoMethod` identity, not merely by bytes.
2. Prove the x86 calling convention, argument/stack layout, original
   trampoline lifetime and detour coexistence with the installed
   mod/ASI stack. Do not infer the ABI from a Steam signature.
3. Prove an Apex-owned simulator/UI-thread continuation that preserves
   both original managed enumerator/finally regions, their selections,
   Store/CC filters and final UI notifications.
4. Test that all native gates fail closed when these facts are absent,
   then compare game behaviour and performance with/without the patch.
5. Repeat tests with the temporary patching framework fully removed;
   **zero dependency on MonoPatcher** in the final ASI/ZIP.

**Disposition: fail closed.** Do not enable either native ICall cache
or a new managed-method detour, and do not deliver a new installable
Hair/Hats optimization on the evidence available here.

## Continued validation: read-only native reference evidence

A second, **fully read-only** loaded-memory scan is implemented:

- `features/ts3_mono_xref.h`: standalone x86 `E8 rel32` byte-target
  observer, with negative displacements, unsigned wrapping and
  four-byte chunk-tail support.
- `features/ts3_mono_runtime_probe.cpp`: after the existing loaded
  `.text` signature scan finds a bounded set of historical candidates,
  it automatically performs a separate reference pass. At most 64 KiB
  of executable pages are read per rendered developer frame.
- Output reports the number of raw `CALL` byte patterns whose rel32
  target equals each candidate RVA, plus at most 24 caller RVAs.
  There are **no page protection changes, detours, hooks or game writes**.
- Unreadable/guarded pages are skipped and the four-byte tail is
  cleared to avoid incorrectly combining noncontiguous bytes.
  Candidate lists truncated by the match cap are rejected instead
  of reporting misleading partial reference counts.
- Tests `tests/test_ts3_mono_xref.cpp` (6 synthetic groups) passed in
  [CAS native diagnostics run 37734957700](https://github.com/idavidveiga/Sims3-ApexRadiance/actions/runs/37734957700).

**Crucial limit:** `E8` byte observations are not an instruction
disassembly. A reference to a candidate is **not evidence that the
candidate is the correct JIT entry**, and is not an ABI proof.
Only a later, independently verified live code identity and managed
`MonoMethod` association can lift the hook safety gate. The older
Hair/Hats MonoPatcher replacement remains withdrawn.

Windows x86 compilation for this probe change runs independently in
`apex-cas-win32-diagnostic.yml`; its result must be checked rather
than inferred from the Linux unit tests. Do not install or test a live
CAS patch from this read-only instrumentation.

## External ABI hypothesis reviewed (not copied into Apex)

For research only, the published MonoPatcher 0.3.0 sources at
`LazyDuchess/MonoPatcher`, pinned commit
`2fa43bf18e4bbc43620f2d3275ad1ad27109877c`,
were inspected (especially `MonoPatcher.CPP/src/Addresses.cpp` and
`MonoPatcher.CPP/src/MonoHooks.cpp`). They declare a
`__cdecl` `GenerateCode(MonoMethod*, void*, void*, void*)` detour
and maintain a per-`MonoMethod` IL replacement map. Those declarations
explain the **upstream experimental design**, not the verified ABI of
this user's EA executable or a safe call site for Apex.

The existing live scan's candidate prologue
`81 EC 08 08 00 00 ...` is **not the upstream declared
`MonoGenerateCodeLookup` entry prologue**, so finding references to
the historical candidate cannot identify it as Mono's JIT compiler.
The new read-only `E8 rel32` reference pass is only a preparatory
observation; do not present it as successful JIT validation.

No MonoPatcher source, headers, libraries, copy of its signatures,
IL patchers or generated artifacts were imported into the Apex native
implementation. Final builds must be entirely independent.

## Second user live log — 2026-10-08 03:14 game-local

The user ran the new read-only diagnostic with
`ApexRadiance_LOG_LIVE(2).txt`. The scan conclusively **completed**,
but did **not** establish JIT identity:

- Game `EA 1.69.47.024017`; `ApexRadiance.asi` v2.10.1,
  `MonoPatcher.asi` still present as a **temporary research tool**.
- Initial signature pass: full loaded `.text` scanned
  **12,441,402 / 12,441,402 bytes**, **0 skipped**;
  **one historical-signature candidate** at RVA `0xA826A0`.
- Cross-reference pass: full loaded `.text` scanned
  **12,441,402 / 12,441,402 bytes**, **0 skipped**.
- Four raw x86 E8 rel32 byte sequences targeted that same RVA:
  `0x98A28C`, `0xA6454C`, `0xA84DDF`,
  `0xA9931E`. These are **byte-level observations**, not
  authenticated CALL instruction boundaries or verified callers.
- No `[ERROR]` entries in the supplied log.
- **No CAS hook installed. No speedup demonstrated.**

These four caller locations offer an offline next step. The existing
record did not capture the caller instruction windows, so an
independent argument/stack analysis cannot yet be performed.
The scanner was updated (commit `c16d24f`) to attach at most
**24 samples**, each with at most **24 bytes before and 96 after**
a raw E8 rel32 sequence, obtained exclusively from the 64-KiB memory
block already read during its bounded reference pass. No additional
memory read or writable page permission is introduced.

**Do not use `0xA826A0` as a Mono JIT hook** merely because it has
four incoming raw-E8 matches. Prove real instruction boundaries,
function identity, `MonoMethod` argument semantics and stack cleanup
before considering any Apex-native runtime hook. Once the research
phase is complete, remove temporary MonoPatcher installation;
it must never be shipped with Apex.

## Fourth live log: caller cache semantics established (03:49 game time)

Player-supplied `ApexRadiance_LOG_LIVE(4).txt` contains **all four
caller instruction windows** from the upgraded read-only probe.
Both full `.text` passes completed, each **12,441,402/12,441,402 bytes
and 0 skipped**. The four E8 rel32 offsets and destinations were
recomputed offline from the exact emitted bytes (all matched).

| Caller RVA | Argument and stack cleanup | EAX return cached into |
|---|---|---|
| `0x98A28C` | `PUSH ESI; CALL 0xA826A0; ADD ESP,4` | `[ESI+0x20]` |
| `0xA6454C` | `PUSH EDI; CALL 0xA826A0; ADD ESP,4` | `[EDI+0x20]` |
| `0xA84DDF` | `PUSH ESI; CALL 0xA826A0; ADD ESP,4` | `[ESI+0x20]` |
| `0xA9931E` | `PUSH EBP; CALL 0xA826A0; ADD ESP,4` | `[EBP+0x20]` |

Each call site is a credible instruction boundary because the
preceding byte is `PUSH r32`, immediately followed by a complete
`E8 rel32`; each has caller-side cleanup and cached return.
The first two also test their `[object+0x20]` pointer for null
before calling the candidate.

These observations support a **single-argument lazy helper** that
initializes/caches metadata rather than a four-argument JIT compiler.
In the pinned **public** MonoPatcher `include/mono.h` research
declaration, `MonoMethod.header` is at offset `0x20`, whereas the
separate `MonoHooks.cpp` research declaration says
`mono_generate_code(MonoMethod*, void*, void*, void*)`, four arguments
on `__cdecl`. The layout is suggestive, **not an EA 1.69 validated
struct declaration or proof of exact function identity**.

**Decision: DO NOT use `RVA 0xA826A0` as the native
`mono_generate_code` hook.** Its observed call semantics do not
match that API. The current research has answered the question it
was designed to answer; **do not ask the user for another repeat of
the same scan**. The native probe's labels were updated to explicitly
describe this candidate as a probable method-header cache helper.
A real speedup still requires a separately verified safe CAS integration
point; unit tests and byte scans alone are not sufficient.

The user log also lists `MonoPatcher.asi` among Game/Bin wrappers.
This verifies *file presence in the startup inventory only*, not that
it installed an active hook. The final native Apex build must not
depend on this temporary research component.


## Follow-up: rejecting the stale ICall resolver guess and identifying a viable bridge

The signature `81 EC 08 08 00 00 53 55 8B AC 24 14 08` had also
been used as a cross-version fallback in `fast_create_a_style.cpp` for
`mono_lookup_internal_call`. The player's fourth live log associates these
bytes with RVA `0xA826A0`, whose four callers pass one argument and cache
the returned EAX at `[pointer + 0x20]`. **That is not verified as an ICall
resolver.** Its cross-version pattern fallback has been removed; EA 1.69
and unknown builds can no longer enable the experimental resolver via that
guess. The separately researched Steam address is retained behind the
existing off-by-default experimental ABI gate, not promoted as proof of
EA compatibility.

Important distinction: standard Mono documents
`mono_compile_method(MonoMethod*)` as a one-argument interface that returns
native code (see `mono/mono/metadata/object.c`). Therefore argument count
alone does **not** exclude every possible compilation-related API. What
the player's observation excludes is claiming the four-argument
`mono_generate_code` entry from those bytes. The cache-at-offset-`0x20`
behavior makes a method-header helper plausible, but exact identity still
requires independent proof.

### Focused read-only baseline, not another signature sweep

Developer > Performance > CAS native runtime inspection now exposes
`Inspect known Mono runtime anchors (read-only)`. It checks the already
resolved GameAddress entries `MonoTypeGetObject` and `MonoDomainFree`,
verifies their known entry bytes and logs their executable RVAs.
It does **not** scan the full image again, invoke Mono, read arbitrary
`MonoMethod` objects, hook a function or alter a game page.

These are **independent known-runtime anchors**, not candidates for the
JIT or a proof of the Mono method ABI. If either appears unavailable or
modified by another hook, the inspector records that and does not
extrapolate from the entry bytes.

### Exact evidence needed for a future native Hair/Hats bridge

1. Establish a loaded-image call graph from independently verified Mono
   APIs, using real x86 instruction boundaries, not only raw `E8`
   offsets or a Steam address transplanted into EA 1.69.
2. Identify the *specific* runtime `MonoMethod` for
   `UI.dll :: Sims3.UI.CAS.CASHair.PopulateTypesGrid(bool)`, whose
   original MethodDef is `0x06001918`, and cross-check owning assembly,
   class, signature, and original IL hash. No blind pointers are callable.
3. Determine how that method's compiled entry is obtained and remains
   valid across JIT, domain and CAS lifecycle changes. Verify executable
   ownership and each native x86 parameter, register, calling convention,
   stack cleanup and trampoline boundary independently.
4. Establish a safe same-simulator-thread continuation for the parent
   enumerations and preserve original Store, presets, selection, grid
   finalization and thumbnail ownership. The current
   `HairPopulationPlan` is *only* the offline ordering core.
5. Enable an opt-in experimental hook **only after** independent
   identity/ABI proof and explicit compatibility checks. Otherwise
   decline installation and run the original CAS. Compare cold/warm
   category and scrolling timings, verify visual completeness, and
   remove all temporary research tools from the final release.

The new source-only CI regression checks that the invalid EA signature is
not restored as an ICall resolver and that the focused anchor inspector
contains no writes or Detours install call. Source-level checks and an
x86 build are not live-game hook validation.
