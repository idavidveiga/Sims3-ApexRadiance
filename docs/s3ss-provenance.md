# Provenance: what Apex Radiance took from Sims3SettingsSetter

> An audit of what Apex Radiance used in the past, and still uses, from **Sims3SettingsSetter (S3SS) by sims3fiend and
> contributors**. It covers code, design, game knowledge, runtime behaviour and licensing. It aims to be factual: it
> should neither downplay nor overstate what came from S3SS.
> Audit date: 2026-09-29. Method, numbers and scripts are in section 9 ("How to verify").

**Sources audited**

| What | Where | Version |
|---|---|---|
| Official S3SS | `%USERPROFILE%\Desktop\S3SS-dev\S3SS-official\` | upstream `origin/main` at `5eb2c65` (S3SS 1.6.3). Every file is byte-identical to that commit (CRLF aside) |
| The old combined build (fork of S3SS) | `%USERPROFILE%\Desktop\S3SS-dev\Sims3SettingsSetter\`, branch `night-remake` | upstream history, then 3 fork commits by the maintainer: `b84d5f1` (v0.1.0), `f18cca8` (v0.2.0), `45e36e2` (tag `combined-final`) |
| Apex Radiance (standalone) | this repository | `HEAD` `20dd641` (1.2.0) **plus the uncommitted working tree** of 2026-09-29 00:56: 30 modified and 10 untracked files, from another agent's ongoing work. That is 92 `.cpp`/`.h` files outside `third_party/`. The first commit, `dd54591` (1.0.0), was audited too |

---

## 1. Summary

Apex Radiance started as a fork of S3SS. Its first two public releases, v0.1.0 ("Night Remake") and v0.2.0 ("Sims3
Settings Setter Apex Edition"), **were** S3SS 1.6.3 with additions. They compiled every S3SS source file (v0.2.0
dropped only the Split-Level Lighting Fix) and shipped S3SS's framework, its 25 patches, its menu and its config.
v0.1.0 even kept S3SS's file name, `Sims3SettingsSetter.asi`.

The standalone Apex Radiance (1.0.0 and later) contains **no S3SS source file**, and a token-level comparison finds
**no copied implementation**:
- 2.6 % of Apex's 8-token sequences also occur in S3SS. For unrelated code that uses the same libraries (Detours
  samples, ImGui backends) the figure is 1.1 %.
- Outside one header, no identical stretch is longer than 43 tokens (a struct's field list).

That is **not the same as "nothing from S3SS"**:
- The new framework was written by AI coding agents working for the maintainer. They had the S3SS source at hand, and
  their brief (`PLANO-SEPARACAO.md`) was written from it. They **kept S3SS's programming interface on purpose**, so the
  fork-era feature code would carry over with renames only.
- The clearest trace is `framework/d3d9_hooks.h`. Its 15 `Register*` declarations and `UnregisterAll` are **verbatim
  S3SS** (a 332-token identical run). It also keeps S3SS's priority values and its Continue/Skip/Block semantics.
- The patch base class, the metadata struct, the `GameVersion` enum, the memory-patch helpers and the Detours batch
  helpers keep S3SS's member names, fields and signatures, over new implementations.
- The architecture as a whole follows S3SS's design.
- Some game knowledge came from S3SS's source:
  - the Split-Level technique (GetLotID returns 0, credited in S3SS to Arro);
  - the string-table layout of `TS3W.exe` for the localized Documents folder;
  - two build timestamps;
  - a few engine function names;
  - two byte signatures, which are identical.
- At run time, Apex Radiance runs **beside** an unmodified official S3SS and executes none of its code. It detects S3SS,
  reads `S3SS.toml`, defers to S3SS where both mods would touch the same thing, and recommends it.

| Item | Used in the past (v0.1.0 / v0.2.0 fork) | Still used today (Apex Radiance 1.x) | Not used |
|---|---|---|---|
| S3SS source code compiled into the binary | **Yes, all of it.** v0.1.0: 43/43 translation units; v0.2.0: 42/43 | **No.** No S3SS file. Two verbatim traces remain, both interface-level (the next two rows) | |
| Framework implementation (D3D bootstrap, hook registry, patch system, memory helpers, logger, config, menu) | S3SS's own, with small fork edits | **No.** Rewritten; no copied implementation text found (section 3) | |
| Framework interface (names, signatures, fields) | S3SS's | **Partly.** `D3D9Hooks` registration API verbatim; `ApexPatch` / `FeatureInfo` / `GameVersion` / `MemPatch` / `DetourBatch` keep S3SS's member names and signatures under new type names (sections 3.3, 4) | |
| Framework design (registry with priorities and Skip, registration macro plus metadata, per-patch TOML, ImGui overlay, Detours batches, byte-checked patching, PE-timestamp versions, debounced reinstall) | S3SS's | **Yes, derived.** Each item has since been changed or extended (section 4) | |
| S3SS's 25 patches (performance, QoL, crash logs, and so on) | **Shipped** (v0.2.0 without Split-Level) | **None shipped.** Two ideas re-implemented in new code: Split-Level (as Every-Story Ground Light) and Borderless window | Every other patch |
| S3SS's menu and UI code | Shipped (S3SS menu plus Apex tabs) | **No.** New Violet UI. Only the "ImGui overlay in the game's EndScene" concept is shared | |
| S3SS's config file | Apex settings were stored in `S3SS.toml` | **Read-only:** detection of S3SS's settings, plus a one-time migration of Apex's own tables | Apex never writes it |
| Game knowledge first seen in S3SS's source | Everything S3SS's patches do | **Some** (section 6): GetLotID technique, localized folder technique, 2 timestamps, a few names, 2 signatures (1 dev-only) | Most of Apex's addresses are its own RE |
| Coexistence with S3SS at run time | n/a (it *was* S3SS) | **Yes:** detection, deference, recommendation card, migration. No S3SS code is called (section 7) | |
| S3SS modules | Shipped | **No** | `pattern_scan`, `vtable_manager`, settings hooks, `config_value_manager`, INI migration, memory statistics, CPU optimisation / CPUID fix, mimalloc allocator, `qol` (UISettings, MemoryMonitor), Resolution Spoofer |

---

## 2. History: what the fork releases shipped

### 2.1 Timeline

| Date | Commit | What |
|---|---|---|
| 2026-07-22 | `5eb2c65` (sims3fiend) | S3SS 1.6.3, the merge-base of the fork |
| 2026-09-27 | `b84d5f1` | "Night Remake alpha". Release `nightremake-v0.1.0-alpha`, asset **`Sims3SettingsSetter.asi`** |
| 2026-09-28 | `f18cca8` | "Sims3 Settings Setter Apex Edition v0.2.0-alpha". Release `apex-v0.2.0-alpha`, asset `S3SSApex.asi`. This was `fork/main` |
| 2026-09-28 | `45e36e2` | `combined-final`: the last state of the combined build (local tag, never released) |
| 2026-09-28 | `dd54591` | Apex Radiance 1.0.0: the standalone, in its own repository `loinyx/Sims3-ApexRadiance` (public, MIT) |
| 2026-09-28/29 | `266c4fd`, `7512168`, `20dd641` | Releases 1.1.0, 1.1.1, 1.2.0 |

The fork has exactly three commits after upstream, all by the maintainer (`git log 5eb2c65..45e36e2`). Everything
before `5eb2c65` is upstream history: 133 commits by sims3fiend (91), Harry Gillanders (25), swiffy (10) and Nahuel
Rocchetti / LazyDuchess (7).

On 2026-09-29 the fork repository `loinyx/Sims3SettingsSetter-Apex` no longer resolves. `gh repo view` fails, and it
is not in `gh repo list loinyx`. So the v0.1.0 and v0.2.0 releases are no longer hosted there. Copies downloaded
earlier may still exist.

### 2.2 S3SS parts compiled into the releases

This comes from the `ClCompile` lists of `Sims3SettingsSetter.vcxproj` at each commit (section 9, command 2).

- **Upstream `5eb2c65`: 43 compiled files.**
  - Framework: `dllmain`, `d3d9_hook`, `d3d9_hook_registry`, `optimization`, `hooks`, `settings`, `qol`, `gui`,
    `logger`, `pattern_scan`, `vtable_manager`, `memory_statistics`, `cpu_optimization`, `allocator_hook`, and
    `config/*` (`config_paths`, `config_store`, `config_value_manager`, `migration`).
  - Header-only framework: `patch_system.h`, `patch_helpers.h`, `patch_settings.h`, `utils.h`, `version.h`.
  - 25 patches: Adaptive Wait, Animation Blend, Brady Bunch Begone, CPU optimization, CreateFileW, Expanded Crash
    Logs, GC Finalize Throttle, GC Stop World, GC Try To Collect, Lighting Quality, Lot Streaming Optimizations,
    mimalloc, Mirror Settings, Online Nuke, Oversized Thread Stack Fix, RefPack Decompressor, Resolution Spoofer,
    Smooth Patch Classic, Smooth Patch Precise, Split-Level Lighting Fix, Startup Warning Dialog Fix, Timer
    Optimization, Uncompressed Compositor, Uncompressed Sim Textures, WorldCache Uncap.
  - Upstream is 13,841 lines of `.cpp`/`.h`.
- **v0.1.0 (`b84d5f1`): all 43 upstream files, plus 15 new ones (58 in total).**
  - The S3SS loader, D3D9 hooks, registry, patch system, menu (window title unchanged), settings hooks, `S3SS.toml`
    config, log (`S3SS_LOG.txt`) and all 25 patches were included.
  - Upstream files edited: `d3d9_hook.cpp` (+9 lines: `RenderCallbacks` fire points), `d3d9_hook_registry.cpp`
    (32 lines: a recursive mutex), the vcxproj, `README.md` and `.gitignore`.
  - Additions: +11,531 lines.
  - The README called it an "Unofficial fork", credited "sims3fiend and contributors (loader, hook system, menu, all
    original features)" and kept S3SS's README below.
- **v0.2.0 (`f18cca8`): 42 upstream files** (`split_level_lighting_fix_patch.cpp` was removed from the build), plus
  new ones.
  - Upstream files edited: `gui.cpp` (Apex and Display tabs, the title became "Sims3 Settings Setter Apex Edition
    (v1.6.3)"), `config/config_store.cpp` (HdrOutput save/load), `d3d9_hook.cpp`, the registry (`CallOriginal*`
    additions) and the vcxproj/filters.
  - The README still said "Everything from the original Settings Setter ... is unchanged". That was no longer exactly
    true: Split-Level was removed, Borderless had moved to the Display tab, and upstream files were edited.
- **`combined-final` (`45e36e2`, not released):** as v0.2.0, plus edits to:
  - `logger.cpp` (buffered rewrite);
  - `patches/smooth_patch_precise.cpp` (+705/-374);
  - `patches/gc_try_to_collect_patch.cpp`;
  - `dllmain.cpp`.

### 2.3 What the fork added in its own files

50 source files (`.cpp`/`.h`/`.hlsl` outside `third_party/`) were added by the fork commits
(`git log --diff-filter=A`):
- 36 in `b84d5f1`, 7 in `f18cca8`, 7 in `45e36e2`.
- All are by the maintainer. None exists in upstream history.

They were **written against S3SS's APIs**. The counts below are lines naming an S3SS API in each file at
`combined-final`:

| File | S3SS APIs referenced |
|---|---|
| `patches/night_terrain_relight_patch.cpp` | 58 lines: `PatchHelper::` ×23, `SettingUIType` ×10, `OptimizationPatch`, `SAFE_IMGUI_BEGIN`, `LOG_*` |
| `frame_profiler.cpp` | 47 lines: `HookResult` ×16, `PatchHelper::` ×9, `ConfigPaths`, `g_gameVersion` |
| `lot_light_bridge.cpp` | 45 lines: `HookResult` ×23, `D3D9Hooks::Register*`, `GetS3SSDirectory` |
| `object_light_bridge.cpp` | 24 lines: `PatchHelper::` ×17 |
| `patches/depth_blur_patch.cpp` | 18 lines: `SettingUIType`, `OptimizationPatch`, `g_pd3dDevice` |
| `d3d9_extra_hooks.cpp` | 5 lines: `DetourHelper::`, `LOG_*` |

They did **not** copy S3SS code. They match S3SS at the same level as unrelated code:
- 2.58 % raw 8-token containment;
- longest identical run 29 tokens;
- 209 of 12,890 eligible lines (section 3.1).

---

## 3. Current code: token and line similarity against S3SS

### 3.1 Method (details and commands in section 9)

- **Corpora.**
  - **A** = the 92 Apex `.cpp`/`.h` files outside `third_party/` (working-tree snapshot).
  - **B** = the 67 S3SS `.cpp`/`.h` files at `5eb2c65`.
  - **Control** = 87 files of unrelated code that uses the same libraries: Microsoft Detours `src/` and `samples/`, and
    ImGui's `imgui_impl_dx9.cpp`, `imgui_impl_win32.cpp`, `imgui_demo.cpp` and `imgui_widgets.cpp`.
- **Tokenizer.** C++ tokens. Comments, `#include` and `#pragma once` are dropped.
  - *raw* mode keeps identifiers and literals.
  - *norm* mode maps every non-keyword identifier to `I`, numbers to `N` and strings to `S`. This catches copies with
    renamed identifiers.
- **Metrics.**
  - *Containment*: the share of A's k-token shingles (k = 8 and 16) found anywhere in B.
  - *Longest aligned run*: the longest stretch of consecutive tokens that is identical, at the same relative
    alignment, in one B file. This is a real copied stretch, not scattered hits.
  - *Exact lines*: lines of 25 characters or more after trimming, excluding `#include`, `#pragma once` and brace-only
    lines.
- **Positive controls**, to show the method detects copying:
  - 7 upstream files as the fork left them in `combined-final` (really copied, then edited);
  - 6 upstream files with every identifier renamed by script (`rename.pl`).

### 3.2 Corpus-level results

| Comparison | raw k=8 | norm k=8 | raw k=16 | norm k=16 | exact lines |
|---|---|---|---|---|---|
| **Apex vs S3SS** | **2.61 %** | 31.82 % | **0.35 %** | 4.21 % | **234 / 17,772 (1.32 %)** |
| Apex vs control (unrelated code, 5.8× larger) | 1.10 % | 33.54 % | 0.02 % | 4.32 % | 107 / 17,772 (0.60 %) |
| S3SS vs control | 1.54 % | 32.29 % | 0.08 % | 3.80 % | 60 / 6,980 (0.86 %) |
| Apex 1.0.0 (`dd54591`, 79 files) vs S3SS | 2.84 % | 32.42 % | | | 195 / 13,682 (1.43 %) |
| Fork-authored combined files (47) vs S3SS | 2.58 % | 31.71 % | | | 209 / 12,890 (1.62 %) |
| *Positive control:* fork-edited upstream files vs S3SS | 45–97 % per file | 67–99 % | | | |
| *Positive control:* S3SS files with every identifier renamed | 0.7–4.5 % | **97.6–100 %** (aligned runs 472–3,769 tokens) | | | |

**Reading the table.**
- *norm k=8* is noise: Apex is as "similar" to Detours and ImGui (33.5 %) as to S3SS (31.8 %). Short normalized
  sequences are just C++ syntax.
- A renamed copy would show up at **97-100 % norm** with aligned runs of hundreds or thousands of tokens. No Apex file
  does. The single exception is the interface header below, at 73 % norm and 334 tokens.
- *raw* is a little higher against S3SS than against the control: 2.6 % vs 1.1 %, and 0.35 % vs 0.02 % at k=16. The
  excess is fully explained by the interface names and boilerplate in 3.3 and the two byte arrays in section 6.

### 3.3 Per-file: the files above a small threshold

These are the Apex files with raw k=8 containment in S3SS of 10 % or more. The control column is the same measure
against the unrelated corpus. "Run" is the longest aligned raw run, in tokens.

| Apex file | tokens | raw in S3SS | raw in control | run | What matches |
|---|---|---|---|---|---|
| `framework/d3d9_hooks.h` | 854 | **53.8 %** | 0.0 % | **332** | S3SS `d3d9_hook_registry.h:84-101`: the 15 `Register*` declarations and `UnregisterAll`, **verbatim**. The same `Priority` values (`First = 0, Early = 25, Normal = 50, Late = 75, Last = 100`, S3SS `:12-18`). The 15 `*Hook` type aliases with the same names and parameter types (S3SS parameter names dropped). `CallOriginalCreateRenderTarget/SetRenderTarget/SetViewport` with the same names |
| `framework/memory_patch.h` | 303 | 24.7 % | 0.0 % | 23 | `bool WriteBytes(uintptr_t address, const std::vector<BYTE>& bytes, std::vector<PatchLocation>* ...` (S3SS `patch_helpers.h:134`). The `GameAddress` fields `addresses / pattern / patternOffset / expectedBytes` (S3SS `AddressInfo`, `:278-288`) |
| `framework/patch_base.h` | 1,087 | 19.6 % | 0.0 % | 43 | `FeatureInfo`'s fields are S3SS `PatchMetadata`'s (`patch_system.h:86-93`), same order, names and defaults. `ApexPatch` members match `OptimizationPatch` (`optimization.h:24-41, 148-193`): `SETTING_CHANGE_DEBOUNCE = 2 s`, `virtual bool Install() = 0` / `Uninstall`, `GetName()`, `IsEnabled()`, `GetLastError()`, `IsCompatibleWithCurrentVersion()`, `std::atomic<bool> isEnabled{false}`, the `settings` vector, `Register*Setting(..., presets = {})` |
| `framework/game_version.h` | 125 | 17.0 % | 0.0 % | 27 | The `GameVersion` enum: `Retail = 0, Steam = 1, EA = 2, EA_1_69_43 = 3, Unknown = 255` (S3SS `patch_system.h:11-17`), same names, values and order |
| `features/borderless.h` | 138 | 16.8 % | 0.8 % | 23 | `void SaveToToml(toml::table& root); void LoadFromToml(const toml::table& root);`: generic declarations |
| `patches/performance_patches.cpp` | 616 | 16.4 % | 0.3 % | 24 | Patch-class boilerplate: `bool Install() override { if (isEnabled) return true; lastError.clear(); ...`, `.category = "Performance"` |
| `patches/split_level_ground_light_patch.cpp` | 859 | 12.3 % | 0.7 % | 28 | The 13 original game bytes of GetLotID, `{0x8B,0x81,0xC0,0,0,0,0x8B,0x91,0xC4,0,0,0,0xC3}` (S3SS `split_level_lighting_fix_patch.cpp:18`), plus patch boilerplate. See section 6 |
| `framework/memory_patch.cpp` | 2,373 | 10.9 % | 1.1 % | 25 | `bool InstallHooks(const std::vector<Hook>& hooks) { if (hooks.empty()) return true;` and `RemoveHooks` (S3SS `patch_helpers.h:532-557`). Same control flow as S3SS's `DetourHelper`, which is also the standard Detours idiom |
| `features/frame_profiler.h` | 186 | 10.6 % | 0.0 % | 13 | `void LoadFromToml(const toml::table& qolTable);` (S3SS `qol.h` parameter name) |
| `features/picture.h` | 379 | 10.2 % | 0.0 % | 25 | `SaveToToml(toml::table& qolTable) const; LoadFromToml(const toml::table& qolTable);`, the same pattern as S3SS's QoL classes |

Every other Apex file is below 10 %, with runs of 31 tokens or less.
- The framework files: `d3d9_hooks.cpp` 5.0 %, `overlay.cpp` 9.0 %, `patch_base.cpp` 8.9 %, `apex_config.cpp` 6.3 %,
  `d3d9_bootstrap.cpp` 5.5 %, `apex_log.cpp` 4.0 %, `apex_paths.cpp` 1.3 %, `apex_gui.cpp` 1.6 %.
- `apex_gui.cpp` is more similar to the ImGui control (3.3 %) than to S3SS.
- The full table is `research\provenance\apex_file_table.txt`.

The next-longest raw runs after the 332-token one:
- 43 tokens: the `FeatureInfo` fields.
- 31: a `std::sort` comparator lambda.
- 29: a virtual-key name table whose first entries are Insert/Delete/Home/End, in both mods.
- 28: `GetName`/`IsEnabled`.
- 28: the GetLotID bytes.
- 27: the `GameVersion` enum.
- 27: `io.DisplaySize = ImVec2(...)`.
- 27: `Uninstall()` boilerplate.

Normalized runs of 40 tokens or more (304 in total) were inspected. Apart from `d3d9_hooks.h` they are all
**structural false positives**:
- lists of `{ "name", value }` pairs or `f("key", q.field)` calls;
- a `D3DFORMAT` to string switch (`frame_capture_patch.cpp:58` vs S3SS `patch_helpers.h:727`, with a different set and
  order of formats);
- blocks of `using X_t = HRESULT(...)` typedefs dictated by the D3D9 API;
- `DetourAttach` lists.

The same kind of match reaches 261 tokens between Apex and the Detours disassembler tables.

**Implementations are different.** For example, S3SS's registry keeps 15 separate vectors, sorts them with `std::sort`
and has per-type `Execute*` functions. Apex's `d3d9_hooks.cpp` uses one templated `Chain` with copy-on-write lists, a
lock-free "nothing registered" test, stable ordering (equal priorities keep registration order), a recursive lock and
per-hook profiler timing. The raw similarity of the two `.cpp` files is 3.1 %.

### 3.4 Exact lines

234 of 17,772 eligible Apex lines (1.32 %) also occur in S3SS:
- **65** also occur in the unrelated control corpus: generic C++, Win32, Detours and ImGui lines.
- **About 82** more are generic idioms that only happen to be absent from the control:
  - 41 × `} __except (EXCEPTION_EXECUTE_HANDLER) {` and 9 × `} catch (const std::exception& e) {`;
  - `QueryPerformanceCounter(&now);`, `const uint64_t now = GetTickCount64();`;
  - `std::lock_guard<std::mutex> lock(m_mutex);`;
  - the ImGui backend calls (`ImGui_ImplDX9_NewFrame();` and similar);
  - the hex-digit parse line;
  - `if (DetourTransactionBegin() != NO_ERROR) {`.
- **About 87** are S3SS-specific interface lines and boilerplate (0.49 % of Apex's lines):
  - the 15 `Register*` declarations plus `UnregisterAll` (declaration and definition);
  - about 20 `ApexPatch` / `FeatureInfo` / settings member declarations (listed in 3.3);
  - 33 lines of the Install/Uninstall/RenderCustomUI boilerplate in 7 feature classes. It follows S3SS's
    `patches/_TEMPLATE.cpp` shape, and those classes were first written against `OptimizationPatch`;
  - 7 metadata initializer lines (`.supportedVersions = VERSION_ALL,` and similar);
  - `enum class GameVersion : uint8_t {`, `g_gameVersion = GameVersion::Unknown;`;
  - `pendingReinstall = false;`, `lastSettingChange = std::chrono::steady_clock::now();`;
  - the `#define SAFE_IMGUI_BEGIN()` macro name, with a different body.

No S3SS **comment** text was found in Apex. Every match is code. The full list is
`research\provenance\lines_apex_vs_s3ss.txt`.

### 3.5 Which S3SS files are most visible in Apex (reverse view)

This is the share of each S3SS file's raw 8-token shingles that occur anywhere in Apex:
- `d3d9_hook_registry.h`: 38.5 % (28.8 % at k=16), from the interface above.
- The small patch files, such as `_TEMPLATE.cpp` (26 %), `gc_try_to_collect_patch.cpp` (23 %) and
  `split_level_lighting_fix_patch.cpp` (23 %): from the patch boilerplate and the GetLotID bytes.
- `config/config_store.cpp`: 17 %, from generic toml++ and exception lines.
- Every other S3SS file: less.

### 3.6 Conclusion on code

- **Copied implementation:** none found.
- **Copied interface:** yes, and deliberate. Mainly `framework/d3d9_hooks.h`, plus the member, field and signature
  names listed in 3.3.
- These declarations are what let the fork-era feature files move over by renaming, for example:
  - `d3d9_extra_hooks.cpp` differs from its `combined-final` version only in two `#include`s and
    `DetourHelper` → `DetourBatch`;
  - the fork patches kept `Install`/`Uninstall`/`isEnabled`/`lastError`.
- The project notes call the framework "rewritten from scratch". That is accurate for the implementation, but it was
  **not a clean-room rewrite**: the interface was kept.

---

## 4. Design derived from S3SS (not code)

Every item below was **derived from S3SS's design**. The history is clear: Apex was built inside S3SS's framework, and
the standalone plan (`PLANO-SEPARACAO.md`, sections 1b and 4) lists each S3SS piece and how the standalone replaces it.

| Design element | In S3SS | In Apex Radiance today | How it differs now |
|---|---|---|---|
| **D3D9 hook registry with priorities and Continue/Skip/Block** | `D3D9Hooks` (`d3d9_hook_registry.*`), 15 device methods, priorities 0/25/50/75/100, `HookResult`. Upstream itself registers no hooks | `D3D9Hooks` (`framework/d3d9_hooks.*`), the same 15 methods, the same priorities and three results (`HookAction`), same `Register*` API | New implementation (3.3): stable order, copy-on-write lists, recursive lock, per-hook profiler timing, `Install`/`Uninstall` public, `CallOriginal*` also for DIP/DP/VS constants. `DeviceContext` holds only the device |
| **Patch registration macro plus metadata struct** | `REGISTER_PATCH(Class, {.displayName = ...})`, `PatchMetadata`, `PatchRegistry` / `PatchRegistrar` / `OptimizationManager`, base class `OptimizationPatch` | `APEX_REGISTER_FEATURE(Class, {...})`, `FeatureInfo` (S3SS's 7 fields plus `gameCodeGroup`), `PatchRegistration` / `PatchManager`, base class `ApexPatch` | Function-pointer factory instead of `std::function`. Adds `Fail()`, `RenderDeveloperUI`, `ApplyTableLive` (profiles), `UnavailableReason`, `GpuCostMs`. Drops S3SS's call sampling, maintained writes and `OnSettingsRefired`. Same 2 s debounced reinstall (`NotifySettingChanged` / `pendingReinstall`) |
| **Settings declared by the patch** | `patch_settings.h`: `SettingUIType {InputBox, Slider, Drag}`, `Register*Setting` with presets | `SettingWidget` (same three kinds), `Register{Float,Int,Bool,Enum}Setting`, class name `PatchSetting` kept | New implementation (`patch_base.cpp`, 8.9 % raw). The UI is drawn with Apex's own widgets |
| **Per-patch TOML sections** | `S3SS.toml`: `[patches.<Name>]` with `enabled` plus setting keys, and `[qol.*]` | `ApexRadiance.toml`: `[patches.<Name>]` with `enabled` plus keys. `[qol.picture]` and `[qol.frame_profiler]` keep the combined build's table names so old settings migrate | Own file and folder. Adds `[ui]`, `[meta]`, `[display]`, `Profiles\*.toml` |
| **ImGui overlay and menu** | ImGui DX9/Win32 drawn inside the game's EndScene; `SetWindowLongPtr` WndProc; bare Insert toggle; blocks all input while open; font scale; mouse and display scaling when the back buffer differs from the window | Same basic pattern (`framework/overlay.*`, `d3d9_bootstrap.*`) | Own ImGui context, `apex_radiance_imgui.ini` and `###ApexWindow`. WndProc installed at the first Present (outermost) with a capture-only policy. Ctrl+Shift+F11 chord, and bare Insert is refused as the Apex key. A completely different Violet sidebar/card UI (`apex_gui.cpp`, `ui/*`) |
| **Detours batching** | `DetourHelper::{Hook, InstallHooks, RemoveHooks}`, one transaction, abort on the first failure | `DetourBatch::{Hook{target, detour}, InstallHooks, RemoveHooks}`, the same control flow | Also the canonical Detours idiom. Apex adds `hook_chain.*` (logs whether a prologue was clean or already an `E9` into which module) and `slot_chain.*` (vtable slots) |
| **Memory patches with expected-bytes checks** | `PatchHelper`: `PatchLocation` undo list, `WriteBytes(addr, bytes, tracker, expectedOld)`, `WriteDWORD`, `RestoreAll`, `ValidateBytes`, `ScanPattern`; `AddressInfo` (per-version address, pattern, offset, expected bytes, `Resolve()`) | `MemPatch`: the same functions and parameter shapes; `GameAddress` (same fields; **declared but not used** by any feature) | Adds `WriteCodeSuspended` (a code write with other threads suspended). The real address system is new: `framework/game_addresses.*`, with 101 signatures of 10 kinds (`Sig`, `Multi`, `InRange`, `CallIn`, `LowestOf2`, `Deref`, `Target`, `CallersOf`, `LightType`, `SlotsOf`), cross-checks, and fixed Steam addresses with a signature self-check. S3SS has nothing like it |
| **Game version from the PE timestamp** | `patch_system.h`: `GameVersion` enum, `VERSION_TIMESTAMPS`, `DetectGameVersion`, `g_gameVersion`, `GameVersionMask` | `framework/game_version.*`: the same enum (verbatim), the same four timestamps, the same function names | 32-bit mask. `VERSION_EA` includes 1.69.43. `VERSION_ALL` means any build, including unknown ones. DOS/NT signatures are checked. Timestamp provenance is in section 6 |
| **Localized Documents folder** | `config_paths.cpp` `ResolveLocalizedGameFolder`: string-table blocks of 100 ids from 1000 in `TS3W.exe`, locale code at +0, folder name at +2, language-prefix fallback | `framework/apex_paths.cpp` `GameFolderName()`: the same technique and constants | New code (1.3 % raw). Adds a fallback to the English folder when the localized one is missing |
| **Update pump** | S3SS's message loop calls `OptimizationManager::Update` about every 10 ms | Apex's own pump thread, every 10 ms (`apex_main.cpp`) | Own thread |
| **Logger macros** | `LOG_DEBUG/INFO/WARNING/ERROR` → `Logger::Handler` | Same macro names → `ApexLog::Write` (buffered writer thread, `ApexRadiance_LOG.txt`) | New implementation (4.0 % raw). The buffered design was a fork-era change |
| **Borderless window** | `qol` `BorderlessWindow` (Disabled / Windowed / Fullscreen). `EnforceBorderlessWindowedParams`: `Windowed = TRUE`, refresh 0, `D3DSWAPEFFECT_DISCARD`, strip `LOCKABLE_BACKBUFFER` | `features/borderless.*` (new in the standalone): Off / Windowed / Fullscreen, the same four present-parameter edits | New code (1.3 % raw). Styles applied on the window's own thread; re-applied on `WM_STYLECHANGED` / `WM_WINDOWPOSCHANGED`. **Stays off when S3SS's borderless is configured** |
| **Config migration from `S3SS.toml`** | S3SS has an INI → TOML migration of its own (`config/migration.cpp`), which is unrelated | `ApexConfig::EnsureMigrated` (`apex_config.cpp`) | Apex-specific. It exists *because* official S3SS rebuilds `S3SS.toml` from its own sections only, dropping Apex's keys. It copies only Apex-owned tables (section 7) |

Not derived from S3SS:
- the `Direct3DCreate9` export bootstrap, designed to avoid racing S3SS's CreateDevice detour;
- `d3d9_extra_hooks` and `render_callbacks` (fork-authored, section 5);
- post-scene chains and the INTZ depth share;
- `game_addresses`;
- the shader patching;
- all of Night Lighting;
- Picture, Edge Smoothing and Depth Blur;
- the Frame Profiler;
- the dev tools;
- the Violet UI;
- `s3ss_detect` and the instance mutexes.

---

## 5. Files carried from the combined build: who wrote them

Each of the 92 current files was compared with every `.cpp`/`.h` of `combined-final`. Each best match was then traced
with `git log --diff-filter=A` (section 9, command 6).

**Carried from fork-authored files: 39 files, 62.5 % of Apex's tokens.** Every one of these was originally written in
the fork by the maintainer. None came from upstream S3SS.

| Current file(s) | Combined-build ancestor (raw similarity) | Added in | Written against S3SS APIs then? |
|---|---|---|---|
| `features/shader_patches.*` | `shader_patches.*` (98.5 %) | `b84d5f1` | No S3SS API |
| `features/light_probe.*`, `features/light_diag.*` | same names (95-100 %) | `b84d5f1` | Yes: `HookResult`, `D3D9Hooks::Register*`, `ConfigPaths::GetS3SSDirectory` |
| `framework/d3d9_extra_hooks.*` | same names (98.8 % / 100 %) | `b84d5f1` | Yes: `DetourHelper` (now `DetourBatch`, the only change besides includes) |
| `features/post_scene.*` | same names (93 %) | `b84d5f1` | Yes: `HookResult` |
| `features/level_light_share.*` | same (85 %) | `b84d5f1` | Yes: `PatchHelper::` |
| `features/lot_light_bridge.*` | same (74 %) | `b84d5f1` | Yes: `HookResult`, `D3D9Hooks::Register*`, `GetS3SSDirectory` |
| `features/rig_tracker.*`, `features/object_light_bridge.*` | same (68-100 %) | `b84d5f1` | Yes: `PatchHelper::`, `DetourHelper::` |
| `features/lightmap_smooth.*` | same (32-53 %) | `b84d5f1` | Barely (`LOG_*`) |
| `patches/frame_capture_patch.cpp`, `edge_smoothing_patch.cpp`, `night_terrain_relight_patch.cpp`, `depth_blur_patch.cpp` | same names (97 %, 80 %, 42 %, 41 %) | `b84d5f1` | Yes: `OptimizationPatch`, `SettingUIType`, `PatchHelper::`, `SAFE_IMGUI_BEGIN`, `g_pd3dDevice` |
| `features/map_view.*` | same (100 %) | `f18cca8` | `LOG_*` only |
| `features/picture.*` | `hdr_output.*` (71 % / 52 %) | `f18cca8` | Yes: `HookResult`, `ConfigStore`, `ConfigPaths`; it followed S3SS's QoL class shape (`qolTable`) |
| `features/frame_profiler.*` | same (79 %) | `45e36e2` | Yes: `HookResult` ×16, `PatchHelper::` ×9 |
| `build_flavor.h`, `features/depth_share.h`, `shaders/*` | same (100 %) | `b84d5f1` | No |

**Written for the standalone: 53 files, 37.5 % of tokens.** Each is under 30 % raw similarity to any combined-build
file.
- The framework: `apex_config.*`, `apex_gui.*`, `apex_main.cpp`, `apex_version.h`, `framework/*` except
  `d3d9_extra_hooks.*`, and `ui/*`.
- New features: `borderless.*`, `lot_lighting_motion.*`, `resource_cache.*`, `patches/performance*`,
  `patches/split_level_ground_light_patch.cpp`, `patches/night_lighting.h`.
- Their nearest combined-build files are S3SS's (for example `d3d9_hooks.cpp` ↔ `d3d9_hook_registry.cpp`, 3.6 %), with
  one exception: **`framework/d3d9_hooks.h`**, at 49.8 % against the combined `d3d9_hook_registry.h`. That is an
  upstream file, and the header is the interface described in 3.3.
- `framework/render_callbacks.h` was fork-authored (`b84d5f1`) but has been rewritten, with growable lists instead of
  `kSlots = 4` (4.8 % raw against anything in the combined build).

**No carried file contains S3SS API names any more.** A `grep` for `OptimizationPatch|OptimizationManager|REGISTER_PATCH|PatchHelper|DetourHelper|AddressInfo|PatchMetadata|SettingUIType|HookResult|ConfigStore|ConfigPaths|GetS3SSDirectory|UISettings|SettingsGui|g_pd3dDevice|PatchRegistry|PatchRegistrar`
over the Apex sources finds nothing.

Kept names with an S3SS prefix are the fork's own, not S3SS's: the `S3SS_PUBLIC` flavour macro, the `S3SS_TR` text
macro (both from `build_flavor.h`, `b84d5f1`), and the dev-tool headers "S3SS Light Probe" and "S3SS Light
Diagnostics".

---

## 6. Game knowledge: learned from S3SS or found independently

**Addresses.** Every hex constant in the `TS3W.exe` image range was extracted (section 9, command 7).
- S3SS source has 62 distinct literal addresses. It finds most of its targets by pattern.
- Apex has 323 in its source and 995 in its docs.
- **26 are shared.**
  - All 26 appear in Apex's **docs**, where they describe S3SS's own patches: Lot Streaming Optimizations, Smooth
    Patch, the GC patches, WorldCache, RefPack, CreateFileW, Expanded Crash Logs. Those descriptions are for
    coexistence, or for the removed features.
  - Only four appear in Apex **source**, in Frame Profiler comments and code (dev build only) and one comment in
    `map_view.cpp`: `0x00C6D570` (`WorldManager::Update`, noted so the profiler avoids LSO's detour), `0x00D819AA`
    (the GC call site, timed by the profiler), `0x00EC9FBA` (Smooth Patch's limiter site, read to label the limiter
    state) and `0x0073E060` (map view, below).
- The hundreds of other Apex addresses come from Apex's own RE (`re\out` Ghidra dumps, `research\engine_map`, F7/F8
  probes): Night Lighting, terrain bake, room light maps, rigs, shaders, the lot light pass, and so on. See
  `docs/engine/*.md`.

**Byte signatures.** S3SS has 51 distinct pattern strings and Apex has 159.
- **2 are identical:**
  - `83 EC 0C 83 B9 64 03 00 00 00 89 4C 24 04 0F 84 ?? ?? ?? ?? 83 B9 08 04 00 00 01` (`Lot::UpdateObjectSceneNode`):
    copied from S3SS's LSO (`lot_streaming_optimizations_patch.cpp:31`) into `features/frame_profiler.cpp:1239`, with
    the comment "same pattern as LotStreamingOptimizations". **Dev build only.**
  - `8B 81 C0 00 00 00 8B 91 C4 00 00 00 C3`: the whole 13-byte body of GetLotID. Apex uses it as the *fallback*
    signature (`game_addresses.cpp:298`; the primary one finds GetLotID as the call target from Apex's own
    gather-call signature) and as the vanilla-bytes check (`s3ss_detect.cpp`, `split_level_ground_light_patch.cpp`).
    Any signature of this tiny function is its body, but the knowledge came from S3SS (below).
- **1 is a prefix:** `80 BE 8D 00 00 00 00 5E 74 15` in `frame_profiler.cpp:3478`. These are the first 10 of the 23
  bytes of S3SS's Smooth Patch limiter pattern (`smooth_patch_precise.cpp:164`). The profiler uses them to report
  whether Smooth Patch has replaced the limiter.
- The only other overlaps (6 bytes or more in a row) are generic x86 prologues and pushes, such as `55 8B EC 83 E4 F0`
  and `53 55 56 8B 74 24`.
- **The other 156 Apex signatures were written independently.** Apex's EA-app port was checked offline with
  `research\port169\sigcheck.pl`.

**Knowledge first learned from S3SS's source:**

| Knowledge | S3SS source | Use in Apex Radiance | Independent part |
|---|---|---|---|
| **GetLotID → 0 lets lot lamps light across lots and stories** (the "Split-Level Lighting Fix"; S3SS's patch and README credit **Arro**). Target `BaseLight::GetLotID`, Steam `0x006BC020`, found by S3SS's pattern | `patches/split_level_lighting_fix_patch.cpp` | Every-Story Ground Light (`patches/split_level_ground_light_patch.cpp`, part of Night Lighting); credited in Settings > About ("a technique first shared by Arro") | S3SS's comments already named the two callers (the outdoor-room gather and the terrain lightmap bake). Apex's own work: their addresses (`0x006B635D`, `0x00C294D0`), a 5-byte `xor eax,eax; xor edx,edx; ret` stub instead of S3SS's 13-byte rewrite, and the finding that the zeroed gather counted lot street lamps a third time. Apex therefore redirects the gather's call to a copy of the original body (`research\borda2`) |
| Localized Documents folder name from the exe's string table (blocks of 100 from id 1000, folder at +2) | `config/config_paths.cpp` (upstream commit `7b0bbcb`, "Localized game names for s3ss folder") | `framework/apex_paths.cpp` | Code rewritten; technique the same |
| PE timestamps of Retail 1.67.2 (`0x52D872DA`) and Origin/EA 1.69.43 (`0x568D4BAC`) | `patch_system.h:29-34` | `framework/game_version.cpp` ("the published values of those builds") | Steam `0x52DEC247` and EA app 1.69.47 `0x6707155C` were **checked by Apex** against the real `TS3W.exe` / `TS3.exe` (`research\port169\method.md`). The other two are unverified |
| Steam/EA address pairs, used to show that the EA 1.69 build has no constant offset | S3SS's per-version address tables (`createfileW`, `expanded_crash_logs`, `lot_streaming_optimizations`) | Reasoning in `docs/engine/game-versions.md` section 2 (it says so) | The EA port itself is Apex's own signature scan |
| Engine function names that are **not** strings in `TS3W.exe`: `LightPointWithAllLights` (`0x0069FD60`), `BaseLight::GetLotID`, `Lot::AddLotObjectsToScene`, `Lot::UpdateObjectSceneNode`, `IdleSimulationCycle` | S3SS comments and address names (`lighting_quality_patch.cpp`, `lot_streaming_optimizations_patch.cpp`, `smooth_patch_precise.cpp`) | Names used in Apex docs and comments (`level_light_share.cpp`, `game_addresses.h`, `frame_profiler.cpp`) | For `LightPointWithAllLights`, the call sites `0x6A1187` / `0x6A126F` / `0x6A3336`, the light vfunc `+0x4C`, the return address `0x69FE19` and the wall test `0x69FE93` are Apex's own RE (`re\out\dump\fn\0069fd60.c`, `NOTAS-ILUMINACAO.md`). `MonoScriptHost`, `WorldManager`, `GC_stop_world`, `RefPack` and `WorldCache` *are* exe strings |
| Where S3SS patches, and with what bytes (LSO, Smooth Patch, GC patches, LightingQuality, and so on) | S3SS patterns, resolved against `re\TS3W.exe` (`PLANO-SEPARACAO.md` section 3) | Coexistence notes (`docs/architecture.md` 12.3, `docs/engine/*`); Frame Profiler avoidance and labels | Descriptive only; no S3SS patch is re-implemented |

**Found independently, although S3SS uses it too:** `Camera_IsMapViewModeEnabled` (`0x0073E060`).
- Apex finds it at run time through the game's script-binding table: the name string "ScriptCore.CameraController::
  Camera_IsMapViewModeEnabled" is in `.rdata`, and the `{function, name}` table is in `.data`. See `map_view.cpp` and
  `docs/engine/camera-and-map-view.md`.
- S3SS's LSO has the same function at a fixed Steam address.
- The notes do not record which the maintainer saw first.

---

## 7. Runtime interaction today

Apex Radiance is designed to run **next to an unmodified official S3SS**. It calls no S3SS function and loads no S3SS
code. What it does with S3SS present:

1. **Detection** (`framework/s3ss_detect.*`).
   - It enumerates the loaded modules and searches their read-only data for S3SS's log header `"S3SS Log - Started at "`
     and its ImGui window id `"###S3SSWindow"`. It records S3SS's module range, so the hook-chain logs can say
     "E9 → Sims3SettingsSetter.asi+0x…".
   - A module that also contains "Sims3 Settings Setter Apex Edition" is the old combined build: Apex keeps its
     features off and shows a banner.
   - Apex's needles are XOR-encoded, so Apex never matches itself.
   - Named mutexes `Local\ApexRadiance.<pid>` and `Local\S3SSApex.<pid>` stop duplicate copies.
2. **Reading `S3SS.toml`** (read-only, never written):
   - `[patches.<Name>].enabled`;
   - `[qol.borderless_window].mode`;
   - `[qol.ui].disable_overlay`.
3. **Deference.**
   - *Borderless*: Apex's borderless stays off while S3SS's is configured ("Handled by Sims3SettingsSetter").
   - *Split-Level*: when S3SS's Split-Level Lighting Fix is on (by config, or because GetLotID's bytes are no longer
     vanilla), Apex writes nothing at GetLotID. It keeps only its gather-call redirect, and the menu says "Already
     handled by Sims3SettingsSetter".
4. **Start-up order.**
   - In DllMain, Apex detours only the `Direct3DCreate9` export. If S3SS is present it waits up to about 3 s for S3SS's
     CreateDevice detour, so two Detours transactions never overlap on one function.
   - It installs its window procedure at the first Present, after S3SS's.
   - It installs game-code features after the first Present plus 1 s, after S3SS has loaded its patches.
   - `framework/conflict_guard.h` (the planned byte-level conflict watchdog) is **only an interface stub**: every query
     reports "no conflict".
5. **Menu.**
   - A "Recommended: Sims3SettingsSetter" card and a Settings > Compatibility entry appear while S3SS is not loaded.
     They open `https://github.com/sims3fiend/Sims3SettingsSetter/releases` with `ShellExecuteW` (`apex_gui.cpp`).
   - The footer shows "Sims3SettingsSetter detected / not installed".
   - Bare Insert, S3SS's key, is refused as the Apex menu key.
6. **One-time migration** (`ApexConfig::EnsureMigrated`). This runs only while `ApexRadiance.toml` is missing.
   - It first copies a previous standalone's `S3SS\Apex\Apex.toml`.
   - Otherwise it reads the combined build's `S3SS.toml`, backs it up to `S3SS.toml.pre-split.bak`, and copies only
     Apex-owned tables:
     - `[qol.picture]` (or the grading keys of the old `[qol.hdr]`);
     - `[qol.frame_profiler]`;
     - `[patches.NightTerrainRelight / EdgeSmoothing / DepthBlur / FrameCapture]`.
   - No S3SS-owned setting is copied.
   - Its own files go to `...\Apex Radiance\`, never to S3SS's folder.

---

## 8. Licensing context and credits

**Upstream has no license.**
- `git log --all -- '*LICENSE*' '*COPYING*'` over the full history finds no license file from upstream. The only hit
  is the fork's `third_party/smaa/LICENSE.txt` (`f18cca8`).
- `S3SS-official\` has no LICENSE.
- S3SS's README has credits (for example, to Arro and @thepancake1) but no license or reuse statement.
- Without a license, default copyright applies. GitHub's terms let other users view and fork a public repository on
  GitHub. They do not clearly grant the right to redistribute modified builds outside that. (This is context, not legal
  advice.)

**What that meant for the fork releases.**
- v0.1.0 and v0.2.0 distributed compiled S3SS code (all of S3SS 1.6.3, plus Apex) with attribution: "Unofficial fork
  ... by sims3fiend and contributors", S3SS's README kept, and credits.
- They had no license from S3SS, and no record of permission was found in the repositories or the project notes.
- The fork repository no longer resolves as of 2026-09-29 (2.1).

**Why the standalone was rewritten.**
- `PLANO-SEPARACAO.md` line 10 says that upstream has no LICENSE file, so only a minimal, attributed framework should
  be carried, with permission asked first.
- The decision of 2026-09-28 went further (`docs/architecture.md` 12.0 and 12.1): rewrite the framework, copy no S3SS
  source, and ask sims3fiend before publishing anyway.
- There were technical reasons too:
  - official S3SS rewrites `S3SS.toml` with only its own sections;
  - both mods would share `S3SS_LOG.txt` and `imgui.ini`.
- `docs/workflow.md` (section 3, item 11) and `CLAUDE.md` still say to ask sims3fiend before publishing the
  standalone.
- Apex Radiance is already public: `loinyx/Sims3-ApexRadiance`, `LICENSE` = MIT (Copyright 2026 loinyx), releases
  1.0.0 to 1.2.0.
- No record of that request, or of an answer, was found in the repository, the docs or the notes read for this audit.

**Credits shown today.**
- **Menu, Settings > About > CREDITS** (`apex_gui.cpp` `AboutTab`).
  - Working tree: "Sims3SettingsSetter by sims3fiend: Apex Radiance started as a fork of Sims3SettingsSetter, and its
    framework (the graphics hook, the in-game menu and the patch system) was rebuilt from that design. Huge thanks to
    sims3fiend: this project wouldn't exist without it. Apex Radiance pairs well with it, and we recommend using
    both."
  - Committed `HEAD` text: "...the mod whose design (a graphics hook with an in-game menu) Apex Radiance's framework is
    modeled on, rewritten from scratch."
  - Also listed: "Every-Story Ground Light uses a technique first shared by Arro.", FXAA (Timothy Lottes), third-party
    code (ImGui, Detours, toml++, SMAA, Lucide), and "Apex Radiance by @loinyx."
- **README, Credits.** The working tree has the same wording as the menu, with a link to the S3SS repository. The
  committed `HEAD` wording is "the mod whose design ... is modeled on. The framework was rewritten from scratch."
- No source file carries a sims3fiend header. That matches the notes: the plan to keep attributed headers on carried
  files was dropped when the decision became "no carried files".

**Accuracy of those texts, against this audit.**
- The working-tree wording, "started as a fork ... rebuilt from that design", matches the findings.
- "Rewritten from scratch" (HEAD) is accurate for the implementation, but does not mention the retained interface
  (3.3–3.6).

**Open points** (for the maintainer; not decided here):
1. Ask sims3fiend, or record an existing answer, as the notes already require.
2. Decide whether to reshape the verbatim registration interface in `framework/d3d9_hooks.h`: rename the methods,
   change the `Priority` values, or merge the 15 `Register*` into one templated call.
3. Decide whether to write the dev-only `Lot::UpdateObjectSceneNode` signature independently.
4. Decide whether the About / README text should mention the kept interface.

---

## 9. How to verify

All scripts are read-only and live in `%USERPROFILE%\Desktop\S3SS-dev\research\provenance\`. They write only report
files there. Use Git Bash; Perl 5 is enough. Set `D=~/Desktop/S3SS-dev` and `P=$D/research/provenance` first.

1. **Official copy equals upstream `5eb2c65`:**
   ```sh
   cd $D/Sims3SettingsSetter
   for f in $(git ls-tree -r --name-only 5eb2c65); do
     git show 5eb2c65:"$f" | tr -d '\r' | cmp -s - <(tr -d '\r' < ../S3SS-official/"$f") || echo "DIFF $f"
   done
   ```
   This prints nothing.
2. **What each release compiled:**
   ```sh
   for t in 5eb2c65 b84d5f1 f18cca8 45e36e2; do
     git show $t:Sims3SettingsSetter.vcxproj | perl $P/compile_list.pl | sort > $P/compile_$t.txt
   done
   comm -23 $P/compile_5eb2c65.txt $P/compile_b84d5f1.txt   # (empty)
   comm -23 $P/compile_5eb2c65.txt $P/compile_f18cca8.txt   # patches/split_level_lighting_fix_patch.cpp
   ```
3. **Who added what:**
   ```sh
   git log --format='%h %an %ad %s' 5eb2c65..45e36e2
   git diff --name-status 5eb2c65 f18cca8
   git log --diff-filter=A --format='%h %an' 45e36e2 -- <file>
   ```
4. **No license upstream:**
   ```sh
   git log --all --format='%h %an %s' -- '*LICENSE*' '*COPYING*'
   ```
   The only hit is `f18cca8` (the fork's `third_party/smaa/LICENSE.txt`).
5. **Token similarity.** `perl simil.pl <listA> <listB> <outprefix> [k] [runmin_raw] [runmin_norm]`:
   ```sh
   cd $P
   perl simil.pl apex.lst s3ss.lst apex_vs_s3ss 8 20 40         # -> .tsv, .reverse.tsv, .runs.txt
   perl simil.pl apex.lst control.lst apex_vs_control 8 20 40
   perl simil.pl s3ss.lst control.lst s3ss_vs_control 8 20 40
   perl simil.pl apex.lst s3ss.lst apex_vs_s3ss_k16 16 20 40
   perl simil.pl posctl.lst s3ss.lst posctl_vs_s3ss 8 20 40      # positive controls (posctl/combined, posctl/renamed via rename.pl)
   perl simil.pl apex.lst combined.lst apex_vs_combined 8 40 80  # section 5; origins in combined_origin.tsv
   ```
   - `apex.lst` points at `snap_apex\`, a copy of the working tree taken for this audit (`snap_apex_HEAD.txt` records
     HEAD, time and `git status`). To audit a later state, rebuild the list from the live tree.
   - Per-file summary: `apex_file_table.txt`.
6. **Exact lines:**
   ```sh
   perl lines.pl apex.lst s3ss.lst 25 > lines_apex_vs_s3ss.txt
   ```
   The same with `control.lst` gives the baseline.
7. **Addresses and signatures:**
   ```sh
   perl addrs.pl <files>   # -> addr_*.tsv; shared list in a_shared.txt
   perl pats.pl <files>    # -> pat_*.tsv
   comm -12 ps.txt pa.txt
   ```
8. **No S3SS API names left:**
   ```sh
   grep -rnE '\b(OptimizationPatch|PatchHelper|DetourHelper|AddressInfo|PatchMetadata|SettingUIType|HookResult|REGISTER_PATCH|ConfigPaths|GetS3SSDirectory)\b' \
     --include=*.cpp --include=*.h . | grep -v third_party
   ```
   Run from the repository root; it prints nothing.
9. **Side by side:** compare `framework/d3d9_hooks.h:36-53` with S3SS `d3d9_hook_registry.h:84-101`, and
   `framework/patch_base.h:28-35` with S3SS `patch_system.h:86-93`.

Limits of the method:
- Token matching detects copied or renamed text. It does not detect the same algorithm written differently. That is why
  sections 4 and 6 are judged by reading the code.
- The working tree was changing during the audit. Numbers for files edited after the snapshot may drift slightly.
