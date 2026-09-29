# Terrain relight (lot lamps in the terrain bake, story gate, dusk rebuild, reconciliation)

> **Status in the standalone:** only the v0.1.0 part is in the standalone: visitor 0xC29626, the 3 arm sites, the dusk
> kick, the experimental switches and the dev buttons, with full rebuilds (never the reconciliation). Its triggers were
> reworked on 28/09 ("Standalone triggers" below, **not yet tested in game**): the load rebuild waits for the world to be
> drawn and a steady night level and merges with the dusk rebuild; lamp additions and removals count like edits (only on
> lots already loaded), coalesced (250 ms quiet, at most one rebuild per 3 s, night only); Apex's own countdown is 3
> frames instead of 50; "Lot lamps light the street" applies live; a reinstall keeps the world state; the game's per-chunk
> texture re-render (call `0x00C8504C`) reports changed chunks to the smoothed maps. v0.1.0 had: a load kick 5 s after
> "world loaded", a lamp-edit kick 0.7 s after an existing lamp's signature changed, and at night with c38 == 0 and
> c3C <= 0 a "lights changed" kick after 120 frames when `g_lotLampArms` changed, at most every 15 s (kept as a fallback).
> **Not in the standalone yet (post-0.1.0, re-add one by one after user tests):** the story gate 0xC294D9
> (`BakeLevelStub` / `BakeLevelTest` and its counters), the reconciliation (`Bakeable`, `DiffBaked`, `Reconcile`,
> `Settle`, `ForEachOutdoorLotLamp`), the setting `relightLocal`, and the localized relight
> `SmoothStreamingRelightTerrainRects` (Smooth Streaming itself did not exist in v0.1.0). Nothing of 28/09 rebuilds on
> streaming churn (the reconciliation's 12-60 ms hitches, [../../changes-since-0.1.0.md](../../changes-since-0.1.0.md) 2.1).
>
> ### Standalone triggers (28/09)
>
> | Trigger | Rule | Signal / code |
> |---|---|---|
> | World load | On a new cells pointer: log `World loaded (...)`, clear chunk maps. The rebuild is armed only when the world is **live** (the lot light bridge recorded a world terrain chunk draw after the change, `LotLightBridge::ChunkCount() > 0`; fallback 30 s after the change when no draw is recorded, e.g. "Street lamps light lots" off), 1 s after that, and once the night level moved less than 0.02 for 1 s (at most 20 s after live). At night the reason is "world load (night: also the dusk rebuild)": a dusk rising edge while the load rebuild waits is merged (log `Dusk during the world load: merged into the load rebuild`) and a waiting dusk kick is cancelled: **one** rebuild | `OnPresent`, `g_live`, `g_levelRef` |
> | Dusk | Night level crosses 0.99 upwards, `automaticoAoAnoitecer`: kick after `atrasoSegundos` (unchanged) | |
> | Lot lamp changes | `LotLightBridge::TrackLotLampEdits` (every 20 frames) compares the lamp SET: only lamps the bake takes, only changes the bake shows, with the streaming rules of [lot-light-pass.md](lot-light-pass.md) "Lot lamp change tracking". Decided once nothing changed for 250 ms; only at night (by day lamps are unlit and not baked: left to the dusk rebuild) unless `automaticoAoAnoitecer` is off; merged into a pending load / dusk / armed rebuild. **29/09:** user-driven changes (placed / moved / removed) at most one rebuild every 3 s, automatic ones (switched, dimmed, recoloured) at most once per 30 s after the last rebuild; both skipped when the bake's lamps equal the last rebuild's snapshot (a rebuild after the change, the game's own included, drops the kick) and deferred while the camera moves: "Lamp change decisions" below | `g_editKickPending`, `NoteEdit`, `DecideEdit`, `kEditQuiet`, `kEditMinInterval`, `kAutoMinInterval` |
> | "Lot lamps light the street" switch | `luzDoLoteNaGrama` is read at run time by `TerrainLightTest` / `ArmTest`, whose patches are now installed whatever the option (with it off they answer exactly like the game). Toggling it goes through the lamp-change rule above (one rebuild at night). A reinstall is only needed if its code bytes could not be installed | `g_lotLampsSeen`, `installedLotLampCode` |
> | Night Lights turned on again in the same world | No new-world handling; one lamp-change rebuild at night ("Night Lights turned on") | `Install` |
> | Stuck countdown | v0.1.0 fallback kept as a trigger: night, c38 == 0, c3C <= 0 for 120 frames, arms changed, looked at most every 15 s. **29/09:** it no longer kicks by itself: it queues an automatic lamp change (reason "lights changed (stuck countdown)"), so it rebuilds only if the bake's lamps differ from the last rebuild's snapshot, at most once per 30 s, camera still. The same state can never rebuild twice (the old session's 15 s cadence of ~240 ms frames). A lamp-change rebuild updates its bookkeeping so it does not fire again for the same arms | `g_stuckFrames`, `g_armsAtLastStuckKick` |
> | Button | "Rebuild terrain light now" | |
>
> Apex's kick writes **3** (`kArmFrames`, was 50) to cells+0x38/+0x3C; the game's own arm sites still write 50. Every Apex
> kick is already debounced, so the extra 50 frames only delayed the result by ~0.8 s. Risk: none known; the decrement
> (`0x006B5DA0`) and the consume (`0x00C84C1B`) treat any positive value the same.
>
> A reinstall (`ReinstallNow`, developer options that change code bytes) keeps `g_lastCells`, the chunk maps, the smoothed
> maps and the atlas (`LotLightBridge::Shutdown(true)`); a real uninstall keeps `g_lastCells` (installing again in the
> same world is not a world load) but releases the maps.
>
> Every armed rebuild also tells the smoothed maps (`LightmapSmooth::NoteKick`, `ExpectRebuild`, `OnTerrainRebuilt`:
> [world-atlas-and-smoothed-maps.md](world-atlas-and-smoothed-maps.md) "Rebuild sweep"). Developer log (dev build):
> `World live: <signal> after X s`, `Night level crossed 0.99 upwards|downwards (...; up U / down D)`, `Lamp change:
> <reason>: rebuilt | merged into ... | left to the dusk rebuild (day)`, `Terrain rebuilt (...) N ms after it was armed`.
> Developer status: "Terrain: armed / rebuilt / last: reason: armed -> rebuilt ms", "World load: <signal>; rebuilt X s
> after the world change | night level crossings", "Lamp changes: counted / ignored / lots tracked / last", "Chunk
> re-render notices".
>
> ### Lamp change decisions (29/09, not yet tested in game)
>
> **Why.** research\perf2\round3.md section 5: the worst stutters (~240 ms: terrain update ~172 ms + DXT encode ~56 ms
> in one frame) are full terrain rebuilds; in the 29/09 00:37 session 5 of 8 were armed by Apex ("lot lamps changed" /
> "lights changed"), two of them about 1 s after a rebuild the game made itself; the older session froze every 15 s
> (the stuck-countdown fallback). The same lots reported "4 edited" / "7 edited" again and again with nobody building.
> Diagnosis from `ApexRadiance_LOG.txt` + `ApexRadiance_LightDiag.txt` (same world, 28/09 19:53):
> - lot 7D6F0019FAF78910 ("7 edited") has exactly 7 outdoor type-3 lamps, all **disabled** (flags 0x35 / 0xB5, no 0x40):
>   `TerrainLightTest` never bakes them, yet the old tracking counted them (it did not check 0x40);
> - lot 6C11001B182E9ED0 ("3 edited"): 3 type-11 lamps (flags 0xB5, disabled) and 3 type-5 (no 0x04, not tracked);
> - lot 4522001BE6BBCA40 ("4 edited") and 6C11001BA7277E30 ("2 edited"): lit (0x20), enabled lamps of types 3..5 with
>   **intensity 0.00** next to others at 1.00: lamps whose intensity the game switches (0 = off in the bake: the bake
>   weight is range x intensity x 0.2) - inferred, the new per-lamp log line says which field moves;
> - lot 6C11001B18CA1000 ("7 edited"): magenta / blue lamps (colour (1, 0, 1), (0, 0, 1)) and type-9 lights: likely
>   colour-cycling lights (inferred);
> - the old signature compared the raw bits of colour, intensity, lit flag and position, so any flicker counted.
>
> **What the bake uses** ([../../engine/terrain-and-light-bake.md](../../engine/terrain-and-light-bake.md) 4.2): lights
> accepted by the visitor (street-lamp class type 0xB; lot lamps 3..6 outdoors, enabled, lit through
> `TerrainLightTest`), their rect +0x134 (from position and range), position (vfunc+0x24), colour +0xF0 and the weight
> range +0x130 x intensity +0x10 x 0.2. Not the fade +0x20, not the effective colour +0xE0. So the tracking and the
> snapshot compare "light" = colour x intensity x range per channel, the position, and whether the lamp is in the bake
> (lit, enabled for 3..6, light not zero). Whether the street-lamp class test (vfunc+0x20) needs the lit flag is
> unverified; it is assumed (a save loaded by day keeps a lamps-off terrain light).
>
> **Rules** (`NoteEdit` / `DecideEdit` in the patch, `TrackLotLampEdits` / `DiffBake` in `lot_light_bridge.cpp`):
> 1. Only bake changes count (lot-light-pass.md "Lot lamp change tracking"): a lamp of the bake added / removed / moved
>    more than 5 cm ("user-driven"), a lamp entering or leaving the bake, or its light changing by more than 5 % per
>    channel ("automatic"). A lamp with 3 automatic changes within 60 s is **animated**: its changes never trigger, its
>    state goes into the next rebuild made for another reason (log line "switches or dims by itself").
> 2. **Snapshot of the last rebuild.** Whenever a rebuild is consumed (Apex's or the game's), the bridge enumerates the
>    lights in that same frame (`RequestLampRefresh`) and the next frame stores the snapshot `g_baked`. A pending change
>    is then compared with `DiffBake(g_baked, current)`: only on lots settled now that were in that snapshot (lots that
>    streamed in later were never baked, and streaming never rebuilds), lamps matched by type and place (5 cm) rather
>    than pointer. No difference -> **skipped** ("the terrain was rebuilt after the change" when a rebuild ran at most
>    2 s before the change was seen, else "no change the terrain bake uses"). A user-driven change on a lot the snapshot
>    did not have still rebuilds. No snapshot (light enumeration unavailable): decided without the compare.
> 3. A rebuild consumed while a change waits takes it: the change is dropped ("the game rebuilt the terrain itself" /
>    "the <reason> rebuild just ran").
> 4. **Camera still.** No lamp-change kick while the camera eye moved within the last 1 s (eye `[[root]+0x24]+0x60`,
>    offsets parsed from the code like `LotLightingMotion`; moving = more than 2 cm from the last reference, so slow
>    orbits add up; eye unknown = still). The change stays pending ("deferred: camera moving").
> 5. **Rate limits.** User-driven changes and the switches: at most one rebuild every 3 s (~0.5-1 s latency otherwise).
>    Automatic changes and the stuck-countdown fallback: at most once per 30 s after the last rebuild of any kind
>    ("rate-limited"); the change stays pending and is re-checked (it is dropped if the lamps go back meanwhile).
> 6. "Lot lamps light the street" and "Night Lights turned on" are switches: fast path, no compare.
>
> Developer log (dev build), one line per decision: `Lamp change: <reason> (<user-driven|automatic|switch>): rebuilt
> (<diff>)` | `skipped: the terrain was rebuilt after the change` | `skipped: the game rebuilt the terrain itself` |
> `skipped: no change the terrain bake uses` | `deferred: camera moving` | `rate-limited: ...` (deferrals once per
> state); plus the bridge's `Lot lamp change: lot X: A added, E edited (M moved), R removed (user-driven|automatic):
> L... type T: lit 1->0, intensity 1.000->0.000 [leaves the bake]; ...` and, at most once a minute per lot, `Lamp changes
> that do not rebuild the terrain, lot X: ... (not in the bake | below the threshold | animated)`. Developer status line
> "Lamp change decisions: rebuilt U user-driven / A automatic | skipped: ... | deferred: camera C, rate-limited R |
> pending: ... | camera: still/moving | last rebuild's lamps: N lamps on K lots, taken S s ago | last: ...".
>
> **Expected effect** (estimate from the 29/09 log): the "7 edited" / "3 edited" lots (disabled lamps) stop counting;
> flickering lamps become animated after 3 changes; the rest rebuilds at most once per 30 s and never twice for the same
> lamps, never right after a game rebuild, never while the camera moves. From about 20 Apex rebuilds in 20 minutes of
> night play to a few.
>
> **Risks / limits.** An automatic change that matters (a lamp switched on by a timer) can take up to 30 s (+ camera
> still) to reach the ground; an animated lamp's ground light follows it only at other rebuilds; a Build-mode recolour
> takes the automatic path (30 s); the first lamp placed on a lot that had no lamp of these types is not seen (its lot
> looks like one streaming in; before 29/09 the stuck fallback caught it within 15 s): it reaches the ground at the next
> rebuild (another change on a known lot, dusk, the button). Whether the game rebuilds by itself after Build-mode lamp
> edits (the "by the game itself" rebuilds of the log) is not known.
>
> ### Local terrain relight and paced sweep (29/09, developer toggles, default off, NOT yet tested in game)
>
> **Why.** A full rebuild (`chunk+0x55` on every chunk) bakes and DXT-encodes every chunk **synchronously in the consume
> frame** (`0x00C83060` → `0x00C7E7A0` at `0x00C8307E`), then sets `+0x54` so the game's sweep renders every chunk a
> second time: MEASURED 229-241 ms frames (#927: terrain 173 ms + DXT 57 ms, 512 DXT calls = 64 chunks x 2 textures x 4
> mips) plus ~3.5 ms per frame for 64 frames. The combined build's local relight used `+0x55` on 1-25 chunks: the same
> synchronous work per chunk, hence its 12-60 ms hitches. `+0x54` alone is the game's one-chunk-per-terrain-update sweep
> branch (`0x00C85041`, the call `0x00C8504C` that `ChunkRenderThunk` already redirects): ~3.5 ms for one chunk, no
> geometry, no road mark, no duplicate. Evidence: research\perf2\chunkrelight.md; [../../engine/terrain-and-light-bake.md](../../engine/terrain-and-light-bake.md) 2, 3.3, 4.1.
>
> **Phase 1, "Relight only nearby terrain"** (TOML `relightNearbyChunks`, developer build only, default **false**; module
> `features/terrain_chunk_relight.cpp`, namespace `ChunkRelight`). In `DecideEdit`, after the snapshot compare and before
> the camera / rate checks, a lamp change (not a switch, and only with a snapshot) goes to `TryLocal`:
> 1. Lamps: `DiffBake` now returns every counted difference (`BakeDiff::changes`) with the rect the lamp had in the bake
>    (old) and the one it has now (new): light `+0x134` {minX, minZ, maxX, maxZ}, stored in `BakeLamp::rect` by the lamp
>    tracking. A user-driven change on a lot the last rebuild did not have is refused (full path): its lamps may have
>    been baked by LOD transitions since, but their old places are unknown, so a removed or moved lamp would leave its
>    old light. A rect that does not hold its lamp's place (+-1 m) is refused too (the rect updaters 0x006BDE66 /
>    0x006BE816 / 0x006BE8AB are not verified to run in the call that moves the lamp).
> 2. Pacing: user-driven changes (placed, moved, removed) are queued at once (no camera wait, no 3 s interval).
>    Automatic changes wait for the camera to be still and relight the same lamp at most once per **5 s**
>    (`kLocalAutoPerLamp`, instead of 30 s after any rebuild).
> 3. Selection (`QueueLocal`): for each rect + 1 m, the grid cells it covers plus one cell on each side; each chunk there
>    is kept when its bake record (`*(*(terrain+0x68)+8)`, the game's own test) overlaps the rect + 1 m. The lamp's own
>    chunk first, then its other chunks nearest first; deduplicated across lamps.
> 4. Validated at use (any failure = "refused", the full path takes the change with today's limits until a new change
>    comes in): WorldManager from `0x011ECBC4`, terrain = WM + disp8 of `0x00C6D68C` (0x58), terrain+0x14 == WM; world in
>    live play (WM+0x1B4 != 0); vector size == nx x nz (terrain +0xC0 / +0xC4), cell +0xC8 == 256; per chunk: corner
>    multiple of 256 and at its slot `(z0/256)·nx + x0/256`, size 256, centre == corner + 128, rect == corner..+256; the
>    world's first rebuild seen (any consumed rebuild; normally the load rebuild) and a sweep render of this terrain seen
>    by the thunk; `0x00C7E7A0`'s early-exit gates open (below); every chunk has a rebuilt light map (`+0xD8 != 0`); at
>    most **16 chunks** and a quarter of the world; at most 32 queued; rects finite and at most 4096 m.
> 5. Release (`ChunkRelight::OnPresent`, every frame after `DecideEdit`): one chunk in flight at a time; its `+0x54` set
>    only when the previous one finished, never in the frame right after a chunk rendered (a free frame in between), at
>    most 8 releases in any 1 s, never while any chunk has `+0x55` or `+0x56` (a full rebuild in progress), never while
>    the gates are closed. Gates mirrored from `0x00C7E7A0`: live and TerrainData (terrain+0x64) `+0x1D == 0` → closed;
>    `[WM+0x54] ? [[WM+0x54]+8] : 0` (= `0x00C61040`) != 0 and TerrainData `+0x20 == 0` → closed; byte
>    `[[TerrainData+0x0C]+0x6C] != 0` → closed (the sweep branch itself is skipped at `0x00C85011`, value read at
>    `0x00C8471A..0x00C8473A`; meaning unknown, probably an edit or tool state). (A chunk flagged while
>    closed would stall the game's whole per-chunk loop: `0x00C7E7A0` returns without clearing `+0x54` and the branch
>    still sets "work done".)
> 6. Completion: the thunk sees the chunk rendered (`+0x54` back to 0 after the call) and passes its QPC time; or
>    `OnPresent` finds `+0x54 == 0` (rendered by another game path, e.g. `0x00C83060` during a LOD change: counted "by
>    another game path", no time). **Timeout**: still set after 120 frames during which no other chunk had `+0x54`,
>    `+0x55`, `+0x56` or `+0x50` pending and the gates were open (or 1200 frames in all) → `+0x54` put back to 0 if Apex
>    set it and no rebuild is in progress, queue dropped, one full rebuild (reason "local terrain relight failed
>    (...)"). Frames count Presents, not terrain updates (a stretch without terrain updates is not excluded), so the
>    first timeout in a world keeps the local path; the second turns it **off for this world**. "The terrain changed
>    under the queue" or a chunk that changed while queued turn it off at once.
> 7. Bookkeeping: while a batch is queued the next lamp change waits ("waiting for the terrain relight in progress").
>    When every chunk of a batch rendered, each changed lamp takes in `g_baked` the state it had when the batch was
>    decided (`LotLightBridge::CoverLots`); the lots' other lamps keep their baked state, so small changes that were not
>    relit still add up to a relight. A consumed rebuild (Apex's or the game's)
>    drops the queue (its snapshot covers everything); a world change resets it; an uninstall / reinstall drops it and
>    decides the change again.
> 8. The smoothed maps are not held (`ExpectRebuild`) for a change the local path is likely to take: the few re-rendered
>    chunks are reported by the thunk and smoothed at once.
>
> **Phase 2, "Paced terrain sweep"** (TOML `relightPacedSweep`, developer build only, default **false**). Apex's own dusk
> rebuild and its lamp-change rebuilds (a change the local path refused or did not take, a switch, the stuck-countdown
> fallback) become `QueueSweep`: every chunk (all must have `+0xD8`), nearest to the camera eye first, same release rules
> (so 64 chunks take about 8 s). It starts like a consumed rebuild (snapshot of the lamps now, `g_lastRebuildAt`,
> pending local batches dropped). Not possible → the kick. The **world-load rebuild and the button stay full
> rebuilds** (the load rebuild creates the light maps and fixes the world-file chunk borders; the local path needs it).
>
> **Developer log**: `[ChunkRelight] WorldManager global 0x011ecbc4, terrain at +0x58` (install); `Lamp change: <reason>
> (<kind>): relit locally (<diff>; N lamps, chunks (ix,iz) ...)`; `Local relight done: N lamps, chunks ...: K chunks in F
> frames, M ms per chunk (max X)`; `Lamp change: <reason>: local relight not possible (<why>): full rebuild path`;
> `Terrain sweep started: <reason> (N chunks, nearest to the camera first)` / `Terrain sweep done: ...`; warning
> `[ChunkRelight] <why>: queue dropped, local terrain relight off for this world`. **Developer status** (Developer >
> Lighting, under "Chunk re-render notices", with the two checkboxes): "Local terrain relight: relit locally U / A, done,
> refused (last: why), failures | paced sweeps" and "Terrain chunks: <ready | why not> | local relights, sweeps, chunks
> re-rendered (by another game path), refused, failures | queue, in flight (ix,iz) for K frames | chunk render: last /
> average / max ms | waits: rebuild flags, terrain not ready, 8 per second | sweep renders seen | last".
>
> **Default off, why**: the mechanism is the game's own sweep branch and every assumption is checked at use, but nothing
> of it has run yet, and `+0x54` does not refresh the road partition mark (`0x00B789B0`): roads should see the in-place
> light map, not verified. Turn it on by default only after the in-game checks below.
>
> **To verify in game** (dev build, both toggles on in turn): (1) the install log line above; (2) Build mode at night:
> place, move, delete and recolour a lot lamp: log `relit locally ... chunks (ix,iz)` with the expected chunks, the
> Hitches file shows no `DXT x512` frame, only a few frames with `DXT x8` and terrain ~3 ms, every other frame; grass,
> lot grass, **roads**, sidewalks, snow and fences show the change; (3) a lamp near a chunk border (x or z = k x 256, e.g.
> 1280): both chunks are relit back to back, no lasting step once the 30-frame smoothing settle passed; (4) map view,
> CAS, Edit Town, save / load: no "was not re-rendered within" warning, and the game's own sweep still completes
> (LightmapSmooth "Rebuild sweep done ... 0 not re-rendered"); (5) 20 minutes of night play while moving: local relights
> per minute, none by day or from streaming; (6) a game rebuild while a batch is queued drops it; a lamp change during
> the load rebuild takes the full path ("the world's first terrain rebuild has not run yet"); (7) paced sweep at dusk:
> "Terrain sweep started / done", chunks near the camera light first, no ~240 ms dusk frame.
>
> Expected latency (estimates; the game re-renders one chunk per frame after a consumed rebuild, in its own chunk order):
> save load: rebuild armed ~1-2 s after the world is first drawn (was 5 s after the cells change, often inside the
> loading screen, then a second dusk rebuild 2 s + 50 frames later); dusk: `atrasoSegundos` + 3 frames; Build-mode lamp
> add / move / remove at night: up to 20 frames (enumeration) + 250 ms + 3 frames (+ 20 frames more for a removal, which
> is confirmed at the next enumeration; since 29/09 + until the camera has been still for 1 s; a switch / dim / recolour:
> up to 30 s after the last rebuild), then the chunk's turn in the sweep (up to ~1 frame per world chunk); menu toggle
> of "Lot lamps light the street" at night: ~250 ms + 3 frames (was 2 s debounce + reinstall + a full new-world cycle).

> The game-code half of Night Lighting, in `patches/night_terrain_relight_patch.cpp`. It makes outdoor **lot** lamps
> part of the world terrain light bake (visitor patch 0xC29626 and, since 28/09, the **story gate** 0xC294D9 so lamps
> of houses on foundations count), makes the game rebuild the terrain light when the lamps switch on at dusk, and keeps
> the terrain light up to date with the lot lamps through a once-a-second **relight reconciliation** that relights only
> the chunks around changed lamps. Status: visitor, arm sites and dusk rebuild **working** since 24/09; story gate and
> reconciliation **installed 28/09, not yet confirmed in game** (see Open items). Both flavours; the counters and the
> "Rebuild terrain light now" button are dev only. Part of [Night Lighting](README.md).

## Purpose

Reverse-engineering summary (header comment of the patch file, Steam 1.67.2.024037; background in
[../../engine/terrain-and-light-bake.md](../../engine/terrain-and-light-bake.md)):
- World terrain chunks are relit by `FUN_00c845c0` (per frame, render thread). When the light cells' countdown fires it
  sets byte **chunk+0x55** on every chunk; that rebuilds the chunk and its light textures (`FUN_00c834f0` /
  `FUN_00c83060` / `FUN_00c7fa70` / `FUN_00c7e7a0` / `FUN_00c25a90`, bake `FUN_00C292B0` "staticTerrainLightmap").
- cells = *(lightMgr + 0x104), lightMgr = *(*(0x011D1860) + 0x1C0). **+0x38 / +0x3C** are countdowns set to 50 by
  `FUN_006b64b0` (register), `FUN_006b6090` (unregister), `FUN_006b6590` (move / toggle) (correction from the
  disassembly: those three write only +0x38; +0x3C, the one that triggers the rebuild in normal play, is written only by
  `FUN_006b5730`, see the resolved discrepancy under "Game addresses"), decremented each frame by
  `FUN_006b5da0`, reset to -1 by `FUN_006b5770` when consumed.
- Picking up a street lamp in Build mode arms the countdown (verified with a call trace: 128 chunk rebuilds and 243
  light-texture rebuilds follow). The night-level setter `FUN_006add60` switches lamps on at dusk but never arms it, so
  a save loaded by day keeps the "lamps off" terrain light.
- Only lights whose vfunc+0x20 returns 1 (class 0xFF42F8, type 0xB "world light") can arm the countdown or enter the
  bake: the visitor at 0xC29620 (vtable 0x010768A0) filters with that vfunc. Lot lamps (types 3..6) never reach the
  world terrain: hard edge around lots.
- **Story gate** (28/09, captures probe2_clara / probe2_escura + S3SS_LightDiag): after the visitor, `FUN_00C292B0`
  drops every light of a lot (lot id != 0) whose storey light+0xD0 != 0 (`cmp dword [edi+0D0h],0; jnz skip` at
  0xC294D9, EDI = light). On a house built on a foundation the ground floor is storey 1, so its outdoor wall and porch
  lamps (room 0, lit, d0 = 1/2 in the diagnostic) lit the lot grass through the lot light map but never the world grass:
  a straight cut along the lot border that **no terrain rebuild changes**. Evidence: lot 6C11001B51A4D8B0, origin
  (900.8, 59.41, 1185.6), rotated 20 degrees; lit pixel at local x +0.70, dark at -0.05 (the exact border); all lit lot
  lamps type 4 with d0 = 1/2; atlas = 0 at those positions. This is why "Rebuild terrain light now" changed nothing and
  only some lots had the cut.
- The terrain maps baked into the world file also miss the part of a lamp's light that crosses into the neighbouring
  256 m chunk (straight cut on world grass at chunk borders, `LightProbe-grama2`); a rebuild fixes it.

## User-facing settings

| UI label | TOML key | Type | Default | Range | UI | Notes |
|---|---|---|---|---|---|---|
| Lot lights light the ground outside the lot | `luzDoLoteNaGrama` | bool | true | | Main | Standalone (28/09): live; the visitor and arm-site patches are always installed and read it at run time; toggling it rebuilds once at night. (Combined build: installs the patches, reinstall on change.) The checkbox also sets `automaticoAoAnoitecer` to the same value |
| Update automatically at dusk | `automaticoAoAnoitecer` | bool | true | | Adv / Dusk | live |
| Delay after dusk | `atrasoSegundos` | float | 2.0 | 0.5..10 s | Adv / Dusk | live |
| Relight only around changed lamps | `relightLocal` | bool | true | | Adv / Dusk | combined build only (not registered in the standalone); off = changes get a full rebuild (at most every 15 s) |
| Relight only nearby terrain | `relightNearbyChunks` | bool | false | | Dev | standalone 29/09, developer build only (the public build never registers it: off); live; see "Local terrain relight" |
| Paced terrain sweep | `relightPacedSweep` | bool | false | | Dev | standalone 29/09, developer build only; live; Apex's dusk and lamp-change full rebuilds become a paced sweep |
| Street lamps count as lit in lot light solves | `postesAcesosNoCalculo` | bool | false | | Dev | 0x6BE18C; see [lot-light-pass.md](lot-light-pass.md) |
| Recalculate every lot at dusk | `recalcularLotesAoAnoitecer` | bool | false | | Dev | re-solve room 0 of every lot after the dusk rebuild |

Dev buttons (Developer > Tools): **"Rebuild terrain light now"** (`g_kickRequested`: arms both countdowns like a lamp
pick-up; its hint says it "runs at night or in Build mode", which is stale: the game consumes it within 50 frames,
day or night, see the resolved discrepancy below) and **"Recalculate lot light now"** (`g_relightLotsRequested`:
queues room 0 of every loaded lot storey).

## How it works

### Install (`NightTerrainRelightPatch::Install`)

1. Root getter check at 0x006E97B0 (`A1 <imm32> 85 C0 75 01 C3 8B 80 C0 01 00 00`, imm32 masked); `g_rootPtrAddr` =
   imm32. Fail otherwise. `LightDiag::Init()` (warning if unavailable).
2. If `luzDoLoteNaGrama`:
   - validate the visitor site and the 3 arm sites (Fail if different);
   - validate the story gate: 7 bytes at 0xC294D9 plus `kBakeLevelBefore` (11 bytes at 0xC294CE:
     `8B CF E8 4B 2B A9 FF 0B C2 74 0D` = `mov ecx,edi; call 0x6BC020 (lot id); or eax,edx; jz keep`) and
     `kBakeLevelAfter` (6 bytes at 0xC294E0: `0F 85 D6 00 00 00` = `jnz 0xC295BC`). Optional: on mismatch only the
     warning `[NightTerrainRelight] Terrain bake story test differs at 0xC294D9: lot lamps above the ground story stay
     off the world grass`;
   - write the visitor: `mov esi,ecx; push edi; call TerrainLightTest; nop x3` (11 bytes);
   - write each arm site: `push edi; call ArmTest; nop x3` (9 bytes);
   - write the story gate: `call BakeLevelStub; nop; nop` over the 7-byte `cmp`, the `jnz` kept;
     `g_bakeLevelInstalled = true` (else warning "Could not patch the terrain bake story test (0xC294D9)").
3. Room queue check at 0x006C7160 (Fail otherwise), experimental patches, Present hook, the other modules
   ([README](README.md)). Log `[NightTerrainRelight] Installed (at dusk=..., lot lights on the ground=..., delay=...s,
   root=0x...)`.

### Light predicates (called from game code)

| Function | Replaces | Accepts |
|---|---|---|
| `TerrainLightTest(light)` (visitor 0xC29626) | vfunc+0x20 | world lights (original test), or, with `luzDoLoteNaGrama`, `IsOutdoorLotLamp` and **lit** (+0x100 & 0x20). Counters "on the ground" (`g_lotLampsBaked`) and "off" (`g_lotLampsSkippedOff`) |
| `ArmTest(light)` (0x6B6516, 0x6B60D3, 0x6B6618) | vfunc+0x20 | world lights, or outdoor lot lamps (counted in `g_lotLampArms`, "armed"); the terrain need itself is decided by the reconciliation |
| `BakeLevelTest(light)` via `BakeLevelStub` (0xC294D9) | `cmp dword [edi+0D0h],0` | storey 0: kept (as the game; counters "ground story" and, for outdoor lot lamps, "lot lamps"); storey > 0: kept only if `luzDoLoteNaGrama`, `IsOutdoorLotLamp` and lit ("upper stories"); anything else refused ("refused": basements and other lights of upper storeys) |

`IsOutdoorLotLamp(L)`: lot id (+0xC0 | +0xC4) != 0, type +0xB0 in 3..6, flags +0x100 with 0x01 alive, 0x40 enabled,
0x04 room known, and room +0x08 == 0 (outdoors). All reads inside `__try`.

`BakeLevelStub` (naked): `push eax/ecx/edx/edi; call BakeLevelTest; cmp al,1; pop edx/ecx/eax; ret`. `cmp al,1` sets
ZF = 1 to keep the light, exactly what the original `cmp [edi+0D0h],0` did for storey 0; pops do not touch the flags,
so the kept `jnz` behaves as before; every register is preserved.

### Per frame (`OnPresent` in the patch file, render thread = the game's light and terrain update thread)

1. `ReadLightState`: root -> lightMgr (+0x1C0) -> cells (+0x104), level = lightMgr+0xF0. None: status "Waiting for the
   game to load a world". night = level > 0.99. Read countdowns c38 / c3C.
2. **New world** (cells pointer changed): clear the reconciliation state, log `World loaded (night level x)`,
   `LotLightBridge::OnWorldChanged()` (drops chunk maps, smoothed maps, atlas), `LevelLightShare::OnWorldChanged()`,
   start **settling**.
3. **Rebuild consumed**: previous c38 >= 0 and now -1. Count "rebuilt", log `Terrain rebuilt (<reason>; night level
   x)` (reason "by the game itself" if the mod did not arm it), `MarkAllCovered(reason)` (the bake now has every lamp
   as it is). If night, `recalcularLotesAoAnoitecer` and the reason is "dusk": lot relight 3 s later.
4. **Dusk**: `automaticoAoAnoitecer` and night rising edge -> schedule a kick after `atrasoSegundos`. When due and still
   night: `Kick(cells, level, "dusk")`; with `recalcularLotesAoAnoitecer`, a fallback lot relight 6 s later.
5. `Settle` while settling; the button kick (reason "button").
6. `Reconcile(s, fullPending)` with fullPending = c3C > 0 || kick scheduled || settling.
7. Lot relight (button or scheduled): `QueueAllLotOutdoorRooms(lightMgr)` walks the lot tree
   (lightMgr+0xD4, buckets +0x58, count +0x5C, node+8 = tracker, next +0x10), and for levels -4..7 whose treeLevel
   (`tracker + 0x6A0 + L*0x1A4`) manager belongs to this lightMgr calls `FUN_006c7160(treeLevel, 0)`. Log
   `Lots: N lot stories queued (...)`.
8. Status string.

`Kick` writes 50 to cells+0x38 **and** +0x3C (like a lamp pick-up), counts "armed", logs `Rebuild armed: <reason>
(night level x)`.

### Relight reconciliation (28/09, replaces the per-event triggers)

The earlier triggers (lot lamp registered while lit, lamp-list edits, the rebuild N s after loading) missed lamps that
register unlit and are switched on later (dusk, scripts, automatic lights) and lots whose lamps finish loading after the
load rebuild: those lots kept a cut until the button. Now:
- `LotLightBridge::ForEachOutdoorLotLamp` provides every light of a lot (lot id != 0) of type 3..6 or 0xB with raw
  values, refreshed with each light enumeration (every 20 frames in `LotLightBridge::OnPresent`, `FUN_006ACF70`):
  rect +0x134 {minX, minZ, maxX, maxZ} (set by `FUN_006BDDF0` to position +0x120/+0x128 +- sqrt(range +0x130 / k)),
  position +0x120, colour +0xF0, intensity +0x10, range +0x130, storey +0xD0, room +0x08, flags +0x100.
- `Bakeable(l)` mirrors the patched game tests: type 0xB -> storey 0 only; lot lamps -> `luzDoLoteNaGrama`, room 0,
  flags 0x01|0x04|0x40|0x20, and storey 0 or (storey > 0 and the story gate installed).
- `g_baked` (sorted by light pointer) = what the bake was last given; `g_current` = what it would take now
  (`CollectCurrent`, max 8192 lamps).
- **`Reconcile`** (about once a second; not while a full rebuild is pending, nor before the first full rebuild of the
  world): `DiffBaked` merges both lists:
  - lamp gone / went dark / left the bake -> relight its old rect (structural);
  - new or switched on -> its rect (structural);
  - moved (rect or position by more than 5 cm) -> old and new rect (structural);
  - colour, intensity or range changed by more than 2 % (abs 0.002) -> old and new rect (value only).
  Value-only changes are relit at most every 5 s (fades, flicker). More than 2048 rects = overflow.
  - Local path (`relightLocal` on, no overflow): `SmoothStreamingRelightTerrainRects(rects, n, 0.4)` flags only the
    chunks whose bake rect overlaps a rect (+1 m margin); returns the chunk count; -1 terrain unreadable, -2 more than
    40 % of the chunks would be hit. Success: "local relights" / "chunks" counters, log `Local relight: N lamps
    changed, C terrain chunks relit (...)`.
  - Fallback (option off, overflow, -1, -2): a full rebuild via `Kick("lamps changed")`, **at most every 15 s**; lamps
    stay "waiting" until it is consumed. Counter "fallbacks".
- **`Settle`** (after a world load): every 500 ms hash the current list (light pointer, x and z at 0.1 m); once the
  hash has not changed for 3 s and at least 5 s have passed since the load (or 60 s at most), ONE full rebuild
  `Kick("world loaded")`. This also runs by day: it fixes the chunk-border defect of the maps shipped with the world.

### Localized relight mechanics (`patches/smooth_streaming_patch.cpp`)

Evidence (comment block before `SmoothStreamingRelightTerrainRects`): the chunk's lamp light is its
"Terrain/LightmapTexture" (+0xD8), rendered by `FUN_00C7E7A0` -> `FUN_00C25A90` -> `FUN_00C256F0`(type 5) ->
`FUN_00C296E0` -> `FUN_00C292B0`, which draws only lights whose rect overlaps the chunk's bake rect. The bake rect is
the cell record `FUN_00C2A470` returns for (chunk+0x0C >> 8, chunk+0x10 >> 8) from the grid at *(*(terrain+0x68)+8)
(cells +0xBC, width +0xCC, height +0xD0; record {x0, z0, w, h}). Setting +0x55 on those chunks only is exactly the
game's full rebuild restricted to them. When Smooth Streaming's terrain spreading is live the chunks are queued there
(released nearest to the camera first); otherwise +0x55 is set directly. Works with Smooth Streaming disabled (the
function is compiled in and falls back to direct flags).

## Files and functions

| File | Function | Role |
|---|---|---|
| patches/night_terrain_relight_patch.cpp | `TerrainLightTest`, `ArmTest`, `BakeLevelTest`, `BakeLevelStub`, `IsOutdoorLotLamp`, `OriginalWorldLightTest` | predicates |
| | `Install` / `Uninstall` / `Update` / `ReinstallNow` / `DeferredReinstall`, `CallPatch` | patching lifecycle |
| | `OnPresent`, `ReadLightState`, `ReadCounters`, `ArmCounters`, `Kick` | dusk and rebuilds |
| | `Bakeable`, `CollectLamp`, `CollectCurrent`, `MarkAllCovered`, `DiffBaked`, `Moved`, `ValueChanged`, `Reconcile`, `Settle` | reconciliation (combined build only) |
| | `NoteEdit`, `DecideEdit`, `FinishEdit`, `WaitEdit`, `EditKind`; `ResolveCamera`, `ReadEye`, `SampleCamera`, `CameraStill`; `g_baked` / `g_bakedDue` | standalone lamp change decisions (29/09) |
| | `TryLocal`, `LocalLamps`, `RebuildAll`, `StartSweep`, `ChunkRenderThunk` (QPC timing, completion signal); `g_localBatches`, `g_relitLamps`, `g_sweepId` | standalone local terrain relight / paced sweep (29/09) |
| features/terrain_chunk_relight.cpp | `ChunkRelight::Init`, `QueueLocal`, `QueueSweep`, `OnPresent`, `OnChunkRendered`, `OnFullRebuild`, `OnWorldChanged`, `Drop`, `Status`; `ReadViewRaw`, `ReadChunkRaw`, `LayoutOk`, `BakeRectRaw`, `GatesRaw`, `ScanFlagsRaw`, `WriteFlag54` | chunk selection, validation, paced release, completion, timeout |
| lot_light_bridge.cpp | `BakeLamp::rect`, `DiffBake` (`BakeDiff::changes`), `BakeTakes`, `LampsOfLots`, `CoverLots` | lamp rects and per-lamp changes for the local relight |
| | `QueueAllLotOutdoorRooms` | lot relight |
| lot_light_bridge.cpp | `EnumerateLights`, `ReadOutdoorLotLamp`, `RefreshOutdoorLotLamps`, `ForEachOutdoorLotLamp` | lamp list |
| patches/smooth_streaming_patch.cpp | `SmoothStreamingRelightTerrainRects`, `RelightRectsImpl`, `ReadCellGrid`, `ChunkBakeRect` | local relight |

## Game addresses and patterns

| Address | What | Verification |
|---|---|---|
| 0x006E97B0 | root getter (imm32 -> 0x011D1860 per the header comment) | byte check with imm32 masked |
| 0x00C29626 | visitor in 0xC29620 (vtable 0x010768A0): `8B 07 8B 50 20 8B F1 8B CF FF D2` then `test al,al; jz` | `ValidateBytes`, Fail |
| 0x006B6516 / 0x006B60D3 / 0x006B6618 | arm tests in `FUN_006b64b0` / `FUN_006b6090` / `FUN_006b6590`: `8B 17 8B 42 20 8B CF FF D0` then `test al,al; jz; mov [esi+38h],32h` | `ValidateBytes`, Fail |
| 0x00C294D9 | story gate in `FUN_00C292B0`: `83 BF D0 00 00 00 00`; before at 0xC294CE and after at 0xC294E0 as above | 3 byte checks; optional |
| 0x006BC020 | lot id of a light (EDX:EAX, called just before the gate) | context bytes |
| 0x006C7160 | room queue thiscall(treeLevel, roomId) ret 4: `83 EC 2C 53 55 56 33 DB 8B F1` | Fail if different |
| 0x006ACF70 | light enumeration stdcall(visitor), visitor vtable[0] = thiscall(visitor, Light*) | `E8 2B 36 00 00 8B 4C 24 04 51 68 40 CF 6A 00` |
| 0x00C84C1B..0x00C84C43 | countdown consume in `FUN_00C845C0`: `call 0x6B5750` (cells test) must be true; then `test bl,bl; jnz` skips the night test, else `call 0x6AC560` (night) must be true; then `call 0x6B5770` (reset to -1) and the loop over chunks (+0xB0/+0xB4) setting +0x55 | read in `re/out/dump/asm/00c845c0.asm` for this doc |
| 0x011ECBC4 | WorldManager global (GameAddr `WorldManagerPtr`: the store at 0x00C6D0CC, `8D 8D 9C 00 00 00 89 2D ?? ?? ?? ?? E8`) | sigcheck.pl: 1 / 1 matches, Steam ok |
| 0x00C6D68C | `mov ecx,[esi+58h]; call 0x00C845C0` (GameAddr `TerrainUpdateCall`; the disp8 = terrain offset, checked `8B 4E ?? E8` at run time) | sigcheck.pl: 1 / 1 matches, Steam ok |
| 0x00C85041..0x00C85056 | the `+0x54` sweep branch (one chunk per terrain update); 0x00C7E7E2..0x00C7E823 the early-exit gates; 0x00C815E0 the chunk layout | full.asm (terrain-and-light-bake.md 2, 3.3) |
| light +0x08, +0x10, +0x20, +0xB0, +0xC0/+0xC4, +0xD0, +0xE0, +0xF0, +0x100, +0x120, +0x130, +0x134 | room, intensity, fade, type, lot id, storey, effective colour, base colour, flags, position, range, bake rect | [../../engine/light-objects-and-rigs.md](../../engine/light-objects-and-rigs.md) |

**Resolved: there is no night condition in normal play (BL at 0xC84C29).** BL is set at `0x00C84B03..0x00C84B11`:
`bl = (WorldManager+0x1B4 != 0)`, and `0x006B5750(bl)` tests `cells+0x3C == 0` when BL is set, `cells+0x38 == 0`
otherwise. WorldManager+0x1B4 is 1/2/3 in every normal game mode (0 is only the engine's tool mode, smooth_streaming
note 1), so in play the only trigger is `cells+0x3C` reaching 0, by day or night; `+0x38` alone never triggers it, and
the night test `0x006AC560` runs only in tool mode. The patch header (24/09) and the status line "the game only
rebuilds the terrain light at night (or in Build mode)" describe the tool-mode branch and are **stale**; `Settle`'s
"runs by day too" is right. Note also that the game's three light register/remove/move sites only arm `+0x38`;
`+0x3C` is written only by `0x006B5730` (called from `0x006B08A0`). Apex's kick writes both, so its behaviour is
unaffected. Evidence and addresses: [../../engine/terrain-and-light-bake.md](../../engine/terrain-and-light-bake.md)
sections 3.1 and 8.

## Shader details

None: this part only changes game code and triggers. Its output (chunk light maps) is consumed by
[world-atlas-and-smoothed-maps.md](world-atlas-and-smoothed-maps.md).

## Interactions

- [world-atlas-and-smoothed-maps.md](world-atlas-and-smoothed-maps.md): the smoothed maps and the atlas re-detect
  changed chunk maps (one hash check per frame), so a relit chunk reaches lots, floors, fences after a few frames.
- Smooth Streaming: spreads full and local rebuilds over frames ("Terrain chunks rebuilt per frame", default 2). A full
  rebuild re-renders the chunk's 4 composited textures one chunk per call, so it costs a few ms per frame for a couple
  of seconds; local relights shorten it.
- [level-light-share.md](level-light-share.md): lot relights (`QueueAllLotOutdoorRooms`) and storey sharing both touch
  room 0 of every storey.
- `postesAcesosNoCalculo` (0x6BE18C) is part of the same patch; see [lot-light-pass.md](lot-light-pass.md).
- **Split-Level Lighting Fix of the official S3SS** (runs next to the standalone; `GetLotID` at 0x006BC020 forced to
  return 0). The bake keeps a light when `GetLotID() == 0 || light+0xD0 == 0` (`re/out/fn_00c292b0.c` line 114, the
  compare that `BakeLevelStub` replaces), so with that patch on the story gate is short-circuited: `BakeLevelTest` never
  runs (the "Terrain bake, lot lights" counters stay 0), and basement lot lamps accepted by the visitor are baked
  although `Bakeable` refuses them, so the reconciliation does not relight when they change. The reconciliation's own
  lamp list reads the lot id directly (`light+0xC0/+0xC4` in `ReadOutdoorLotLamp`), not through `GetLotID`. Inferred
  from the decompile, not tested in game.
- `SmoothStreamingRelightTerrainRects` needs the Steam build and resolves the WorldManager global itself
  (`ResolveWorldGlobals`: `mov eax,[0x011ECBC4]` checked in the Smooth Streaming budget code, and `8B 4E 58 E8` =
  `mov ecx,[esi+58h]; call` at 0x00C6D68C; a call target outside TS3W is accepted because the Frame Profiler may
  redirect that call). If either check fails it returns -1 and the reconciliation falls back to a full rebuild.

## Known limitations

- Basement lamps never enter the bake (refused by `BakeLevelTest`, like the game).
- Lot lights of upper storeys other than outdoor lamps (types 3..6, room 0, lit) stay out (the game's choice).
- A full rebuild consumed by the game itself (e.g. a Build-mode pick-up) is treated as covering every lamp.

## Pitfalls and failed approaches

- 24/09: a full rebuild 15 s after "world loaded" (reason "carregamento") -> later 5 s (25/09 16:25, the user waited
  more than 20 s for snow), and finally replaced by `Settle` (5..60 s, quiet for 3 s), because a fixed delay often ran
  before nearby lots had loaded and lit their lamps.
- 28/09 study (standalone): the 5 s load kick fired during the loading screen; the countdown was consumed at the first
  world update while the night level still read 0.00 (a lamps-off bake), then the dusk rebuild followed. Do not arm a
  load rebuild from a timer started at the cells change: wait for the world to be drawn and a steady night level.
- Do not rebuild on lamp changes that streaming produces (lots loading or unloading, lamps switching together at dusk
  or dawn): NOTAS 1c and the reconciliation's hitches. The lamp-change tracking counts only settled lots.
- 24/09 23:54 (`LightProbe-grama3-escura` session log): 16 rebuilds per session, "luzes mudaram" every ~30 s: the
  automatic unlock reacted to **any** armed +0x38, including street lamps of lots streaming in as the camera moved.
  Fixed then by "only if `g_lotLampArms` changed, at most once per minute"; superseded by the reconciliation (arms are
  now only counted).
- v5.4 (25/09): the lamp signature included the position (recalc 0.7 s after a move) and the safety recalc went from
  60 s to 15 s; superseded by the reconciliation.
- Relighting lots after **every** rebuild made a loop (relighting re-registers lot lamps, which re-arms the countdown)
  that kept invalidating the slow high-quality lot solves -> lots are re-solved only after the dusk rebuild, and only
  with `recalcularLotesAoAnoitecer`.
- The "Rebuild terrain light now" button cannot fix houses on foundations: the story gate drops their lamps from every
  bake. Do not debug that cut by rebuilding.
- The earlier notes' statement "rebuild happens at dusk; needs +0x38 and +0x3C armed" (section 2) is why `Kick` arms
  both.

## Testing in game

- Load a save by day, wait for dusk: log `Rebuild armed: dusk (night level ...)`, then `Terrain rebuilt (dusk; ...)`;
  world grass around street lamps and lot lamps lights up.
- House on a foundation with porch/wall lamps: at night the world grass outside the lot gets their light (no straight
  cut at the lot border). Developer > Status > "Terrain bake, lot lights: ground story G (lot lamps L) | upper stories
  U | refused R": U > 0 on such lots. The hint: if "on the ground" grows but "lot lamps" stays 0, the lamps are lost
  between the gathering and the bake (their light rect).
- Switch a lot lamp off/on, move it, recolour it: within about 1 s (value-only changes: within 5 s) Adv / Dusk shows
  "Last check: N lamps changed, C terrain chunks relit (...)" and the log `Local relight: ...`.
- Adv / Dusk "Lot lamps on the ground: N up to date | M waiting": M returns to 0.
- Developer > Status: "Night level: x | countdown: c38 / c3C", "Terrain: armed K | rebuilt R | local relights L (C
  chunks, F fallbacks)", "Lot lamps: armed A | on the ground B | off O".
- If the story gate is missing: the warning line at install and "upper stories 0".

## Open items

- 28/09: in the atlas captured at 12:05, even the lot lamps of storey 0 did not appear; the story-gate counters were
  added to find where they are lost. Needs an in-game check with the counters (not done at the freeze).
- Story gate and reconciliation not yet confirmed by the user in game.
- Resolve the BL / night-condition discrepancy above.
- 29/09 lamp change decisions (standalone): check in game with the dev log which field the repeatedly "edited" lamps
  change (`Lot lamp change: ...` / `Lamp changes that do not rebuild the terrain`), that the Hitches file shows no
  ~240 ms terrain frame right after a "Terrain rebuilt (by the game itself)", and that a Build-mode lamp placed / moved
  on the home lot at night still lights the grass within about 1 s once the camera stops. Then decide on the local
  relight (needs `GameAddr` ids for the terrain chunk list and the bake cell grid).
- 29/09 local terrain relight and paced sweep (developer toggles, off): the in-game checks listed in "Local terrain
  relight"; roads after a local relight (the `+0x54` path does not set the road partition mark); then decide the
  defaults.
