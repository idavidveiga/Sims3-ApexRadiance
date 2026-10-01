# Light Diag (Ctrl+Shift+F8)

> **30/09:** now in both builds as a player capture of the Report a problem page, written to `Captures\<date time> <kind>\` (never overwritten); see [../bug-reports.md](../bug-reports.md). Older output paths below are historical.

> Read-only dump of the game's lighting state to `S3SS_LightDiag.txt`: every light object the game knows (enumerated
> with the game's own light enumerator), every loaded lot lighting manager per story with every room and the lights in
> that room's light list, the Level Light Share diagnostics (which lamps each story sees and what the wall tests
> decided), and the "LUZ POR PIXEL" section added for PASSO3 increment 0 (the game's own lamp-law globals, the room-0
> transforms and every outdoor light with its range, strength, colours and cone). **Status: working, dev-only.**
> `light_diag.cpp` is compiled in both flavours; the hotkey poll (`LightDiag::OnPresent`) and the Developer button exist
> only when `!kPublicBuild` (`build_flavor.h`). `LightDiag::Init` (address validation) also runs in the public build,
> harmlessly.

## Purpose

The probe ([light-probe.md](light-probe.md)) shows what the GPU draws; Light Diag shows what the CPU-side light system
contains: which lamps exist, whether they are on, which rooms (and which stories) list them, and the constants of the
game's lamp formula. It answered, among others: which lamps reach which story (the "cut at the floor line" bug), the
pink factory colour of every lamp, why enclosed plazas are dark (rooms with 0 lights), the spotlight's cone, and the
story field of lot lamps behind the foundation-house lot-edge cut.

## User-facing settings

No TOML keys. Part of the Night Lighting patch (`[patches.NightTerrainRelight]`); needs it installed.

| UI | Where | Notes |
|---|---|---|
| Hotkey **Ctrl+Shift+F8** | in game | `GetAsyncKeyState` chord, edge-triggered, polled every Present; only after `Init` succeeded (`g_rootPtrAddr != 0`) |
| "Save light diagnostic" button (+ "(or Ctrl+Shift+F8)") | Apex tab > Night Lighting > Developer > Tools | `LightDiag::RequestDump()` sets an atomic flag; the dump happens on the next Present (render thread) |
| "Diagnostic: <status>" | Developer > Status | `Ready`, `No world loaded`, or `Saved S3SS_LightDiag.txt: N lights, M lot stories, R rooms` |

## How it works

1. **Init** (`LightDiag::Init`, called from `NightTerrainRelightPatch::Install`): checks the bytes of the root getter
   and of the light enumerator (table below). On mismatch it returns false and Night Lighting logs
   `[NightTerrainRelight] Light diagnostics unavailable in this game version`. On success `g_rootPtrAddr` = the imm32
   of `mov eax, [imm32]` at 0x006E97B1 (the global holding the lighting root).
2. **Per frame** (`LightDiag::OnPresent`, first call inside Night Lighting's `OnPresent()`, render thread, before the
   world-state check): polls Ctrl+Shift+F8 and the request flag; either triggers `WriteDiag()`.
3. **WriteDiag** (synchronous, on the render thread; with ~5700 lights the file is ~1.9 MB, a visible hitch):
   - root = `*g_rootPtrAddr`, lightMgr = `*(root + 0x1C0)`; none -> writes `Nenhum mundo carregado.` ("no world
     loaded") and returns.
   - Calls the game enumerator `FUN_006ACF70(visitor)` inside `__try` with a fake visitor whose vtable[0] is
     `VisitLight` (`__fastcall` standing in for `thiscall(visitor, Light*) ret 4`), collecting up to 200000 `Light*`.
   - Walks the lot tree and each story's room hash, formats everything. Every memory read goes through `SafeCopy`
     (`__try` memcpy), so a stale pointer yields `?`/0 instead of a crash.
   - Appends `LevelLightShare::DiagText()` and `PixelLampDiag(index)`.
   - Logs `[LightDiag] Saved S3SS_LightDiag.txt: ...`.

Output: `Documents\Electronic Arts\<localized game folder>\S3SS\S3SS_LightDiag.txt` (`ConfigPaths::GetS3SSDirectory()`),
truncated each time. Archived copies used in the notes: `S3SS_LightDiag-1.txt` (24/09), `-neve-praca.txt` (25/09 01:40),
`-passo3.txt` (11:55), `-passo3-holofote.txt` (14:01), `-passo3-inc0.txt` (16:09).

## File sections and fields

Labels are the literal (Portuguese) strings the code writes; the English meaning follows.

### 1. Header

`S3SS Light Diagnostics` / `nivel de noite=<f> lightMgr=<ptr> cells=<ptr> contador=<a> / <b>`

| Field | Source | Meaning |
|---|---|---|
| nivel de noite | lightMgr+0xF0 (float) | night level 0..1 (also used by the mod as the night factor) |
| lightMgr | root+0x1C0 | the world light manager |
| cells | lightMgr+0x104 | light cells (32 m grid used by the gathers) |
| contador | cells+0x38 / cells+0x3C (int) | the terrain light rebuild countdowns that Night Lighting arms (-1 = idle in the 16:09 sample); see [terrain-relight.md](../night-lighting/terrain-relight.md) |

### 2. `==== TODAS AS LUZES (N) | postes da rua (tipo 11, lote 0): X acesos de Y ====` (all lights)

`, ENUMERACAO FALHOU` is appended if the enumerator faulted. Street lamps = type 0xB with lot id 0; lit = flag 0x20.
One line per light, `#i` = enumeration index:

```
#0 L7F022780 vt=00FF43A8 tipo=7 lote=6C11001BCDB06290 comodo=0 d0=1 flags=90[apagada desabilitada] pos=(964.5 92.1 722.5) raio=0.00 intens=(1.00 1.00 1.00 1.00) cor=(1.00 1.00 1.00 1.00) efetiva=(0.00 0.00 0.00 0.00)
```

| Label | Offset in Light | Meaning |
|---|---|---|
| `L<ptr>` | - | the Light object address (the only stable identity inside one session) |
| vt | +0x00 | vtable = light class: 0xFF42A0 type 3, 0xFF42F8 type 11 (street lamp), 0xFF4570 type 4 (spot), 0xFF4350 type 5, 0xFF43A8 type 7 (window light, sample above), 0xFF4408 CircleWindowLight, 0xFF4468 TubeLight (notes; see [light-objects-and-rigs.md](../../engine/light-objects-and-rigs.md)) |
| tipo | +0xB0 (int) | light type |
| lote | +0xC4 : +0xC0 | 64-bit lot id (0 = world) |
| comodo | +0x08 (int) | room id (0 = outdoors) |
| d0 | +0xD0 (int) | story of the light (28/09 story-gate finding: the terrain bake refuses lot lights with +0xD0 != 0) |
| flags | +0x100 (byte) | 0x01 `viva` (alive), 0x02 `celulas` (in the cells), 0x04 `comodo-conhecido` (room known), 0x20 `ACESA`/`apagada` (on/off), 0x40 `habilitada`/`desabilitada` (enabled). Bits 0x08/0x10/0x80 are printed in hex but not decoded. Discrepancy: the notes (25/09 ~02:30) call flag 0x04 "outdoor"; the code labels it "room known". Unverified which is right |
| pos | +0x120 (3 floats) | position (lamp head) |
| raio | +0x130 | range (the "alcance" of section 5; 40/70/97/100 typical) |
| intens | +0x10 (4 floats) | intensity ("forca" in section 5) |
| cor | +0xF0 | colour set at creation / by script (factory pink (1, 0.75, 0.79)) |
| efetiva | +0xE0 | effective colour = intensity x colour when on, 0 when off |

**Light indices are not stable between dumps** (PASSO3 F-J5: `#5494` was a type-4 spot in one dump and a type-7 window
light in the next). Identify lights by `L<ptr>`, and only within one session.

### 3. `==== LOTES (arvore <ptr>, baldes <n>) ====` (lots and rooms)

Tree = lightMgr+0xD4; buckets tree+0x58, bucket count tree+0x5C; the node at `buckets[count]` is the end sentinel; each
node: tracker at +0x08, next at +0x10. For each tracker, stories -4..7: manager = `*(tracker + 0x6A0 + level*0x1A4)`.

`-- LOTE <id> andar <a> (nivel da arvore <L>) gerenciador=<mgr> sistema=ok|DIFERENTE flag280=<b> qualidade288=<q>`

| Label | Source | Meaning |
|---|---|---|
| LOTE | mgr+0x94 : mgr+0x90 | lot id |
| andar | mgr+0x88 | the manager's story (the real story; see [level-light-share.md](../night-lighting/level-light-share.md): lot load recollects room 0 of all stories with the level-0 tree level) |
| nivel da arvore | loop index -4..7 | slot in the tracker |
| gerenciador | - | lot lighting manager of that story |
| sistema | `*(mgr) == lightMgr` | sanity check (`DIFERENTE` = inconsistent) |
| flag280 | mgr+0x280 (byte) | unknown flag (1 in all samples) |
| qualidade288 | mgr+0x288 (byte) | lighting quality, 1 = high (the active lot; Level Light Share uses it as "active lot") |

Rooms: hash at mgr+0x230 (buckets +0x234, count +0x238); node key (room id) +0x00, value (Room*) +0x10, next +0x80.

`   comodo <id> (<ptr>) estado=<s> classe=<c> x100=<x> externo=<e> no_hash24=<h> luzes=<n> (postes da rua a / acesos b, outras c): #i #j L<ptr> ...`

| Label | Source | Meaning |
|---|---|---|
| estado | room+0xF0 | solve state |
| classe | room+0xF4 | LOD class (class 2 = the wall atlas gets `[0x1158B1C]` blur passes, PASSO3 F-J1) |
| x100 | room+0x100 | unknown |
| externo | room+0x18 (byte) | 1 for room 0 (outdoors) and for roofless fenced areas (notes m15: "room+0x18 = 1") |
| no_hash24 | hash node+0x24 | unknown |
| luzes | room+0xC8 .. +0xCC | the room's light list (vector of Light*), with the count of street lamps (lit) and others; each entry printed as enumeration index or raw pointer |

### 4. `==== ANDARES (luz externa entre andares) ====` (stories; `LevelLightShare::DiagText`)

Written by `level_light_share.cpp`. First line: Level Light Share status (`Active | outdoor lights carried to other
stories: N | stories updated: N | walls on the light's story: a/9 classes, T tests, B blocked ...`). Then the sample
records collected by the light-eval hook during the last solve: only for the **active lot** (mgr+0x288 != 0), only
samples within 3.5 m of the light, at most 4000 (`N registrados de M vistos`). Columns: story of the point | light,
type, story of the light (`casa`, -1 = own story or world) | point | normal | colour sum | game's wall test (`jogo` 1
passed, 0 blocked, -1 not run) and its factor `t` | our factor (`nosso`, -1 = not tested) | `lote` (batch flag) |
`filtro` (culled list). A per light/story summary precedes the points.

`DiagText` **swaps the records out**: a second F8 without a new lot solve shows 0 records. Records are also cleared when
the option is toggled (`RefreshAllLots`). Details in [level-light-share.md](../night-lighting/level-light-share.md).

**Since 2026-09-29 the records are collected only while armed** (they cost time in every lot light solve): Developer >
Lighting "Record story light samples for the diagnostics", or the first dump of a session, which then writes only the
status line and "No samples: recording them was off ... It is on now: let the lot relight (or use "Relight lots now")
and save the diagnostics again." and arms the recording. From then on each dump has the samples of the solves since the
previous one, as before. Sections 1-3 and 5 are not affected.

### 5. `==== LUZ POR PIXEL (passo 3, incremento 0) ====` (per-pixel lamp light)

Added 25/09 ~15:15 as PASSO3 increment 0 ("diag only, no visual change"; `PASSO3-PLANO.md` sections 3.7, 4 and 6):
the values a per-pixel lamp term must copy from the game's own wall/floor light solve.

```
k1 [0x11D0A60]=1 k2 [0x11D0A68]=0.075 Cmax [0x11D1160]=(2 2 2 2) poste [0x1158DA8]=3.3333 tipo5 s [0x11D11A0]=5
desfoque das paredes: passadas [0x1158B1C]=2 modo [0x11D02E4]=0
-- lote 4522001BE6BBCA40 andar 0 comodo0=3DDFE700 classe=0 limiar[+0x63C]=0.0235294 matriz[+0xF8]=(0.9397 0 0.342 0 0 1 0 0 -0.342 0 0.9397 0 896.7 60.53 1232 1)
-- luzes nas listas do comodo 0 (64): alcance +0x130, forca +0x10.x, cores E0/F0, cone
L7D515AB0 #5666 vt=00FF4570 tipo=4 listas=6 pos=(882.4 63.55 1188) alcance=30 forca=(1 1 1 1) E0=(0.85 0.8625 1 0.85) F0=(0.85 0.8625 1 0.85) cone: eixo[+0x170]=(0.005923 -0.9999 -0.01627) desloc[+0x158]=-0.3211 escala[+0x154]=4.902
```
(excerpt of `S3SS_LightDiag-passo3-inc0.txt`, 25/09 16:09)

| Item | Address / offset | Meaning (PASSO3) | Measured 16:09 |
|---|---|---|---|
| k1 | `[0x011D0A60]` float | divisor of the lamp weight | 1 |
| k2 | `[0x011D0A68]` float | lamp weight factor: W = +0x130 x (+0x10).x x k2 / k1 | 0.075 |
| Cmax | `[0x011D1160]` 4 floats | per-lamp clamp, `min(Cmax, W*NdotL/d^2)` | (2 2 2 2) |
| poste | `[0x01158DA8]` float | street-lamp factor (the x3.333 of the ground) | 3.3333 |
| tipo5 s | `[0x011D11A0]` float | cone scale of type 5 lights | 5 |
| passadas | `[0x01158B1C]` uint32 | wall atlas blur passes (class-2 rooms) | 2 |
| modo | `[0x011D02E4]` byte | blur branch: 0 = separable [1 2 1]/4 rounded up; else 2x2 box with half-texel shift | 0 |
| classe | room0+0xF4 | LOD class of room 0 | 0 or 2 |
| limiar | room0+0x63C float | the game's per-light threshold (applied at 0x69FE19..0x69FE4D) | 0.0235294 (class 0), 0.0117647 (class 2) |
| matriz | `*(float**)(room0+0xF8)`, 16 floats | lot-to-world matrix, row-vector: world = x*m[0..3] + y*m[4..7] + z*m[8..11] + m[12..15] (PASSO3 F-J2) | rotation + lot origin |

The light list after it contains every light found in any **room 0 of any story of any loaded lot**, deduplicated;
`listas` = in how many room-0 lists (counting repeats) it appears. Fields: `#index` (or `-`), vt, type, pos +0x120,
`alcance` +0x130, `forca` +0x10 (4 floats), E0 +0xE0, F0 +0xF0, and the cone:

| Class | vt | Cone fields |
|---|---|---|
| type 4 spot | 0x00FF4570 | axis `eixo[+0x170]` (3 floats), offset `desloc[+0x158]`, scale `escala[+0x154]` |
| type 5 | 0x00FF4350 | `a1[+0x1A0]` (3), `o1[+0x174]`, `a2[+0x190]` (3), `o2[+0x170]`, `S[+0x150]` (3) |

Measured: the test spotlights (type 4) have axis ~ (0, -1, 0), offset -0.32, scale 4.9 (NOTAS 16:25).

Not implemented from the PASSO3 plan (3.7): the per-lot "key", the lamp block (pointer, W, weight, importance), the
self-test errors per type and the last 8 unmatched draw keys.

### 6. Footer

`Fim: <N> luzes, <M> andares de lotes, <R> comodos` (end: lights, lot stories, rooms).

## Files and functions

| File | Symbol | Role |
|---|---|---|
| `light_diag.h` | `LightDiag::Init`, `RequestDump`, `OnPresent`, `Status` | API |
| `light_diag.cpp` | `WriteDiag` | sections 1-3, 6; calls the others |
| | `PixelLampDiag` | section 5 |
| | `EnumerateLights`, `VisitLight`, `g_visitor` | game enumerator with a fake visitor |
| | `LightLine`, `Vec4`, `Floats`, `SafeCopy`, `Rd<T>` | formatting and guarded reads |
| `level_light_share.cpp` | `DiagText`, `Diag`, `GameWallTest`, `ClearDiag` | section 4 |
| `patches/night_terrain_relight_patch.cpp` | `Install` (`LightDiag::Init`), `OnPresent` (`LightDiag::OnPresent` when `!kPublicBuild`), `RenderDeveloperUI` (button, status) | host |

**The F8 writer is `light_diag.cpp` (namespace `LightDiag`), NOT `patches/light_diag_patch.cpp`.** The latter is an
untracked dev leftover (see Pitfalls).

## Game addresses and patterns

| Address | What | Verification |
|---|---|---|
| 0x006E97B0 | root getter: `A1 <imm32> 85 C0 75 01 C3 8B 80 C0 01 00 00` (`mov eax,[g]; test eax,eax; jnz; ret; mov eax,[eax+0x1C0]`) | `Init`: byte 0xA1 at +0, the 11 tail bytes at +5; root global = imm32 at +1. Same check in Night Lighting's own `Install` ("Light manager code differs at 0x6E97B0") |
| 0x006ACF70 | light enumerator, `stdcall(visitor*)`, visitor vtable[0] = `thiscall(visitor, Light*) ret 4`; bytes `E8 2B 36 00 00 8B 4C 24 04 51 68 40 CF 6A 00` | `Init` memcmp; Ghidra `re\out\dump\fn\006acf70.c`: `FUN_006b05a0(); FUN_006b0710(&LAB_006acf40, param_1);` |
| 0x011D0A60, 0x011D0A68, 0x011D1160, 0x01158DA8, 0x011D11A0 | lamp law globals (section 5) | read only; values match PASSO3 expectations (1, 0.075, 2, 3.3333) |
| 0x01158B1C, 0x011D02E4 | wall blur pass count and branch flag | PASSO3 F-J1 (`passo3\out\fn_0069f650.c`) |

Structure offsets used (Light, lot manager, room, tree) are listed in the tables above; the engine side is in
[room-light-maps.md](../../engine/room-light-maps.md) and [light-objects-and-rigs.md](../../engine/light-objects-and-rigs.md).

## Interactions

- Hosted by Night Lighting (install and Present). Without it no F8.
- The enumerator is the same `FUN_006ACF70` the bridge uses every 20 frames for its lamp lists; calling it from the
  Present hook is safe (same thread as the game's light update).
- Section 4 depends on Level Light Share being installed ("Outdoor lights reach every story", TOML
  `luzExternaEntreAndares` in `[patches.NightTerrainRelight]`, default true).
- Typical pairing: F8 once, then Ctrl+Shift+F7 on the surfaces (PASSO3 test protocol: stay 10 s at the camera, F8, F7
  on the wall, F7 on the floor, send the file and both LightProbe folders).

## Known limitations

- Synchronous and large (~1.9 MB, 5724 lights in the test town): a frame hitch.
- Indices change between dumps; pointers change between sessions.
- Section 4 covers only the active lot and only points within 3.5 m of a light, 4000 records max.
- Unknown fields are printed raw (`flag280`, `x100`, `no_hash24`, flag bits 0x08/0x10/0x80).

## Pitfalls and failed approaches

- **Two writers existed.** On 25/09 ~10:43 an F8 came back without the stories section because the hotkey is handled by
  `light_diag.cpp`, while the section had been added to `patches/light_diag_patch.cpp`; the section was then put in both
  (NOTAS "F8 sem a secao dos andares"). `patches/light_diag_patch.cpp` is **untracked and not in the vcxproj**: it
  registers its own patch "Light Diagnostics" with its own Ctrl+Shift+F8 poll, status `Pronto`, an extra "all lots high
  quality" switch calling `FUN_006A5EF0` (`thiscall(manager, char hq) ret 4`, bytes `8A 44 24 04 56 8B F1 38 86 88 02 00
  00`), and no "LUZ POR PIXEL" section. Its `APEX_REGISTER_FEATURE` is inside `#ifndef S3SS_PUBLIC`. Never commit it, never add
  it to the project. The same applies to the other untracked leftovers: `patches/call_trace_patch.cpp` (Call Trace,
  Ctrl+Shift+F10, detours ~1400 lighting/terrain functions listed in `trace_targets.h`, generated by
  `re/scripts/SafeHookList.java`, output `S3SS_CallTrace.txt`), `patches/lot_edge_lighting_patch.cpp` and
  `trace_targets.h` (PLANO-SEPARACAO.md: "Untracked and NOT in the vcxproj").
- **Do not identify lights by `#index`** (F-J5).
- **"High quality on all lots" was a failed fix** for the lot-edge cut (NOTAS section 1: it also dropped the FPS to 63);
  the leftover quality switch in `light_diag_patch.cpp` belongs to that experiment.
- Findings that came from F8 (so the next session does not redo them):
  - 24/09: enclosed plaza rooms (walls without roof) have 0 lights on non-active lots (NOTAS 4c, 6).
  - Lake test: street lamps have 2 lights at the same spot (+0x130 = 97 and 40), intensity 1.0, colour (1, 0.75, 0.79);
    the visual radius must not come from the bounds (+0x134) (NOTAS 4d).
  - `S3SS_LightDiag-neve-praca.txt` (25/09 01:40, with m17): 56 street lamps (type 11) and 46 lot lamps (type 3) still
    pink after the creation-time colour fix: the object script sets the colour later through
    `FUN_006B0B50 -> FUN_006BC3E0` (fix at 0x6B0BDE, [lamp-colour.md](../night-lighting/lamp-colour.md)).
  - F8 of 01:39: in room 0 of story 0 the lot's own outdoor lamps appear 2x (type 3) or 3x (type 11); other stories
    only see type 11 once (through the world cells); ground-floor type-3 lamps never reach other stories -> Level Light
    Share. F8 of 09:50 (lot C49C001BCF2DEA20, levels 0..4): the same 19 lights on every story with correct weights ->
    the remaining difference was wall blocking, not the list.
  - `S3SS_LightDiag-passo3.txt` (11:55): 5724 lights, 30 lot stories, 92 rooms; `-holofote.txt` (14:01): the test
    spotlight is one of 5 type-4 lights (vt 0xFF4570), range 30, colour (0.85, 0.86, 1.0), y 63.5.
  - 28/09 (probe2): the lot's lit lamps were all type 4 with `d0` = 1/2, which led to the terrain bake story gate at
    0xC294D9 ([terrain-relight.md](../night-lighting/terrain-relight.md)).

## Testing in game

- Dev build, Night Lighting on, a world loaded. Press Ctrl+Shift+F8 (or the button).
- `S3SS_LOG.txt`: `[LightDiag] Saved S3SS_LightDiag.txt: N lights, M lot stories, R rooms`.
- If Init failed: `[NightTerrainRelight] Light diagnostics unavailable in this game version` at install, and F8 does
  nothing.
- For section 4 to have data, trigger a lot solve first (e.g. Developer > "Recalculate lot light now", or change a lamp)
  on the active lot.

## Open items

- Standalone rename (PLANO-SEPARACAO.md section 4): `Documents\Electronic Arts\<localized>\Apex Radiance\ApexRadiance_LightDiag.txt`.
- Decode the unknown fields (flag bits, `flag280`, `x100`, `no_hash24`) and settle the flag 0x04 meaning.
- Move the room walk into a shared `LightDiag::ForEachRoom` (PASSO3 plan) instead of the two copies in `WriteDiag` and
  `PixelLampDiag`.
- Optionally write the dump on a worker from a snapshot to avoid the hitch.
