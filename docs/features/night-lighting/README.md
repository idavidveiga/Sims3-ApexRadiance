# Night Lighting

> **Status in the standalone:** the standalone's Night Lighting starts from **v0.1.0** (commit b84d5f1, 27/09), not from
> `combined-final`. Everything below that came after v0.1.0 is **not** in the standalone yet and is re-added one change at
> a time after a user test, fences first: the story gate 0xC294D9 and the relight reconciliation / local relight
> ([terrain-relight.md](terrain-relight.md); v0.1.0 has fixed triggers instead), the bake-matched per-pixel law
> `W = 0.4 x range` with `SelectPixelLamps`, the rule `max(rig + vertex lights, per-pixel, ground)`, the ground facing
> factor `sat(N.y + 1)` with strength `max(1, forcaNosObjetos)` ([objects-and-rigs.md](objects-and-rigs.md)), per-pixel
> lamps on fences (user-approved, the first to come back, [fences.md](fences.md)), shader pre-creation at
> `CreatePixelShader`/`CreateVertexShader` (`PrecreatePs/Vs`, foliage VS pool, `OwnCreate*`, `CodeBytes`), the batched
> shader log (`FlushShaderLog`), and the English status/log strings (v0.1.0 still has Portuguese ones; the standalone must
> be English). v0.1.0 has no HDR code at all. Unchanged since v0.1.0 (in the standalone as described): lot pass, smoothed
> maps and atlas, walls, floors, roads, snow, level light share, lamp colour, CPU rig boost, foliage, roofs, water.

> Rebuilds The Sims 3's night lamp lighting while the game draws: street-lamp light no longer stops in a straight
> line at lot borders, lot lamps light the world ground, roads and every storey, and walls, floors, snow, objects,
> fences, foliage, roofs and ponds all get lamp light. One patch, `NightTerrainRelight` (UI name "Night Lighting",
> Apex tab), made of several modules. Status: **working**, marked `experimental` in its metadata and **off by default**
> (`enabledByDefault` is not set in `APEX_REGISTER_FEATURE`). It exists in both build flavours; the "Developer" subsection,
> F7/F8 and the census exist only in the dev build. Steam 1.67.2 only (`supportedVersions = VERSION_STEAM`).

Old names you will meet in notes and code: "Night Remake", "Iluminacao melhorada", "Lot Edge Lighting" (a dev-only
predecessor patch, `patches/lot_edge_lighting_patch.cpp`, never shipped).

## Purpose

How the game lights the night (measured with Light Probe captures, see [NOTAS-ILUMINACAO.md section 1]):

| Surface | Where its lamp light comes from in the stock game | Defect |
|---|---|---|
| World grass (terrain chunks) | Per-chunk 256x256 DXT5 light map ("StaticTerrainLightmap"), baked by `FUN_00C292B0` from world lights only | Blocky 1 texel/m circles, RGB565 colour specks, cut at 256 m chunk borders (bake defect), lot lamps absent |
| Lot grass | CPU room solve of room 0 (`FUN_006be020`), 1/d^2 from the lamp head, drawn by a modulate2x light pass | Street lamps arrive very faint: **straight cut at the lot border** |
| Roads, sidewalks | Their own copy of the chunk light map, without the lamps | Dark roads next to lit grass |
| Walls, floors | Per-storey room light maps (atlas per level) | Only the lamps of that storey: cut at the floor line; walls much dimmer than objects |
| Objects, fences, foliage | Per-object "rig": sun + 3 strongest lamps at the object centre | Many objects get nothing (flag, cut-off 0.1, vertex-light slots empty); moon shadow kills lamp light; modular pieces differ |
| Roofs, lake water | No lamp term at all | Black roofs and ponds at night |
| Snow variants | Separate shaders (snow lot pass, snow on floors, fence tops, stair tops) | Each misses lamp light in its own way |

Night Lighting fixes each path at the point where the game computes it: game-code byte patches for the bakes and light
gathering, and D3D9 draw interception that swaps in patched copies of the game's own shaders (pattern-patched
bytecode or small HLSL replacements), with textures/constants bound for one draw and restored.

## User-facing settings

All settings are registered in the `NightTerrainRelightPatch` constructor (`patches/night_terrain_relight_patch.cpp`)
with `RegisterBoolSetting` / `RegisterFloatSetting`. They are saved by `OptimizationPatch::SaveToToml`
(`optimization.h`) under **`[patches.NightTerrainRelight]`** in `S3SS.toml` (combined build), together with
`enabled = true|false`. On load, `FloatSetting::LoadFromToml` (`patch_settings.h`) clamps floats to [min, max]. The code
comment says: "The setting keys ... are the TOML keys of saved configs: never rename them." All settings exist in both
builds (the public build still loads/saves the dev-only ones; it just shows no control for them).

UI location codes: **Main** = directly under the Night Lighting header; **Adv/x** = collapsed "Advanced" tree, section x;
**Dev** = "Developer" tree (dev build only); **Refl** = the separate "Reflections" header of the Apex tab
(`ApexRenderReflectionsUI`, same patch, same TOML table).

| UI label | TOML key | Type | Default | Range | UI | Applied | Sub-doc |
|---|---|---|---|---|---|---|---|
| Street lamps light inside lots | `luzDoPosteNaGramaDoLote` | bool | true | | Main | live (`ApplyLive` -> `LotLightBridge::SetEnabled`) | [lot-light-pass](lot-light-pass.md) |
| Lot lights light the ground outside the lot | `luzDoLoteNaGrama` | bool | true | | Main (also sets `automaticoAoAnoitecer` to the same value) | live since 28/09 (read at run time by the always-installed predicates; one rebuild at night) | [terrain-relight](terrain-relight.md) |
| Outdoor lights reach every story | `luzExternaEntreAndares` | bool | true | | Main | live (`LevelLightShare::Install/Uninstall`) | [level-light-share](level-light-share.md) |
| Lamps light nearby objects | `postesNosObjetos` | bool | true | | Main | live (`ObjectLightBridge::Install/Uninstall`, `SetObjectShadowFix`) | [objects-and-rigs](objects-and-rigs.md), [foliage](foliage.md) |
| Roofs receive lamp light | `telhadosComLuz` | bool | true | | Main | live (per frame) | [roofs](roofs.md) |
| Lamp colour | `luzDasLampadasNatural` | float | 1.0 | 0..1 | Main | when a save loads | [lamp-colour](lamp-colour.md) |
| Object light strength | `forcaNosObjetos` | float | 1.0 | 0.25..3 | Adv/Objects | live | [objects-and-rigs](objects-and-rigs.md) |
| Include stairs, railings and columns | `lampadasEmTodosObjetos` | bool | true | | Adv/Objects | rigs created later (world load) | [objects-and-rigs](objects-and-rigs.md) |
| Doors, windows and counters get the ground light | `objetosDeForaComLuzDoChao` | bool | true | | Adv/Objects (disabled unless bridge + smooth maps) | live (`RigTracker::Install/Uninstall`) | [objects-and-rigs](objects-and-rigs.md) |
| Seamless lamp light on outdoor objects | `luzPorPixelNosObjetos` | bool | true | | Adv/Objects (same gate) | live | [objects-and-rigs](objects-and-rigs.md) |
| Object lamp light strength | `forcaLuzPorPixelNosObjetos` | float | 1.0 | 0.25..3 | Adv/Objects | live | [objects-and-rigs](objects-and-rigs.md) |
| Fences, railings and stairs get the ground light | `cercasComLuzDoChao` | bool | true | | Adv/Objects (same gate) | live | [fences](fences.md), [snow](snow.md) |
| Fence light strength | `forcaNasCercas` | float | 1.0 | 0.25..2 | Adv/Objects | live | [fences](fences.md), [snow](snow.md) |
| Lamp light on outside walls | `forcaNasParedes` | float | 2.0 | 1..4 (`SetWallGain` clamps 0.25..8) | Adv/Walls and roofs | live | [walls](walls.md) |
| Roof light strength | `forcaNosTelhados` | float | 0.6 | 0.05..2 | Adv/Walls and roofs | live | [roofs](roofs.md) |
| Smooth light on the ground | `mapaDeLuzSuavizado` | bool | true | | Adv/Ground and snow | live | [world-atlas-and-smoothed-maps](world-atlas-and-smoothed-maps.md) |
| Smooth the ground light maps on the GPU (A/B) | `mapaDeLuzSuavizadoNaGpu` | bool | true | | Dev only (registered in the dev build; public = always GPU when available) | live (next Present: switching drops the smoothed maps, the new path rebuilds them) | [world-atlas-and-smoothed-maps](world-atlas-and-smoothed-maps.md) "GPU path" |
| Trodden snow on sidewalks | `calcadaComNevePisada` | float | 0.5 | 0..1 | Adv/Ground and snow (needs bridge) | live | [roads](roads.md) |
| Update automatically at dusk | `automaticoAoAnoitecer` | bool | true | | Adv/Dusk | live | [terrain-relight](terrain-relight.md) |
| Delay after dusk | `atrasoSegundos` | float | 2.0 s | 0.5..10 | Adv/Dusk | live | [terrain-relight](terrain-relight.md) |
| Relight only around changed lamps | `relightLocal` | bool | true | | Adv/Dusk | live | [terrain-relight](terrain-relight.md) |
| Street lamps count as lit in lot light solves | `postesAcesosNoCalculo` | bool | false | | Dev (experimental) | reinstall (0x6BE18C) | [terrain-relight](terrain-relight.md), [lot-light-pass](lot-light-pass.md) |
| High lighting quality on every lot | `qualidadeAltaEmTodosOsLotes` | bool | false | | Dev (experimental) | reinstall; lots loaded afterwards | [lot-light-pass](lot-light-pass.md) |
| Soft lot edges (A/B) | `bordaSuaveLote` | bool | true | | Dev only (registered in the dev build; public = always on) | live (per frame) | [lot-light-pass](lot-light-pass.md) "Soft lot edges" |
| Lot grass keeps the lot's own light | `gramaDoLoteUsaLuzDoLote` | bool | false | | Dev (experimental) | reinstall (0xC7F87D) | [lot-light-pass](lot-light-pass.md) |
| Recalculate every lot at dusk | `recalcularLotesAoAnoitecer` | bool | false | | Dev (experimental) | live | [terrain-relight](terrain-relight.md) |
| Ponds reflect lamps | `lagosRefletemLampadas` | bool | true | | Refl | live | [water](water.md), [../reflections.md](../reflections.md) |
| Lamp glow on water | `brilhoNaAgua` | float | 1.0 | 0.1..3 | Refl/Advanced | live | [water](water.md) |
| Shore reflection | `reflexoNoLago` | float | 1.0 | 0..3 | Refl/Advanced | live | [water](water.md) |

Notes on the table:
- "Reset to defaults" in Advanced (`ResetDefaults`) restores every value above except the three pond settings, which have
  their own reset in the Reflections section (`ResetReflectionDefaults`).
- Dependency gates in the UI (`BeginDisabled`): `groundLight = g_bridge && g_smoothMaps` gates the three
  ground-light object/fence options (they read the world atlas, which exists only with both on); the strengths are
  greyed out when their parent is off; "Trodden snow on sidewalks" needs the bridge.
- **Reinstall vs live.** `Update()` (message-loop thread) schedules a reinstall only when `postesAcesosNoCalculo`,
  `qualidadeAltaEmTodosOsLotes` or `gramaDoLoteUsaLuzDoLote` changed (they change code bytes), or when `luzDoLoteNaGrama`
  is on but its code could not be installed (28/09: it is otherwise live). The reinstall keeps the world state and the
  ground light maps (`LotLightBridge::Shutdown(true)`, `g_lastCells` kept).
  The reinstall itself (`ReinstallNow`) runs on the render thread through `DeferredReinstall`, registered in
  `RenderCallbacks::endSceneBeforeOverlay` (a crash fix, see Pitfalls). Everything else is pushed every frame from the
  Present hook or applied by `ApplyLive`.
- The combined build's HDR lamp gain (`HdrOutput::LampGain()` multiplied into several constants) is not a Night
  Lighting setting and is gone from the standalone; see [../../removed-features.md](../../removed-features.md).

### Status lines

| Where | Text source | Values |
|---|---|---|
| Main, first line `Status:` | `g_status` in `OnPresent` (night_terrain_relight_patch.cpp) | "Waiting for the game to load a world", "Rebuild pending: the game only rebuilds the terrain light at night (or in Build mode)" (stale wording: in normal play the game rebuilds day or night once cells+0x3C reaches 0; this line only means `+0x38 == 0` by day, the idle state; see [terrain-relight.md](terrain-relight.md)), "Rebuilding in N frames", "Night: OK", "Day: OK" |
| Adv/Dusk (3 grey lines) | reconciliation state | "Lot lamps on the ground: N up to date \| M waiting[ \| waiting for the lots to load \| waiting for the first full rebuild]", "Local relights: N (C chunks) \| full rebuilds: K", "Last check: ..." |
| Dev/Status "Diagnostic" | `LightDiag::Status()` | "Saved S3SS_LightDiag.txt: ..." |
| Dev/Status "Street lamps on lots" | `LotLightBridge::Status()` | bridge state ("Off", "Waiting for the first draw", "Active", "Failed: ...", "Off after an internal error (see S3SS_LOG.txt)") + chunk count + per-path draw counters + "without terrain texture" |
| Dev/Status "Objects" | `ObjectLightBridge::Status()` | classes patched N/9, boosted lights, forced rigs, fenced-area rigs |
| Dev/Status "Shadow" | `LotLightBridge::ObjectStatus()` | moon-shadow fix, foliage counters |
| Dev/Status "Walls" | `LotLightBridge::WallStatus()` | "outside walls: strength S \| draws: N \| variants seen: M" |
| Dev/Status "Roofs", "Water" | `RoofStatus()`, `WaterStatus()` | |
| Dev/Status "Smoothed map" | `LightmapSmooth::Status()` | GPU path: "GPU (F intermediates) \| chunks smoothed: R of N \| waiting \| built (in view, out of view, borders) \| GPU time per chunk \| changes seen \| world map ... \| GPU vs CPU: ..."; CPU path: "CPU \| chunks smoothed: R of N \| queued \| uploaded \| unreadable \| world map: WxH chunks (C copies)". Below it (dev): the GPU A/B checkbox and "Compare GPU vs CPU (one chunk)" |
| Dev/Status "Lamp colour" | `ObjectLightBridge::LampColourStatus()` | |
| Dev/Status "Stories" | `LevelLightShare::Status()` | |
| Dev/Status counters | `RenderDeveloperUI` | last event, night level + countdowns +0x38/+0x3C, terrain armed/rebuilt/local relights, lot relights, "Street lamps counted as lit", lot-lamp arms/baked/off, story-gate counters |

## How it works

### Per frame (render thread = main thread)

The patch registers one Present callback (`D3D9Hooks::RegisterPresent("NightTerrainRelight", ..., Priority::Last)`)
that runs, in this order:
1. once: logs `[NightTerrainRelight] Shader limits: PS 3.0 N instruction slots, VS 3.0 M, PS version X` (D3DCAPS9);
2. `OnPresent()` of the patch: world detection, dusk rebuild, relight reconciliation, lot relight, status
   ([terrain-relight.md](terrain-relight.md)); dev only: `LightDiag::OnPresent` (Ctrl+Shift+F8);
3. dev only: `LightProbe::OnPresent` (Ctrl+Shift+F7);
4. `ObjectLightBridge::SetStrength/SetAllObjects/OnPresent`, `LevelLightShare::OnPresent`;
5. `LotLightBridge::SetNightLevel(lightMgr+0xF0)`, `SetRoofFix`, `SetWaterFix`, `LotLightBridge::OnPresent` (shader log
   flush, every 20 frames the light enumeration `FUN_006ACF70` that feeds the lamp lists);
6. `LightmapSmooth::SetEnabled`, the remaining setters (`SetSidewalkClear`, `SetLampTint`, `SetFenceGroundLight`,
   `SetWallGain`, `SetObjectPixelLamps(g_objPixel && RigTracker::IsInstalled(), g_objStrength)`,
   `SetObjectPixelLights`), `LightmapSmooth::SetGpuPreferred`, then `LightmapSmooth::OnPresent(device)` (GPU path: timings, fallback hash checks, atlas growth; CPU path: reads changed chunk maps, queues smoothing jobs,
   uploads one finished map).

### Per draw (lot_light_bridge.cpp)

`LotLightBridge::UpdateHooks` registers `SetPixelShader`, `SetVertexShader`, `DrawIndexedPrimitive`, `DrawPrimitive`
and (Priority::Last) `CreatePixelShader` / `CreateVertexShader` hooks under the name `LotLightBridge`, whenever any of
bridge / object fix / roof fix / water fix / wall gain != 1 is on (the combined build also counted the HDR gain).

- **Classification**, cached per shader pointer; every classified shader is AddRef'd into `g_pinned` until `Shutdown`
  (pointer reuse would give a new shader an old class).
  - Pixel shader class (`ClassifyPsCode`, enum `PsClass`): exact size+FNV-1a matches from `shader_ids.h`
    (`LotLight` 568 B, `ObjectRig` 600, `Roof` 1136, `Lake` 1344, `LotLightSnow` 1852, `RoofSnow` 4992), then
    `WallGain` (58-entry `wall_lamp_table.h`), `FloorAtlas` (261-entry `floor_atlas_table.h`), then `WorldCandidate` =
    any PS that declares a sampler s6 or higher, else `Other`.
  - Vertex shader class (`ClassifyVsCode`, `g_vsCache`): exact `kRoofVs`=1, `kLakeVs`=2, `kSnowLotVs`=3, `kFloorVs`=5;
    then by pattern, in this order: `IsRoadVs`=4, `IsInstancedStructureVs`=7, `IsSnowCoverVs`=8, `IsSnowReliefVs`=9,
    `IsFloorVs`=5, `PatchFoliageVs`=6, `PatchObjectLampVs`=10, `IsSnowFloorVs`=11 (last on purpose), else 0.
- **Dispatch** (`OnDrawTracked` -> `OnDrawInner`, first match wins; each handler returns Skip after drawing itself, or
  falls through to Continue = the game draws unchanged):

| Order | Condition | Handler | Doc |
|---|---|---|---|
| 0 | VS class 6 and object fix on | patched foliage VS bound around everything below | [foliage](foliage.md) |
| 1 | PS `ObjectRig` | `DrawObjectRig` (moon-shadow-free HLSL, c3.x = night) | [foliage](foliage.md) |
| 2 | PS `Roof` | `DrawRoof` | [roofs](roofs.md) |
| 3 | PS `RoofSnow` and VS not class 9 | `DrawRoofSnow` (additive pass) | [roofs](roofs.md) |
| 4 | PS `Lake` | `DrawLake` (additive pass) | [water](water.md) |
| 5 | VS class 6 | `DrawLeafShadow` | [foliage](foliage.md) |
| 6 | PS `WallGain` | `DrawWallGain` | [walls](walls.md) |
| - | bridge off (`luzDoPosteNaGramaDoLote` false) | stop here (combined build: HDR gain only) | |
| 7 | VS class 4 | `DrawRoad` | [roads](roads.md) |
| 8 | VS class 5 | `DrawFloor` | [floors](floors.md), [snow](snow.md) |
| 9 | PS `FloorAtlas` and VS not class 11 | `DrawFloorAtlas` | [floors](floors.md) |
| 10 | VS class 7 | `DrawInstanced` | [fences](fences.md) |
| 11 | VS class 8 | `DrawSnowCover` | [snow](snow.md) |
| 12 | VS class 9 | `DrawSnowRelief` | [snow](snow.md) |
| 13 | VS class 10 and the object patch applies | `DrawObjectLamp` (falls through otherwise) | [objects-and-rigs](objects-and-rigs.md) |
| 14 | PS `LotLightSnow` | `DrawLotSnow` | [snow](snow.md), [lot-light-pass](lot-light-pass.md) |
| 15 | PS `WorldCandidate` | `RecordWorldChunk` + smoothed-map swap; if not a chunk and VS class 11: `DrawSnowFloor` | [world-atlas](world-atlas-and-smoothed-maps.md), [snow](snow.md) |
| 16 | PS not `LotLight` | VS class 11: `DrawSnowFloor`; else game | [snow](snow.md) |
| 17 | PS `LotLight` | the lot light pass replacement | [lot-light-pass](lot-light-pass.md) |

- **Own draws** set `g_inOwnCall` so the hooks ignore the mod's own `SetPixelShader`/`Draw*` calls; shaders the module
  creates go through `OwnCreatePs/OwnCreateVs` (thread-local `t_ownCreate`) so the create hooks do not classify them.
- **Pre-creation** (`PrecreatePs/PrecreateVs`): when the game creates one of the exact-match shaders (lot pass, snow
  lot pass, object rig, roof, snowy roof, lake) the replacement is compiled then (usually during loading), so DXVK does
  not compile it in the frame the object first appears. (Combined build. The standalone, 2026-09-28, instead compiles
  the five HLSL replacements of `lot_light_bridge.cpp` and the eight world-light smoothing shaders of
  `lightmap_smooth.cpp` at start-up on a background thread, `framework/shader_cache.h`, see
  [architecture 4.6](../../architecture.md#shader-precompile); the draw hooks only create the objects from the bytecode.) Foliage VS copies are pooled (`g_vsPool`, max 64). Pattern
  patches (roads, floors, fences, objects...) stay lazy (made at first draw), because which patch applies depends on
  the VS it is drawn with.
- **Robustness** (review 25/09): hooks catch C++ exceptions (`HookFailed` switches everything off until restart, log
  "Excecao dentro do gancho de desenho ..."); `g_stateUnknown` reads the bound VS/PS from the device at the first draw
  after hooks are registered; `UpdateHooks` has a mutex; `Shutdown` clears every fix flag before `SetEnabled(false)` so
  hooks are unregistered before shaders are released.
- **Own cost (standalone, 2026-09-29; `research\perf2\apexcost\report.md` items P3-P9; written, not compiled or tested in
  game yet).** Same pixels, less CPU per replaced draw:
  - the handlers' own `SetPixelShader` / `SetVertexShader` / `SetTexture` / `Set*ShaderConstantF` go straight to the
    device below Apex's detours (`D3D9Hooks::CallOriginal*`, wrappers `SetPs` / `SetVs` / `SetTex` / `SetPsConst` /
    `SetVsConst`): before, each re-entered Apex's own chains, where only the bridge's own tracking (which skips them,
    `g_inOwnCall`) and the profiler's state counts looked at them. The replaced draw itself is still re-issued through
    the device, so Post-scene / Picture trigger counts, Light Probe, Frame Capture and the profiler see it as before.
    `SetSamplerState` / `SetRenderState` are not in the registry: plain device calls, as before;
  - `SamplerBind` (and the lot pass s2 / snowy lot pass s12 bindings, which now use it) sets and restores only the
    sampler states and texture that differ from what is bound;
  - `TrackPs` / `TrackVs` do nothing when the game sets the same shader again; everything known about a vertex shader
    is one `VsInfo` entry (class, road uv constant, snow-floor TEXCOORD, patched foliage / object copy, outdoor-floor
    copy) with a pointer to the current one, instead of six maps looked up per draw;
  - `SelectLamps` results are memoized per (x, z, maxScore) (exact float bits) until the lamp list changes; the 20-frame
    lamp refresh reads the enumeration once (`ReadEnumeratedLamps`), tracks lot lamps in two reused sorted vectors
    instead of a std::map, and rebuilds the bake snapshot's lamps only when a lamp changed (see
    [lot-light-pass](lot-light-pass.md) "Lot lamp change tracking", [roofs](roofs.md)); the Frame Profiler shows it as
    "Lamp refresh (mod)";
  - `RecordWorldChunk` hands back the chunk's `g_chunks` entry (no second lookup); world light smoothing keeps its
    chunks-by-use order between calls ([world-atlas](world-atlas-and-smoothed-maps.md)).

### Game-code side

| Module | What it changes | Doc |
|---|---|---|
| night_terrain_relight_patch.cpp | terrain bake visitor 0xC29626, arm sites 0x6B6516/0x6B60D3/0x6B6618, story gate 0xC294D9; experimental 0x6BE18C, 0xADB66B/0xADB884, 0xC7F87D | [terrain-relight](terrain-relight.md) |
| level_light_share.cpp | room-0 gather 0x6C5816/0x6C7094, cascade jcc 0x6C73B1, solve-point calls, light vfunc+0x4C of the 9 classes | [level-light-share](level-light-share.md) |
| object_light_bridge.cpp | light-colour vfunc+0x10 of the light classes, rig brightness cap read 0x6B9418, rig ctor calls in `FUN_006f7880`, room gather thunk 0x6BBE70, lamp colour 0x6B0BDE + creation sites | [objects-and-rigs](objects-and-rigs.md), [lamp-colour](lamp-colour.md) |
| rig_tracker.cpp | binder call 0x6F68C5, Detours on `FUN_006f6250` and `FUN_006cf920` | [objects-and-rigs](objects-and-rigs.md) |

## Files and functions

| File (combined tree) | Role |
|---|---|
| `patches/night_terrain_relight_patch.cpp` | The patch class (`NightTerrainRelightPatch`), all settings, the whole UI (`RenderCustomUI`, `RenderDeveloperUI`, `ApexRenderReflectionsUI`), terrain bake patches, dusk rebuild, relight reconciliation, Present driver, `APEX_REGISTER_FEATURE` metadata (`displayName = "Night Lighting"`, category Graphics) |
| `lot_light_bridge.cpp/.h` | D3D9 draw interception: classification, dispatch, every `Draw*` handler, HLSL replacements (`kReplacementHlsl`, `kObjectRigHlsl`), lamp enumeration and selection (`g_allLamps`, `SelectLamps`, `SelectPixelLamps`), outdoor lot lamp list for the reconciliation (`ForEachOutdoorLotLamp`), census/false colour, `DescribeDraw` for F7 |
| `shader_patches.cpp/.h` | Pure functions on D3D9 bytecode: recognisers (`IsRoadVs`, `IsFloorVs`, `IsSnowFloorVs`, `IsSnowCoverVs`, `IsSnowReliefVs`, `IsInstancedStructureVs`) and patchers (`PatchRoad`, `PatchFloor`, `PatchSnowFloor`, `PatchBakedAtlasPs`, `PatchSnowCover`, `PatchSnowRelief`, `PatchInstancedLamps`, `PatchObjectLampVs/Ps`, `PatchFoliageVs`, `PatchLeafShadow`), `LightMapScaleConst`, `CodeBytes`. Testable offline on captured shaders |
| `lightmap_smooth.cpp/.h` | Smoothed 1024x1024 chunk light maps and the world light atlas |
| `level_light_share.cpp/.h` | Outdoor lamps shared across storeys, cross-storey wall test |
| `object_light_bridge.cpp/.h` | Rig lamp boost for all light classes, lamp colour, stairs/railings rig flag, fenced-area gather |
| `rig_tracker.cpp/.h` | Which rig (mode 0/1/2, centre) lit the current draw |
| `shader_ids.h` | Size + FNV-1a of the exact-match game shaders (never the bytecode) |
| `wall_lamp_table.h` | 58 ExteriorWall PS: size, FNV-1a, lamp constant K |
| `floor_atlas_table.h` | 261 ExteriorFloors PS accepted by `PatchBakedAtlasPs` |
| `roof_ps.hlsl` / `roof_ps_hlsl.h`, `roof_snow_lamps_ps.hlsl` / `_hlsl.h`, `water_lamps_ps.hlsl` / `_hlsl.h` | HLSL sources embedded as generated headers |
| `patches/smooth_streaming_patch.cpp` | `SmoothStreamingRelightTerrainRects`, the localized terrain relight used by the reconciliation |
| `light_probe.cpp`, `light_diag.cpp` | Dev tools F7 / F8 ([../dev-tools/light-probe.md](../dev-tools/light-probe.md), [../dev-tools/light-diag.md](../dev-tools/light-diag.md)) |
| `render_callbacks.h` | `endSceneBeforeOverlay` (deferred reinstall) and `preReset` (`LightmapSmooth::OnPreReset`) slots |
| `d3d9_extra_hooks.h`, `depth_share.h` | Raw depth-stencil get/set and the INTZ depth used by the lake pass |
| `build_flavor.h` | `kPublicBuild`: hides Developer UI, F7, F8 |

## Game addresses and patterns

Only the addresses owned by the patch file itself; each sub-doc has its own table.

| Address | What | Verification |
|---|---|---|
| 0x006E97B0 | root getter: `A1 <imm32> 85 C0 75 01 C3 8B 80 C0 01 00 00`; imm32 = address of the root pointer; lightMgr = *(root+0x1C0) | `Install` compares the bytes with the imm32 masked; Fail "Light manager code differs at 0x6E97B0" |
| lightMgr+0xF0 | night level (0 day .. 1 night); "night" = > 0.99 | read every frame |
| lightMgr+0x104 | light cells; +0x38 / +0x3C countdowns | read every frame |
| 0x00C29626 | terrain bake light visitor (in 0xC29620, vtable 0x010768A0) | bytes `8B 07 8B 50 20 8B F1 8B CF FF D2` |
| 0x006B6516, 0x006B60D3, 0x006B6618 | light register / remove / move-toggle arm tests | bytes `8B 17 8B 42 20 8B CF FF D0` |
| 0x00C294D9 | story gate of the bake `FUN_00C292B0` | 7 bytes + context before/after (optional; warning if absent) |
| 0x006C7160 | room queue `FUN_006c7160` thiscall(treeLevel, roomId) | bytes `83 EC 2C 53 55 56 33 DB 8B F1` (Fail otherwise) |
| 0x006BE18C | street lamp colour in the lot solve (experimental) | `0F 28 86 E0 00 00 00` |
| 0x00ADB66B, 0x00ADB884 | lot quality byte (experimental) | `C6 44 24 0C 00` -> `... 01`. The technical-details text says 0xADB66F/0xADB888 (the immediate byte); the patched instruction starts 4 bytes earlier |
| 0x00C7F87D | lot pass terrain lightmap bind (experimental) | 8 bytes + 16 bytes context + 9 bytes at 0xC7F8B7 |
| 0x006ACF70 | light enumeration (lot_light_bridge.cpp `EnumerateLights`) | `E8 2B 36 00 00 8B 4C 24 04 51 68 40 CF 6A 00` |

## Shader details

See the sub-docs. Common conventions of all pattern patches (`shader_patches.cpp` header): extra sampler = highest
declared sampler + 1, temporary = highest temp + 1, new constant = highest constant + 1 (refuse if >= 224 or no room);
the patch fails and leaves the shader alone when its pattern is absent; world position comes either from the VS
constants c8/c10 (world matrix rows, `.w` = translation) or from a TEXCOORD the game already writes.

## Interactions

- **Split-Level Lighting Fix** (S3SS patch `SplitLevelLightingFix`, rewrites `BaseLight::GetLotID` at 0x006BC020 to
  return 0): with it on, type-11 lot lights enter every storey's room-0 list through the world-light gather; Night
  Lighting's level share was designed alongside it (level_light_share.cpp header). The combined build no longer compiles
  it (`patches/split_level_lighting_fix_patch.cpp` is not in the vcxproj since v0.2.0), but the **official S3SS that runs
  next to the standalone still has it**. Inferred from the decompile of `FUN_00C292B0` (`re/out/fn_00c292b0.c` line
  114: `GetLotID() == 0 || light+0xD0 == 0`): with it on, every light the visitor accepts passes the bake's lot/storey
  test, so `BakeLevelStub` is never reached (story-gate counters stay 0) and basement lot lamps are baked too, which the
  reconciliation's `Bakeable` model does not track (their changes get no local relight). Unverified in game; see
  [terrain-relight.md](terrain-relight.md).
- **Smooth Streaming**: the reconciliation's local relight goes through its terrain queue when that part is on, else
  flags chunks directly ([terrain-relight](terrain-relight.md)).
- **Depth Blur**: the lake pass reads its INTZ depth and marks its own pass as internal ([water](water.md)).
- **Picture filters / Edge Smoothing**: post-scene, no interaction with the draw hooks.
- **D3D9 hook order**: the bridge's draw hooks return Skip after drawing, which cuts the hook chain for that draw.
  In the combined build the HDR hooks registered at `Priority::First` for this reason.

## Known limitations

- Game version must be Steam 1.67.2; every patch site is byte-checked.
- Ground light (atlas) has no height and no wall occlusion; the terrain stamp itself has no wall occlusion (inferred,
  ground_report.md section 5), so `max(lot, terrain)` can show street-lamp light through lot walls.
- Paths still drawn only by the game (ground_report.md section 4, re-checked against the code): summer multi-pass
  terrain light pass 475E594D/756 (declares only s0, s1, s2, s5: never `WorldCandidate`); winter lot light pass with VS
  436BB272/1348 (needs exact `kSnowLotVs`); TerrainLow distant terrain; Sims.
- Per-pixel lamp light on walls/floors (PASSO3 plan) was never implemented; walls only get a gain.

## Pitfalls and failed approaches

Global ones (details in each sub-doc):
- Reinstalling on the message-loop thread crashed (textures released while drawn) -> `DeferredReinstall` on the
  render thread (review 25/09 item 6).
- `Uninstall` called `DirtyAllRigs` off the render thread -> deferred to `OnPresent` (item 3).
- `PatchLeafShadow` once emitted an `lrp` with two constant sources: DXVK accepts it, native D3D9 refuses (item 5). All
  patches must follow native D3D9 rules.
- Caches keyed by shader pointer without AddRef gave stale classes when addresses were reused -> `g_pinned`.
- Any menu change used to switch off `automaticoAoAnoitecer` when `luzDoLoteNaGrama` was off (fixed 25/09 10:40).

## Testing in game

- `S3SS_LOG.txt`: `[NightTerrainRelight] Installed (at dusk=..., lot lights on the ground=..., delay=...s, root=0x...)`,
  the story-gate warning if 0xC294D9 differs, `[LotLightBridge] Active` (lot pass compiled) or `Failed: ...`, and the
  batched line `[LotLightBridge] Shaders at their first draw: <kind>: corrigido xN (...)` /
  `<kind>: sem o padrao esperado, fica como o jogo xN (...)` (the combined code still logs these kinds in Portuguese).
- Dev build: `S3SS\ShadersRecusados\<fix>_PS_<hash>.bin` holds every shader a pattern patch refused (max 300/session).
- Ctrl+Shift+F7 over a pixel: `S3SS_LightProbe.txt` lists the draws covering it; the `mod:` line
  (`LotLightBridge::DescribeDraw`) says whether the mod redrew it and with which class.
- Ctrl+Shift+F8: `S3SS_LightDiag.txt` (all lights, lots, storeys, rooms, the "LUZ POR PIXEL" section).
- Developer > "False colour: magenta ..." paints lamp-lit draws no fix claimed; "Census" writes `S3SS_Censo.txt`
  ([../dev-tools/census.md](../dev-tools/census.md)).

## Open items

From `ROADMAP-NIGHT-REMAKE.md` (phases) and the notes' last entries:
- Phase 1: plants with ground light (m79), OutdoorProp shaders (C0C6E0FF, 2BE88B48), 4-light-matrix foliage
  (VS 4375A3EE), instanced SingleObject, 34 object shaders with no free input.
- Phase 2: per-pixel lamps on walls and floors (PASSO3-PLANO.md, with its 8 MUST-FIX items); terrain stamp re-rendered
  at 4 texels/m.
- Phase 3: one lamp model for every surface (today: rig bounds radius, sqrt(range) radius for roofs/water, W = 0.4 x
  range for per-pixel objects, wall gain).
- Terrain bake: the 28/09 capture showed even story-0 lot lamps missing from the atlas; the story-gate counters were
  added to find where they are lost ([terrain-relight](terrain-relight.md)).

## Sub-documents

Part 1: [lot-light-pass.md](lot-light-pass.md), [world-atlas-and-smoothed-maps.md](world-atlas-and-smoothed-maps.md),
[walls.md](walls.md), [floors.md](floors.md), [roads.md](roads.md), [snow.md](snow.md),
[terrain-relight.md](terrain-relight.md).
Part 2: [level-light-share.md](level-light-share.md), [foliage.md](foliage.md),
[objects-and-rigs.md](objects-and-rigs.md), [fences.md](fences.md), [lamp-colour.md](lamp-colour.md),
[roofs.md](roofs.md), [water.md](water.md), [../reflections.md](../reflections.md).
Engine background: [../../engine/terrain-and-light-bake.md](../../engine/terrain-and-light-bake.md),
[../../engine/room-light-maps.md](../../engine/room-light-maps.md),
[../../engine/light-objects-and-rigs.md](../../engine/light-objects-and-rigs.md),
[../../engine/shaders.md](../../engine/shaders.md).
