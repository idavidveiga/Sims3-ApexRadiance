# Shader census, false colour, refused-shader dump and offline coverage tests

> Four related development tools that answer "which of the game's shaders still get no lamp-light fix, and would a
> pattern patch accept them?":
> 1. **False colour** (in game): every draw that receives baked lamp light or an outdoor rig but that no Night Lighting
>    fix claimed is painted solid magenta.
> 2. **Census** (in game): the same classification recorded for ~3 frames per (VS, PS) pair into `S3SS_Censo.txt`, with
>    the bytecode of every unfixed pair saved to `S3SS\Censo\`.
> 3. **Refused shaders** (in game, automatic): every shader a fix tried and refused is saved to `S3SS\ShadersRecusados\`.
> 4. **Offline coverage tests** (scratchpad): small read-only C++ harnesses that compile `shader_patches.cpp` and run the
>    patchers over captured shaders or over every shader of `Shaders_Win32.precomp`, validating output with
>    `D3DDisassemble`.
>
> **Status: dev-only.** 1-3 live in `lot_light_bridge.cpp` and are disabled by `kPublicBuild` (`OnDraw` falls straight
> through, `SaveRefused` returns at once); the UI is in Night Lighting's Developer section, which the public build
> compiles out. 4 is not part of the mod.

## Purpose

After many surface-by-surface fixes, NOTAS' 25/09 deep review (~14:35, "Pendente, proposto pelos revisores", item 2)
asked for an automatic census: "list every outdoor VS/PS pair no fix took, with the refusal reason and pixel coverage;
false-colour mode: magenta = no fix". It was built at 25/09 ~17:00 (NOTAS "Censo + cor falsa") and drove the later
generalisations (Phong outdoor objects, summer ExteriorFloors, Counters/Phong coverage). ROADMAP-NIGHT-REMAKE.md makes
it the acceptance measure: no outdoor object in magenta on a summer and a winter test lot.

## User-facing settings

All in Apex tab > Night Lighting > **Developer** > Tools (`RenderDeveloperUI`, `patches/night_terrain_relight_patch.cpp`),
below "Save light diagnostic". No TOML keys (the false-colour checkbox is a function-static bool, not saved).

| UI label | Type | Effect |
|---|---|---|
| "False colour: magenta = gets lamp light but no fix applied" | checkbox | `LotLightBridge::SetFalseColor(bool)` |
| "Census: write S3SS_Censo.txt" (+ status `(ready)` / `(writing...)`) | button | `LotLightBridge::RequestCensus()`; tooltip: "Lists every shader pair that draws with baked lamp light or an outdoor rig, and which ones the mod fixes." |

Both need the bridge's draw hooks registered, i.e. Night Lighting installed with at least one of: the bridge ("Street
lamps light inside lots"), object shadow fix, roofs, water, wall strength != 1, or HDR lamp gain != 1
(`UpdateHooks` in `lot_light_bridge.cpp`). Otherwise nothing is recorded or painted.

## How it works

### Candidate test (`LitCandidate`, `lot_light_bridge.cpp` ~line 1945)

A draw is a lamp-lit candidate when any sampler s0..s15 binds:
- a baked light map: 2D, `D3DFMT_A8R8G8B8`, not DEFAULT pool, 1 mip level, width and height <= 1024 (room, wall, floor
  and lot maps; described as `s<n>:mapa<W>x<H>`), or
- a terrain chunk map: `D3DFMT_DXT5` 256x256 with <= 5 levels (`s<n>:terreno`),

or it is drawn with an outdoor rig (`RigTracker::CurrentMode() == 2`; described as ` (so rig)` = rig only).

### Classification (`OnDraw` template, the bridge's draw hook, `Priority::Normal`)

`OnDraw` wraps `OnDrawTracked` (all the fixes). When neither false colour nor a census is active, or in the public
build, or inside the bridge's own call, it is a plain pass-through. Otherwise: evaluate the candidate test and rig mode
first, run `OnDrawTracked`, and treat the draw as **claimed** if it returned `HookAction::Skip` (a fix redrew it).

- **Census**: `g_census[{g_curVs, g_curPs}]` accumulates draws, claimed draws, primitives (`g_curPrims`, set by both
  the DIP and DP hooks), the last rig mode and the first texture description.
- **False colour**: an unclaimed candidate with a pixel shader is redrawn with a magenta shader and the hook returns
  `Skip`. The magenta shaders are built once from tokens (ps_2_0 and ps_3_0, chosen by the game PS's version via
  `PsIs3`): `def c0, 1, 0, 1, 1` + `mov oC0, c0` (`0xFFFF0200/0300, 0x05000051, 0xA00F0000, 1.0, 0, 1.0, 1.0,
  0x02000001, 0x800F0800, 0xA0E40000, 0x0000FFFF`), created with `OwnCreatePs`. All other state (blending, depth,
  textures) is the game's, so a magenta draw in a multiplicative pass (e.g. the modulate2x lot light pass) appears
  tinted rather than pure magenta.

### Census window and file (`RequestCensus`, `LotLightBridge::OnPresent`, `WriteCensus`)

`RequestCensus` clears the map and sets `g_censusFrames = 3`; each Present decrements it and at 0 `WriteCensus` runs
(on the render thread) and `g_censusPending` is cleared. The window is therefore the rest of the current frame plus
roughly two full frames.

Output `Documents\Electronic Arts\<localized>\S3SS\S3SS_Censo.txt` (truncated) and folder `S3SS\Censo\`:

```
S3SS Censo: desenhos que recebem luz assada (mapa de luz) ou rig de fora, por par de shaders
colunas: desenhos | corrigidos | triangulos | rig | VS hash/tamanho | PS hash/tamanho | texturas

ok     10 |    10 |     288 |  2 | VS 9CA7D90C/1296 | PS 4D1636F8/804 | (so rig)
SEM    26 |     0 |    1068 | -1 | VS 3A9A86BF/696 | PS 1E84E3E5/740 | s1:mapa32x32
...
62 pares, 38 sem correcao (codigo em S3SS\Censo)
```

| Column | Meaning |
|---|---|
| `ok` / `SEM` | at least one draw of the pair was claimed / none ("SEM" = without a fix) |
| desenhos | candidate draws of the pair in the window |
| corrigidos | of those, claimed by a fix |
| triangulos | sum of primitive counts |
| rig | last `RigTracker::CurrentMode()` seen: -1 none, 0 indoor with ceiling, 1 roofless fenced area, 2 outdoor |
| VS/PS hash/tamanho | FNV-1a 32 of the bytecode (`h = 2166136261; h = (h ^ dword) * 16777619` over DWORDs) and size in bytes; 0 if unreadable |
| texturas | light maps found, or `(so rig)` |

For every `SEM` pair, `Censo\VS_<hash>.bin` and `Censo\PS_<hash>.bin` hold the raw bytecode (named by content hash,
stable across sessions; 187 files on disk). Log line: `[LotLightBridge] Censo gravado: <pairs> pares, <n> sem correcao`.

Caveat on "claimed": a fix that changes the draw without returning `Skip` counts as unclaimed. Example: the wall
strength fix (`DrawWallGain`) only redraws when the gain != 1, so with "Lamp light on outside walls" at 1 the
ExteriorWall pairs show as `SEM`.

### Refused shaders: `S3SS\ShadersRecusados\` (`SaveRefused`)

`PatchedFor` (the per-shader patch cache in `lot_light_bridge.cpp`) calls `SaveRefused(what, original)` whenever a fix's
pattern did not match (`NoteShader "<fix>: sem o padrao esperado, fica como o jogo"`) or D3D refused the patched shader
(`LOG_WARNING "[LotLightBridge] <fix>: shader <ptr> recusado pelo D3D (<hr>)"`). Dev build only.

- Files: `<tag>_PS_<fnv>.bin` and, when the current VS is readable, `<tag>_PS_<fnv>_VS_<fnvVS>.bin` (the pair's VS),
  where `<tag>` is the fix name with non-alphanumerics replaced by `_` (e.g. `Folhagem__sombra_da_lua__PS_0556959F.bin`
  from "Folhagem (sombra da lua)").
- Named by content, never rewritten if the file exists, at most 300 files per session (`static int written`). The
  folder is never cleared (405 files on 28/09, from several sessions).
- History: first version (25/09 ~13:50) named files by pointer (`<fix>_PS_<ptr>.bin`); the deep review (~14:35) switched
  to content names and the 300 cap. Its first use diagnosed 14 refused "Objeto de fora (luz do chao)" PS
  (`scratchpad\snowcover\test5.cpp`), leading to 4 newly accepted variants and 6 identified as HD "Night" shaders that
  already have per-pixel lamps.

## Results recorded

**Census of 25/09 ~17:05: 84 pairs, 49 without a fix.** Mapped to technique names with
`scratchpad\passo3\ground\techof.pl` (precomp + `scratchpad\shd\knm.tsv`), output `scratchpad\censo_tech.txt`
(one line per census PS: `PS_<fnv>.bin md5=<8 hex> blobs=<precomp blob indices> techs=<Technique:passes ...>`).

| Verdict | Pairs / techniques |
|---|---|
| Need nothing | picking passes (Pick*, LotPondPick, PoolPick...), shadows and imposters (ImposterShadows, LotImposter), TerrainFog, glass (GlassForObjects/Portals); interiors (InteriorWall/Floor, Ceiling, Rug, FloorThickness, Masked/Normal); TerrainHigh (layers without light); "Night" (FFAEA7F4 and others, 7 PS in `censo_tech.txt`: the HD mode with 6 per-pixel lamps); Phong with a 32x32 map and rig 0 (94 draws: indoor objects, left as is) |
| Fixed right after | outdoor Phong C249A5C0 and BC7DE009 (rig 2): `PatchObjectLampVs` picks the triple fed directly by POSITION when several exist (VS_E3A718D3: world c19..c21, view c12..c14); `PatchObjectLampPs` accepts sun in c13; flow control allowed after everything the patch reads and inserts |
| Pending (as of 25/09) | OutdoorProp C0C6E0FF (and 2BE88B48): 1 lamp + sun structure, needs its own recogniser, VS already uses TEXCOORD7; foliage vs_2_0 VS_4375A3EE/EAB58655 with PS 936D7C02 (46 draws, 4-light matrix c8..c13, `max r0, r0, c31.w`, unknown which is the sun); instanced (POSITION1) SingleObject D4FD3CB3 / VS 9E59FCE3 and 8F40BA1C; TerrainLow FF6760A8 (distant terrain, DXT5 map in s3, not smoothed); ExteriorFloors 9F1F5543 (then fixed at ~17:25 with `floor_atlas_table.h`, 261 PS); Sims |

`scratchpad\snowcover\test12.cpp` (`censotest.exe`, output `censotest.txt`) is the census follow-up: for every `SEM`
line of `S3SS_Censo.txt` it loads `Censo\VS_/PS_<hash>.bin` and prints how the classifiers see the VS
(`classe=objeto/nenhuma`) and whether the object / foliage PS patchers accept the PS.

The census file currently on disk (28/09 12:47, another scene) reads 62 pairs, 38 `SEM`: numbers depend on the scene;
compare censuses of the same save and camera only.

## Offline coverage tests

Method (user memory note): "small read-only C++ harnesses in the scratchpad that include `shader_patches.cpp`...
**Never write many files from a test exe: Kaspersky flagged one as ransomware**" (also ROADMAP-NIGHT-REMAKE.md,
risks: "the offline tests cannot write many files, because Kaspersky already flagged one as ransomware"). Harnesses
print to stdout; at most a handful of `.txt`/`.bin` dumps.

Build pattern (`scratchpad\snowcover\build14.bat`):
```
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul
cl /nologo /std:c++20 /EHsc /O1 /I %USERPROFILE%\Desktop\S3SS-dev\Sims3SettingsSetter test14.cpp ^
   %USERPROFILE%\Desktop\S3SS-dev\Sims3SettingsSetter\shader_patches.cpp d3dcompiler.lib /Fe:countertest.exe
```
(some tests `#include` `shader_patches.cpp` directly instead). Each loads `.bin` bytecode as DWORDs, runs a
`ShaderPatches::Patch*`/`Is*Vs` function, and checks the patched output with `D3DDisassemble` (valid = disassembles).
This mirrors the in-game `D3D` refusal only partly: DXVK accepts some invalid SM2 code that native D3D9 refuses (review
25/09 ~03:30: a `lrp` with two constants in ps_2_0), so validity must be judged by `D3DDisassemble`, not by "it ran in game".

Shader sources for the tests:
- captured shaders: `Documents\...\S3SS\LightProbe-*\{VS,PS}_<ptr>.bin` (deduplicate by content);
- `S3SS\Censo\*.bin`, `S3SS\ShadersRecusados\*.bin`;
- every game shader: `C:\Games\Hydra\The Sims 3\Game\Bin\Shaders_Win32.precomp` (read-only), split by
  `scratchpad\passo3\shdlist.pl` (walks `VSHD`/`PSHD` blobs, finds the version token, trims at `0x0000FFFF`), with
  technique names from `TECH`/`PASS` records (`passo3\ground\techof.pl`, `techps.pl`, name hashes in `shd\knm.tsv`);
  extracted families in `scratchpad\counterfam\` (2139 Counters/Phong variants), `floorfam\` (867), `passo3\...`.

Harness index (`scratchpad\snowcover\`, plus `fencetest\`, `leaftest\`, `roadtest\`, `revtest\`, `review1\`):

| Test | What it checked (result in NOTAS) |
|---|---|
| `test4.cpp` | captured draws whose VS is an outdoor-object VS but whose PS `PatchObjectLampPs` refuses |
| `test5.cpp` | why `PatchObjectLampPs` refuses each `ShadersRecusados` PS (first failing step); 14 refused -> 4 fixable |
| `test6.cpp` | object patch over refused and known-good shaders, validated |
| `test7.cpp` | foliage patchers after the m63 generalisation over 77 VS / 99 PS: foliage VS 6 -> 8, leaf-shadow PS 2 -> 3, all valid |
| `test8.cpp`, `test9.cpp`, `test10.cpp` | `IsFloorVs`, `IsSnowFloorVs`/`PatchSnowFloor` over all captured shaders (only the intended pairs match) |
| `test11.cpp` | wall lamp table: all ExteriorWall PS of the precomp -> `wall_lamp_table.h` (58 variants, K = c2 in 26, c3 in 32); no InteriorWall/AOSI/Unlit match |
| `test12.cpp` | census follow-up (above) |
| `test13.cpp` | floor table: ExteriorFloors PS accepted by `PatchBakedAtlasPs` -> `floor_atlas_table.h` (261 of 571; 0 of 58 InteriorFloor) |
| `test14.cpp` (`countertest.exe`) | Counters/Phong coverage over `counterfam\`: VS 588/588, Counters PS 136/184, Phong PS 426/444, 0 invalid (v5.6) |
| `fencetest`, `leaftest`, `roadtest*.ps1`, `snowtest.ps1` | fence/instanced, leaf shadow, the 4 road variants (m06/m10/m18/m24), snow patches |
| `counter_sh\REPORT.md` | an agent's perl port of the object patchers over the 2139 disassemblies (reproduced the C++ counts, planned the v5.6 generalisation) |

Also in the scratchpad, from before the in-game census: **`census.ps1`** (25/09 10:44) is a token-level PowerShell
census of captured shader `.bin` files (temps, constants used, `def`s, relative addressing, flow control, inputs,
samplers; for PS the first `dp3 ?, rN, c9`, the `texld s1` cube read, the ambient `mad rA, rCube, cK.x, rD` and the
diffuse chain c7 -> c6 -> c8 -> c5 behind it, and `add rX, rA, v0`; for VS the `dp4` world triples and the object position
`mul rP, r?.w, v0`). Its output `census_ps.txt` covers 72 shaders from the `LightProbe-*` folders; it was the pattern
study behind `PatchObjectLampPs`.

## Files and functions

| File | Symbol | Role |
|---|---|---|
| `lot_light_bridge.cpp` | `LitCandidate`, `PsIs3`, `OnDraw<DrawFn>`, `g_falseColor`, `g_magenta`, `g_census`, `CensusRow` | candidate test, magenta, recording |
| | `WriteCensus`, `RequestCensus`, `CensusStatus`, `SetFalseColor`, `OnPresent` (countdown) | census file |
| | `SaveRefused`, `PatchedFor`, `ShaderCode` | refused-shader dump |
| `lot_light_bridge.h` | `SetFalseColor`, `RequestCensus`, `CensusStatus` | API |
| `patches/night_terrain_relight_patch.cpp` | `RenderDeveloperUI` | UI |
| `shader_patches.cpp/.h` | `ShaderPatches::*` | the patchers the offline tests exercise |

## Game addresses and patterns

None (D3D9 level). The candidate formats encode the light-map families: A8R8G8B8 managed single-level maps (room / wall
/ floor / lot / per-object 32x32), DXT5 256x256 terrain chunk maps.

## Interactions

- Rig mode comes from `RigTracker` (Night Lighting's rig tracker, installed with the per-pixel object option).
- False colour returns `Skip`, so later hooks (HDR, Frame Capture, Light Probe at `Last`) see the magenta redraw, not the
  game's draw. The Light Probe's "mod:" line would read "desenho do mod" for those (inferred).
- The census counts the Light Probe's 1x1 occlusion copies too if both run in the same frame (inferred; avoid).

## Known limitations

- Candidates are found by texture format only: an unlit surface that binds any small A8R8G8B8 single-level texture is
  also a candidate; a lamp-lit surface that uses per-pixel lamp constants (HD "Night") and no map is not.
- Pairs are keyed by D3D pointers during the window; the file shows content hashes.
- Rig column keeps only the last value.

## Pitfalls and failed approaches

- Do not treat `SEM` as "broken": many pairs need nothing (interiors, picking, shadows, HD Night); check the technique
  with `techof.pl` first.
- Never write hundreds of output files from a test executable (Kaspersky ransomware heuristic). Keep dumps to a few
  examples (as `test14.cpp` does: one example per PS shape).
- Name dumped shaders by content hash, not pointer (the ShadersRecusados rename, 25/09 review).
- Offline "valid" means `D3DDisassemble` accepts it; also check instruction slots (ps_3_0 512 minimum; the object
  patches were sized against it) and that the patch does not claim unintended families (tests 8-10, 13 check that
  InteriorFloor / terrain / water stay at zero).

## Testing in game

1. Dev build, Night Lighting on (bridge on), at night on a test lot.
2. Tick false colour, walk around: magenta = lamp-lit surfaces no fix handles; Ctrl+Shift+F7 on them.
3. Click "Census: write S3SS_Censo.txt"; status returns to `(ready)`; log `[LotLightBridge] Censo gravado: ...`.
4. Map the `SEM` PS hashes to techniques offline (`techof.pl` on `S3SS\Censo\PS_*.bin`), then run the relevant harness.

## Open items

- Standalone rename (PLANO-SEPARACAO.md section 4): `Documents\Electronic Arts\<localized>\Apex Radiance\ApexRadiance_Censo.txt`;
  the `Censo\` and `ShadersRecusados\` folders are not named in the plan (presumably under `Apex\` too).
- NOTAS item "count refused vs replaced ExteriorWall/ExteriorFloors draws before enabling by default" (PASSO3 MUST-FIX 8)
  can reuse the census.
- The census does not record the refusal reason or pixel coverage asked for by the review; the reason is only in the log
  and in `ShadersRecusados`.
