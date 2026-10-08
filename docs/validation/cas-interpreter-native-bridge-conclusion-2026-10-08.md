# EA 1.69 CAS Hair/Hats: native Mono interpreter integration decision

Date: 2026-10-08. Research branch only. Not a playable Hair/Hats speedup.

## Decision: stop searching for a conventional Mono JIT entry

Independent, publicly documented research into the older embedded Mono
runtime used by The Sims 3 shows that its gameplay/CAS managed methods
run through a MINT interpreter. The meaningful method-level objects
are `MonoMethod` (method identity/IL metadata) and an interpreter
`RuntimeMethod` (the translated representation), **not necessarily a
native x86 JIT function pointer for every C# method**.

External research references, for architecture ONLY, without copying
or importing their GPLv3 implementations:
- https://github.com/sims3fiend/Sims3MonoModder/blob/main/src/core/mono_types.h
- https://github.com/sims3fiend/Sims3MonoModder/blob/main/src/core/addresses.cpp
- https://github.com/sims3fiend/Sims3MonoModder/blob/main/src/core/mono_bridge.h
- https://www.mono-project.com/docs/advanced/runtime/
- https://github.com/LazyDuchess/MonoPatcher (historical comparison only)

The external Sims3MonoModder implementation uses GPLv3, so **do not
copy its source or ship any of its components in MIT-licensed Apex**.
Apex must implement any runtime integration independently.

## Important correction to previous diagnostic interpretation

The bytes `81 EC 08 08 00 00 53 55 8B AC 24 14 08`
are independently identified in public TS3 Mono research as the entry
of `mono_lookup_internal_call` on Steam 1.67 (legacy VA `0x00E82680`).

The player's EA 1.69 loaded TS3.exe logs find one occurrence of this
pattern at **RVA 0xA826A0**; four CALL callers pass one pointer and save
the returned EAX at `[pointer + 0x20]`. Older TS3 `MonoMethod`
research identifies `+0x20` as a union holding the native ICALL
function pointer for internal-call methods or cached header data for
IL methods. Therefore the evidence is **consistent with an internal
call resolver**, NOT a four-argument `mono_generate_code` and not
necessarily a method-header loader.

Prior assertions that this was 'not mono_lookup_internal_call' were
premature and have been corrected in the Apex diagnostic/source
comments. A likely identity is not automatic approval of x86 Detours
hooks: calls, branch boundaries, live target and ABI interactions must
be independently verified first. The old resolver *remains disabled*
on all builds, including developer configurations.

Also confirmed in the player's earlier live logs:
- `MonoDomainFree`: unique live function VA `0x00E75390`.
- `MonoTypeGetObject`: unique live function VA `0x00EA8A70`.
These are already useful live runtime reference points, so the user
does NOT need to repeat the four earlier full-image scans.

## Method identity: correct target, independent guard

From the user's unmodified original `UI.dll`:
- Owning image: `UI.dll` (SHA256
  `c78716f1eb0191f35b12eb8dfa4b47ef1bc1e22edcf88234eb633569074dec10`).
- Namespace: `Sims3.UI.CAS`
- Class: `CASHair`
- Method: `PopulateTypesGrid(bool)`
- MethodDef token: `0x06001918`, original IL size 1,621 bytes.
- `AddHairTypeGridItem` is separate token `0x0600191B` and is
  **not** a yield-safe interception target.

The new Apex-only `features/ts3_mono_method_identity.h` validates
an independently supplied candidate x86 `MonoMethod` pointer via
read-only, bounded memory reads. It checks token, method name,
owning class/namespace, UI.dll image and one-argument signature.
It never invokes Mono or dereferences arbitrary candidate memory
without a safe-read callback, does not hook/transform the method and
does not treat a runtimeMethod pointer as executable machine code.
Its synthetic regression tests contain both accepted and rejected
fixtures; they prove the guard, not EA runtime compatibility.

A candidate should eventually be obtained from verified class/method
lookup or enumeration *on the simulation thread*, not from raw heap
searches or the `mono_lookup_internal_call` return value.
External TS3 research demonstrates a possible route:
`MonoScriptHost::FindClass` (or a known loaded `MonoImage`) →
`mono_class_get_methods` / get-by-name → strict identity checks →
inspect `MonoMethod+0x14` interpreter `RuntimeMethod`.
Each native resolver entry and ABI must still be validated on EA
before calling; unknown build/owner/identity causes **no mutation**.

## Why resolving the method is necessary but not sufficient

The original `CASHair.PopulateTypesGrid(bool)` contains two managed
enumeration/finally regions, sequential Store items and grouped hair
part/presets, and terminal updates. It is synchronous. A method
interception that calls `Simulator.Sleep(0)` in
`AddHairTypeGridItem` breaks the UI contract: the prior managed
experiment made all Hair/Hats disappear. Never repeat that.

A working independent native implementation will need:
1. A verified method identity and same-simulator-thread bridge.
2. A safe way to retain managed source enumerators or reconstruct the
   parent sequence as atomic Store and part/preset work across ticks,
   preserving exception/finally behavior and GC ownership.
3. Original `AddHairTypeGridItem`, `UIManager`, thumbnail ownership,
   selection, filters, presets and terminal grid notifications,
   with no yield within any original atomic part group.
4. Lifecycle generation/cancellation (already implemented in
   `cas_hair_population_plan.h`), plus a rollback/fallback to the
   original method for unknown identities, mods, ABI or reentry.
5. Only after that, native WIN32 build and one controlled in-game
   cold/warm Hair ↔ Hats correctness and timing comparison.

An ICall resolver alone can profile the native thumbnails or metadata
that original managed code requests; it **cannot** make the managed
`PopulateTypesGrid` loop incremental. Likewise an interpreter
`RuntimeMethod` pointer is not a conventional x86 JIT entry that can
be blindly detoured.

## Delivery restrictions and stop criteria

- Only ApexRadiance.asi. No MonoPatcher, no S3MM dependency, no core
  UI.dll/gameplay.package replacement, no copied GPL source.
- No hook on the old `0xA826A0` candidate as if it were JIT.
- No script UI replacement or `Simulator.Sleep` inside append.
- Do not distribute another gameplay 'optimization' build until the
  native bridge is implemented and compatibility is independently
  validated.
- Do not ask the user to repeat previous xref logs. Actual in-game
  execution/visual checks cannot be simulated by GitHub CI; a single
  final game test remains necessary to claim any real speedup.


## Smooth Patch comparison — why it is useful and where it stops

Primary developer description and public release:
https://modthesims.info/d/658759/smooth-patch-2-1.html
https://www.patreon.com/lazyduchess/posts/ts3-smooth-patch-67835239
https://www.patreon.com/lazyduchess/posts/ts3-smooth-patch-72964777

**Two different components must not be conflated:**

- Native TS3Patch.asi (historically TS3FrameratePatch): changes simulation
  timing / sleeps (TPS), general UI responsiveness and frame pacing.
  It does **not** identify or split the managed CASHair.PopulateTypesGrid
  loop. The Apex reference `docs/engine/timers-and-sleeps.md` already
  documents the shared sleep wrapper and overlapping locations. Apex
  must NOT install a second competing timer/TPS patch.
- `ld_SmoothPatch.package` (2.x): a managed CAS behavior modification.
  The author's description states that **clothes** populate on scroll
  instead of staggered; it also unlocks CASt pattern controls while
  loading. In released notes it does not establish the same behavior
  specifically for `CASHair.PopulateTypesGrid(bool)` / Hair/Hats.
  Prior compatibility defects included absent hairstyles with NRaas
  MasterController; subsequent fixes and limitations were published.
  This is important negative evidence: do not assume a clothing
  optimization can simply be applied to hair/preset groups.

**Lesson for our method, not copied code:** Prefer the original
`ItemGrid.BeginPopulating` / `OnPopulateTick` lifecycle already used by
clothing; determine whether Hair/Hats can schedule its **original
complete part+presets operation** as a grid task on the verified
simulation/UI thread. Do not reorder hair entries or make
`AddHairTypeGridItem` yield; that broke Hair/Hats in the earlier
experiment. Preserve MasterController overrides and vanilla fallback.

A native-only Apex still needs a verified managed-method bridge and
an independently written adapter. The Smooth Patch 2.x package is NOT
a drop-in Apex ASI library. Study its user-visible behavior and
compatibility reports as a reference, not an assumption that
Hair/Hats is already solved.

As of 2026-01-26 the MTS listing marks Smooth Patch 2.1
unsupported and suggests Sims3SettingsSetter for supported native TPS
features. S3SS offers its own Smooth Patch variants; this also argues
against layering another global tick-rate hook in Apex.


## New: strict native discovery adapter and corrected signature

The native-only research branch adds:
- `features/ts3_cas_mono_discovery.h`: original `FindClass` and
  `mono_class_get_methods` adapter expressed as injected callbacks,
  with explicit enabled/simulation-thread/ABI/UI-assembly gates,
  bounded enumeration, duplicate-token detection and exact method
  ownership check. Opaque iterator cookies are not treated as sorted
  integers. The adapter's output is a **MonoMethod pointer**; it
  neither invokes that method nor assumes a callable JIT entry.
- `features/ts3_mono_method_identity.h`: corrected
  `MonoMethodSignature.param_count` width from uint32 to **uint16**
  at `+0x04`, and now validates the actual managed signature
  **instance void(bool)** via return MonoType `0x01`, parameter
  MonoType `0x02`, and a non-static method attribute check.
  The shorter, NUL-terminated strings are read byte-by-byte instead of
  requiring a completely accessible 256-byte page span.
- `tests/test_ts3_cas_mono_discovery.cpp`: synthetic successes,
  negative gates, wrong ABI metadata, foreign declaring class, duplicate
  method, malformed iterator, opaque nonmonotonic iterator and bound
  exhaustion. Neither those fixtures nor GitHub CI prove EA 1.69
  runtime behavior.

Do not present this as a live bridge yet: the adapter is deliberately
NOT wired to unverified Mono entry points. To attach it in production,
first establish trustworthy EA 1.69 native ABI and lifetime for
`MonoScriptHost::FindClass` and `mono_class_get_methods`, and a
simulation-thread dispatch; only then consider an independently
written Hair/Hats parent-method adapter.

## S3IO research, user-suggested 2026-10-08

- Official mod page:
  https://modthesims.info/d/700387/s3io-in-game-file-access-api-for-script-modders.html
- Public source:
  https://github.com/brando130/S3IO

S3IO uses two explicit components: managed `S3IO.package` and native
`S3IO.asi`, handshaking through a `S3IO_IPC` buffer placed via
`Marshal.AllocHGlobal`. The native module uses `VirtualQuery`
to locate the buffer; managed callers use `Simulator.Sleep` while
waiting for filesystem operations, not during synchronous CAS grid
construction. This is evidence for robust cross-component IPC and
late-start handling, **not** for a MonoMethod resolver or an
incremental `CASHair.PopulateTypesGrid` replacement. No `System.IO`
is needed by the Apex native module, which already has Win32 filesystem
APIs.

The user requires one final `ApexRadiance.asi` with NO mandatory
`.package`; therefore no S3IO runtime dependency or S3IO-derived
IPC is installed. Do not copy S3IO's `Simulator.Sleep(0)` into
`AddHairTypeGridItem`: this caused disappearing Hair/Hats in
the earlier managed experiment.
