# Light Probe (Ctrl+Shift+F7)

> GPU probe of one screen pixel. With the mouse over a pixel, Ctrl+Shift+F7 records, for the next frame, every draw
> call that touches that pixel (found with a 1x1 scissored occlusion-query copy of each draw), and writes
> `S3SS_LightProbe.txt` with each draw's shaders (bytecode + disassembly), 14 render states, the 16 samplers' textures
> and filters, all non-zero PS/VS float constants, what the Night Lighting code did with the draw, the final screen colour
> and a register diff against the previous capture. The textures are dumped as BMP. A UI list lets you replace any of
> those textures with solid black or white live, to see what it contributes. **Status: working, dev-only.** Compiled
> into both flavours (`light_probe.cpp` is in the vcxproj unconditionally) but only reachable in the dev build: the
> Present call and the UI are behind `if constexpr (!kPublicBuild)` (`build_flavor.h`, `S3SS_PUBLIC`).
> This tool is the single most important instrument of the project: almost every lighting fix in
> `NOTAS-ILUMINACAO.md` starts from a capture (`LightProbe-<name>`, `LightProbe-m01`..`m80`).

## Purpose

Static reverse engineering of the lighting paths "failed repeatedly" (user memory note `sims3-lot-edge-lighting.md`).
What works is: look at the exact pixel that is wrong, list the draws that paint it, read their shader and the
constant values the game actually uploaded, and compare with a pixel that is right. The probe answers:

- which draw(s) paint the pixel, in draw order, with blend state (e.g. a modulate2x lot light pass over a base pass);
- which shader pair (VS/PS) and which technique family, with the disassembly to write a byte-pattern patch;
- where the light comes from: a baked light map on some sampler (size/format tell the family: 256x256 DXT5 terrain
  chunk map, A8R8G8B8 room/wall/floor atlases, 32x32 per-object maps), rig lamps in PS c1..c3/c5..c7, vertex lights in
  VS c4..c11, or nothing (roofs, water: "no lamp term at all");
- whether the mod already replaced the draw ("mod:" line) and, for objects, why not;
- which constants differ between two neighbouring pieces (automatic comparison of two consecutive captures).

## User-facing settings

No TOML keys. The tool is part of the Night Lighting patch (TOML `[patches.NightTerrainRelight]`) and only runs while
that patch is installed.

| UI | Where | Type | Notes |
|---|---|---|---|
| Hotkey **Ctrl+Shift+F7** | in game, menu closed | chord, edge-triggered | Polled with `GetAsyncKeyState` in `LightProbe::OnPresent`. There is no bare-F7 binding (task text sometimes says "F7": it always means Ctrl+Shift+F7). |
| Status line | Apex tab > Night Lighting > **Developer** > "Light probe" | text | `Ready. Mouse over the grass and press Ctrl+Shift+F7.` / `Measuring pixel (x, y)...` / `Measured: N draws cover pixel (x, y), M textures saved. See S3SS_LightProbe.txt and the LightProbe folder.` |
| "Replace with white (unticked = black)" | same | checkbox | colour used for blanked textures (`g_blankWhite`) |
| `T<n>: <desc>` | same, one per texture of the last capture | checkbox | blank this texture in every draw that binds it (live, until unticked) |
| "Untick all" | same | button | |

Measure at night, with the S3SS menu closed, mouse over the wrong spot (notes, header of `NOTAS-ILUMINACAO.md`).

## How it works

State machine in `light_probe.cpp` (`enum class State { Idle, Armed, Capturing }`), driven from the Night Lighting
Present hook (`patches/night_terrain_relight_patch.cpp`, `RegisterPresent("NightTerrainRelight", ..., Priority::Last)`,
which calls `LightProbe::OnPresent(ctx.device)` only when `!kPublicBuild`):

1. **Present N (Idle, key edge detected).** `OnPresent` reads the back buffer size, converts `GetCursorPos` to client
   coordinates of `D3DDEVICE_CREATION_PARAMETERS::hFocusWindow` (or the foreground window), scales to back-buffer
   pixels (`g_pixel`, clamped), releases the previous draw list, registers the two draw hooks
   (`RegisterDrawIndexedPrimitive` / `RegisterDrawPrimitive`, name `"LightProbe"`, `Priority::Last`) and goes Armed.
   The hotkey is checked after the state transitions of the same call, so the frame captured is the next one.
2. **Present N+1 (Armed -> Capturing).** Every draw of frame N+2 is now examined by `OnDraw`.
3. **Each draw while Capturing** (up to `kMaxDraws = 4000`), only if render target 0 has the back-buffer size
   ("screen-sized"; smaller targets such as bloom, shadow maps and reflections are ignored):
   - `GetTexture` s0..s15 (AddRef'd until the file is written), MIN/MAG/MIP filter of each sampler, VS and PS, 14 render
     states (`kRecordedStates`: ZENABLE, ZWRITEENABLE, ZFUNC, ALPHABLENDENABLE, SRCBLEND, DESTBLEND, BLENDOP,
     ALPHATESTENABLE, COLORWRITEENABLE, STENCILENABLE, CULLMODE, SRGBWRITEENABLE, DEPTHBIAS, SLOPESCALEDEPTHBIAS);
   - all PS constants c0..c223 (`kPsConsts = 224`) and VS constants c0..c255 (`kVsConsts = 256`), non-zero registers
     only (skinned objects keep their world matrix and vertex lights at c184..c199);
   - `LotLightBridge::DescribeDraw()` (the "mod:" line, see below);
   - an occlusion query (`D3DQUERYTYPE_OCCLUSION`, pooled in `g_queryPool`) around a **second copy of the same draw**
     with colour writes off on all 4 MRT slots, Z write off, stencil write mask 0, scissor test on with a 1x1 rect at
     the pixel. `g_inProbeCall` prevents the probe from recording its own copy. The original draw then continues
     normally (`HookAction::Continue`).
4. **Present N+2 (Capturing -> Idle): `FinishCapture`.** Polls every query with `D3DGETDATA_FLUSH` (Sleep(1) loop, at
   most 1.5 s: this stalls the render thread for the time the GPU needs), keeps draws with `samples > 0`, reads the
   final screen colour (`ReadScreenPixel`: StretchRect of the back-buffer pixel to a 1x1 render target, then
   `GetRenderTargetData`; X8R8G8B8/A8R8G8B8 back buffers only), writes the text file, dumps shaders and textures,
   releases the references and unregisters the hooks unless a texture is blanked.

Occlusion samples mean "fragments of this draw at that pixel that passed the depth/stencil/alpha tests at the time the
draw ran". A draw later overdrawn by another still shows samples > 0, and the value is the MSAA sample count
(`amostras=8` with 8x MSAA; that is how capture m25 revealed that the game's anti-aliasing was on, see Pitfalls).

**Texture blanking (live).** While any `T<n>` box is ticked, the hooks stay registered; every draw that binds a ticked
texture on any sampler is re-issued with a 1x1 managed A8R8G8B8 texture (0xFF000000 or 0xFFFFFFFF, created once), the
original textures are restored and the hook returns `Skip`. Unticking everything unregisters the hooks when Idle.

**Two-capture comparison.** For each capture the probe picks one "object draw" among the covering draws: rank 2 if the
"mod:" text starts with `OBJETO` (an object the mod lit), rank 1 if it contains `objeto com rig`, else rank 0; the last
draw of the highest rank wins. The pick (pixel, index, VS/PS pointers, "mod:" text, constants, screen colour) is kept in
memory (`g_prevPick`). The next capture writes a `COMPARACAO` section listing the PS and VS registers whose values differ
(relative tolerance 1e-4). Use: Ctrl+Shift+F7 on piece A, then Ctrl+Shift+F7 on piece B (added 25/09 18:40, "Captura F7
aprofundada"). The first capture after a game start has no comparison.

### Output files

All in `ConfigPaths::GetS3SSDirectory()` = `Documents\Electronic Arts\<localized game folder>\S3SS\`
(`config/config_paths.cpp`, `ResolveLocalizedGameFolder`; on this machine `...\The Sims 3\S3SS\`).

| File | Content |
|---|---|
| `S3SS_LightProbe.txt` | the report (truncated and rewritten by every capture) |
| `LightProbe\VS_<ptr>.bin`, `PS_<ptr>.bin` | raw shader bytecode (`GetFunction`), `<ptr>` = the D3D object address in hex |
| `LightProbe\VS_<ptr>.txt`, `PS_<ptr>.txt` | disassembly by `D3DDisassemble` from `d3dcompiler_47.dll` (`LoadLibraryA`, resolved once); if missing the report says `.bin (sem desmontador)` |
| `LightProbe\T<n>_<W>x<H>_<FMT>.bmp` | level 0 of each 2D texture, 32-bit top-down BMP; RGB scaled by 1/peak if any channel exceeds 1 (FP render targets) |

The `LightProbe\` folder is **never cleared**. Shader files are named by pointer and textures by `T<n>` + size +
format, so files from earlier captures accumulate (1177 files on 28/09) and a `T3_...` BMP may be from another
capture. Trust only the files named in the current `S3SS_LightProbe.txt`.

Texture dump rules (`DumpTexture`): cube and volume textures are listed but not saved; DEFAULT-pool textures are read
only if they are render targets (copied with `GetRenderTargetData`); DXT1/3/5 are decoded (`DecodeDxt`, added 25/09 for
the m65 plant) only when not in DEFAULT pool and at most 2048x2048; INTZ, ATI2 and unknown formats are skipped. Supported
texel formats: A8R8G8B8, X8R8G8B8, A16B16G16R16F, A32B32G32R32F, R32F, R16F, G16R16F, G32R32F, L8, A8L8, A8,
A16B16G16R16, A2R10G10B10, R5G6B5.

### Archived captures (the "watcher")

Each capture overwrites the previous one, so every capture that mattered was copied to a sibling folder:
`S3SS\LightProbe-<name>` (e.g. `LightProbe-lote`, `LightProbe-andar2-b`), `LightProbe-seq-N-name` (the 25/09 ordered
sequence, notes section 4g), and from 25/09 00:23 onward automatically `LightProbe-m01`, `-m02`, ... `-m80` (m80 is from
27/09 17:39). The notes cite captures by these names ("m44", "LightProbe-telhado"). The watcher that made the `mNN`
copies was run by an earlier Claude session and its script is **not** in the scratchpad (searched: no `.ps1/.sh/.pl`
refers to `LightProbe-m`). From the folder contents (inferred): an `mNN` folder holds `S3SS_LightProbe.txt` plus the
`.bin`/`.txt` of the shaders it names (13 files in m01 and m80); some named folders (e.g. `LightProbe-m44`) also hold many
BMPs, including stale ones. Several archived text files were also copied to the scratchpad as `probe2_clara.txt`,
`probe4_portao.txt`, ... `probe_borda_escura.txt` (28/09).

To rebuild the practice in the standalone: after each Ctrl+Shift+F7, copy `ApexRadiance_LightProbe.txt` and the shader files it
names into `LightProbe-mNN` (next free number). Do not copy the whole `LightProbe\` folder (it holds everything ever
dumped).

Offline indexers over the archives (scratchpad, read-only): `draws.ps1` (every `== DESENHO` line of every
`LightProbe-*` folder, with MD5 of the VS/PS files -> `draws.csv`), `consts.ps1` (selected constants per shader
family from those lines), `passo3\index.ps1`, `census.ps1` (token-level scan of captured `.bin` files, output
`census_ps.txt`, see [census.md](census.md)).

## Reading a capture

Header and per-draw block (literal Portuguese labels, from `FinishCapture`):

```
S3SS Light Probe | pixel (1372, 991) | tela 3840x2160 | desenhos na tela: 446 | sem resposta: 0
Estados: z/zwrite/zfunc/blend/src/dst/blendop/atest/cw/stencil/cull/srgb/depthbias/slopebias (bits de float)

== DESENHO #13 DIP tipo=4 prims=408 amostras=1 VS=3179CDA0 PS=3179DC78
   shaders: VS VS_3179CDA0.txt | PS PS_3179DC78.txt
   estados: z=1 zwrite=1 zfunc=4 blend=0 src=2 dst=1 blendop=1 atest=0 cw=15 stencil=0 cull=2 srgb=0 depthbias(bits)=0 slopebias(bits)=0
   s0: T1 2D 256x128 A8R8G8B8 MANAGED/SYS mips=1 filtro min/mag/mip=2/2/0
   mod: jogo (o mod nao trocou este desenho) | VS outro | PS outro | rig modo -1
   PS c0..c223 (zeros omitidos): [0](1 1 1 0) [1](...) ...
   VS c0..c255 (zeros omitidos): [0](...) ...
== COR NA TELA NO PIXEL: (0.137 0.090 0.051)
== COMPARACAO COM A CAPTURA ANTERIOR (desenho do objeto em cada pixel) ==
== TEXTURAS DOS DESENHOS QUE PINTAM O PIXEL ==
T1 2D 256x128 A8R8G8B8 MANAGED/SYS mips=1: salva T1_256x128_A8R8G8B8.bmp; media RGBA=(...) min=(...) max=(...)
```
(excerpt of `LightProbe-m80`)

| Field | Meaning |
|---|---|
| `tela WxH` | back-buffer size (the probe's pixel space) |
| `desenhos na tela` | screen-sized draws recorded in the frame (not the covering ones) |
| `sem resposta` | queries still pending after 1.5 s (their draws are dropped) |
| `DESENHO #i DIP/DP` | index among recorded draws; DrawIndexedPrimitive or DrawPrimitive (UP draws are not seen) |
| `tipo` | `D3DPRIMITIVETYPE` (4 = triangle list, 5 = strip) |
| `amostras` | occlusion samples at the pixel (1 without MSAA, 8 with 8x MSAA) |
| `VS=/PS=` | object pointers, also the dump file names. **Not stable** between sessions; identify shaders by content (MD5 or FNV-1a of the bytecode, as `draws.ps1` and the census do) |
| `estados` | the 14 states; depth bias values are raw float bits |
| `sN: T<k> ...` | texture on sampler N; `T<k>` indexes the texture list at the end; `filtro min/mag/mip` = D3DTEXF (0 none, 1 point, 2 linear, 3 anisotropic), added in PASSO3 increment 0 |
| `mod:` | `LotLightBridge::DescribeDraw()` (see below) |
| `PS c.. / VS c..` | non-zero float constants, 9 significant digits since 27/09 (earlier captures had 3 digits, which caused a wrong "near = 1.0" reading; see Pitfalls) |
| `COR NA TELA` | final back-buffer colour at the pixel, 8-bit, or `(nao lida)` |
| texture line | desc, dump file, mean RGBA, min RGB, max RGBA, and `(imagem escalada por 1/x)` if normalised |

**The `mod:` line** (`lot_light_bridge.cpp`, `DescribeDraw`, `DescribeObjectDraw`, fence branch near `DrawInstanced`):

- `Night Remake sem ganchos ativos`: the bridge's hooks are not registered (bridge and all fixes off, or hook failure).
- `jogo (o mod nao trocou este desenho) | VS <class> | PS <class> | rig modo <m>`: the game's own draw. VS classes:
  `outro, telhado, lago, lote com neve, rua, piso, folhagem, cerca/escada (instanciado), neve em objeto, neve com relevo,
  objeto com rig, piso com neve` (= classes 0..11). PS classes: `desconhecido, outro, candidato de mundo, luz do lote,
  rig de objeto, telhado, lago, lote com neve, telhado com neve, parede externa, piso externo`. Rig mode from
  `RigTracker::CurrentMode()`: -1 none, 0 indoor with ceiling, 1 roofless fenced area, 2 outdoor.
  For VS class 10 (objects) it adds the world constant `c<K>`, the vertex-light base, and why the object was not
  replaced: `PS ainda nao testado`, `PS aceito`, `PS RECUSADO (fora do padrao)`, `opcao dos objetos desligada`,
  `rig de dentro de casa ...` (indoor rig, game uses the lot light map), `sem atlas de luz do chao` (world atlas not
  ready).
- `desenho do mod (outra correcao)`: a draw issued by the mod itself (`g_inOwnCall`) for a fix without extra detail.
- `OBJETO corrigido pelo mod | ...`: an outdoor object the mod redrew (`DescribeObjectDraw`, only filled while
  `LightProbe::Capturing()`): PS shape (A/B with rig lamps in the PS, C without), rig mode, per-pixel lamps on/off and
  strength, ground-light strength, object position (VS world triple .w), the up to 8 per-pixel lamps chosen (position,
  weight W = 0.4 x range, colour, distance to the object centre, "N used of M in range"), the game's rig in PS c0..c13 and
  the vertex lights in VS `c(vl-4)..c(vl+3)` as the game set them (kept: the patched shader takes max(rig, per-pixel)).
- `CERCA/ESCADA corrigida pelo mod | luz do chao forca X | luz por pixel: ...`: an instanced fence/stair draw.

Because a hook that returns `Skip` stops the registry chain (`d3d9_hook_registry.cpp`, `Execute*Hooks`), the probe
(`Priority::Last`) never sees the game's original call of a draw the bridge replaced; it sees the bridge's own re-issued
draw, which re-enters the registry with `g_inOwnCall` set, hence the "mod" wording (inferred from the registry code and
`DescribeDraw`).

## Files and functions

| File | Function / symbol | Role |
|---|---|---|
| `light_probe.h` | `LightProbe::OnPresent`, `RenderUI`, `Shutdown`, `Capturing` | public API |
| `light_probe.cpp` | `OnPresent` | hotkey, pixel mapping, state machine |
| | `OnDraw<DrawFn>` | per-draw recording, occlusion copy, blanking |
| | `RegisterHooks` / `UnregisterHooks` | draw hooks, name `"LightProbe"`, `Priority::Last` |
| | `FinishCapture` | query collection, report, pick, comparison |
| | `DumpShader<S>` | `.bin` + `D3DDisassemble` `.txt` |
| | `DumpTexture`, `DecodeDxt`, `Texel`, `HalfToFloat`, `WriteBmp` | texture dump |
| | `ReadScreenPixel` | final colour |
| | `WriteComparison` | register diff between two picks |
| | `RenderUI` | status + blanking checkboxes |
| `lot_light_bridge.cpp` | `DescribeDraw`, `DescribeObjectDraw`, `g_objDrawInfo` | the "mod:" line |
| `patches/night_terrain_relight_patch.cpp` | Present hook (calls `LightProbe::OnPresent`), `RenderDeveloperUI` (calls `LightProbe::RenderUI` under "Light probe"), `Uninstall` (calls `LightProbe::Shutdown`) | host |
| `build_flavor.h` | `kPublicBuild` | dev/public switch |

## Game addresses and patterns

None. The probe works purely at the D3D9 level (device methods through the S3SS hook registry). It reads no game
memory.

## Shader details

The probe does not patch shaders. Constants it records that recur in the notes (all measured by captures):

| Register | Meaning (capture) |
|---|---|
| VS c40..c43 | the camera view-projection, identical in every scene draw (m80); also c0, c4, c180, c192, c216 in other families (m70..m80) |
| VS c8.w / c10.w | chunk or object translation (terrain chunk centre, roof origin) |
| PS c1..c3 / c5..c7 | rig lamp directions / colours (objects); sun c8/c9 or c0/c4 or c12/c13 |
| VS c4..c11 | vertex-light directions / colours (fences, rails, stairs; were all zero in m03/m15/m33) |
| VS c27..c29[a0], c54..c56[a0] | per-instance lamp directions / colours (instanced foliage and fences) |
| PS c3.x / c4.x | light-map scale of lot light passes (summer / snow) |

## Interactions

- Hosted by Night Lighting: if `NightTerrainRelight` is not installed there is no Present call and no UI, so the probe
  does nothing.
- `LotLightBridge` fills `g_objDrawInfo` only while `LightProbe::Capturing()` is true (zero cost otherwise).
- The 1x1 occlusion copy goes through the full hook registry again: hooks with a higher priority (bridge, HDR, Frame
  Profiler counters, the census) see each recorded (screen-sized) draw twice during the capture frame, covering or not,
  because the copy is issued before coverage is known (inferred from the registry design; harmless for rendering,
  visible in per-frame counters).
- With HDR output the back buffer is A16B16G16R16F, which `ReadScreenPixel` does not accept: the 28/09 captures
  (`probe2_*.txt`, `probe4_portao.txt`) show `COR NA TELA NO PIXEL: (nao lida)` (cause inferred).
- Texture blanking returns `Skip`, which cuts the chain for later hooks (Frame Capture, other `Last` hooks).

## Known limitations

- Only draws whose RT0 has the back-buffer size are probed; draws into smaller or other targets (shadow maps, bloom
  chain, reflection maps, half-res passes) never appear. `DrawPrimitiveUP`/`DrawIndexedPrimitiveUP` are not hooked.
- The pixel is mapped by client-rect scaling; with the S3SS menu open, the cursor may be over the menu.
- Up to 4000 screen-sized draws per capture; the 1.5 s query wait freezes the game briefly.
- Draw indices (`#n`) count only recorded draws, so they are not the same numbers as Frame Capture's.
- `LightProbe\` grows forever and mixes captures (see above). NOTAS item 6 of the 25/09 deep review lists "LightProbe
  without a size limit and holding render targets" as a remaining risk.
- The comparison state is lost when the game closes.

## Pitfalls and failed approaches

- **Pointer names are not identities.** The same shader has a different address every session and an address can be
  reused for a different shader. Dedupe by bytecode hash (MD5 in `draws.ps1`; FNV-1a in the census and in
  `ShadersRecusados`). PASSO3 F-J5 shows the same trap for F8 light indices.
- **Captured 3-digit constants were misleading.** "near = 1.0 on the road" was rounding (w ~ 243); the probe has written
  9 significant digits and the depth-bias states since 27/09 (NOTAS "SSAO refeito com GTAO"). m80, with 9 digits, gave
  near = 0.250 identical in c0/c40/c180/c192/c216 for every draw of the frame, varying 0.2..0.3 between frames.
- **Screen constants arrays.** Before 25/09 18:40 the probe stored only PS c0..c31 and VS c0..c95; skinned objects keep
  the world matrix and vertex lights at VS c184..c199, which were invisible. Now all registers are stored.
- **`amostras=8` means MSAA is on**, which also means the Depth Blur INTZ swap is not active (multisampled depth cannot be
  read). m25 (`LightProbe-lago-preto`): the water's SSR fell back to a 40 m guess sampling the wrong place of the scene
  copy, giving black stains at night; fix: no screen reflection without depth.
- **A dark pixel may be the wrong culprit.** NOTAS section 1c (`LightProbe-grama3-escura`, 24/09 23:54): the "dark"
  grass was only terrain paint, not light.
  Always compare with a correct pixel (`-clara`/`-escura`, `-lote`/`-mundo` pairs).
- **Different draws, same pixel.** m50: the snow on the stairs is draws #303/#315 on top of the stair #243 that was
  already fixed; the pixel's final look came from the last draw.
- **DXT textures looked absent** until `DecodeDxt` was added (25/09 ~14:15, plant m65: s2 128x128 DXT1 multiplying
  the lamp light).
- The F8 writer is `light_diag.cpp`, not `patches/light_diag_patch.cpp` (see [light-diag.md](light-diag.md)); the probe
  has no such duplicate.

### How captures solved the lighting bugs (index)

Captures named in `NOTAS-ILUMINACAO.md`; see the night-lighting docs for the fixes themselves.

| Capture | What it showed | Outcome (doc) |
|---|---|---|
| `LightProbe-lote` / `-mundo` | world grass adds `tex2D(s8 terrainLightMap)*c7.x`; lot grass is a modulate2x pass adding `tex2D(s1 lotLightMap)*c3.x` (568-byte PS); the two meet at the lot edge | bridge draws lot light with `max(lot, terrain)` ([lot-light-pass.md](../night-lighting/lot-light-pass.md)) |
| `-grama2` / `-grama2-claro` | chunk seam at x = 1280: the baked chunk map lacks the neighbour's lamp overflow; PS_294418E0 keeps the map on s7 not s8 | auto terrain rebuild after load; search s1..s15 for the 256x256 map |
| `-cerca`, `-arbusto`, `-arvore`, `-arvore2` | instanced VS: 3 lamps per instance c27..c29 / c54..c56[a0.z], nearly all zero | object gather boost ([objects-and-rigs.md](../night-lighting/objects-and-rigs.md)) |
| `-chafariz`, `-lago`, `-oceano` | fountain uses per-pixel rig lamps; lake PS has no lamp term; ocean has a real-time planar reflection | lake additive lamp pass ([water.md](../night-lighting/water.md)) |
| `-conjunto`, `-arbusto2` | lamp light multiplied by the moon shadow `(t2*shadow + sky)*t1.z` | shadow lerp to 1 by night level (`ObjectRig`) |
| `-escada`, m03 | vertex lights VS c4..c11 all zero | first wrong hypothesis (bit 0x10), see m32..m41 |
| `-telhado`, `-telhado2` | roof PS has no lamp term; world pos in TEXCOORD4.zw/5.w | `roof_ps.hlsl` 16 lamps ([roofs.md](../night-lighting/roofs.md)) |
| `-neve` | snow lot light variant PS_2A13D200, lot light via `texld r0, v3, s2` | `PatchSnowBytecode` ([snow.md](../night-lighting/snow.md)) |
| m01/m02 | snow ground: a road/sidewalk mesh with its own chunk map copy | road bridge |
| m04/m05/m06 | snow formulas: lot x0.25 (def c9.x), world, road own map in s6 | max(lot, terrain x4); road patch |
| m12/m14, m17 | lamp colour (1, 0.75, 0.79) is the factory pink; script sets it again later | `LampColourSet` + 0x6B0BDE ([lamp-colour.md](../night-lighting/lamp-colour.md)) |
| m15 | fence in an enclosed area: rig mode 1, room-list only | `RoomGatherThunk` at 0x6BBE70 |
| m16/m18, m24, m42/m43 | 3rd and 4th road variants; summer road VS | generic `PatchRoad`, `IsRoadVs` ([roads.md](../night-lighting/roads.md)) |
| m21, m22, m28, m63/m64, m78 | winter/summer foliage: moon shadow, back-face lamp cut `max r0, r0, cK.w` | `PatchLeafShadow`, `PatchFoliageVs` ([foliage.md](../night-lighting/foliage.md)) |
| m23, m29 | roof snow PS_2793C3E8 (no lamps); m29 VS c19 = 0 put the roof "at the world origin" | additive `roof_snow_lamps_ps.hlsl`, position from COLOR0 x c15.x |
| m25 | black pond, `amostras=8` | MSAA on: no SSR without depth |
| m32..m41 | fences, stone, mailbox, window, plants: rig lamp colours zero or weak; m41 correct vegetation | root cause in the rig gather (VertexLight slots only get the excess); only street-lamp class boosted -> extend to all light classes |
| m44/m45, m46/m47 (+F8) | walls lit from a per-story 256x128 map; cut at the floor line | [level-light-share.md](../night-lighting/level-light-share.md) |
| m48 | snow cover on fences (VS_2F27C9C0 / PS_2F27C510), no lamp | `PatchSnowCover` |
| m50/m51 | stair snow shares the roof-snow PS byte for byte | route by VS class 9, `PatchSnowRelief` |
| m52..m57 | doors/windows/sofa/counter: rig lamps arrive (~0.6) but from above; Counters vs Phong outdoors; animated door VS | `PatchObjectLampVs/Ps`, per-pixel lamps |
| m59/m60 | passo3 test wall (ExteriorWall PS_2734BB80, 1024x512 atlas) and ground | PASSO3 plan |
| m61/m62, m66, m69, m71/m72 | summer floor baked family; pool edge VS; snow floor / door sill variants | `PatchBakedAtlasPs`, `IsFloorVs`, `IsSnowFloorVs` ([floors.md](../night-lighting/floors.md)) |
| m65 | plant lamp light multiplied by a real DXT1 texture | added DXT decode |
| m73/m74, m76 | dark lot beside a lit sidewalk: lot pass reads the "home chunk" map with CLAMP | lots read the world atlas ([world-atlas-and-smoothed-maps.md](../night-lighting/world-atlas-and-smoothed-maps.md)) |
| m75 | ExteriorWall lamp light is atlas x cK.x only | "Lamp light on outside walls", `wall_lamp_table.h` ([walls.md](../night-lighting/walls.md)) |
| m70..m80 | projection: row2 = A*row3 + (0,0,0,-near), A = 1.00008 (m80); c40..c43 = world view-projection | PostScene camera vote, AO, Lot Map Probe |
| `probe2_*`, `probe4_portao` (28/09) | lot edge on houses with foundations (story gate); dark gate with the rig zeroed | story gate 0xC294D9 ([terrain-relight.md](../night-lighting/terrain-relight.md)); keep rig, max() |

## Testing in game

1. Dev build, Night Lighting enabled, at night, menu closed. Mouse over the pixel, Ctrl+Shift+F7.
2. `S3SS_LOG.txt`: `[LightProbe] Measured: N draws cover pixel (x, y), M textures saved. ...`.
3. Developer > Light probe status shows the same; the texture list appears; tick one to see its effect.
4. Open `S3SS_LightProbe.txt`; read the covering draws bottom-up (the last one is on top unless blending); open the
   `PS_<ptr>.txt` of the suspect draw.
5. Archive the capture (`LightProbe-mNN`) before the next one.

## Open items

- Standalone rename (PLANO-SEPARACAO.md section 4): output folder `Documents\Electronic Arts\<localized>\Apex Radiance\`,
  report `ApexRadiance_LightProbe.txt`, dump folder `LightProbe\` (inside `Apex Radiance\`). Hotkey stays Ctrl+Shift+F7; the proposed Apex
  menu key Ctrl+Shift+F11 sits next to the dev chords F7..F10.
- The probe must move off the Night Lighting Present hook if Night Lighting is not the host in the standalone.
- Clear or version `LightProbe\` per capture (e.g. subfolder per capture) and write the archive copy automatically.
- Accept FP16 back buffers in `ReadScreenPixel` (HDR).
- PASSO3 section 3.7 planned F7 additions not implemented: replacement PS/VS pointers for mod draws, c144..c149, the
  block id (only the sampler filters were added, for all 16 samplers).
