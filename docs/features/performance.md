# Performance: Faster Game File Lookups, Lot Lighting While Moving, Faster Texture / Cache Compression

> Four anti-stutter features from the perf round 2 plan (`research\perf2\plan.md`, candidates C1, C7, C9 and C4), written
> on 2026-09-29 from the game's disassembly and Apex's own framework (no Sims3SettingsSetter code). Both builds (public
> and development). Menu: SYSTEM > **Performance** (one card), plus two Overview rows; developer lines under Developer >
> Profiler > "Performance".
>
> - **Faster Game File Lookups** (`[patches.ResourceLookupCache]`, experimental, **off by default**): remembers which
>   package answers each resource lookup (`ResourceMgr::FindProvider`) and re-checks the answer cheaply instead of asking
>   all ~290 packages again.
> - **Lot Lighting While Moving** (`[patches.LotLightingMotion]`, **on by default**): while the camera moves, the lot
>   lighting budget is scaled down (the current lot gets 3 ms instead of 15 ms per frame); the game's budget returns when
>   the camera stops.
> - **Faster Texture Compression** (`[patches.FastTextureCompression]`, experimental, **off by default**): the game's CPU
>   DXT1 / DXT5 encoders replaced by the same algorithm on four blocks at once; **bit-identical output** (C9, section
>   below).
> - **Faster Cache Compression** (`[patches.FastCacheCompression]`, experimental, **off by default**): the RefPack stream
>   write answered by a fast compressor in the game's stream format; the game decompresses it unchanged (C4, section
>   below).
>
> Status: implemented, **not yet compiled or tested in game** (the user compiles). Everything below marked VERIFIED was
> read in `research\engine_map\full.asm` / `S3SS-dev\re\TS3W.exe`; INFERRED = deduction, not confirmed at run time.

Related: [frame-profiler.md](frame-profiler.md) (the counters that measured both problems and that keep measuring them),
[../engine/lot-loading-and-streaming.md](../engine/lot-loading-and-streaming.md),
[../engine/room-light-maps.md](../engine/room-light-maps.md), [../engine/main-loop-and-services.md](../engine/main-loop-and-services.md),
[../engine/game-versions.md](../engine/game-versions.md) (signatures), [../ui.md](../ui.md) (the card).

## Purpose (measured baseline)

Dev profiler, the user's game, a ~110 s camera test, 1293 hitches:
- **Small hitches (8-16 ms):** "Resource lookup" dominant in ~65%: 246-288 lookups per hitch frame on the render
  thread, each asking ~291 packages (packages per lookup 291.1, misses 0), 4.6-5.3 ms per frame, i.e. ~18 us per lookup.
  Materials resolving textures (Scene::BeginFrame's pending-node drain), async-load finalize jobs, CAS and lot loading all
  go through it.
- **Medium hitches (16-50 ms) while the camera moves:** "Lot room solve" dominant in 50-77% at ~15-17 ms: the game's
  15 ms per-frame budget for the "priority" lot, spent in one frame.
- **Large hitches (50 ms+), later runs:** "DXT encode" is the dominant counter in 22-40% of them (44-49 ms per such
  hitch; also ~8-11% of the small ones), "RefPack compress" in 20-30% (20-37 ms per hitch; some small ones).

## User-facing settings

Menu: SYSTEM > Performance, card "Performance" ("Fewer stutters while you play"); no header switch, one row per feature
(the feature description, ending with the credit, on hover of its row). Overview rows "Faster File Lookups" and "Lot
Lighting While Moving" (switches; the names open the page). Search finds the rows ("Performance" breadcrumb).

| Row (label / description) | Feature / TOML key | Type | Default | Range | Notes |
|---|---|---|---|---|---|
| "Faster game file lookups" / "Fewer small stutters when objects and textures load" | `[patches.ResourceLookupCache] enabled` | bool | **false** | - | Experimental. Off until the in-game checks below pass; then flip `enabledByDefault` in `patches/performance_patches.cpp`. |
| "Spread lot lighting while moving" / "Lots relight in small steps while the camera moves" | `[patches.LotLightingMotion] enabled` | bool | **true** | - | |
| "Lot lighting time while moving" (shown while the switch is on) / "The current lot's time per frame while moving; 3 ms is the default" | `[patches.LotLightingMotion] budgetWhileMovingMs` | int | **3** | 1-15 | ms; end labels "Smoother" / "Lights sooner"; 15 = the game's own. Applied live (the hook reads it every call; `Update` clears the reinstall request). Never rename the key. |
| "Faster texture compression" / "Fewer hitches when the game builds terrain, Sim and lot textures" | `[patches.FastTextureCompression] enabled` | bool | **false** | - | Experimental until the in-game checks below pass; then flip `enabledByDefault` in `patches/performance_patches.cpp`. No Overview row. |
| "Faster cache compression" / "Fewer hitches when the game stores Sims and objects in its caches" | `[patches.FastCacheCompression] enabled` | bool | **false** | - | Same. No Overview row. |

Development build only (not saved): Developer > Profiler > "Performance" card: the cache's counters, "Check 1 answer in
N against the game" (default 64, 0 = never), "Check every answer for 10 s", the last difference; the lot lighting call,
camera source, last camera move, budget calls / scaled, last budget (game -> applied); texture compression: textures,
blocks (flat-luma, solid, encoded by the game's function), time, "checked textures: game X ms, Apex Y ms (Zx)", CPU
features, "Check 1 texture in N against the game" (default 8), "Check every texture for 30 s", the last difference;
cache compression: streams, MB in / out, time, counting runs, writes after a counting run, did not fit, temporary
contexts, checks (game's decoder), the comparison with the game's compressor, "Check 1 stream in N by decompressing"
(default 1), "Also run the game's compressor on 1 stream in N" (default 0), "Search depth" (default 32).

Both features take part in undo (the menu's state capture covers every `[patches.*]` table) but not in Profiles (only
the look features are profile features).

## How it works: Faster Game File Lookups (C1)

### The game side (Steam 1.67.2; VERIFIED)

`ResourceMgr::FindProvider` **0x004AFFC0**, thiscall(key*, int* priorityOut), ret 8:
1. Locks the manager's `EA::Thread::Mutex` at `this+0x48` (0x004E16F0 with the infinite-timeout constant 0x00FB2CD0).
2. Reads `begin = [this+0x30]`, `end = [this+0x34]` **once** and walks the 8-byte entries `{Database*, int priority}`.
3. Per entry: unlock (0x004E17B0), `db->vfunc+0x34(key, 0, 1, 6, 1, 0)`, relock. The first `true` wins:
   `*priorityOut = entry.priority`, return `db`. None: return 0, `*priorityOut` untouched.
- Reached only through vtable slot +0x40 of the base resource manager vtable **0x00FB2DA0** (slot 0x00FB2DE0) and of the
  derived ResourceSystem one **0x00FFE250** (0x00FFE290), and through the wrapper 0x004AFDA0 (slot +0x44, calls +0x40).
  No direct CALL.
- **The "cookie" out-parameter is the winning package's priority** (the list's second dword). So the result
  (package, priority) is fully decided by which list entry answers first.
- `db->vfunc+0x34` is the database's OpenRecord(key, record**, access, disposition, flag, info*): with no record and no
  info asked (arguments 2 and 6 = 0) it only answers "do you hold this key?".

The package list (the only code that writes `[mgr+0x30..0x38]`; its vector helpers 0x004B1F50, 0x004B25B0, 0x004B0B90
have no other callers):

| Address | Slot | What | Convention |
|---|---|---|---|
| 0x004B2D00 | +0x34 of 0x00FB2DA0 (0x00FB2DD4) | RegisterDatabase(bool add, db*, int priority): add = insert before the first entry of lower priority (list sorted highest first, ties in registration order), after IsRegistered (+0x38, 0x004AFF70) refuses duplicates; then `db->vfunc+0x48(1, mgr, 1)` (Attach). Remove = Attach(0, ...) then erase. AddRef / Release through `db+4`. | thiscall, ret 0xC, returns bool |
| 0x00736A70 | +0x34 of 0x00FFE250 (0x00FFE284) | ResourceSystem's override (its own name map at +0x2A0 etc.), calls 0x004B2D00 **directly** (0x00736C69) | same |
| 0x004B2EC0 | +0x3C of both (0x00FB2DDC, 0x00FFE28C) | SetDatabasePriority(db*, int): erase + sorted re-insert | thiscall, ret 8 |
| 0x004B35A0 / 0x007366A0 | +0 | destructors (0x004B30B0 frees the vector): game exit | |
| 0x004B0960 | +0x4C of both (0x00FB2DEC, 0x00FFE29C) | **DatabaseChanged(db*, keyVector*)**: the engine's own "these keys of db changed" notice; runs FindProvider (+0x44) per key and calls the resource cache's listeners (`mgr+0x10` list) for keys whose answer is now `db` or nothing | thiscall, ret 8 |

Other slots: +0x38 IsRegistered, +0x48 0x004B17E0 = list every package holding a key (not a writer), +0x54 0x004B3200 =
a different list at +0xA0 (factories), not the package list.

Who sends DatabaseChanged: the ResourceSystem's file watcher (`ResourceSystem/ShadowWatcher`): its update 0x00737560
pops changed paths and calls ResourceSystem vfunc+0x14 = **0x00734D10**, which for each package object with that path
calls outer+0xC (0x007343C0 "file changed?": size / time vs +0x100 / +0xF8, drops the key set) and, if changed,
GetKeyList then `mgr->+0x4C(db, keys)` (0x00734DBF). The loose-file folder databases do the same after a rescan
(0x004A4160).

**Package classes** (every constructor that stores the IDatabase base vtable 0x00FB21F8; all have OpenRecord with
`ret 18h`):

| Vtable | Constructor | What | Can gain keys at run time? |
|---|---|---|---|
| **0x00FFE078** | 0x007342F0 ("ResourceSystem/ShadowedDBPF"; inner object at outer+8, outer vtable 0x00FFE0D0) | Every `PackedFile` line of a Resource.cfg without `writable` (0x00737D70 -> 0x00737950): the game's packages, the EP / SP packages, Mods\Overrides / Packages, DCCache .dbc files. Closes idle files; keeps a key set (`this+0xB0`) meanwhile. | **No**, except when its file changes on disk (VERIFIED below) |
| 0x00FFD790 | 0x0072CC60 | base packed-stream DBPF (read-only OpenRecord, but Open / Close change what it holds) | treated as yes |
| 0x00FB2600 | 0x004A8F70 ("ResourceMan/DPF") | `PackedFile <path> writable` (opened with access 3) and many world / save / cache callers | yes |
| 0x00FB2420 | 0x004A5500 ("ResourceMan/DDF") | `DirectoryFiles <folder> [autoupdate]` (loose files) | yes |
| 0x00FFD5F8 | 0x0072B8F0 ("ResourceSystem/MemoryDB") | in-memory databases | yes |
| 0x01046EE8 | 0x0098B150 (social cache) | socialCache.package | yes |

The read-only class in detail (VERIFIED): OpenRecord **0x007345D0**: with no record / info asked and a key set present it
only probes the key set (0x0072DBD0 -> 0x0072DAD0: hash `key[0] ^ key[3]`, `div` by the bucket count, chain walk
0x005492E0 comparing 16 bytes) under the package mutex (`this+0x40`, 0x0072C6C0); otherwise AcquireOpen (slot +0x50,
0x00734710: opens the file with access 1 = read, drops the key set), the base OpenRecord **0x0072D470** (accepts only the
open-existing dispositions 6 and 3: it can never create a record), Release (slot +0x54, 0x007347C0). DeleteRecord (slot
+0x40, 0x007346A0) calls 0x00624F70 = `xor al,al; ret 4`. So its key set is the file's index; it changes only when the file
on disk changes, which the watcher reports through DatabaseChanged. The user's Resource.cfg files (Game\Bin,
GameData\Shared, GameData\Win32, the root one, Mods) have no `writable` line: every package is of this class; the
non-read-only ones are the `DirectoryFiles` folders (Mods\Files, NonPackaged\Ini, UI\Layouts) and whatever the game
registers from code. Code registrations found (RegisterDatabase through the manager getter 0x004AFD20): the CAS part
cache ("CAS/CASPartCacheService/OpenCachePackage" 0x005AE170, a writable DPF), 0x005BC6D0 (another DPF), the compositor
caches (0x006CC570, "CAS/CompositedTextureMediator", base packed-stream class; removed with priority -1000 at
0x006CC0A0), removals in 0x005E44C0 and 0x007D8E50 ("World/KeyList"). All of these are probed on every answer below them.

Threads (plan section 2.2, MEASURED): render thread (materials in the pending-node drain, finalize jobs 0x007297C0,
CAS), loader worker threads, simulation thread.

### The cache (features/resource_cache.cpp)

- **Key** (manager `this`, the 16 key bytes). **Value** {package, priority, its index in the list, stored tick}. Only
  found keys; the game's own misses are never stored (the measurement had none).
- **Table:** 64k entries x 40 bytes = 2.5 MB, `VirtualAlloc` once, never freed (a thread may still be inside the hook
  after the feature turns off). Open addressing, linear probing (at most 64 slots). Each entry carries a stamp; entries
  whose stamp is not the table's current one are empty, so "empty the table" is `stamp++` (on a new generation, and when
  75% full), never a memset. SRW lock: shared for lookups, exclusive for stores; **never held while game code runs**.
- **Generation:** `g_gen` is bumped before and after every RegisterDatabase (both slots), SetDatabasePriority and
  DatabaseChanged (hooked through their vtable slots); `g_mutating > 0` while one runs. Meanwhile lookups go straight to
  the game and nothing is stored; a store happens only if the generation read before the game's lookup is unchanged after
  it and no change was in progress (so an answer computed across a list change is never kept). The table belongs to one
  generation; a lookup under another generation sees it as empty.
- **Snapshot** per (manager, generation): list begin, size, an FNV fingerprint of all entries, and the index + pointer of
  every package that is **not** of the read-only class (the first 64). Built by the first store after a change, outside
  the lock.
- **A lookup** (`Hook_FindProvider`): entry found for the current generation -> `Recheck`:
  1. younger than 60 s;
  2. the list is where the snapshot saw it (begin, size) and entry `index` is still {package, priority};
  3. the package still holds the key (one OpenRecord probe, exactly as the game asks);
  4. none of the non-read-only packages **above** it holds the key (one probe each; answers with more than 32 such
     packages above them are never stored);
  5. the generation did not move meanwhile.
  All true: `*priorityOut = priority`, return the package. Otherwise the game's own lookup runs and its answer is stored.
  Correctness argument: the game's scan returns the first holder; the stored index says every package above it did not
  hold the key when stored; read-only packages above it cannot have gained it (only a file change can, which the engine
  reports through DatabaseChanged = full invalidation); the others were just probed.
- **Changes the hooks could miss:** a store that finds the list moved (begin / size differ) under the same generation,
  or the full fingerprint (checked every 1024 answers) differing, bumps the generation, logs once
  ("changed without RegisterDatabase / SetDatabasePriority") and counts it ("changes the hooks missed").
- **Start** checks the read-only class on the running build (its OpenRecord bytes, the base OpenRecord's "only
  dispositions 6 / 3" prologue reached through its slow-path CALL, DeleteRecord -> "return false"); if that fails nothing
  could be cached, so the feature refuses to start. Then it hooks the four list methods first and FindProvider last.
- **Verification (development build):** 1 answer in N (default 64) and, on demand, every answer for 10 s: the game's own
  lookup runs too and its (package, priority) and the key bytes are compared. Equal = counted; list changed during the
  check = inconclusive (not a difference); different = logged with both answers (key, package, vtable, priority, index)
  and **the cache turns itself off for the session** (the layer passes everything through; status "Turned itself off
  ..."). The game's answer is returned in every checked case.
- **Statistics:** lookups, answers from memory, game lookups (not found, re-check failed, passed through during changes),
  stored / not stored, entries, table restarts, list size, non-read-only packages, list changes, change notices, missed
  changes, checks; development build: time in answers and in game lookups (QPC), average of each, "saved about X ms per
  second" (= answers x the average game lookup - the time in answers).

Cost per answer (INFERRED): one shared SRW acquire, a hash probe, a list read, 1 + (non-read-only packages above) OpenRecord
probes (~0.1 us each), a few atomics; against ~18 us for the game's 291 probes.

### Hooking and the Frame Profiler (framework/slot_chain.{h,cpp})

FindProvider is reached only through vtable slots, and both the profiler (dev build, counter "Resource lookup") and the
cache wrap it. `SlotChain` gives each wrapper a fixed layer (0 = Frame Profiler, outer; 1 = Resource cache, inner),
whatever the install order:
- the slots hold the outermost installed layer's hook; each hook calls `SlotChain::Next(site, layer)` = the next inner
  installed layer's hook, or the game function;
- install: the new layer's next pointer first, then either the slots are swapped to it (one interlocked
  compare-exchange per slot while the page is writable; every slot must hold the expected pointer or nothing is written)
  or the outer layer's next pointer is re-pointed to it (one atomic store);
- remove: the reverse; a removed hook keeps its next pointer (threads inside it finish normally).
The profiler's `AttachSlots` / `DetachSlots` use it for `T_ResLookup` only (the RefPack slot keeps the old path). So the
profiler always times every call (answers from memory included) and the cache sees every call. The profiler reads
`ResourceCache::TakeLookupNote()` after each call: an answer from memory adds "from cache" and counts the packages the
cache asked as "packages probed". The list methods (sites RegisterDb, RegisterDbDerived, SetDbPriority, DbChanged) use
the same mechanism with only the cache layer.

## How it works: Lot Lighting While Moving (C7)

### The game side (VERIFIED)

- **0x00ADB8F0** lot lighting update, thiscall(lot lighting manager), from the lot renderer update 0x00AEB2E0 ->
  0x00AE4CB0 (render thread, per lot with lighting work, every frame; also reached from the lot impostor builder
  0x00AD9E30 -> 0x00AEB3F0). Returns at once unless `[this+0x18]` and not `[this+0x4E]`. Starts an EA stopwatch in ms
  (0x004F35B0(4, 0), 0x00408700), then **`call 0x00ADB120` at 0x00ADB95D**; `fst [esp+0Ch]` keeps the budget for the loop
  test while ST0 stays loaded. For each level object of the lot (deque at `this+0x24..0x40`): `0x006A8BA0(stopwatch,
  budget)` (the CALL at 0x00ADB9AD), then `elapsed = 0x004F33C0`; stop when elapsed > budget. At least one level runs.
- **0x006A8BA0** per level: its two light solvers (vfunc +0xC of `this+0x290` / `+0x2E8`) and the dirty rooms
  (0x006A88B0 -> ... -> 0x006A3C90), all with (stopwatch, budget). **0x006A3C90** is a resumable state machine (state at
  room+0xEC, 9 = done): while room+0x164 is set it stops as soon as elapsed >= budget and continues next frame; rooms
  without +0x164 finish in one go. A smaller budget spreads the work, it skips nothing.
- **0x00ADB120** budget, thiscall, ret, result in ST0 (its only caller is 0x00ADB95D). Values read in TS3W.exe:

| Case | Test | Budget |
|---|---|---|
| Tool mode | WorldManager `[0x011ECBC4]` +0x1B4 == 0 | `[0x011833DC]` = 1000 ms |
| Priority lot, loading | 0x006FDC80(SceneObjectManager, lot id) and manager +0x4F | `[0x011833D8]` = 30 ms |
| Priority lot | 0x006FDC80 true | `[0x00F9A62C]` = 15 ms |
| Other lot, loading | manager +0x4F | `[0x01045CCC]` = 10 ms |
| Other lot | | `[0x00FBD498]` = 5 ms |

- Lot id = `[[manager+0x14]+0x48/+0x4C]`. **Priority lots** = the two lot ids at SceneObjectManager (`[0x011D1CF8]`)
  +0x10D0 / +0x10E0, copied from +0x10D8 / +0x10E8 in 0x00701270 (writers 0x00703510, 0x00703D60, 0x007042F0, all
  renderer / object-hiding code): most likely the active or focused lot (INFERRED, not proven). The measured 15-17 ms
  "Lot room solve" hitches are this 15 ms budget.
- Why the current lot keeps relighting while the camera moves is still open (plan section 10, question 2); Night Lights
  also queues room relights (below).

### The patch (features/lot_lighting_motion.cpp)

- The 5 bytes of the CALL at 0x00ADB95D become `call Hook_LotLightBudget` (`MemPatch::WriteCodeSuspended`: every other
  thread suspended and none stopped inside the 5 bytes; restored the same way, only if it still points to Apex's hook).
  Start checks that the CALL reaches 0x00ADB120 and that the next instruction reads ST0 (`D9` / `DD`).
- `float __fastcall Hook_LotLightBudget(mgr, edx)`: calls 0x00ADB120 (a float return is ST0 in every x86 convention; the
  caller's x87 stack is empty at the call), samples the camera, and while the camera moved in the last 300 ms returns
  `max(0.25, min(game, game x budgetMs / 15))` for budgets under 100 ms; else the game's value. At the default 3 ms: the
  priority lot 15 -> 3 ms (30 -> 6 while loading), other lots 5 -> 1 ms (10 -> 2 while loading): the engine's own order
  and the priority lot's larger share stay; only the time per frame shrinks. The tool mode's 1000 ms is never changed.
- **Camera motion:** the camera eye, `[[root]+0x24]+0x60`, the read WorldManager::Update does at 0x00C6D5BD..0x00C6D5C9
  (`call 0x006E8330` = `mov eax,[0x011D1860]; ret`, `call 0x006E8400` = `mov eax,[ecx+24h]; ret`, `movaps xmm0,[eax+60h]`;
  its y is compared with the terrain height threshold WorldManager+0xE8). The root global, the camera offset and the
  eye offset are parsed from those bytes at Start on every build. Moving = any eye component changed by more than 5 mm
  since the previous budget call; it lasts 300 ms after the last change. Orbit, zoom and pan all move the eye; following
  a walking Sim does too. SEH-guarded read; not readable (no world) = not moving. Sampled only inside the hook (no other
  hook, no WorldManager::Update detour).
- The Frame Profiler keeps both its lot lighting targets (the entry of 0x00ADB8F0, Detours; the CALL 0x00ADB9AD): other
  bytes. Its "Lot room solve" calls now receive the scaled budget.

## How it works: Faster Texture Compression (C9)

### The game side (Steam 1.67.2; VERIFIED instruction by instruction)

**Drivers.** `0x006152F0` (DXT1) and `0x006154B0` (DXT5), `cdecl(Dst*, Src*)`, both `ret` with `eax = width & ~3` (no
caller reads it). `Dst` = {+0 first block, +4 width, +8 height, +0xC bytes per row of blocks}; `Src` = {+0 32-bit pixels
B,G,R,A, +0xC bytes per pixel row, +0x10 format}. The DXT5 driver does nothing unless `Src.format` is 0x3D or 0x3E; the
DXT1 driver never reads it. Blocks in raster order; full blocks use the fetch `0x00614000`, blocks of the last column
(width not a multiple of 4) or the last row use `0x00614C50(src, pitch, cols, rows)`. DXT1: colour block `0x006143F0(out,
1)`; DXT5: alpha block `0x00614150(alpha[16], out)` then `0x006143F0(out + 8, 0)`. The helpers have no other callers.

**Callers** (8 DXT1, 7 DXT5, several threads): `0x005FBBB0` (CAS composited textures, switch on the format),
`0x00618600` / `0x00618930` ("Services/ImgData"), `0x009DC8D0` ("VideoRecording/SceneCaptureTexture"), `0x009DD250`
("SceneCaptureManager/PackedImage", a mip loop), `0x00ADE030` ("LotLODCreator/LODTexture", lot impostor textures, DXT5),
`0x00C25C30` ("TerrainBuilder/PackedNormalMap", a mip loop, DXT5), `0x00D523E0` ("UI/ThumbnailManager/PackedImage", mip
loop, DXT1), `0x00D52FF0`, `0x00D653A0` (mip loops). Sizes are whatever those images are (mips down to 1x1); the
profiler's "pixels" extra measures them.

**Colour block, step by step** (a 16-byte-aligned object: axis at +0, 16 pixels at +0x10, endpoints at +0x110 / +0x120):
1. *Fetch.* Each pixel becomes {R/256, G/256, B/256, L} with L = 0.59f·G' + (0.11f·B' + 0.3f·R') (single precision, that
   order). The full fetch gets x/256 with float bit tricks (mask + or + add; R's bit 7 through the exponent), the partial
   fetch with `cvtsi2ss` × 1/256: both exact, identical. Partial blocks repeat each row's last pixel to 4 columns, then
   the last row to 4 rows.
2. *Moments* (`0x00615100`): sums over the 16 pixels in order of x, x², x·L (4 lanes); mean = sum/16; var = sum(x²)/16 −
   mean². total = (varL + varG) + (varB + varR).
3. *Axis.* total < 1e-5 → "solid": axis {1,1,1,0}, both endpoints = mean. Else if varL > 1e-5: axis = cov(RGB, L)
   (= sum(x·L)/16 − meanL·mean), lane 3 masked, × `rsqrtss` of (covB² + covR²) + covG² (no Newton step). Else
   (flat luma, colours vary): `0x00614DD0`, power iteration: covariance matrix of R,G,B (off-diagonals from the sums of
   G·R, B·G, R·B, sums/256), scaled by max(1, NR(rsqrtps(|var|²))), start = the row of the largest variance (ties go to
   G then B), 16 iterations of w = M·v, stop with the luma weights {0.3, 0.59, 0.11} when |w| ≤ 2^-23 in all three
   lanes, else v = w × (rsqrtps + 2 Newton steps).
4. *Endpoints.* For each pixel t = (dB + dR) + dG with d = (x − mean)·axis; mn = `minps`(0, t), mx = `maxps`(0, t);
   ep0 = `rcpss`(Σmx) · Σ(−mn·x), ep1 = `rcpss`(−Σmn) · Σ(mx·x) (4 lanes; the two weights are equal in exact arithmetic,
   the game uses them crossed). If (ddB + ddR) + (ddL + ddG) < 0.004444 (ep1 − ep0 squared), both move apart by
   (ep1 − ep0) · `rsqrtss`(d²) · (1/31).
5. *565.* channel · 248/255 (G: 252/255) + 1.5·2^18 (G: 1.5·2^17), the float's bits minus the magic = the rounded
   integer; clamp to 0..31 / 0..63 with a byte trick. Equal colours → {c, c, 0, 0}.
6. *Projection range.* Both colours decoded (× 1/31, 1/63) and projected: p = (x3 + x1) + (x2 + x0) with x = e·axis;
   lo = min, hi = max (lo = p0 unless p0 > p1). **x87:** `fld hi; fsub lo; fabs; fcomip 1e-5` → if 1e-5 > |hi − lo| then
   hi += 1 (the only x87 code; its precision is the thread's x87 control word: 24-bit on the Direct3D device thread,
   53-bit elsewhere). range = hi − lo; inv = 1/range (`divss`).
7. *Ordered dither.* strength d = clamp((0.125 − range)·16, 0, 1) by sign bits; pixel k adds dither[k]·d, dither = the 4x4
   Bayer matrix/16 − 0.5 at `0x00FE8238`.
8. *Indices.* v4 = (p − lo)·(inv·3) + dither·d, code = round(v4) via + 1.5·2^23, clamped 0..3; error4 += (code − v4)²
   in pixel order. DXT1 also: v3 with inv·2, 0..2, error3. DXT1 picks 3 colours when error4 > error3·2.25 (DXT5 never).
9. *Packing.* 4 colours: rank 0..3 → code 0, 2, 3, 1; colour0 must be > colour1, else swap and xor 0x5555. 3 colours:
   codes 1↔2 swapped; colour0 must be ≤ colour1, else swap and codes 0↔1. (The index assignment assumes p0 ≤ p1; when
   quantisation reverses them the game still does this: kept.)

**Alpha block** (`0x00614150`, MMX): extremes = 0 or 255. No extreme in the block: 8-alpha mode, a0 = max, a1 = min.
Otherwise 6-alpha mode: a0 = min over the non-zero values, a1 = max over the values that are neither 0 nor 255 (none:
a0 = 63, a1 = 192), extremes get codes 6 / 7. Code = min(K, (max(0, pmulhw((a − min)·32, round(K·4096/range))) + 1) >>
1), K = 7 or 5, then reversed in 8-alpha mode and remapped (rank 0 → 0, K → 1, others +1). **Game quirk kept:** the
alpha fetch of right-edge blocks (`0x00613F80`) has an empty column-fill loop, so rows are packed `cols` apart and
pixels get another pixel's alpha code (only images whose width is not a multiple of 4). Worse, for a 1..3-pixel block in
the last row (rows × cols < 4) its final fill (`rep movsd` from 16 bytes back) starts before the array and copies the
DXT5 driver's four locals that precede it (array at the driver's `esp+40h`): `[esp+30h]` = the block's source pointer
(stored at 0x0061561E just before the call), `[esp+34h]` = y, `[esp+38h]` = height, `[esp+3Ch]` = the destination row
padding. Those dwords go through the same saturations (words, then bytes) as alpha values. `FetchAlphaPartial` models
them exactly (`DriverLocals`); the fast path uses the scalar alpha translation when such a value is not a byte, and
recomputes the alpha half of blocks it hands to the game's function (whose one-block image has other locals). Found by
`tools/dxt_test` (edge sizes, 2026-09-29).

Constants used (all from `.rdata`, exact bit patterns in `features/dxt_codec.cpp`): 1e-5 (`0x00FE82C0`, `0x00F9D2C8`),
0.004444 (`0x00FE8288`), 1/31 (`0x00FE8284`), 1/63 (`0x00FAD528`), 1/16, 1/256, 248/255, 252/255, 1.5·2^18, 1.5·2^17,
1.5·2^23, 0.125, 16, 3, 2, 2.25, FLT_MAX, −1, 0.5, 1, 2^-23, the luma weights and the dither matrix.

### The replacement (`features/dxt_codec.{h,cpp}`, `features/fast_dxt.{h,cpp}`)

- `DxtCodec::Ref`: a literal translation (same instruction sequence with intrinsics, addresses in comments). It is the
  offline test oracle.
- `DxtCodec::Fast`: four blocks per SSE register ("structure of arrays": pixel k of blocks 0..3 in one register per
  channel). Blocks are gathered 4 at a time in raster order (a group can span rows); edge blocks get the game's
  replicated pixels; the pixel bytes are transposed and converted exactly (x/256 is exact either way). All sums,
  moments, projections and the index loop run on the four blocks at once; the DXT5 alpha block is SSE2 on all 16 pixels.
- **Why the bytes are identical** (the exactness argument): every float operation is the same IEEE operation on the
  same values with the same association order as the game's (e.g. (x3 + x1) + (x2 + x0), sums in pixel order);
  add, subtract, multiply and divide are correctly rounded, so for finite values the result does not depend on the SSE
  lane or on scalar vs packed; commutative operand swaps change nothing for finite values; `minps`/`maxps` keep the
  game's operand order (signed zeros); the approximate instructions `rcpss` / `rsqrtss` run per block as the same scalar
  instruction on the same input (the power iteration uses `rsqrtps` like the game); the x87 range test runs the game's
  exact x87 sequence (inline assembly) under the calling thread's control word; MXCSR (rounding, FTZ/DAZ) is the calling
  thread's in both. The one thing that could differ is NaN payloads (which NaN survives when two meet), so a block whose
  axis, endpoints, 1/range or errors are not finite is encoded by the game's own function (`GameBlock`: a one-block
  image at the block's pixels, which the game's drivers fetch exactly like the same block inside the whole image).
  Remaining assumptions, all checked in game: the translation itself (dev checks against the real function), and that
  the compiler keeps legacy SSE encodings (the file refuses `/arch:AVX`).
- **Hook:** the entries through `framework/entry_chain.h` (prologue `55 8B EC 83 E4 F0` to a trampoline, JMP written with
  every other thread suspended); layer 0 = Frame Profiler (times every call), layer 1 = this encoder; the trampoline is
  the game's function.
- **Checks:** the first 16 textures of every session (both builds) and in the development build 1 in N (default 8) are
  also encoded by the game into a scratch buffer and compared; a difference: `[FastDxt] Verification mismatch: ...`
  (block, both byte strings, the pixels), the game's bytes are used, and the feature turns itself off for the session.
- **Cost / expected speed (INFERRED, to be measured):** the game spends roughly 1,200-1,500 cycles per block (two MMX /
  SSE passes with store-forwarding, a divide, x87, a per-pixel horizontal dot product); the fast path about a quarter
  of that (the per-pixel work is vertical over four blocks; the scalar parts are 4 rcp/rsqrt, 1 x87 test and 1 divide
  per block). Expected 3-5x on the encoder, i.e. 44-49 ms hitches to about 10-15 ms. `tools/dxt_test` times it offline;
  the Developer card shows game vs Apex on the checked textures.

## How it works: Faster Cache Compression (C4)

### The game side (VERIFIED)

- **Stream vtable** `0x00FB9018` (constructor `0x004EBFD0`, `[stream+4]` = allocator): +0 destructor, **+4 write
  `0x004EC200`**, +8 read `0x004EC010`, +C `0x004EC080` (magic test). Slot +4 (`0x00FB901C`) is the write's only
  reference.
- **`0x004EC200` thiscall(src, size, dst, capacity, flags), ret 14h.** mode = 1 when flags & 2, 2 when flags & 0x10000,
  else 0. dst == 0 and flags & 1: returns ((size × 20) >> 4) + 32 (no work). Otherwise `0x004EC0A0(dst, capacity, src,
  size, allocator, mode)`; with dst == 0 it compresses without writing and returns the size (a "counting run").
  `capacity` is never read; the call never fails.
- **`0x004EC0A0`.** Header word 0x10FB (0x90FB and a 4-byte size when size ≥ 0x1000000), | 0x4000 and window 0x3FFF
  unless mode & 1 (window 0x1FFFF, no 0x4000); then the size, big-endian, 3 or 4 bytes. size ≤ 0x4000: `0x004EB750` with
  a 256-entry table (1 KB `_alloca`); larger: `0x004EBB90` with a 64K-entry table (**256 KB** from the allocator). Both
  also allocate the chain array (window + 1) × 4 bytes (**512 KB** for the 128 KB window), free both at the end. Returns
  payload + 2 + size bytes.
- **`0x004EB750` / `0x004EBB90`** (identical but for the hash): `memset(table, -1, ...)` **every call** (256 KB);
  hash of 3 bytes (`b0 ^ b1 ^ b2`, or `(b0 << 8 | b2) ^ (b1 << 4)`); chain[pos & window] = previous head. For every
  position: **every** candidate of the chain inside the window is tried (quick reject on the byte at the current best
  length, then a byte loop), best = largest (length − opcode bytes), stop only at a 1028-byte match. Opcode cost 2 when
  offset ≤ 1024 and length ≤ 10, 3 when offset ≤ 16384 and length ≤ 67, else 4. Matches never reach the last 4 bytes.
  mode 2 inserts only the first position of each match, otherwise every position. Literal runs of 4..112, stop opcode
  0xFC + 0..3. On repetitive data the chains hold thousands of candidates per position: that is the hitch.
- **Callers.** MemoryDB commit `0x0072B0A0` (the Sim / object compositor caches, terrain world caches): size query
  (flags 1), buffer of that size, write with **flags 2** (128 KB window); -1 → store uncompressed. Package writer
  `0x004A7030` ("ResourceLoad/PackedFile/CompressionRefpack", saves and writable caches, entries 0x32..16 MB, called from
  `0x004A82C0`): a size query, or **counting runs** (flags 2, no destination) of each registered compressor to choose one
  and size the buffer, then the write with flags 1, 0x10001 or 2 (by the package's mode) into that buffer; -1 → the
  resource is written uncompressed (`0x004A82C0` also drops streams whose ratio is above its threshold).
- **Decompressor `0x004EB3B0`** cdecl(dst, capacity, src, srcSize): header flags 0x8000 (4-byte sizes) and 0x100 (a
  compressed-size field to skip), ignores 0x4000; checks every literal and copy against the capacity and the source
  length, every match offset against the start of dst; needs the stop opcode; returns the header's size or 0. The only
  RefPack decoder in the exe (the stream read `0x004EC010`, magic `(word & 0x1FFF) == 0x10FB`). The official S3SS
  replaces this function; Apex never patches it (it only calls it to check its own streams).

### The replacement (`features/refpack_codec.{h,cpp}`, `features/fast_refpack.{h,cpp}`)

- **Stream format = the game's:** same header for the same size and flags, same window per flags (offsets ≤ 0x3FFF or
  0x1FFFF), the same four opcode forms, the same "matches stop 4 bytes before the end" rule, same literal runs and stop
  opcode. Any RefPack decoder, the game's included, reads it; the bytes differ from the game's.
- **Search:** 4-byte multiplicative hash, 64K heads + 64K chain links (512 KB per context, reused: positions are stored
  as base + index and older entries fall below the base, so the table is never cleared per call); up to 32 chain
  candidates per position (dev slider 4..256), stop at 96 bytes; the game's cost / gain rule; one step of lazy
  matching; all positions of short matches inserted, a sample of long ones. Match lengths compared 16 bytes at a time.
- **Memory:** a pool of 4 contexts (allocated on first use, never freed: at most 2 MB); a 5th simultaneous call gets a
  temporary context. Check buffers up to 1 MB are kept per context.
- **Counting runs:** answered by the fast compressor too, and the thread remembers (source, size, flags, search depth,
  which compressor): the write that follows uses the same compressor, even if the feature was switched meanwhile, so a
  buffer sized by one compressor is never filled by the other; the fast compressor also writes it with the counted
  flags and depth, so the stream has exactly the counted size (the package writer counts with flags 2 but writes with
  the package's flags 1 / 0x10001 / 2; with the game's own compressor a smaller window could make the write longer than
  its buffer, INFERRED from `0x004A7030`, not observed). When the feature is switched off, its vtable layer stays until the last
  counting run is 2 s old (`FastRefPack::Tick` from the patch's `Update`).
- **Capacity:** unlike the game, the write never goes past `capacity` (when non-zero): a stream that does not fit returns
  -1 (the callers store the data uncompressed).
- **Checks:** the first 16 streams of each session (both builds) and in the development build every stream (default)
  are decompressed with the game's decoder (`RefPackDecompress`, else Apex's translation) and compared with the source.
  A difference: `[FastRefPack] Verification mismatch: ...`, the game's compressor writes that stream (when the
  destination can hold its bound, else -1) and the feature turns itself off for the session. Optional (dev): the game's
  compressor on 1 stream in N, counting only, for the size / time comparison.
- **Expected (INFERRED, to be measured with `tools/refpack_test` and the dev comparison):** no per-call 768 KB
  allocation or 256 KB clear, and a bounded search instead of the whole chain: 5-20x faster on repetitive data (where
  the hitches are), a few percent larger streams (the caches are size-capped, so slightly earlier evictions).

## Files and functions

| File | Symbols | Role |
|---|---|---|
| `features/resource_cache.{h,cpp}` | `Start`, `Stop`, `Hook_FindProvider`, `Find`, `Recheck`, `Remember`, `BuildSnapshot`, `MaybeFingerprint`, `Verify`, `Hook_RegisterDb*`, `Hook_SetDbPriority`, `Hook_DbChanged`, `CheckReadOnlyClass`, `TakeLookupNote`, `GetStats`, `StatusText`, `RenderDeveloperUI` | the cache |
| `features/lot_lighting_motion.{h,cpp}` | `Start`, `Stop`, `Hook_LotLightBudget`, `SampleCamera`, `ReadEye`, `ParseRootGetter` / `ParseCameraGetter` / `ParseEyeRead`, `SetBudgetMs`, `CameraMoving`, `StatusText`, `RenderDeveloperUI` | the budget wrapper |
| `features/dxt_codec.{h,cpp}` | `Ref::EncodeDxt1/5`, `Ref::EncodeBlock`, `Fast::EncodeDxt1/5`, `FetchFull` / `FetchPartial` / `FetchAlphaPartial` / `Endpoints` / `PowerAxis` / `EncodeColor` / `EncodeAlpha` (the game's helpers), `EncodeColorGroup` / `EncodeAlphaFast` / `FlushGroup` (fast), `X87RangeBelowEps`, `CpuHasSse2` | the DXT algorithm, pure (also built by `tools/dxt_test`) |
| `features/fast_dxt.{h,cpp}` | `Start`, `Stop`, `Hook_Dxt1/5` -> `Encode`, `RunFast`, `GameBlock` (fallback), `Checked`, stats, `RenderDeveloperUI` | the hook, checks and counters |
| `features/refpack_codec.{h,cpp}` | `Compress` (fast), `Decompress` (0x004EB3B0 translated), `GameCompress` / `GameCore` (0x004EC0A0 + 0x004EB750 / 0x004EBB90 translated), `ParamsFor`, `SizeBound`, `Context` | the RefPack format, pure (also built by `tools/refpack_test`) |
| `features/fast_refpack.{h,cpp}` | `Start`, `Stop`, `Tick`, `Hook_StreamWrite`, the context pool, the per-thread counting-run pairing, `Check`, stats, `RenderDeveloperUI` | the hook, checks and counters |
| `tools/dxt_test/dxt_test.cpp`, `tools/refpack_test/refpack_test.cpp` | offline tests (console only, no files written) | build and run lines in their headers and below |
| `patches/performance_patches.cpp`, `patches/performance.h` | `ResourceLookupCachePatch`, `LotLightingMotionPatch`, `FastTextureCompressionPatch`, `FastCacheCompressionPatch`, `Performance::LotLightingBudgetMs` / `SetLotLightingBudgetMs` / `*Status` | the ApexPatch features, their registration and the menu's accessors |
| `framework/slot_chain.{h,cpp}` | `SlotChain::Install` / `Remove` / `Next` / `Installed` / `GameFunction`; sites incl. `RefPackCompress`, layer `FastCompress` | layered vtable-slot hooks (profiler + cache, profiler + fast compressor) |
| `framework/entry_chain.{h,cpp}` | `EntryChain::Install` / `Remove` / `Next` / `Installed` / `GameFunction` / `Original`; sites `DxtEncode1/5`, layers `FrameProfiler`, `FastDxt` | layered entry hooks (trampoline + JMP written with all threads suspended) |
| `framework/memory_patch.{h,cpp}` | `MemPatch::WriteCodeSuspended` | code write with every other thread suspended |
| `framework/game_addresses.{h,cpp}` | ids `ResRegisterDb` .. `CameraGetter`, `RefPackDecompress`, groups `ResourceCache`, `LotLightingMotion`, `FastTextureCompression`, `FastCacheCompression` | addresses (fixed on Steam, signatures elsewhere) |
| `features/frame_profiler.cpp` | `Hook_FindProvider` (SlotChain::Next, `kXCacheHits`), `Hook_RefPackCompress` (SlotChain::Next), `DxtEncode` (EntryChain::Next), `AttachSlots` / `DetachSlots` (`T_ResLookup`, `T_RefPackCompress`), `AttachTarget` / `DetachTarget` (`T_DxtEncode1/5`), report lines | profiler side |
| `apex_gui.cpp` | `PerformancePage`, `PerformanceCard`, `FeatureSwitchRow`, Overview rows, Developer > Profiler "Performance" card, search part "Performance" | menu |

## Game addresses and patterns

Resolved through `framework/game_addresses.cpp` (fixed on Steam 1.67.2; masked signatures on other builds; all checked
with `research\port169\sigcheck.pl`: 133 of 133 ok, and each alternate matches once at the same place). Full table in
[../engine/game-versions.md](../engine/game-versions.md) section 6.

| Id | Steam | Kind |
|---|---|---|
| ResFindProvider / ResFindProviderSlot0/1 | 0x004AFFC0 / 0x00FB2DE0, 0x00FFE290 | Sig / SlotsOf(2) |
| ResRegisterDb / ResRegisterDbSlot | 0x004B2D00 / 0x00FB2DD4 | Sig / SlotsOf(1) |
| ResRegisterDbDerived / ResRegisterDbDerivedSlot | 0x00736A70 / 0x00FFE284 | Sig / SlotsOf(1) |
| ResSetDbPriority / Slot0/1 | 0x004B2EC0 / 0x00FB2DDC, 0x00FFE28C | Sig / SlotsOf(2) |
| ResDbChanged / Slot0/1 | 0x004B0960 / 0x00FB2DEC, 0x00FFE29C | Sig / SlotsOf(2) |
| ShadowedDbVtable | 0x00FFE078 | Sig (dword in the ctor 0x007342F0) |
| LotLightBudgetCall / LotLightBudget | 0x00ADB95D / 0x00ADB120 | Sig / Target (fallback Sig) |
| CameraRootCall / CameraGetterCall | 0x00C6D5BD / 0x00C6D5C4 | Sig (+0 / +7) |
| CameraRootGetter / CameraGetter | 0x006E8330 / 0x006E8400 | Target |
| DxtEncode1 / DxtEncode5 | 0x006152F0 / 0x006154B0 | Sig (entry) |
| RefPackCompress / RefPackCompressSlot | 0x004EC200 / 0x00FB901C | Sig / SlotsOf(1) |
| RefPackDecompress | 0x004EB3B0 | Sig: the CALL in the stream read 0x004EC010 (not the entry, which S3SS detours); optional |

Run-time checks on every build (not signatures): the read-only class's OpenRecord / base OpenRecord / DeleteRecord bytes
(`CheckReadOnlyClass`); the budget CALL's target and the `D9` / `DD` after it; the getters' shapes (`A1 imm32 C3`,
`8B 41 disp8 C3`) and the eye read `0F 28 40 disp8`; the DXT entries' prologue `55 8B EC 83 E4 F0` (or the entry
chain's own JMP); the RefPack slot holding the write (or the slot chain's outer hook); the first textures / streams of
each session compared with the game.

## Interactions

- **Frame Profiler (dev build):** see "Hooking and the Frame Profiler". The "Resource lookup" counter shows all calls;
  its per-frame text adds "% from cache", the hitch lines ", from cache N" (after "misses", so `agg.pl` still parses
  them: it splits counters on "; "), the report "Resource lookup cache: ..." and "Lot lighting while moving: ...". The
  profiler's "Lot room solve" calls = lot levels (corrected 2026-09-29; it was "rooms").
- **Official Sims3SettingsSetter:** its source was read only to list the game sites it patches; none is touched here.
  Its 51 byte patterns were matched against TS3W.exe: nothing overlaps FindProvider, the resource manager methods,
  0x00736A70, the budget call 0x00ADB95D / 0x00ADB120, the room solve 0x006A8BA0 or the camera read 0x00C6D5BD (its LSO
  detours WorldManager::Update at 0x00C6D570; the camera site is 0x4D bytes later and only read). Its RefPack patch is
  the decompressor 0x004EB3B0, its lighting quality patch hooks 0x006A0E00 / 0x006A4480 / 0x0069FD60 / 0x006A8D20 /
  0x006A8C88 / 0x006A8DE0 (none of ours). A literal scan of `Sims3SettingsSetter.asi`, `Sims3Performance.asi` and
  `MonoPatcher.asi` for the addresses and slots used here found nothing.
- **Sims3Performance 1.0.0-beta1** (its log): hooks 0x00EA3820 (isinst cache), 0x00D7FC60 / 0x00D74800 / 0x00D7F8B0 /
  0x00D80BC0 (simulator stages), 0x0059C5B0 (localized strings), 0x00EBBDE0 / 0x00EA8720 (Mono ehash), the allocator
  arenas, STBL layout, thread stacks, short waits; PackageIndexWarmup only reads the Mods / DCCache files into the OS
  cache (188 files). No resource-manager or lot-lighting site.
- **Night Lights:** it queues room relights (QueueRoom 0x006C7160 on the light tree levels: at world load, dusk, the
  "relight lots" button, lamp edits) and level_light_share re-gathers outdoor rooms; those relights are solved by this
  same budgeted lot lighting update. While the camera moves they now take more frames (the rooms resume where they
  stopped, which the game already does at its own 5 / 10 / 15 ms budgets); nothing in Night Lights waits for a relight to
  finish within a frame. The resource cache does not touch lighting.
- **Old combined build / older S3SSApex.asi:** Apex idles when they are loaded (existing detection); Smooth Streaming
  (combined build) detoured 0x00ADB120's entry, which the call-site redirect would pass through.
- **Frame Profiler and the compression features:** the profiler's "DXT encode" and "RefPack compress" counters are the
  outer layers of the entry chain / slot chain, so they time whichever encoder runs (the fast one when on). A texture
  or stream checked against the game is timed with both inside the counter (the dev check is slower on those calls).
  The profiler's Hooks table says "the fast encoder / compressor is inside".
- **Official Sims3SettingsSetter and compression:** its "RefPack decompressor" replaces the decoder 0x004EB3B0 (it only
  reads streams); the fast compressor's streams use the same format and are checked in game through whatever decoder
  is installed (the game's or S3SS's). Nothing in S3SS touches the compressor, the stream vtable or the DXT encoders
  (plan section 6, literal scan of the ASIs).

## Pitfalls and risks

- **Stale answer after a silent file change (C1, INFERRED risk):** a read-only package whose file changes on disk
  without the watcher noticing, or a closed read-only package re-targeted with SetLocation (slot +0x2C, only allowed while
  closed; only seen at registration) would change what the game finds without a notice. Bounded by the 60 s entry age
  and caught by the development checks.
- **Transient open failure (C1, INFERRED risk):** if a read-only package without its key set fails to open during the
  game's own lookup (file locked by another program), the game answers with a lower package; that answer could be stored
  and served up to 60 s (the game's own resource cache keeps such a resource too). The development checks would log it.
- **Many non-read-only packages above the answers (C1):** each answer probes them all; more than 32 above an answer =
  not stored. The Developer line "N not of the read-only class" shows the count; with `writable` Resource.cfg lines or
  many code-registered databases the gain shrinks.
- **The 60 s re-check** makes each distinct key cost one game lookup per minute (about 0.1 ms per second for 5000 keys).
- **Do not free the table** or move the SRW lock: threads may still be inside the hook after Stop.
- **Do not detour 0x004AFFC0's entry** (the slots are its only references; code bytes would also be seen by the
  game-address self-check) and do not swap the FindProvider slots outside `SlotChain` (the profiler and the cache would
  lose each other).
- **Lights settle later while moving (C7, by design):** the current lot's lights (and lots streaming in) finish over more
  frames while the camera moves; they catch up within ~300 ms of stopping plus the remaining work at full budget. The
  earlier Smooth Streaming "current lot while moving" cap was never tested with non-default values (removed-features.md).
- **Tool mode (C7):** budgets of 100 ms or more are never scaled.
- **DXT translation errors (C9):** the offline test compares the fast path with the translation, not with the game; the
  in-game checks (first 16 textures of every session, dev 1 in 8) compare with the real function. Any "Verification
  mismatch" line: keep the feature off and send the line (it has the block's pixels and both byte strings).
- **DXT and compiler flags (C9):** `dxt_codec.cpp` must be built with legacy SSE (`/arch:SSE2`, the x86 default; it
  refuses `/arch:AVX`) and without `/fp:fast` (it forces `float_control(precise)` and `fp_contract(off)`). Do not
  "optimise" its arithmetic order, replace `rcpss` / `rsqrtss` by divisions, use `rcpps` / `rsqrtps` on four blocks, or
  replace the x87 inline assembly: the output would stop being the game's.
- **DXT edge alpha (C9):** the game's partial-width alpha fetch is wrong (packed rows); it is kept on purpose (bit
  identity). A "fix" would change the output of every image whose width is not a multiple of 4.
- **Other game builds (C9):** the encoders' constants are assumed equal to Steam 1.67.2's; the per-session checks catch a
  difference on the first textures.
- **RefPack stream sizes (C4):** streams are a few percent larger than the game's (bounded search). The package writer
  drops streams above its ratio threshold (stored uncompressed) and the memory caches are size-capped: slightly more
  memory / disk per cached item. The dev comparison and `tools/refpack_test` measure it.
- **RefPack counting-run pairing (C4):** the write that follows a counting run must use the same compressor (the buffer
  was sized by it); the pairing is per thread and matched on (source pointer, size); the fast write then uses the
  counting run's flags (the header then says 128 KB window where the package asked for 16 KB: every RefPack decoder
  accepts it, the game's ignores that bit). If the game ever wrote a counted
  stream from another thread, the write would use the current compressor and could return -1 (safe: stored
  uncompressed), never overflow (the fast compressor checks the capacity; the game's is only used when the capacity can
  hold its bound or the counting run was the game's).
- **Unloading (C4):** after switching the feature off the vtable layer stays up to 2 s; `FreeLibrary` of the ASI in that
  window would leave the slot pointing into unloaded code (ASI loaders never unload; noted for completeness).
- **Do not** swap the RefPack slot or hook the DXT entries outside `SlotChain` / `EntryChain` (the profiler and the fast
  paths would lose each other).

## Testing in game

Faster Game File Lookups (development build, Developer > Profiler > Performance):
1. Turn it on (SYSTEM > Performance). Log: `[SlotChain] ...: layer 1 installed` five times and `[ResourceCache] On: ...`.
2. Load a save, pan and zoom around busy lots for a few minutes, enter CAS, Buy and Build, Edit Town, travel, save and
   load again. Expect: "Checks: N equal, 0 different"; "changes the hooks missed 0"; list changes counted at world load;
   "answered from memory" above ~90% after warm-up; "packages asked" a small number (1 + the non-read-only packages).
3. "Check every answer for 10 s" while panning and while loading a lot: still 0 different.
4. Frame Profiler on (either order): its Hooks table says "outer layer of the slot chain; the resource lookup cache is
   inside"; the "Resource lookup" row shows "% from cache" and its ms per frame drops from ~5 ms to well under 1 ms in the
   same scene. Turn the profiler off and on again: both keep working.
5. Any "Verification mismatch" line in `ApexRadiance_LOG.txt`: keep the feature off and send the line.
6. Only after several clean sessions: flip `enabledByDefault` to true.

Lot Lighting While Moving:
1. On by default; log `[LotLightingMotion] On: the call at 0x00adb95d ...` with the camera offsets (root 0x011d1860,
   +0x24, +0x60 on Steam).
2. Developer line: "Camera moving" while panning, "Camera still" 0.3 s after stopping; "last budget: game 15.00 ms ->
   3.00 ms" while moving over the current lot, "game 5.00 -> 1.00" for other lots.
3. Frame Profiler, camera test as before: the 16-50 ms moving hitches dominated by "Lot room solve" should drop; "Lot
   room solve" per hitch ~3 ms instead of ~15.
4. Visual: after stopping, the current lot's lights and a newly loaded lot's lights settle within about a second; try 1,
   3 and 6 ms. At night with Night Lights: lamps placed / removed still relight their rooms (a moment later while the
   camera moves).
5. Build / Buy mode and Edit Town still relight at once when still.

Offline tests first (console only, no files written; x86 Native Tools Command Prompt for VS 2022, or after
`"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat"`):
```
cd /d C:\Users\luiz_\Desktop\S3SS-dev\S3SSApex\tools\dxt_test
cl /nologo /O2 /EHsc /std:c++20 /arch:SSE2 /fp:precise /I..\..\features dxt_test.cpp ..\..\features\dxt_codec.cpp /Fe:dxt_test.exe
dxt_test.exe                      (10 million blocks per format; --blocks N, --seed N, --bmp file.bmp)
cd /d C:\Users\luiz_\Desktop\S3SS-dev\S3SSApex\tools\refpack_test
cl /nologo /O2 /EHsc /std:c++20 /I..\..\features refpack_test.cpp ..\..\features\refpack_codec.cpp /Fe:refpack_test.exe
refpack_test.exe                  (--quick; --file path for real data)
```
Expected: "RESULT: all blocks identical" and "RESULT: every round trip and stream check passed"; the timing lines and
the size / speed table against the game's compressor go into the report.

Faster Texture Compression (development build, Developer > Profiler > Performance):
1. Turn it on. Log: `[EntryChain] DXT1 encoder (0x006152f0): layer 1 installed ...` (twice) and `[FastDxt] On: ...`.
2. Play: load a save, enter CAS, change outfits, pan over lots at different distances, change the season / weather, take
   a screenshot or a video capture. Expect "Checks: N equal, 0 different" growing (1 texture in 8 plus the first 16).
3. "Check every texture for 30 s" in CAS and while panning: still 0 different. Note "checked textures: game X ms, Apex
   Y ms (Zx)".
4. Frame Profiler on (either order): its Hooks table says "outer layer of the entry chain ...; the fast encoder is
   inside"; the "DXT encode" per-hitch ms should drop by about the measured factor.
5. Visual: identical by construction; any difference would be a mismatch line in the log.
6. After several clean sessions: flip `enabledByDefault` to true (public build: only the first 16 textures are checked).

Faster Cache Compression (development build):
1. Turn it on. Log: `[SlotChain] RefPack stream write: layer 2 installed ...` and `[FastRefPack] On: ... the game's
   decoder 0x004eb3b0`.
2. Play as above plus save the game (the package writer's counting runs) and load it again. Expect "Checks: N equal, 0
   different" (every stream is checked by default), "writes after our counting run" > 0 after a save, "did not fit" 0.
3. Set "Also run the game's compressor on 1 stream in N" to 4 for a few minutes: note "size +x%, y.yx faster" (this
   slows those calls down; set it back to 0).
4. Frame Profiler: "RefPack compress" per-hitch ms should drop. Load the saved game after turning the feature off again:
   it must load normally (the streams are standard RefPack).
5. After several clean sessions: flip `enabledByDefault` to true.

## Open items

- Negative caching (the game's misses retry with a transformed key, `0x007D8110`): not done; misses were 0 in the
  measurement.
- Per-package write hooks for the writable DPF / memory databases (so they need no probe per answer): only if the
  Developer line shows many of them.
- Why the current lot relights continuously while the camera moves (plan section 10, question 2).
- How often the game registers / unregisters packages or changes priorities while playing (lot streaming, travel, CAS):
  each one empties the cache. The Developer line "list changes" shows it; if it is frequent, a targeted invalidation
  (only entries at or below the changed index, after probing the new package) would keep the rest.
- Texture compression: the images are encoded on the calling thread. Splitting a large texture's block rows over
  worker threads would divide the remaining time again (bit identity is unaffected: blocks are independent); not done
  (thread management inside a game hook). Measure first how much "DXT encode" is left with the feature on.
- Texture compression: an AVX2 path (8 blocks per register) would need proof that the VEX forms of `rcpss` /
  `rsqrtss` give the same results as the legacy ones on the user's CPU; not done.
- Cache compression: the search depth (default 32) and the "nice length" (96) are chosen without data from the game;
  pick them from `tools/refpack_test`'s sweep and the dev comparison (size vs time).
- Cache compression: the package writer compresses each resource twice (counting run, then the write); caching the
  counted stream per thread and copying it on the write would halve its cost (needs the buffer ownership rules checked).
