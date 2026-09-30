# Frame Profiler

> Measures every frame on the player's machine and records what each hitch is made of: timed wrappers around ~27 game
> functions (render / Present path, frame limiter, main-loop steps, every ServiceManager service, jobs, job / mutex /
> semaphore waits, file and RefPack I/O, lot streaming and LOD, lot lighting, terrain, script GC), D3D9 call counts and
> the time the mod's own D3D9 registry hooks take. Optional statistical sampler (render and/or simulation thread) for the
> time no timed function covers. Output: live ImGui tables plus `S3SS_Hitches.txt`. **Status: working** (used for the
> 2026-09-28 engine study; 11 sessions / 51 MB of output in the user's `S3SS_Hitches.txt`). In the combined build it is
> present in both build flavours (the UI is not gated by `kPublicBuild`). **In the standalone it is DEV BUILD ONLY**
> (decision 2026-09-28): compile it, its UI and the registry per-hook timing out of the public build. Off by default;
> when off nothing is hooked.
> Source: `frame_profiler.cpp` (~4400 lines), `frame_profiler.h`, instrumentation in `d3d9_hook_registry.cpp`.
>
> **Standalone additions (2026-09-28, anti-stutter plan `research\perf2\plan.md` section 8; written, not compiled or
> tested in game yet):** six **counters** (resource lookups, scene pending nodes, RefPack compression, DXT encoding,
> object lookups by ID, lot room solves; see [Counters](#counters-2026-09-28)), a **dominant cause** per hitch (the
> `dominant:` line, same rule as `research\perf2\tools\dom.pl`), the option **"Time the Mutex::Lock hook"** (default
> off), two **measurement presets** (timing run / sampling run, plan section 9.2), and the remote-call job keys fixed.
> The output file is `ApexRadiance_Hitches.txt` in `Documents\Electronic Arts\The Sims 3\Apex Radiance\`.

See also: [engine main loop and services](../engine/main-loop-and-services.md) (the addresses this tool times and the
thread model), [removed features](../removed-features.md) (Service Frame Budget, Smooth Streaming and Script GC
Scheduler: measured with this tool, removed from the standalone on 2026-09-28 for no perceptible gain; their sections
there replace the former per-feature pages).

## Purpose

Hitches (single long frames) in TS3 come from many places: lot streaming, lot light solves, terrain, CAS builds,
resource finalisation on the main thread, script GC, the GPU / vsync, the frame limiter, and the mod's own hooks. The
profiler answers "which of these was it, on which thread, in this particular frame" without a debugger, so a user can
send a file and a later session can decide which subsystem to fix. It was the measuring tool behind the Frame Budget
feature and the "Performance: engine map" study (NOTAS-ILUMINACAO.md, last section).

## User-facing settings

UI: tab **Apex**, section "Performance", collapsing header **Frame Profiler** (`gui.cpp`, approx. line 577;
`FrameProfiler::RenderUI()` inside `PushID("FrameProfiler")`). Saved in `S3SS.toml` (folder
`Documents\Electronic Arts\<localized game folder>\S3SS\`, `ConfigPaths::GetS3SSDirectory`) under `[qol.frame_profiler]`
by `FrameProfiler::SaveToToml`, called from `ConfigStore::SaveAll` (`config/config_store.cpp`, approx. line 53); loaded by
`LoadFromToml` from `ConfigStore::LoadAll` (approx. line 91). Toggling the main checkbox calls `SaveAll` immediately; the
sliders save on `IsItemDeactivatedAfterEdit`.

| UI label | TOML `qol.frame_profiler.` key | Type | Default | Range (clamped on load) | Notes |
|---|---|---|---|---|---|
| Enable frame profiler | `enabled` | bool | false | | `SetEnabled`; if absent (no table) the profiler is set off |
| Hitch multiplier (Advanced) | `hitch_multiplier` | float (TOML double) | 2.0 | 1.2 .. 5.0 | hitch = frame > max(mult x median of last 120 frames, floor) |
| Hitch floor (Advanced) | `hitch_floor_ms` | float | 8.0 | 1 .. 100 ms | |
| Count state calls (Advanced) | `count_state_calls` | bool | true | | shows SetTexture / Set*Shader / Set*ShaderConstantF / SetRenderTarget per frame. Standalone since 2026-09-29: the registry's detours count them with plain counters (always, development build); the option only decides whether they are shown. Before, it registered six counting callbacks, which made every state call of the game run a full dispatch (0.25-0.35 ms per frame) |
| Write S3SS_Hitches.txt (Advanced) | `write_file` | bool | true | | hitches are queued to the writer only while this is on |
| Sample the render thread (Advanced > Sampling) | `sample_render` | bool | false | | starts / stops the sampler thread |
| Sample the simulation thread (Advanced > Sampling) | `sample_simulation` | bool | false | | needs the simulation thread id (first GC call) |
| Sampling rate (Advanced > Sampling) | `sample_hz` | int | 2000 | 250 .. 4000 Hz | per sampled thread |
| Time lot object building (this session) (Advanced) | not saved | bool | false | | attaches / detaches `Lot::UpdateObjectSceneNode` 0x00ABFAC0 live |
| Per-hook registry timing (Advanced) | not saved | bool | false | | needs the registry instrumentation (present for DIP / DP only) |
| Time the Mutex::Lock hook (Advanced) | `time_mutex_lock` | bool | **false** | | standalone, 2026-09-28: attaches / detaches the hand-made hook on `Mutex::Lock` 0x004E16F0 live; off = "Mutex wait" stays empty. The resource lookup takes that lock ~580 times per full scan, so the hook's two clock reads per call inflated exactly the lookup path (plan caveat 1b) |

Buttons (shown while on or once data exists): **Clear** (forgets frames, hitches, session tables; the file keeps what
was written) and **Save report now** (appends a full report, see "Output file").

**Measurement preset** buttons (always shown; standalone, 2026-09-28), for the 60-second protocol of plan section 9:
- **Timing run** (run type A): hitch multiplier 2.0, floor 8 ms, count state calls on, write the file on, sampling off,
  Mutex::Lock not timed, per-hook registry timing and lot object building off; turns the profiler on.
- **Sampling run** (run type B): the same plus sampling of the render and simulation threads at 2000 Hz.
Both save the settings and log `[FrameProfiler] Measurement preset: ...`. They do not press Clear: the protocol presses
Clear right before the run and Save report now right after it.

## How it works

### Enabling (`FrameProfiler::SetEnabled(true)`, under `g_ctrlMutex`)
1. `InitClock` once (`std::call_once`): if CPUID 0x80000007 EDX bit 8 (invariant TSC) is set, RDTSC is calibrated against
   QPC by a 20 ms busy wait and used as the clock (`g_useTsc`, never changed later); else QPC. `g_blockTicks` = 0.1 ms.
   `RefineClock` (every 128 frames on the render thread) recomputes ms/tick from the long QPC/TSC baseline.
2. `g_needBaseline = true`, `StartWriter()` (writer thread, see Output), `g_attachPending = true`.
3. `RegisterD3DHooks()` registers the registry hooks under the name `"FrameProfiler"`: Present at priority **-1000**
   (start) and **+1000** (end), DrawIndexedPrimitive / DrawPrimitive counting hooks at -1000 (standalone since 2026-09-29:
   no +1000 draw hooks, the registry times its dispatches itself, see "D3D9 counters and mod hook time"), CreateTexture /
   CreateRenderTarget / CreateVertexShader / CreatePixelShader counters at -1000. (Until 2026-09-29, with
   `count_state_calls`, also SetTexture, Set{Vertex,Pixel}Shader, Set{Vertex,Pixel}ShaderConstantF and SetRenderTarget
   counters; now counted in the registry's detours.) `Priority` is an int enum (First=0 .. Last=100,
   `d3d9_hook_registry.h`) and the registry `std::sort`s by its value, so -1000 / +1000 bracket every other module.
4. `UpdateSamplerLocked()` starts the sampler thread if a sampling option is on.
5. **The game functions are attached at the next frame boundary**, inside the first Present hook, on the render thread
   (`OnPresentStart` -> `AttachAllLocked`), with `try_lock` on `g_ctrlMutex` (SetEnabled holds it while it may wait for
   the registry mutex the Present hook runs under). Reasons: call-site writes never race the render thread executing
   them, and the startup patches (Smooth Streaming, LSO, Frame Budget...) have installed first. `g_summary` becomes
   "Timing N of M game functions" (logged `[FrameProfiler] Timing 27 of 27 game functions` in S3SS_LOG.txt on Steam).

Disabling (`SetEnabled(false)`, also `Shutdown()`): stop sampler, `UnregisterAll("FrameProfiler")`, `DetachTarget` for
every target, `StopWriter()`. The collected statistics stay visible until Clear.

### Target resolution and attach (`ResolveTarget`, `AttachTarget`)
`kTargets[]` (28 entries, table below). On Steam (`g_gameVersion == GameVersion::Steam`) the pattern must match exactly at
the Steam address; on other builds it must match exactly once in TS3W's `.text` (`ScanUnique`; first result cached in
`TargetState::scanned`). A target whose bytes do not match (another module detoured / patched it, other build) is
skipped with a status string, listed in Advanced > Hooks and logged as a `LOG_WARNING`. Three attach methods:

| Method | Used for | How |
|---|---|---|
| Detours on the entry | render / present path, lot LOD / lighting, GC, scene, app state, clock, impostor pump | `DetourAttach` with `DetourUpdateThread(GetCurrentThread())` only; entry bytes re-checked by `MatchAt` right before |
| Call site (`callOffset >= 0`) | `0x00AEB306` (-> 0x00AEA680), `0x00C6D68F` (-> 0x00C845C0) | 5-byte `E8 rel32` rewrite to the hook through `PatchHelper::WriteBytes` (tracked, restored byte for byte); on Steam the CALL must reach the expected callee (`CallTarget == callee`), else skipped "redirected by another module?"; `g_orig` = the original callee |
| Hand-made hook (`safeLen > 0`) | functions many threads call: service loops, ExecuteJob, WaitForJob, Mutex::Lock, Semaphore::Wait, FileStream::Read / Flush, RefPack read | `AttachSafe`: builds a trampoline first (verified relocation-free prologue of `safeLen` bytes + `JMP` back; 4 KB RWX pool, 32-byte slots, never freed), opens all other threads (Toolhelp snapshot), suspends them, checks no thread's EIP is strictly inside the replaced prologue, writes the 8-byte patch (`E9 rel32` + original tail bytes) with one `_InterlockedCompareExchange64` (entry must be 8-byte aligned), resumes; retries up to 100 times with `Sleep(1)` |

Why not Detours for the hot multi-thread functions: Detours only fixes up threads passed to `DetourUpdateThread`, and
`DetourUpdateThread` / the commit allocate from the heap, which can deadlock once other threads are suspended (one may
hold the heap lock). `DetachSafe` restores with one locked 8-byte write (the JMP is a single instruction so no thread
can be inside it); if the entry no longer holds our JMP (another module patched over it), it is left alone and the
hook keeps forwarding through the trampoline. Call-site detach likewise leaves a CALL that someone else rewrote.

Hooks are `__fastcall(ecx, edx, stack args...)` returning `uint64_t`: ABI-identical to the originals' `__thiscall` with
callee cleanup, ECX/EDX passed through, EDX:EAX preserved, float args passed as raw dwords.

### Timed game functions (Steam 1.67.2; `kTargets` in `frame_profiler.cpp`)

| # | Name in UI | Address | Convention | Attach | Category | Expected thread (kTargets) |
|---|---|---|---|---|---|---|
| 0 | Render frame | 0x00EC9F00 | thiscall(1), ret 4 (header comment says `__stdcall(1)`; ECX is set by the caller at 0x00ECAAF6 and passed through) | Detours | Render frame (game) + Frame limiter carve | render |
| 1 | End frame + Present | 0x00611680 | thiscall(3), ret 0xC | Detours | EndScene + overlays, then Present (driver) | render |
| 2 | Lot LOD scoring | 0x00C6C290 | thiscall(4), ret 0x10 | Detours | Lot LOD scoring; arg 2 = camera point (motion signal) | render (WorldManager::Update) |
| 3 | Lot detail request | 0x00AC20E0 | thiscall(1), ret 4 | Detours | Lot detail request; counts promotions / demotions | render |
| 4 | Lot renderer update | 0x00AEB2E0 | thiscall(1), ret 4 | Detours | Lot renderer update | render (lot pass 0x00C7CEA0) |
| 5 | Lot load stages | CALL 0x00AEB306 -> 0x00AEA680 | thiscall(0) | call site (pattern at 0x00AEB2F8, CALL at +14) | Lot load stages | render |
| 6 | Lot LOD switch | 0x00AD9E30 | thiscall(1), ret 4 | Detours | Lot LOD switch | lot impostor builder |
| 7 | Lot lighting setup | 0x00ADBAD0 | thiscall(0) | Detours (47-byte pattern) | Lot lighting setup | lot impostor builder |
| 8 | Room lighting + solve | 0x006A80E0 | thiscall(0) | Detours | Room lighting + solve | lot impostor builder |
| 9 | Lot lighting update | 0x00ADB8F0 | thiscall(0) | Detours | Lot lighting update | render |
| 10 | Terrain update | CALL 0x00C6D68F -> 0x00C845C0 | thiscall(2), ret 8 | call site (pattern at 0x00C6D682, CALL at +13) | Terrain update | render |
| 11 | GC_try_to_collect | 0x00E4A050 | cdecl(1) | Detours | Script GC; its caller thread becomes `g_simTid` | simulation |
| 12 | Lot::UpdateObjectSceneNode | 0x00ABFAC0 | thiscall(3), ret 0xC | Detours, **optional**, session only | Lot object scene nodes | AddLotObjectsToScene's thread |
| 13 | ServiceManager main loop | 0x0059ED20 | thiscall(float,float), ret 8 | hand-made, safeLen 6, **body replaced** | Services (self) | render |
| 14 | ServiceManager sim loop | 0x0059ED70 | same | hand-made, safeLen 6, body replaced | Services (self) | simulation |
| 15 | JobManager::ExecuteJob | 0x00599720 | thiscall(job), ret 4 | hand-made, safeLen 7 | Jobs (self) | any (keyed on render) |
| 16 | JobManager::WaitForJob | 0x0059A220 | thiscall(job), ret 4 | hand-made, safeLen 6 | Wait for job | render (others pass through) |
| 17 | Mutex::Lock | 0x004E16F0 | thiscall(timeout*), ret 4 | hand-made, safeLen 5 | Mutex wait (> 0.1 ms, render) | any |
| 18 | Semaphore::Wait | 0x004E2760 | thiscall(timeout*), ret 4 | hand-made, safeLen 5 | Semaphore wait (> 0.1 ms, render) | any |
| 19 | FileStream::Read | 0x004DB850 | thiscall(buf,size), ret 8 | hand-made, safeLen 6 | File read (+ bytes) | any (timed on render) |
| 20 | FileStream::Flush | 0x004DB8E0 | thiscall(0) | hand-made, safeLen 6 | File flush | any (timed on render) |
| 21 | RefPack stream read | 0x004EC010 | stdcall(5), ret 0x14 | hand-made, safeLen 5 | RefPack read | any (timed on render) |
| 22 | Scene::BeginFrame | 0x006EBB70 | thiscall(0) | Detours | Scene::BeginFrame | render |
| 23 | Scene::EndFrame | 0x006E8810 | thiscall(0) | Detours | Scene::EndFrame | render |
| 24 | SceneCaptureManager | 0x009DE140 | thiscall(2), ret 8 | Detours | Scene capture | render |
| 25 | App state | 0x00EC6C30 | thiscall(0) | Detours | App state update | render |
| 26 | Game clock tick | 0x005943F0 | thiscall(0) | Detours | Game clock tick | render |
| 27 | Lot impostor pump | 0x00AD97E0 | thiscall(job), ret 4 | Detours | Lot impostor pump | render (see Open items) |
| 28 | Resource lookup | 0x004AFFC0 | thiscall(2), ret 8 | vtable slots 0x00FB2DE0 / 0x00FFE290 | Resource lookup (counter) | any |
| 29 | Scene pending nodes | CALL 0x006EBC49 -> 0x006E4130 | thiscall(0) | CALL rewrite, outer layer of `CallChain` (the scene node budget is layer 1) | Scene pending nodes (counter) | render (Scene::BeginFrame) |
| 30 | RefPack compress | 0x004EC200 | thiscall(5), ret 0x14 | vtable slot 0x00FB901C, outer layer of `SlotChain` (site RefPackCompress; the fast compressor is layer 3 since round 3 added the gate layer 0) | RefPack compress (counter) | any |
| 31 | DXT1 encode | 0x006152F0 | cdecl(2) | entry JMP, outer layer of `EntryChain` (the fast DXT encoder is layer 1) | DXT encode (counter) | any |
| 32 | DXT5 encode | 0x006154B0 | cdecl(2) | entry JMP, outer layer of `EntryChain` | DXT encode (counter) | any |
| 33 | Object lookup by ID | 0x00C62D40 | thiscall(3), ret 0xC | entry JMP, outer layer of `EntryChain` (the object lookup index is layer 3) | Object lookup by ID (counter) | any (render / simulation) |
| 34 | Lot room solve | CALL 0x00ADB9AD -> 0x006A8BA0 | thiscall(2), ret 8 | call site, all threads checked | Lot room solve (counter) | render (lot lighting update) |
| 35 | Wall AO pass | 0x0068B810 | thiscall(2), ret 8 | vtable slot 0x00FF05B0, `SlotChain` layer 1 inside the wall shading gate (layer 0, outermost) | Wall AO pass (counter) | render (room solve) |
| 36 | Key list | 0x004B1AE0 | thiscall(3), ret 0xC | vtable slot 0x00FB2DC0, outer layer of `SlotChain` (the file list cache is layer 2) | Key list (counter) | any |
| 37 | Key list, ResourceSystem | 0x00736660 | thiscall(3), ret 0xC | vtable slot 0x00FFE270, same | Key list (counter) | any |

Targets 28-34 (standalone, 2026-09-28) and 35-37 (round 3, 2026-09-29) take their addresses from `framework/game_addresses.h` (`TargetInfo::addrId`,
`calleeId`, `slotId` / `slots`); see [Counters](#counters-2026-09-28). Target 17 (Mutex::Lock) is attached only with the
option "Time the Mutex::Lock hook". With the defaults the log reads `[FrameProfiler] Timing 36 of 36 game functions` (33 of 33 before round 3)
(35 targets; lot object building and Mutex::Lock off by option are not counted).

Full byte patterns are in `kTargets` (they are the ground truth; the header comment of `frame_profiler.cpp` lists the
first bytes, callers and the per-target reasoning). The header states every pattern was checked against
`re\TS3W.exe`: unique in `.text`, matching at the Steam address, and for Detours targets the relocated first
instructions were decoded by hand with no branch in `.text` landing inside them. The two service-loop patterns are the
whole 0x44-byte bodies (0x0059ED20..0x0059ED63, verified in `engine_map\full.asm`; `profiler_targets.tsv` says 0x42,
which is off by 2).

**Deliberately not hooked** (header comment of `frame_profiler.cpp`):
- `0x006A3EC0` room lightmap solve: LotEdgeLighting (`patches/lot_edge_lighting_patch.cpp`, site "SolveRoomNow")
  checks its entry bytes before installing and would refuse while it is detoured. Timed through its only caller
  0x006A80E0.
- `Lot::AddLotObjectsToScene` 0x00AC1130: the Lot Streaming throttle (S3SS) overwrites its entry with a JMP.
- `0x00ABFAC0` by default: LotStreamingOptimizations finds it by an entry pattern each time it installs; while detoured
  that fails and its object throttle stays off. Opt-in, not saved.
- `WorldManager::Update` 0x00C6D570: detoured by LotStreamingOptimizations' map-view blocker.
- The entries of 0x00AEA680 / 0x00C845C0: detoured by Smooth Streaming, which verifies their entry bytes; hence the
  call-site redirects. When Smooth Streaming is on, the measured time includes its hook.

### Attribution model (per thread, lock-free)
- Each thread that runs timed code gets a `ThreadSlot` (max 128 = `kMaxSlots`; later threads are not timed) with a
  32-deep call stack (`Frame{start, child, sp, cat, flags}`) and per-category `excl` / `incl` / `calls` counters that
  only the owner writes (relaxed load+store, no locked op).
- `Push` / `Pop`: on return, the call's inclusive time is added to the parent's `child`. **Self (exclusive)** =
  inclusive minus timed children; exclusive times never overlap, so on the render thread they sum to at most the
  frame time and the rest is **Unattributed**. **Inclusive** is counted only for the outermost open call of a category
  (`kOuter` flag via `open[cat]`), so recursion is not double-counted.
- Frame identity is a stack address (`volatile char marker` or `this` of a `Scope`), used to match pops and to drop
  stale frames.
- At each frame boundary `SnapshotThreads` reads every slot's counters as deltas against `g_seen[]` and buckets them:
  **render** (the thread calling Present, `g_renderTid`), **simulation** (`g_simTid`, the thread that called
  `GC_try_to_collect`), **other**. `Split` books the time open calls on the render thread have spent so far and
  restarts them at `now`, so render-thread time lands in the frame where it was spent; other threads' calls are
  attributed to the frame in which they return.
- Special splits: 0x00611680 is pushed with `kSwitchAtSplit`: it counts as "EndScene + overlays" until the Present
  boundary and is switched to "Present (driver)" by `Split`. In `Hook_RenderFrame`, the time after 0x00611680 returned
  (`presentFnExits` / `lastPresentFnExit`) is carved into "Frame limiter". So one frame interval = previous Present's
  wait + limiter + clock tick + app state + services + capture + BeginFrame + this frame's render + EndScene.
- Derived per frame (`FrameBoundary`): `cpuMs = frame - Present(driver) - Frame limiter`;
  `modMs = D3D hooks (mod) + Present hooks (mod)`; `unattributedMs = frame - sum(render exclusive)`.

### ServiceManager loop replacement (`RunServices`)
The originals of 0x0059ED20 / 0x0059ED70 are never run while attached. The C++ walk is identical: if
`[list+0xC] != 0`, for each node from `[list]` until the list head: if `byte[node+0x1A] != 0` and
`byte[node+0x14] & flag` (1 main, 2 sim), service = `[node+8]`, call `vtable[+0x1C]` (main) / `[+0x20]` (sim) as
`thiscall(dt, realDt)`, ret 8; the next node is read after the call, as the original does. Each call is a `kService`
frame; afterwards, on the render thread it goes to the per-frame `g_fSvc` table keyed by the update function address
(aux = vtable), on other threads to `g_otherSvc` (64 atomic slots, session totals). Names: `kServiceNames` (Steam only):

| Update fn | Name | | Update fn | Name |
|---|---|---|---|---|
| 0x00588890 | MessageServer | | 0x00733D20 | ResourceChangeMonitor |
| 0x00598660 | Input (message pump) | | 0x00733D30 | ResourceChangeMonitor (sim) |
| 0x00599A10 | JobManager (main-thread jobs) | | 0x007377F0 | ResourceSystem |
| 0x005F0E50 | CAS SimService | | 0x007A08C0 | ShaderSystem |
| 0x00608630 | CAS TextureCompositor | | 0x00A37E60 | Crossroads (AccountManager) |
| 0x006E3620 | Scene service (SceneObjectManager) | | 0x00B3A960 | ObjectDesigner |
| 0x0071E640 | Swarm (VFX) | | 0x00C7E3C0 / 0x00C7E300 | WorldManager / WorldManager (sim) |

Others print as `service XXXXXXXX (vtable YYYYYYYY)`. Because the key is the function read from the vtable, services
detoured by Frame Budget (Detours patches code, not vtables) keep their names.

### Jobs, waits, I/O
- `Hook_ExecuteJob` (every thread; keyed only on the render thread): key = `[job+0x10]` (job function), read before the
  call (the job may be freed). Intended for remote calls (job function 0x007D9840): key = the method, `vtable+0x10` of
  `[job+0x14]`, or `[obj+0x10]` when the object's vtable is 0x010650C4 (PostRemoteMethodCall objects, built at
  0x00ABEA0A), with bit 31 set (`kRemoteCallBit`, printed "remote call -> X"). **Bug: `g_remoteCallJobFn` and
  `g_remoteMethodVtable` are declared 0 ("0 = not verified (non-Steam)") and never assigned anywhere**, so this branch
  never runs and remote calls show as `job 007D9840` (seen in the 13:19 report). Fix: set them to 0x007D9840 /
  0x010650C4 on Steam after a bytes check. SEH-guarded; an unreadable or null function gives key 0, printed as
  `job 00000001` (TimeTable maps key 0 to 1).
- `Hook_WaitForJob`: render thread only; keyed by the job waited for; self time goes to the wait table.
- `Hook_MutexLock` / `Hook_SemaphoreWait`: every thread pays two clock reads; render-thread calls longer than 0.1 ms are
  booked by `RecordBlocked` as a leaf under the running timed call, keyed by the caller's return address; a semaphore
  wait directly inside WaitForJob is not double counted.
- File read / flush / RefPack read: render thread only (other threads pass straight through); bytes read are summed.
- Remote-call job keys (standalone, 2026-09-28): the bug above is fixed. `ResolveRemoteCallKeysLocked` (at attach, render
  thread) takes `RemoteCallJob` 0x007D9840 (entry bytes `83 7C 24 0C 04 56 57 75` checked), `RemoteMethodVtable`
  0x010650C4 and `RemoteMethodVtable2` 0x010650D8 from the game-address table. The second vtable (stored at 0x00ABEA93,
  a method with one byte argument; its vtable +0x10 = 0x00ABD3E0 calls `[obj+0x10]` too) was not known before. Remote
  calls now print as `remote call -> XXXXXXXX` (the method, e.g. 0x00AC1130 AddLotObjectsToScene) instead of
  `job 007D9840`; the perl tools accept both.

### Counters (2026-09-28)

Anti-stutter plan section 8 (`research\perf2\plan.md`). Six categories at the end of `kCats`, eight since round 3 (`kFirstCounterCat` =
`kResLookup`, `kNC` = 8), timed on every thread they run on with the same attribution machinery (self / inclusive, thread
buckets). For each: per frame, calls and inclusive ms per bucket (render / simulation / other), the longest single call
(render thread / other threads; `g_cMax`, atomic max, taken at the frame boundary), and an extra count (per-thread
`ThreadSlot::extra[]`, snapshotted as deltas like the times). Addresses come from the game-address table
(`framework/game_addresses.h`, ids `ResFindProvider` .. `RoomSolve`: fixed on Steam 1.67.2, signature elsewhere; all
checked with `research\port169\sigcheck.pl`: 116 of 116 ok); the profiler checks the bytes at that address before
hooking. Conventions verified in `research\engine_map\full.asm`:

| Counter (category) | Function | Convention (verified) | Attach | Extra count |
|---|---|---|---|---|
| Resource lookup | `ResourceMgr::FindProvider` 0x004AFFC0 | thiscall(key*, cookie*), ret 8 (5 pushes; key `[esp+18h]`, cookie out `[esp+20h]` after them); returns the provider or 0; the cookie is the provider's priority (the list's second dword, [performance.md](performance.md)) | **vtable slots**: 0x00FB2DE0 (base vtable 0x00FB2DA0 +0x40) and 0x00FFE290 (derived 0x00FFE250 +0x40), the only references (no direct CALL; slot +0x44 0x004AFDA0 calls through +0x40). Since 2026-09-29 through `framework/slot_chain.h`: the profiler is the **outer layer**, the resource lookup cache (when on) the inner one, whichever installs first; the hook calls `SlotChain::Next` | packages probed (index of the returned provider in `[this+0x30, this+0x34)`, 8-byte entries, + 1; all of them on a miss; for an answer from the cache: the packages the cache asked, from `ResourceCache::TakeLookupNote`), misses, **from cache** (`kXCacheHits`, answers from the resource lookup cache) |
| Scene pending nodes | 0x006E4130 (pending-node drain) | thiscall(), ret; zeroes `[this+0x18]` and adds 1 per node (0x006E41EF) | **call site** 0x006EBC49 in `Scene::BeginFrame` (the other 5 callers are not per frame). Since 2026-09-29 through `framework/call_chain.h` (site SceneDrain): the profiler is layer 0 (outer), Spread New Objects Over Frames (`features/scene_budget.h`) layer 1; the CALL is written with every other thread suspended; the hook calls `CallChain::Next` | nodes = `[this+0x18]` read after the call (with the budget on: the nodes processed this frame); **deferred** (`kXDeferred`, from `SceneBudget::TakeDrainNote`: nodes the budget left queued) |
| RefPack compress | RefPack stream write 0x004EC200 | **thiscall**(src, size, dst, capacity, flags), ret 0x14 (uses `[ecx+4]`, the allocator; the plan said stdcall); returns the compressed size. `dst == 0 && flags & 1` = size bound only, passed through untimed; `dst == 0` otherwise = a counting run (compresses without writing, timed) | **vtable slot** 0x00FB901C (stream vtable 0x00FB9018 +4), its only reference; FUN_004EC0A0 / 004EBB90 / 004EB750 have no other caller. Since 2026-09-29 through `framework/slot_chain.h` (site RefPackCompress): the profiler is the outer layer, Faster Cache Compression (`features/fast_refpack.h`) the inner one; the hook calls `SlotChain::Next` | bytes in (size), bytes out (return value) |
| DXT encode | DXT1 0x006152F0, DXT5 0x006154B0 | cdecl(dst*, src*) (callers `add esp,8`); dst = {pixels, width +4, height +8, pitch +0xC}; src = {pixels, pitch +0xC, format +0x10}; both return eax = width & ~3 | 8 + 7 callers on several threads. Since 2026-09-29 through `framework/entry_chain.h`: the prologue `55 8B EC 83 E4 F0` moves to a trampoline and a 5-byte JMP to the outermost layer is written with every other thread suspended (`MemPatch::WriteCodeSuspended`); the profiler is layer 0, Faster Texture Compression (`features/fast_dxt.h`) layer 1; `DxtEncode` calls `EntryChain::Next`. (Before: a hand-made hook, safeLen 6.) | pixels (width x height) |
| Object lookup by ID | 0x00C62D40 | thiscall(idLo, idHi, int* visited), ret 0xC (ecx passed on to 0x00C60D30; every caller passes visited = 0, [performance.md](performance.md)) | 233 callers: script natives on the simulation thread, lot lighting, camera. Since 2026-09-29 through `framework/entry_chain.h` (site ObjectById: the 8-byte prologue `8B 44 24 0C 8B 54 24 08` moves to a trampoline, a 5-byte JMP written with every other thread suspended); the profiler is layer 0, Faster Object Lookups (`features/object_index.h`) layer 3; the hook calls `EntryChain::Next`. (Before: a hand-made hook, safeLen 8.) | **from index** (`kXIndexHits`, from `ObjectIndex::TakeLookupNote`: answers from the object lookup index); calls per bucket = render vs simulation |
| Lot room solve | 0x006A8BA0 | thiscall(timer*, float budget), ret 8; x87 stack empty at the call and on return; ecx = one **level object** of the lot (the deque at manager+0x24..0x40) | **call site** 0x00ADB9AD in the lot lighting update 0x00ADB8F0 (its only caller), written with every other thread suspended | calls = **lot levels updated** (corrected 2026-09-29; it said "rooms relit"); the hitch line adds the lot lighting update's inclusive ms. With Lot Lighting While Moving on, the budget argument is the scaled one ([performance.md](performance.md)) |
| Wall AO pass (round 3) | 0x0068B810, the wall ambient-occlusion step | thiscall(stopwatch*, float budget), ret 8; returns the solver's next state; the budget argument is unused; one call = one pass over every outdoor wall of a lot level ([performance.md](performance.md), "Wall Shading While Moving") | **vtable slot** 0x00FF05B0 (solver vtable 0x00FF0594 +0x1C), its only reference, through `SlotChain` site WallAoStep: layer 1, inside the wall shading gate (layer 0, which must be outermost), so with the gate on only the passes that run are timed | calls = passes |
| Key list (round 3) | ResourceMgr::GetKeyList 0x004B1AE0 and ResourceSystem::GetKeyList 0x00736660 | thiscall(vector* out, filter*, bool unique), ret 0xC; returns the count (the derived one: the vector size after its sort + unique) | **vtable slots** 0x00FB2DC0 and 0x00FFE270, the only references (the derived function calls the base directly: not counted twice), through `SlotChain` sites KeyListBase / KeyListDerived: outer layer, the file list cache inside | keys (growth of `out` in 16-byte keys, read SEH-guarded before and after), packages from cache (`kXListCached`, from `ResourceCache::TakeKeyListNote`) |

- No branch in `.text` lands inside a replaced prologue or CALL (checked in full.asm). S3SS and the other installed ASIs
  touch none of these sites (plan section 6). All the entries are 8-byte aligned (hand-made hooks need it).
- The two counter call sites are written with all other threads suspended, none executing inside the 5 bytes: the room
  solve CALL by `WriteCallSuspended` (the same method as `AttachSafe`), because the lot lighting update is also reached
  from the lot impostor builder path; the scene drain CALL (since 2026-09-29) by `CallChain` (`MemPatch::WriteCodeSuspended`).
  The older call-site targets keep the plain tracked write.
- Vtable-slot targets: `AttachSlots` requires every slot to hold the function (else "replaced by another module?") and
  swaps them with `_InterlockedCompareExchange` while the page is `PAGE_READWRITE`; `DetachSlots` puts back only a slot
  that still holds the hook. Exception (2026-09-29): the Resource lookup target (`T_ResLookup`) goes through
  `SlotChain::Install / Remove` (Site FindProvider, Layer FrameProfiler), because the resource lookup cache wraps the
  same slots; its Hooks-table status says "outer layer of the slot chain" (+ "; the resource lookup cache is inside").
  `g_orig[T_ResLookup]` is only displayed; `Hook_FindProvider` calls `SlotChain::Next`, and reads
  `ResourceCache::TakeLookupNote()` after every call (it is cleared per call). The RefPack compress target
  (`T_RefPackCompress`) does the same on site RefPackCompress ("; the fast compressor is inside"). Since round 3 the
  shared slots are listed in `kSharedSlots` (target, site, the other module's layer): also the Wall AO pass (site
  WallAoStep; "; the wall shading gate is outside") and both Key list targets (sites KeyListBase / KeyListDerived; "; the
  file list cache is inside"); the Hooks-table status reads "a layer of the slot chain".
- Entry-chain targets (2026-09-29): the DXT1 / DXT5 encoders (`T_DxtEncode1/5`) and the object lookup (`T_ObjectById`)
  attach through `EntryChain::Install / Remove` (Layer FrameProfiler) in `AttachTarget` / `DetachTarget`, before the
  `safeLen` branch (their `safeLen` 6 / 8 is only informational now). `ResolveTarget` skips its pattern check for them
  (the entry may hold the fast encoder's / the object index's JMP; the chain checks the prologue itself). Hooks-table
  status: "Timed at the entry, outer layer of the entry chain (...; JMP written with all threads checked[; the fast
  encoder is inside | ; the object lookup index is inside])". `Hook_ObjectById` reads `ObjectIndex::TakeLookupNote()`
  after every call (cleared per call).
- Call-chain target (2026-09-29): the scene pending nodes CALL (`T_SceneDrain`) attaches through
  `CallChain::Install / Remove` (Site SceneDrain, Layer FrameProfiler) in `AttachTarget` / `DetachTarget`, before the
  call-site branch; `ResolveTarget` checks only that an E8 is there (the CALL may already reach the scene node budget's
  hook; the chain checks the target itself). Status: "Timed at the CALL ..., outer layer of the call chain (...[; the
  scene node budget is inside])". `Hook_SceneDrain` reads `SceneBudget::TakeDrainNote()` after every call.
- Hitch lines and the report: "Scene pending nodes ..., nodes N[, deferred D]" and "Object lookup by ID ...[, from index
  H]" (the added parts only when non-zero, after a ",", so `agg.pl` still splits counters on "; "); the per-frame
  Counters table shows "x nodes, y deferred" and "z% from index".
- Profiler on at start (any build): the table is scanned after the first Present + 1 s (`GameAddr::Scanned()`), so these
  targets show "Waiting for the game-address scan" and are attached at the first frame boundary after it
  (`AttachWaitingLocked`; the remote-call keys too). Waiting on Steam as well keeps the scan's self-check from seeing the
  profiler's own hooks (hooked entries and swapped slots would log as "DIFFERS"). When the scan never runs (the old
  combined build is loaded: Apex idles) they stay waiting.
- Nesting: the lookups mostly run inside the pending-node drain (materials), jobs (async-load finalize 0x007297C0) and
  services (CAS); object lookups inside room solves. Self times therefore move from those parents to the counters; the
  dominant cause (below) is computed on self times, so it names the counter when the counter is the real cost.
- Overhead: two clock reads + a TLS read per call; the lookup adds the probe count (a scan of up to ~290 8-byte entries on
  a hit, ~0.1-0.3 us against a ~50 us lookup; read without the manager's lock, SEH-guarded, a statistic).

**Dominant cause** (`ComputeDominant`, hitch frames): the largest single item on the render thread, exactly as
`dom.pl` picks it: every render self category except "Services (self)" / "Jobs (self)", each service's self time and each
job's self time of the frame (the per-hitch top lists), and Unattributed. Also the **top counter**: the counter with the
largest render-thread self time (>= 0.05 ms), or none.

### D3D9 counters and mod hook time
- `OnDrawStart` (DIP / DP start hook): counts DIP / DP; a draw while `open[kEndScene] > 0` is an "end-of-frame draw"
  (S3SS overlay, post passes inside EndScene), else a game draw (primitives summed).
- **"D3D hooks (mod)" (standalone, 2026-09-29; M1 of `research\perf2\apexcost\report.md`):** the registry itself
  (`framework/d3d9_hooks.cpp` `Run`, development build) books its **outermost** dispatch on a thread, of every chain
  except Present, with `FrameProfiler::BeginModTime(ModTime::D3DDispatch, &ctx)` / `EndModTime` around the whole chain:
  a `kModD3DHooks` frame from before the first callback to after the last one, **also when a callback returns Skip /
  Block**. Nested dispatches (a draw Night Lighting replaces is re-issued inside its own dispatch) are part of the outer
  one, so its driver call is counted too (the game's own call of that draw is skipped). Before, the -1000 hook pushed a
  dispatch frame that the +1000 hook popped; a dispatch cut short by Skip (every replaced draw) never reached the +1000
  hook and was dropped by `CleanStale`, and the state chains were not timed at all: the report measured Apex code at
  ~0.8 ms of a 5.1 ms frame while "mod D3D" read 0.16 ms. The frames carry no `kDispatch` flag (never dropped as stale).
  The time includes the bookkeeping: two clock reads and a push / pop per outermost dispatch, about 20-40 ns, now also
  for every SetPixelShader / SetVertexShader of the game (Night Lighting's shader tracking), so expect a few tenths of a
  ms of measurement overhead per frame in "mod D3D" with the profiler on.
- Present: the -1000 hook is the frame boundary (`OnPresentStart`) and pushes a `kPresentHooks` dispatch; the +1000 hook
  pops it. So "Present hooks (mod)" includes all modules' Present hooks and the profiler's own per-frame bookkeeping.
- **"Lamp refresh (mod)"** (standalone, 2026-09-29): Night Lighting's 20-frame lamp list refresh
  (`LotLightBridge::OnPresent`, `FrameProfiler::ModTimeScope(ModTime::LampRefresh)`), a category of its own inside
  "Present hooks (mod)" (its time moves out of the Present hooks' self time). The per-frame `mod D3D` value is
  D3D hooks + Present hooks + Lamp refresh.
- **State-call counts** (standalone, 2026-09-29, P1): SetTexture, Set{Vertex,Pixel}Shader,
  Set{Vertex,Pixel}ShaderConstantF and SetRenderTarget are counted in the registry's detours with plain per-method
  counters (`D3D9Hooks::ReadStateCallCounts`, monotonic; `FrameBoundary` takes the difference from `g_stateBase`), not by
  registered callbacks. Calls Apex makes with `CallOriginal*` (Night Lighting's own state changes around a replaced
  draw) bypass the detours and are not counted; before they were.
- **Off-thread dispatches:** the draw and state chains run without a lock on the render thread; a call from another
  thread takes the registry lock and is counted (`D3D9Hooks::OffThreadDispatches()`, Advanced line "Draw / state hooks
  called from another thread than the render thread", report line "Draw / state hooks called off the render thread").
  The draw counts above assume draws come from the render thread; a non-zero value says they may race.
- The registry only calls hooks before the device method, so resource-creation *duration* is not measurable (counts
  only). Not exposed by the registry at all: CreateVertexBuffer / CreateIndexBuffer, Lock / Unlock, SetRenderState;
  DrawPrimitiveUP / DrawIndexedPrimitiveUP exist only through ExtraHooks' single observer slot, owned by Frame Capture.

### Per-hook registry timing (optional)
`d3d9_hook_registry.cpp` `Internal::ExecuteDrawIndexedPrimitiveHooks` and `ExecuteDrawPrimitiveHooks` (approx. lines
409-436) wrap each `entry.hook(...)` with `FrameProfiler::Ticks()` when `RegistryHookTimingActive()` (relaxed atomic:
option checked AND profiler on) and call `AddRegistryHookTime(entry.name, dt)` under the registry mutex. **Only the two
draw executors are instrumented**; Present, Set*, Create* hooks are not timed per name. `AddRegistryHookTime` keys by
the name string's address (fast path) verified by content (registry names live inside vector elements and can move), at
most 64 names / 256 pointer cache entries. `RollRegistryWindow` (render thread, per frame) publishes ms/frame and
calls/frame per name once per second into `g_regDisplay` ("Registry hooks by name" in Advanced). The list includes the
profiler's own "FrameProfiler" entries.

Standalone (`framework/d3d9_hooks.cpp` `RunList`, 2026-09-29): the draw chains are timed per name as above (option
"Per-hook registry timing"); **every Present callback is timed per name whenever the profiler is on** (M3,
`PresentHookTimingActive()`), reported as `"<name> (Present)"` (e.g. `NightTerrainRelight (Present)`, `PostScene
(Present)`, `ApexCore (Present)`, `FrameProfiler (Present)`), so the Present hooks' cost is split by module without
the option. The "Registry hooks by name" list is shown while the option is on or any name has data, and "Save report
now" adds it ("Registry hooks by name (last second; ...)"). `AddRegistryHookTime` is still serialised by the registry's
own timing mutex.

### Hitch detection (`FrameBoundary`)
- Frame time = time between two Present boundaries. Median of the last 120 frames (`kMedianWindow`, `nth_element`),
  threshold = `max(hitch_multiplier x median, hitch_floor_ms)` computed **before** the current frame enters the window.
- Warm-up: no hitch until 30 frames are in the window (`kWarmupFrames`); the window is reset at every enable
  (the first frames include attaching the hooks).
- Histogram: 2000 bins of 0.05 ms up to 100 ms, 900 bins of 1 ms up to 1000 ms, one overflow bin; percentiles are bin
  centres (the overflow bin reads 1000.0). "max" is exact (`maxFrameMs` is tracked separately).
- Camera signal per frame (`SampleCamera`): moved if the point passed to 0x00C6C290 changed by > 1e-3 in any axis;
  if that function did not run this frame (map view, loading), PostScene's view-projection
  (`PostScene::CameraViewProj`, only captured while an effect using PostScene is on) is compared instead;
  `camera -1` = unknown.
- Per hitch: foreground check (`GetForegroundWindow` pid == ours; else "(window in background)"), top 8 services,
  6 jobs, 6 waits of the frame, file bytes, and the sampler summary; stored in a 200-entry ring and pushed to the writer
  queue (256 entries, SPSC; `dropped` counted when full).

### Statistical sampler (optional)
`SamplerProc` thread, `THREAD_PRIORITY_HIGHEST`, waits on a high-resolution waitable timer
(`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`, fallback plain timer or `Sleep(1)`), period `1/hz` relative per iteration
(so the achieved rate is below target; the UI shows the measured rate). For each wanted thread (render =
`g_renderTid`, simulation = `g_simTid`): `SuspendThread`, `GetThreadContext(CONTEXT_CONTROL)`, copy up to 4 KB (512
bytes until 30/09: deep DXVK / driver / kernel frames hid the game's in ~24% of the hitch samples) from ESP (bounded by the stack region found with `VirtualQuery` on the first sample, SEH-guarded), `ResumeThread`.
Nothing between suspend and resume allocates, logs or locks. After resuming, dwords of the copy that point into
TS3W's `.text` right after a CALL instruction (`IsCallSite`: `E8 rel32` landing in `.text`, or FF /2 forms) are kept
as candidate return addresses (up to 12; a frame-pointer-free heuristic that can pick up stale slots, so deep entries
are hints). Samples go to an 8192-entry SPSC ring with the TSC timestamp; at each frame boundary the render thread
assigns those in `[intervalStart, now)` to the frame (`ConsumeSamples`).

Added 30/09 (loading RE follow-ups):
- For a sample whose EIP is in system code, the sampler also keeps the first dword of the copy that is a return address
  after a CALL in any non-system module (TS3W, d3d9.dll, a driver, an ASI; `FirstOutsideSystem`, over a sorted copy of
  the module table, SEH-guarded). Report table "system code: first caller outside system code", printed `TS3W XXXXXXXX`
  or `module+RVA`.
- The exact-EIP table names system-code EIPs by the nearest export (`ntdll.dll NtWaitForAlertByThreadId+0xC`).
- Page faults of the process per frame (`GetProcessMemoryInfo`): per hitch ("page faults N" on the lots line) and the
  session average for hitch / other frames.
- Two counters on the DDS texture loader's calls (GameAddr `TexCreateCall` / `TexFillCall`, CALL targets with all threads
  checked): "Texture create" (FUN_0060cea0, the D3D CreateTexture of a loaded texture; level-0 pixels counted) and
  "Texture fill" (FUN_0060d290, the mip copy), plus a report table by size class (loads, create / fill ms total, average,
  max, MB) from the profiler's CreateTexture callback, which notes the size on the creating thread.

Symbolisation:
- Module classes (`ClassifyModule`): **TS3W** (main module), **S3SS** (the module containing `ClassifyModule` itself,
  i.e. this ASI; shows as `s3ssapex.asi` in the hot list), **other ASI** (`*.asi`), **DXVK/driver** (`d3d9.dll`,
  `dxgi.dll`, `d3d11.dll`, `vulkan-1.dll`, names starting `nv`, `amd`, `ati`, `igvk`, `igd`), **system** (`ntdll`,
  `kernelbase`, `kernel32`, `win32u`, `user32`, `gdi32`, `gdi32full`, `wow64*`), **other**. Module table: up to 384
  entries, append-only, refreshed at most once a second when an unknown address appears (`EnumProcessModules`).
- TS3W code key = `FnStartGuess(eip)`: walk back in 16-byte steps (max 4096) to the first aligned address preceded by an
  `int3` (0xCC). Checked on the 4126 functions of the RE dump: 94% start that way, 5.5% follow a RET without padding
  (merged with the previous function), 0.1% of aligned positions inside bodies follow a CC (split). Printed
  `TS3W fn~XXXXXXXX`: a grouping key, resolve with the decompile / `engine_map`. Cached in a 16384-entry direct map.
- **Apex code** (standalone, 2026-09-29, M2): samples in this ASI are keyed the same way inside its own `.text`
  (`OwnFnStartGuess`) instead of one key for the whole module, and printed as an RVA, `apexradiance.asi fn~+1A2B0` (and
  `apexradiance.asi+1A2B3` in the exact-EIP list). The Release configuration writes `ApexRadiance.map` next to the .asi
  (`GenerateMapFile`); RVA = map address - preferred base (the map's "Preferred load address", normally 0x10000000).
  The module base, size and PE TimeDateStamp are logged once per session when the sampler starts (`[FrameProfiler] Apex
  code in the samples: apexradiance.asi base ..., PE TimeDateStamp ...`) and printed at the top of the report's sampling
  section, so a report can be matched with the map of the same build. The int3-padding guess is unverified for MSVC's
  x86 output of this project: if a key lands far from any symbol, use the exact-EIP list.
- Per hitch: share of samples per class, top 8 code keys per thread, top 8 TS3W call sites on the render stack, top 4
  "system code called from" (first TS3W return address of samples whose EIP is in system code: waits, heap, I/O).
  Session tables (`g_agg[0]` other frames, `[1]` hitch frames): 4096-entry count tables, plus exact EIPs in hitch frames.

### Output file
`S3SS_Hitches.txt` in the S3SS folder (standalone plan: `ApexRadiance_Hitches.txt` in `...\Apex Radiance\`, PLANO-SEPARACAO.md).
Writer: raw Win32 thread (a global joinable `std::thread` would `std::terminate` at exit), appends in binary mode, wakes
every 1000 ms or on `SetEvent(g_wake)`; nothing is written or flushed on the render thread. Content, in order:
1. Session header at start: `==== Frame profiler session <time> | game <version> | clock <RDTSC (invariant, X GHz) |
   QueryPerformanceCounter> | hitch = frame > max(M x median of the last 120 frames, F ms) ====` plus one legend line.
2. "Timed functions:" + the hook status table (queued by `AttachAllLocked`).
3. One block per hitch (`FormatHitch`): `#frame t=..s frame X ms (median, threshold) cpu present limiter mod D3D camera`,
   then `render thread (self ms)` sorted list + `Unattributed`, `render thread (incl. nested ms)` (only categories whose
   inclusive exceeds self by > 0.05; note the label "Services (self)" is reused for the inclusive value there),
   `simulation thread (self ms)`, `other threads (self ms)`, `calls:`, `d3d:` counts, `lots promoted/demoted`,
   `services (render thread, ms incl / self)`, `jobs run on the render thread`, `render-thread waits`,
   `file reads`, and `samples render thread (N): ...` / `samples simulation thread (N): ...` when sampling.
4. "Save report now" (`BuildReport`): percentiles, last-60-frame averages, camera hit rates, hook status, GC call-site
   state, limiter state, totals since Clear per category and bucket, per-hitch averages over the last 200 hitches, all
   200 hitches, keyed session tables (services / jobs / waits: ms per hitch vs per other frame), sim-loop services, and
   the sampling tables. With the profiler off the report is written by a detached one-shot thread.

Standalone additions (2026-09-28) to every hitch block, after the `lots promoted` line (existing lines unchanged, so
`agg.pl`, `dom.pl`, `cond2.pl` and `smp.pl` still read older and newer files alike):
```
   counters (calls x ms incl. on the render / simulation / other threads): Resource lookup 812 x 9.12 (max 0.31) / 12 x 0.20 / 400 x 3.10 (max 1.20), packages per lookup 245.3, misses 40; Scene pending nodes 1 x 14.13 (max 14.13) / 0 x 0.00 / 0 x 0.00, nodes 120; Lot room solve 3 x 12.10 (max 5.20) / 0 x 0.00 / 0 x 0.00, lot lighting update 12.40 ms incl.
   dominant: Scene pending nodes 5.01 ms (21% of the frame) | top counter: Resource lookup 9.12 ms self (9.12 ms incl.)
```
(illustrative values). Only counters with calls in the frame are listed, `; ` between them; `(max ..)` = longest single
call, after the render group and after the other-threads group (simulation + other). Extras: RefPack compress
`, in X KB, out Y KB`; DXT encode `, pixels X M`; Resource lookup `, from cache N` after `misses` when the resource
lookup cache answered any call of the frame (2026-09-29; a comma, so `agg.pl`, which splits counters on "; ", is
unaffected), then `, absent from cache N` when Remember Missing Files answered "no package holds it" (round 3); Key
list `, keys N` and `, packages from cache N` (round 3); Wall AO pass: none (calls = passes). The dominant key is written as `dom.pl` names it: a category name,
`svc:<service>`, `job:<job name>` (`job:job 007297C0`, `job:remote call -> 00AC1130`) or `Unattributed`; `top counter:
none` when no counter reached 0.05 ms of render self time.

The report adds "Counters since Clear" (per bucket calls x ms, longest calls, render-thread per-frame averages, extras),
"Counters per hitch" (the hitch ring), "Apex shaders: ..." (the shader precompile status, see
[architecture](../architecture.md#shader-precompile)), (standalone, 2026-09-29) "Registry hooks by name (last second; ...)"
with one line per name (Present hooks always, draw hooks with the option) and "Draw / state hooks called off the render
thread (dispatched under the lock): N", "Resource lookup cache: ...", "Resource lookup cache counters:
..." (round 3: lookups, from memory, absent, answers with no probe of the counted packages, game lookups, re-check
failed, stored / absent stored / not stored / unreliable read-only package, list changes, notices, missed changes, the
package counts, counted classes and writes, sums refreshed, checks; and the file list counters when that cache ran),
"Remember missing files: ...", "File list cache: ...", "Lot lighting while moving: ...", "Wall shading while moving:
...", "Spread new objects over frames: ..." and "Faster object lookups: ..." (v1.5.0) (the performance features' status lines, [performance.md](performance.md)) and "Dominant cause of the last hitches" (per camera state and
frame-time bucket < 16 / 16-25 / 25-50 / >= 50 ms: share and average ms of each dominant item, then the top-counter
distribution; window in foreground only, like `dom.pl`).

Analysis tools (`research\perf2\tools\`, updated 2026-09-28): `dom.pl [--since "YYYY-MM-DD HH:MM"] [--computed] FILE`
uses the `dominant:` line when present (else computes it the old way; `--computed` forces that), prints the top-counter
distribution per bucket, and reads remote-call job names; the default `--since` (13:14 of 28/09) keeps every Apex Radiance
session. `agg.pl` also prints per-hitch counter averages (`C:`) and dominant shares (`D:`). `cond2.pl` accepts
`dom:<dominant key>` and `job:remote call -> X`. Checked on the combined build's 51 MB `S3SS_Hitches.txt`: the same
numbers as plan section 2.1 (dom.pl) and 2.2 (cond2.pl `Scene::BeginFrame 8`: 772 hitches).

`GcCallSiteText` inspects 0x00D819AA (Steam, after `MatchAt(0x00D819A0, "68 ?? ?? ?? ?? A3")`; non-Steam
`ScanUnique("68 ?? ?? ?? ?? A3 ?? ?? ?? ?? ?? ?? ?? ?? ?? A1 ?? ?? ?? ?? 83 C4 04 3B 05") + 10`): `90` = removed by
Chunky Patch "Disable GC_try_to_collect", `E8` to 0x00E4A050 = direct, `E8` elsewhere = redirected (e.g. Script GC
Scheduler; still timed because the redirected code calls the detoured entry). `LimiterText` inspects render frame +
0xBA (0x00EC9FBA): `80 BE 8D 00 00 00 00 5E 74 15` = game default (30 ms sleep only while inactive),
`3E 56 E8 ?? ?? ?? ?? 5E EB` = Smooth Patch limiter.

### UI (`RenderUI`)
- Checkbox, Clear, Save report now; live line (averages of the last 60 frames: frame / FPS / CPU / Present / Limiter;
  draws + end-of-frame draws, mod D3D hook ms; optional state-call counts); `PlotLines` of the last 300 frame times
  (scale top = clamp(1.5 x threshold, 33.4, 250)); p50 / p95 / p99 / max since Clear; frames / hitches / %; camera
  moving share and hitch rate moving vs still.
- "Last hitches" (default open): table Category | Render ms | Other threads ms | Worst ms | Calls (per hitch averages
  over the ring, categories above 0.005 ms/hitch, sorted), Unattributed row, textures / shaders created per hitch, lots
  promoted per hitch; sub-node "Dominant causes" (the report's summary, standalone). "Recent hitches": last 25, one line
  each with the top 3 categories and (standalone) the dominant cause.
- "Counters" (standalone, default open): Counter | Render / frame | Simulation / frame | Other / frame (calls x ms incl.,
  averages of the last 60 frames) | Per hitch (render, incl. and self) | Longest call (since Clear, render / other) | Per
  frame (packages per lookup, misses and % from cache, nodes, KB in -> out, Mpx, lot levels). A warning line while Mutex::Lock is timed.
- Measurement preset buttons (see Settings).
- **Advanced** (tree node): multiplier, floor, count state calls, write file, time lot object building, time the
  Mutex::Lock hook (standalone), per-hook registry timing; "Services, jobs and waits" (four tree nodes: services render thread with self column, jobs, waits, services
  on other threads); "Sampling" (checkboxes, rate slider, measured rate, us paused per sample, estimated % of a sampled
  thread, dropped, thread ids, per-thread class shares and three tables); "Hooks" table (Function | Address |
  Calls R / S / O | Status), GC call site, frame limiter, clock, the not-measurable list, file written / dropped counts;
  "Totals since Clear" (seconds per bucket); "Registry hooks by name" when that option is on.

## Files and functions

| File | Function / symbol | Role |
|---|---|---|
| `frame_profiler.h` | public API | `SetEnabled`, `IsEnabled`, `RenderUI`, `SaveToToml`, `LoadFromToml`, `Shutdown`, `RegistryHookTimingActive`, `Ticks`, `AddRegistryHookTime` |
| `frame_profiler.cpp` | `kCats`, `kTargets`, `kTargetCat` | categories (31) with UI hints; targets (28) |
| | `InitClock`, `RefineClock`, `Now` | RDTSC / QPC clock |
| | `ThreadSlot`, `GetSlot`, `Push`, `Pop`, `Split`, `CleanStale`, `Scope` | attribution machinery |
| | `Hook_*` (28) | wrappers; `RunServices` for the two loops |
| | `ResolveTarget`, `AttachTarget`, `AttachCallSite`, `AttachSafe`, `DetachSafe`, `DetachTarget`, `BuildTrampoline`, `WriteQwordAtomic`, `OpenOtherThreads` | attach / detach |
| | `TimeTable<N>`, `JobKey`, `ServiceName`, `RecordBlocked`, `AddOtherService` | keyed tables |
| | `SamplerProc`, `TakeSample`, `IsCallSite`, `ClassifyModule`, `RefreshModules`, `FnStartGuess`, `ConsumeSamples` | sampler |
| | `FrameBoundary`, `SnapshotThreads`, `SampleCamera`, `UpdateStats`, `Percentile`, `MedianOfWindow` | per-frame bookkeeping |
| | `OnPresentStart`, `OnDrawStart`, `EndDispatch`, `RegisterD3DHooks` | registry hooks |
| | `StartWriter`, `StopWriter`, `WriterMain`, `FormatHitch`, `BuildReport`, `SaveReport`, `KeyedReport`, `SamplingReport` | output |
| | `RenderLive`, `RenderHitches`, `RenderKeyed`, `RenderSampling`, `RenderAdvanced` | UI |
| | `CounterScope`, `NoteMax`, `AddX`, `PackagesProbed`, `Hook_FindProvider`, `Hook_SceneDrain`, `Hook_RefPackCompress`, `DxtEncode` / `Hook_DxtEncode1` / `Hook_DxtEncode5`, `Hook_ObjectById`, `Hook_RoomSolve` | counters (standalone) |
| | `AttachSlots`, `DetachSlots`, `SwapSlot`, `WriteCallSuspended`, `AttachWaitingLocked`, `UpdateSummaryLocked`, `ApplyMutexOptionLocked`, `ResolveRemoteCallKeysLocked` | attach (standalone) |
| | `CounterFrame`, `ComputeDominant`, `DomName`, `DominantText`, `FormatCounters`, `CounterExtraText`, `CounterReport`, `DominantSummaryLines`, `RenderCounters`, `ApplyPreset` | counters output / UI (standalone) |
| `framework/game_addresses.*` | ids `ResFindProvider` .. `RemoteMethodVtable2`, kind `K::SlotsOf` | the counters' addresses (standalone) |
| `d3d9_hook_registry.cpp` (combined) / `framework/d3d9_hooks.cpp` (standalone: `Run`, `RunList`, `ModTimeGuard`, `CountCall`, `ReadStateCallCounts`, `OffThreadDispatches`) | `Internal::ExecuteDrawIndexedPrimitiveHooks`, `ExecuteDrawPrimitiveHooks` (combined) | per-hook-name timing; standalone also the mod D3D time, the Present per-name timing and the state-call counts |
| `config/config_store.cpp` | `SaveAll`, `LoadAll` | `[qol.frame_profiler]` |
| `gui.cpp` | Apex tab, "Performance" | collapsing header |
| `dllmain.cpp` | `DLL_PROCESS_DETACH` | `FrameProfiler::Shutdown()` |

## Game addresses and patterns

All addresses Steam 1.67.2 (`TS3W.exe`, image base 0x00400000, no ASLR). Patterns: see `kTargets`.

| Address | What | How found / verified |
|---|---|---|
| 0x00EC9F00 | render frame, arg = "present" flag; +0xBA = inactive limiter site 0x00EC9FBA | pattern at entry; `engine_map\full.asm` (callers 0x00ECA059, 0x00ECA2E6, 0x00ECAAFC) |
| 0x00611680 / 0x00611760 | end frame + Present / its only caller wrapper (`push 0 x3; call`) | pattern; full.asm |
| 0x00611620 | BeginFrame (checks renderer +0x8C / +0x8D) | full.asm |
| 0x0059ED20 / 0x0059ED70 | ServiceManager main / sim loops, indirect calls 0x0059ED57 / 0x0059EDA7 | whole-body pattern; full.asm; `calls.tsv` (IND:eax) |
| 0x00599720 | ExecuteJob: `[job+0x24]=4`, `call [job+0x10](job+8, [job+0x14], 4)` at 0x0059974F | pattern; full.asm |
| 0x0059A220 | WaitForJob (reached via Job::Wait 0x0059A500) | pattern; calls.tsv |
| 0x007D9840 | remote-call job function (phase arg == 4) | full.asm; intended for `g_remoteCallJobFn`, which the code never sets (see Jobs) |
| 0x010650C4 | PostRemoteMethodCall object vtable, stored at 0x00ABEA0A (native method at +0x10) | full.asm |
| 0x004E16F0 / 0x004E2760 | EA::Thread::Mutex::Lock / Semaphore::Wait | pattern; profiler_targets.tsv (864 Mutex::Lock sites) |
| 0x004DB850 / 0x004DB8E0 | FileStream::Read (ReadFile) / Flush (FlushFileBuffers) | pattern |
| 0x004EC010 | RefPack stream read (magic check `& 0x1FFF == 0x10FB`; calls 0x004EB2F0, 0x004EB3B0); referenced only from data 0x00FB9020 | full.asm, datarefs.tsv |
| 0x00C6C290, 0x00AC20E0, 0x00AEB2E0, 0x00AEA680 (CALL 0x00AEB306), 0x00AD9E30, 0x00ADBAD0, 0x006A80E0, 0x00ADB8F0, 0x00C845C0 (CALL 0x00C6D68F), 0x00ABFAC0 | lot / terrain targets | see [lot loading and streaming](../engine/lot-loading-and-streaming.md) |
| 0x00E4A050 | GC_try_to_collect, only caller 0x00D819AA | see [mono-gc](../engine/mono-gc.md) |
| 0x006EBB70, 0x006E8810, 0x009DE140, 0x00EC6C30, 0x005943F0, 0x00AD97E0 | main-loop steps | see [main loop](../engine/main-loop-and-services.md) |
| lot +0xC1 / +0xC9 | detailed-view flag / bulldozing flag (promotion counter condition) | header comment; code in `Hook_LotDetailRequest` |
| WorldManager +0x3A0 | camera point stored by 0x00C6C290 | header comment; `gc_scheduler_patch.cpp` (movaps at 0x00C6C2B3) |
| 0x004AFFC0; slots 0x00FB2DE0, 0x00FFE290 | ResourceMgr::FindProvider and its two vtable slots | `GameAddr` `ResFindProvider` (Sig), `ResFindProviderSlot0/1` (`K::SlotsOf`: every aligned dword equal to it in the read-only data sections, exactly 2); full.asm, datarefs.tsv |
| 0x004EC200; slot 0x00FB901C | RefPack stream write | `RefPackCompress`, `RefPackCompressSlot` (SlotsOf, exactly 1) |
| 0x006EBC49 -> 0x006E4130 | BeginFrame's CALL of the pending-node drain | `SceneDrainCall` (Sig), `SceneDrain` (Target, fallback Sig) |
| 0x006152F0 / 0x006154B0 | DXT1 / DXT5 encoders | `DxtEncode1`, `DxtEncode5` (Sig) |
| 0x00C62D40 | object lookup by ID | `ObjectById` (Sig); its walk / search 0x00C60D30 / 0x00C5FA60 = `ObjectTreeWalk` / `ObjectTreeSearch` (used by the object lookup index) |
| 0x00ADB9AD -> 0x006A8BA0 | lot lighting update's CALL of the room solve | `RoomSolveCall` (Sig), `RoomSolve` (Target, fallback Sig) |
| 0x007D9840, 0x010650C4, 0x010650D8 | remote-call job function, PostRemoteMethodCall vtables (stored at 0x00ABEA0A / 0x00ABEA93) | `RemoteCallJob` (Sig), `RemoteMethodVtable`, `RemoteMethodVtable2` (Sig, dword) |

Runtime verification: status strings in Advanced > Hooks and the "Timed functions:" block of each session in
`S3SS_Hitches.txt` ("Timed (pattern matches at the Steam 1.67.2 address[; hand-made hook, all threads checked])",
"Timed at the CALL 0x00aeb306 -> 0x00aea680 ..."); log line `[FrameProfiler] Timing N of M game functions`.

## Shader details

None.

## Interactions

- **Smooth Streaming** (`patches/smooth_streaming_patch.cpp`): detours 0x00AEA680 and 0x00C845C0; the profiler uses the
  call sites 0x00AEB306 / 0x00C6D68F instead, so both coexist and the timed value includes Smooth Streaming's hook.
  The profiler header warns that Smooth Streaming, installed while the profiler is on, would see a foreign target at
  0x00C6D68F and skip its terrain-queue flush; **the current Smooth Streaming code (`ResolveWorldGlobals`, approx. line
  303) accepts a call target outside TS3W's image**, so that warning looks outdated (inferred from code, not re-tested).
- **Service Frame Budget** (`patches/frame_budget_patch.cpp`): detours 0x00599A10, 0x007377F0, 0x00608630, 0x005F0E50;
  the profiler's loop replacement calls `vtable+0x1C`, lands in those detours, and keys by the unchanged vtable entry,
  so names and per-service times keep working ("Compatible with the Frame Profiler", its Advanced text). Frame Budget
  counts the impostor pump's nested ServiceManager passes as passes too.
- **Script GC Scheduler** (`patches/gc_scheduler_patch.cpp`): redirects the CALL at 0x00D819AA; GC is still timed at the
  0x00E4A050 entry. On Steam it reads the camera point at 0x00C6C2B3 (inside the body, not the detoured prologue); on
  non-Steam builds it finds 0x00C6C290 by an entry pattern and falls back to its second camera source while the
  profiler is on.
- **Chunky Patch "Disable GC_try_to_collect"** (`patches/gc_try_to_collect_patch.cpp`, S3SS): NOPs the call; then
  "Script GC" is empty and **the simulation thread is never identified** (`g_simTid` stays 0): simulation-thread time
  lands in "other threads" and simulation sampling cannot start.
- **Smooth Patch** limiter (S3SS, `smooth_patch_precise.cpp`, site 0x00EC9FBA inside the 0x00EC9F00 body, not in the
  relocated prologue): measured as "Frame limiter".
- **LotStreamingOptimizations / Lot Streaming throttle / LotEdgeLighting**: avoided on purpose (see "Deliberately not
  hooked").
- **PostScene** (camera view-projection) is only available while an effect using it is on; otherwise camera motion
  comes only from 0x00C6C290.
- **D3D9 registry**: priorities -1000 / +1000 must stay outside every other module's range. The standalone plan
  (PLANO-SEPARACAO.md) reserves -500 for an Apex Present hook that must run after the profiler's boundary and before
  PostScene / Depth Blur / HDR frame-boundary hooks at Priority::First.
- **HDR output / Native HDR / AO**: in the combined build, the HDR pass and AO draws run inside EndScene / the
  registry, so they appear in "EndScene + overlays", "end-of-frame draws" and "D3D hooks (mod)" / "Present hooks (mod)"
  (and by name with per-hook timing). The profiler has no per-effect rows of its own. These features are removed from
  the standalone ([removed features](../removed-features.md)), so there those costs disappear from the same rows;
  Picture filters (SDR) remain.
- **S3SS (original) running beside the standalone**: the standalone's registry is separate, so "per-hook registry
  timing" only sees Apex hooks, and S3SS's own hooks show as "other ASI" in samples (PLANO-SEPARACAO.md, risks and
  step 8: planned "owned by S3SS" labels in the Hooks table).

## Known limitations

- Steam 1.67.2 is the verified build; other builds rely on unique pattern scans and service names are Steam-only.
- The profiler's own overhead (see below) is included in what it measures: "Present hooks (mod)" includes its own bookkeeping;
  every draw pays one extra registry hook (standalone since 2026-09-29; two before) and every outermost registry dispatch a push / pop; every Mutex::Lock / Semaphore::Wait on every thread pays two clock reads.
- Other threads' time is attributed to the frame in which the call returns (a 100 ms job on a worker lands in one frame).
- Threads beyond 128 that run timed code are not timed; call stacks deeper than 32 timed levels drop the deeper calls.
- Keyed tables are fixed-size open addressing (per frame: 64 services, 256 jobs, 128 waits; session: 256 / 1024 / 512),
  90% fill limit, overflow counted in `lost` (not displayed).
- Duration of CreateTexture / shader creation and anything the registry does not expose is not measurable.
- Per-hook registry timing covers only DrawIndexedPrimitive / DrawPrimitive hooks, and (standalone, always while on) the Present hooks.
- Sampling: stack walk is heuristic; `fn~` keys are guesses; a target thread is paused 5-30 us per sample under WOW64
  (header comment), roughly 1-6% of the sampled thread at 2000 Hz, plus similar CPU on the sampler's core.
- Services on other threads are session totals only (not per frame / per hitch).
- Camera "moving" needs 0x00C6C290 to run or PostScene to be active.
- The writer drops hitches when more than 256 are queued within a writer period.
- File grows without bound (the user's file reached 51 MB over 11 sessions).
- Counters (standalone): "packages per lookup" reads the provider list without the manager's lock (a statistic); the
  two DXT encoders share one category (the Hooks table shows the same calls on both rows); the scene pending nodes and
  the room solves are counted only from their per-frame CALLs (BeginFrame, lot lighting update); the longest call of
  the other threads mixes simulation and other threads. Not added from plan section 8: the async-load request counter
  (slot 0x00FFD3BC) and cache-eviction counting.

Overhead when on (from the code and header): per timed call two `RDTSC` + a few stores; per draw two registry hooks
(a few ns each); per Mutex/Semaphore call two clock reads; per frame one `SnapshotThreads` over used slots x 31
categories, table merges, optionally `RefineClock`. Measured in the 2026-09-28 13:19 report (51 600 frames): "Present
hooks (mod)" 10.3 s total = ~0.2 ms/frame (all modules, profiler included), "D3D hooks (mod)" 4.1 s = ~0.08 ms/frame.

## Pitfalls and failed approaches

- **Do not detour entries that other modules verify** (0x006A3EC0, 0x00AEA680, 0x00C845C0, 0x00ABFAC0, 0x00AC1130,
  0x00C6D570): they refuse to install or silently disable features. Use call sites or callers.
- **Do not use Detours for functions many threads call**: only the calling thread is updated, and suspending threads
  then allocating (DetourUpdateThread / commit) can deadlock on the heap lock. Use `AttachSafe` (or, for a function
  reached only through vtables, swap the slots: `AttachSlots`). This is why the 2026-09-28 counters do not use Detours /
  `DetourBatch` for FindProvider, the RefPack write, the DXT encoders or the object lookup.
- **Do not hook a site another Apex module also wraps outside its chain**: FindProvider and the RefPack write go
  through `SlotChain`, the DXT encoders and the object lookup through `EntryChain`, the scene drain CALL through
  `CallChain` (the cache, the fast compressor / encoder, the object lookup index and the scene node budget of
  [performance.md](performance.md) are the inner layers). A direct slot swap, entry or CALL write would cut the other
  layer out (or be refused by its expected-value check).
- **The Mutex::Lock hook distorts the resource lookup** (plan caveat 1b: ~580 lock calls per full scan, two clock reads
  each, and they show up in the samples as this ASI). Keep "Time the Mutex::Lock hook" off for lookup measurements.
- **Attach on the render thread at a frame boundary**, never from the UI click directly (call-site writes would race
  the render thread; startup patches must install first). `SetEnabled` holds `g_ctrlMutex` while it may wait for the
  registry mutex, hence `try_lock` in `OnPresentStart`.
- **Nothing blocking on the render thread**: all file I/O is on the writer thread or a one-shot thread.
- **Skip / Block in other modules' hooks** leave dispatch frames open; they are cleaned by stack depth (`CleanStale`).
  Changing the registry to call hooks from a different stack layout would break that.
- **Global `std::thread` objects** must not be used for long-lived threads (terminate at exit); raw `CreateThread`.
- **Stack-address identity**: hooks use a `volatile char marker` so the compiler keeps a unique stack slot per call.
- Shutdown: `FrameProfiler::Shutdown()` runs in `DLL_PROCESS_DETACH` (only when `lpReserved == NULL`, i.e. FreeLibrary),
  after the patches are uninstalled and the hook thread is stopped, and **before `CleanupD3D9Hook()`** (the registry
  hooks and game detours must be gone before the D3D9 hooks are torn down; PLANO-SEPARACAO.md step 3 keeps "profiler
  first" relative to the D3D9 cleanup). `StopWriter` waits up to 5 s and the sampler stop up to 2 s: under the loader
  lock a thread cannot finish exiting, so these waits can run to their timeouts (inferred from Windows loader
  semantics, not observed). On process exit (`lpReserved != NULL`) nothing is stopped; hitches queued in the last
  writer period can be lost (inferred).
- Reading the output: "Services (self)" in the "incl. nested" line is the inclusive value of the service category (label
  reuse), not self time.

## Testing in game

1. Apex tab > Performance > Frame Profiler > Enable. Status line changes from "Waiting for frames..." to the live line.
   S3SS_LOG.txt: `[FrameProfiler] On`, then `[FrameProfiler] Timing 27 of 27 game functions` (28 targets, the
   optional one excluded). Any skipped target is logged as a warning with its reason. Standalone (2026-09-28):
   `ApexRadiance_LOG.txt`, `Timing 36 of 36 game functions` with the defaults (38 targets since round 3; Mutex::Lock and lot object
   building off by option), 34 of 34 with "Time the Mutex::Lock hook" on. With the profiler on from start-up the first
   line is `Timing 26 of 33 game functions (7 waiting for the game-address scan)`, then 33 of 33 about a second later.
   For the plan's 60-second protocol use the Measurement preset buttons, then Clear right before the run.
2. Advanced > Hooks: every row "Timed (...)"; Calls R / S / O increase; "GC call site" and "Frame limiter" lines show
   which patches are active.
3. Walk or pan the camera across the neighbourhood; hitches appear under "Last hitches". Save report now, then read
   `Documents\Electronic Arts\The Sims 3\S3SS\S3SS_Hitches.txt`.
4. With sampling on: `[FrameProfiler] Sampler on` in the log; Advanced > Sampling shows ~samples/s and us paused.

How to read results (examples from the user's 2026-09-28 13:19 report, 51 600 frames, p50 4.38 / p95 14.28 / p99 32.38
ms, 6.86% hitches):
- **CPU vs Present**: high Present (driver) = GPU / vsync bound; high CPU = game or submission bound.
- **Unattributed** = render-thread time outside all timed functions (main-loop glue, UI...). Typical ~5 ms per hitch
  (5.08 in that report; the notes' "~5 ms/frame unattributed"). A hitch flagged "(window in background)" with ~10 ms
  Unattributed and samples in ntdll called from 0x005887C9 is the game's own background `Sleep(10)` (0x005887A0, called
  from the main loop at 0x00ECA9E8), not a real hitch.
- **Services (self)** dominated hitches (13.6 ms per hitch; worst 134 ms): per service, CAS SimService, JobManager
  (main-thread jobs, i.e. job 0x007297C0 = async resource finalize, 2.2 ms per hitch), CAS TextureCompositor,
  WorldManager (whose inclusive includes Terrain update and lot functions). This is what motivated Service Frame Budget.
- **Terrain update** and **Scene::BeginFrame** produced the largest single spikes (worst 52 ms / 46 ms).
- Textures created per hitch (`created: textures N`): spikes > 50 ms create ~16 textures (NOTAS-ILUMINACAO.md, engine
  map). Smooth Streaming (limiting lot / light work) did not reduce those spikes (same note).
- Compare "ms per hitch" with "ms per other frame" in the keyed tables: a row high in both is steady load, high only in
  hitches is a spike source.

## Open items

- `kTargets` labels the lot impostor pump 0x00AD97E0 "render", but in the 13:19 report its two calls (563 ms) were
  booked in the **simulation** column; which thread really runs it is unverified.
- Scene::EndFrame shows ~2 calls per frame (93 494 calls / 51 600 frames); its other callers are 0x007EB6D0,
  0x00AD97E0 and 0x00ECA2A0; which one runs every frame is not established.
- Unnamed services seen in reports: render `0x00588A00` (vtable 0x00FCBD34), `0x00D61670` (vtable 0x01088F2C),
  `0x009D96D0` (0x01051C78), `0x00687C80` (0x00FF0070); simulation loop `0x00EC5340` (0x010F3D00, 16 s self in 13:19),
  `0x007F1530` (0x0101E758, 11 s), `0x00869000` (0x01029060, 3 s), `0x0076B1A0` (0x01007B74, Scripting service,
  2.4 s). Name them in `kServiceNames` once identified.
- ~~Set `g_remoteCallJobFn` / `g_remoteMethodVtable` so remote calls are keyed by method~~: done in the standalone
  (2026-09-28, both PostRemoteMethodCall vtables), not yet seen in game.
- Counters (2026-09-28): verify in game that `Timing 36 of 36` (33 of 33 before round 3) is logged, every counter row fills in, and the Hooks table
  shows "Timed through 2 vtable slots" / "Timed at the CALL ... written with all threads checked" / "hand-made hook".
- Instrument Present / Set* executors for per-hook timing if needed.
- Standalone: rename output file (`ApexRadiance_Hitches.txt`), add "owned by S3SS" in the Hooks table, treat S3SS's module as
  its own sampler class (PLANO-SEPARACAO.md).
