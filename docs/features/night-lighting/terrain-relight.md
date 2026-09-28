# Terrain relight (lot lamps in the terrain bake, story gate, dusk rebuild, reconciliation)

> **Status in the standalone:** only the v0.1.0 part is in the standalone: visitor 0xC29626, the 3 arm sites, the dusk
> kick, the experimental switches and the dev buttons. v0.1.0 triggers full rebuilds instead of the reconciliation: a load
> kick 5 s after "world loaded" (reason "carregamento"), a lamp-edit kick 0.7 s after the lot-lamp signature changes
> (`TrackLotLampEdits` / `LotLampEdits` in `lot_light_bridge.cpp`, colour/brightness/on-off/position, every 20 frames), and
> at night with c38 == 0 and c3C <= 0 a "luzes mudaram" kick after 120 frames when `g_lotLampArms` changed, at most every
> 15 s. **Not in the standalone yet (post-0.1.0, re-add one by one after user tests):** the story gate 0xC294D9
> (`BakeLevelStub` / `BakeLevelTest` and its counters), the reconciliation (`Bakeable`, `DiffBaked`, `Reconcile`,
> `Settle`, `ForEachOutdoorLotLamp`), the setting `relightLocal`, and the localized relight
> `SmoothStreamingRelightTerrainRects` (Smooth Streaming itself did not exist in v0.1.0).

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
| Lot lights light the ground outside the lot | `luzDoLoteNaGrama` | bool | true | | Main | Installs the visitor, arm-site and story-gate patches (reinstall on change). The checkbox also sets `automaticoAoAnoitecer` to the same value |
| Update automatically at dusk | `automaticoAoAnoitecer` | bool | true | | Adv / Dusk | live |
| Delay after dusk | `atrasoSegundos` | float | 2.0 | 0.5..10 s | Adv / Dusk | live |
| Relight only around changed lamps | `relightLocal` | bool | true | | Adv / Dusk | live; off = changes get a full rebuild (at most every 15 s) |
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
| | `Bakeable`, `CollectLamp`, `CollectCurrent`, `MarkAllCovered`, `DiffBaked`, `Moved`, `ValueChanged`, `Reconcile`, `Settle` | reconciliation |
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
