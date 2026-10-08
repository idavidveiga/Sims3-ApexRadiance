# Hair/Hats missing-grid regression — original UI.dll vs withdrawn patch

Date: 2026-10-08. **Status: root-cause candidates found; not yet proven individually.**
The user's EA App 1.69 `gameplay.package` and `scripts.package` were inspected
offline; no copyrighted game assemblies or method bodies were committed.

## Confirmed in-game behavior

- MonoPatcher 0.3.0 initialized (CPP).
- The opt-in managed experiment reported `APPLIED: 1 Hair/Hats replacement(s)`.
- Both hair and hat categories then displayed no items.
- Removing `ApexHairTemporaryResearch.package` and restarting restored
  both original catalogues. This establishes **the experiment** as the
  cause of the observed regression; it does **not** identify which
  statement of the experimental replacement first failed.
- The unsafe experiment is withdrawn. Current
  `research/temporary-hair-hats/TemporaryCasHairExperiment.cs`
  contains **no game method replacement**.

## Original method identity and call graph

The extracted original `UI.dll` SHA-256 is:
`c78716f1eb0191f35b12eb8dfa4b47ef1bc1e22edcf88234eb633569074dec10`.

| Original method (CASHair) | MethodDef | RVA | IL size | Calls |
|---|---|---|---:|---|
| `SetHairTypeCategory` | 060018E4 | 0009EA74 | 215 | `PopulateTypesGrid` |
| `RefreshHairGrid` | 060018EA | 0009F2F1 | 14 | `PopulateTypesGrid` |
| `OnTrashButtonClick` | 060018ED | 0009F37C | 127 | `PopulateTypesGrid` |
| `OnSaveButtonClick` | 060018F0 | 0009F448 | 353 | `PopulateTypesGrid` |
| `OnUndo` | 060018F9 | 0009F8E7 | 14 | `PopulateTypesGrid` |
| `OnRedo` | 060018FA | 0009F8F6 | 14 | `PopulateTypesGrid` |
| `PopulateTypesGrid(bool)` | 06001918 | 000A0464 | 1,621 | 2x `AddHairTypeGridItem` |
| `AddHairTypeGridItem` | 0600191B | 000A0CB0 | 292 | `ItemGrid.AddItem` |

The parent `PopulateTypesGrid` processes featured Store items and
multiple part/preset paths under **two enumerator finally regions**.
It calls `AddHairTypeGridItem` separately for a default hair part and
for extra presets. Its current selection and terminal updates depend on
the order and return values.

The original `AddHairTypeGridItem` uses a direct call to
`UIManager.LoadLayout`, sets the custom-content icon, checks the
`mContentTypeFilter` with a by-reference Boolean, constructs the
`ThumbnailKey` in-place, sets its `ImageDrawable`, and finally invokes
`ItemGrid.AddItem` before returning true. It returns false on
layout/filter failure. **It does not call `Simulator.Sleep`.**

## What the withdrawn code changed (not just the intended pause)

The experimental `[ReplaceMethod]` replaced the **entire 292-byte**
managed method and therefore changed more than scheduling:

1. It introduced `Simulator.Sleep(0)` **inside the synchronous
   `AddHairTypeGridItem` call path**, after `ItemGrid.AddItem`.
   Because callers include UI category, save, trash and undo/redo
   handlers, there was **no validated yieldable task context**. The
   scheduler cannot safely resume the C# parent method's enumerator
   locals from this replacement.
2. It replaced `CASHair.GetPartName(part)` with
   `MethodInfo.Invoke` and field access with `FieldInfo.GetValue`.
   These are not equivalent under all Mono runtime constraints, can
   allocate/throw, and were not tested against the original game method
   on all filter/tooltip paths.
3. It replaced the original IL `newobj ThumbnailKey::.ctor` with
   `ConstructorInfo.Invoke` followed by a cast. That depends on
   runtime reflection, array boxing and a constructor signature lookup,
   none of which was present in the original method.
4. The full replacement reproduced some *observed* logic, but its
   equivalence was never demonstrated by controlled replay of Store,
   filtering, presets, wardrobe and selection.

**Most likely architectural cause** is injecting a sleep into a
non-yieldable synchronous method, with reflection changes as additional
credible failure paths. Without an exception stack or further isolated
tests it would be misleading to label a single instruction proven guilty.

## Changes made and enforceable stop conditions

- The withdrawn managed replacement **must not be reintroduced**.
- `tools/verify_hair_ui_contract.py` verifies the matching original
  user-owned UI.dll as a whole and all eight relevant method IL hashes
  before it considers call-site information reliable. All unknown
  builds are rejected. The original DLL is never uploaded to CI.
- `tests/test_verify_hair_ui_contract.py` uses synthetic PE images and
  verifies the retired research class contains no `ReplaceMethod`,
  `PatchAll`, `ReplaceIL`, or `Simulator.Sleep` invocation.
- Keep `APEX_ENABLE_UNVERIFIED_TS3_MONO_ICALLS` disabled.
- Never tell the user to reinstall artifacts from the rejected build.

## Next functional implementation

Change **the parent scheduling strategy**, not the inner append
method. First reconstruct the original two enumerator loops into a
persistent state machine that preserves part/preset iteration, Store
filtering, selection and final notifications. A verified simulator
task/tick continuation would process a bounded number of originals in
their existing order; cancellation must invalidate obsolete category
work. The existing `ApexCasSchedule::HairGridSession` can govern
those slices once a **verified** UI/Mono bridge exists.

The runtime bridge **remains unimplemented**. A standalone native C++
scheduler cannot make a managed synchronous UI method yield. An
additional test package must **not** be issued until it can preserve
the original method and the design can fail closed on incompatible
runtime conditions. Permanent Apex builds remain native-only, with no
MonoPatcher dependency or binary.

## Native integration finding from the user's TS3.exe

The user also provided `TS3.exe` (EA 1.69; SHA-256
`7352dd6e599f4475f812bbca19bbc7e5d8e84bbd5b6d21acaac9b1c5236adfe4`).
An offline, read-only inspection of its PE sections found:

- Image base `0x00400000`, x86 PE32, `.text` RVA `0x1000`,
  virtual size `0xBDD73A` = **12,441,402 bytes**.
- The on-disk `.text` has approximately **8.000 bits/byte**
  Shannon entropy (a strong sign of packed/obfuscated/encrypted
  code rather than readable original instructions). Representative
  entries at the MonoPatcher upstream EA 1.69 JIT RVA `0xF04B0`
  and the in-memory candidate `0xA826A0` both decode to
  implausible x86 instructions in the **disk file**.
- The upstream `mono_generate_code` and JIT check byte
  signatures from MonoPatcher's `Addresses.cpp` have **no
  corresponding matches in the on-disk executable**.
- An earlier **live memory** log from the user's running game
  reported a unique, plausible function prologue at
  `RVA 0xA826A0`, but it was **not identified as the Mono JIT
  method compiler** and its calling convention remains unverified.

**Implication:** do not extrapolate JIT function entry points or
vtable layouts from this obfuscated on-disk file. Any native JIT
integration must verify the actual loaded/deobfuscated image,
method identity, ABI and lifecycle, and must decline to hook when
not proven. On-disk signatures for upstream 1.69 builds are not
automatically valid for this player's executable. This is a
technical block, not grounds to force-enable the unsafe Mono ICall
switches or ask for repetitive log captures.

## C++ phase-level implementation added

`features/cas_hair_population_plan.h` now implements a staged,
original-order native **planning core**:
`FeaturedStore → PartGroups → Finalize → Finished`. A whole CAS
part (default hair plus its optional saved presets and selection
side effects) is one indivisible work unit: it avoids invalidating
`ObjectDesigner.SetCASPart` state midway through a group.

The game UI bridge is **not implemented**. The driver retains only
count/index and generation, never managed objects; rejects calls
from a different thread, prevents duplicate retries after possible
partial mutation, and cancels obsolete work when the Sim/category
context changes. It does not call `Simulator.Sleep` anywhere. A
time limit applies **between** completed part groups, not inside
the original game UI operations. Because one very expensive part
can still exceed the time budget, there is no guaranteed per-frame
latency limit yet.

`tests/test_cas_hair_population_plan.cpp` tests store-before-parts
ordering, atomic preset groups, no-commit retry, partial-commit
abort, stale generation cancellation, thread ownership and terminal
updates. These tests validate *only the C++ task state machine*,
not its eventual game integration or actual speedup.
