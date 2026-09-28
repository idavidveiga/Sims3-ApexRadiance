# Lot Map Probe

> Development study tool (27/09): a feasibility test for a world-space ambient occlusion. During a ~4 s capture every
> scene draw with depth write is issued a second time, with all hooks bypassed, into an orthographic top-down camera
> (160 x 160 m around where the camera looks), accumulating a 2048x2048 height map (INTZ depth) and a top-down colour
> image. At the end it saves `altura.f32`, `altura.bmp`, `cor.bmp` and `info.txt` to `S3SS\MapaLote_N\`.
> **Status: dev-only study, abandoned as the main AO path** (NOTAS "AO novo: deterministico, estudo no lab (27/09)"):
> redrawing into an ortho camera works, but roofs cover interiors and Sims would need a redraw every frame. The whole
> file is inside `#ifndef S3SS_PUBLIC`.

## Purpose

Answer: can the game's own scene draws (terrain, houses, furniture, trees, Sims) be re-rendered from above with the
game's shaders, textures and vertex data, to build a lot height map for a world-space AO? Result (NOTAS 27/09): yes,
"0 failures, 100% covered", but the shader families read the camera from different constant blocks (c0, c4, c40,
c180...), and the approach was dropped in favour of the screen-space HBAO in `patches/ambient_occlusion_patch.cpp`
(Ambient Occlusion is removed from the standalone; see [removed-features.md](../../removed-features.md)). Keep it as a
reusable technique: "redraw the scene from another
camera" with hooks bypassed.

## User-facing settings

| UI label | TOML | Type | Default | Notes |
|---|---|---|---|---|
| Lot Map Probe > Enabled | `[patches.LotMapProbe] enabled` | bool | false (category "Experimental", `experimental = true`) | Apex tab > Performance > "Developer tools" (dev only) |
| "Capture lot map" | - | button | | disabled while busy; starts a 120-frame countdown ("close the menu within 2 s") |
| "Status: ..." | - | text | | `Ready`, `Starting in 2 s: close the menu`, `Capturing: turn the camera slowly around the lot...`, `Saving...`, `Saved to MapaLote_N (R draws redrawn, P% covered)`, `Failed: the game's camera was not found (are you in Live mode?)`, `Failed: not enough video memory for the map`, `Failed to copy the map`, `Failed to save the files`, `Cancelled (video change)`, `Off` |

No hotkey. Instructions shown: click, close the menu within 2 s and turn the camera slowly around the lot for about 4 s.

## How it works

`patches/lot_map_probe_patch.cpp`, state `State g`. Constants:

| Constant | Value | Meaning |
|---|---|---|
| `kMapSize` | 2048 | map resolution (colour RT + INTZ depth) |
| `kMapHalf` | 80.0 m | half extent: the map covers 160 x 160 m |
| `kAheadM` | 25.0 m | map centre this far ahead of the camera, horizontally |
| `kAboveCamM` | 20.0 m | top of the height range above the camera |
| `kRangeM` | 300.0 m | height range |
| `kCountdownFrames` | 120 | countdown before capturing |
| `kCaptureFrames` | 240 | capture length (~4 s) |
| `kCamBlocks` | {0, 4, 40, 180, 192, 216} | VS constant blocks where shader families keep the camera (measured in LightProbe-m70..m80) |

Hooks (name `"LotMapProbe"`, all `Priority::First`): Present (`OnFrameBoundary`), SetRenderTarget (tracks RT0 when not
inside its own call), DrawIndexedPrimitive / DrawPrimitive (`OnGameDraw`), plus `RenderCallbacks::preReset`
(`OnPreReset` cancels and frees everything).

1. **Camera vote, every game draw during countdown/capture** (`OnGameDraw`): only draws into the back buffer with
   ZENABLE on. Reads VS c0..c255; for each block in `kCamBlocks` that `LooksLikeCamera` (row 3 unit length = view
   forward; rows 0 and 1 longer than 0.3 and orthogonal to it and to each other), votes for it (up to 32 candidates,
   compared on rows x, y, w only (`SameXYW`), because the z row differs between draws of the same frame: near 0.2..1.0,
   10). A candidate with >= 8 votes and the most votes is the frame's camera; at Present it becomes `prevCam`, the camera
   for the first draws of the next frame.
2. **Start** (`OnFrameBoundary`, when the countdown reaches 0): needs `prevCam`; creates the 2048 INTZ depth texture
   (`D3DUSAGE_DEPTHSTENCIL`, DEFAULT pool), a 2048 A8R8G8B8 render target (`CallOriginalCreateRenderTarget`, so other
   hooks do not see it) and a tiny ps_3_0 copy shader (`D3DCompile` of `tex2Dlod(sDepth, uv).r`); builds the ortho
   matrix once (`SetupMap`).
3. **SetupMap**: from the camera rows (row0 = Px x right, row1 = Py x up, row3 = forward, the game's projection layout)
   recovers R (rows normalised), t, the camera position `camPos = -(R^T t)`, forward; world up sign `upSign` = sign of
   R[1][1]; centre = camera xz + 25 m along the horizontal forward; `hTop = upSign*camPos.y + 20`. Ortho view-projection
   (rows): `(1/80, 0, 0, -cx/80)`, `(0, 0, 1/80, -cz/80)`, `(0, -upSign/300, 0, hTop/300)`, `(0, 0, 0, 1)`, i.e. x -> world
   x, y -> world z, depth = (hTop - upSign*y)/300.
4. **Redraw** (each qualifying game draw while capturing, just before the game's own draw): finds every 4-register block
   in VS c0..c252 whose x, y, w rows equal the frame camera (`camRegs`; draws with none are skipped as "other camera");
   skips draws without Z write and draws with a second render target (MRT). Then saves 10 render states, binds its colour
   RT (`CallOriginalSetRenderTarget`), its INTZ surface (`ExtraHooks::RawSetDepthStencilSurface`, bypassing Depth Blur's
   substitution) and a 2048 viewport (`CallOriginalSetViewport`); sets cull none, ZFUNC LESSEQUAL, no blending, colour
   writes 0xF, stencil/scissor/clip planes off, no depth bias, no sRGB write; clears once per capture (after scissor is off,
   because Clear honours it); writes the ortho matrix into every camera block (`CallOriginalSetVertexShaderConstantF`);
   issues the draw with `CallOriginalDrawIndexedPrimitive/DrawPrimitive` (no hook sees it); restores constants, states,
   RT, depth surface and viewport (viewport last: setting the RT resets it). The game's shaders, textures, vertex data and
   alpha test are untouched (leaf cut-outs stay).
5. **Accumulation**: the map is not cleared between frames (the game only draws what is in view, so turning the camera
   fills it).
6. **Dump** (`Dump`, at the first game draw after the 240 frames): copies INTZ -> R32F render target with the copy shader
   (a pre-transformed `D3DFVF_XYZRHW | D3DFVF_TEX1` quad through `DrawPrimitiveUP`, point sampling), `GetRenderTargetData`
   for depth and colour, restores state with a `D3DSBT_ALL` state block applied twice (again after the RT reset) and puts
   the shaders and texture back through the hooked setters so tracking modules see the game's own again. Then writes the
   folder.

### Output: `Documents\Electronic Arts\<localized>\S3SS\MapaLote_N\` (N = first free number)

| File | Content |
|---|---|
| `altura.f32` | raw 2048x2048 float32 depth, row-major, top row first (16 MB). Height = hTop - depth x 300 (in `upSign*y` units); depth 1.0 = nothing drawn |
| `altura.bmp` | 24-bit BMP, brightness = height stretched over the covered range (brighter = higher); nothing drawn = dark blue (B = 60) |
| `cor.bmp` | 24-bit BMP of the top-down colour RT (the game's shaders as seen from above) |
| `info.txt` | map size/extent/centre, depth-to-height mapping, starting camera position and forward, frames, redrawn/failed counts, skips (other camera, no depth write, multiple targets), coverage %, depth min/max and heights, and `camera found in registers (draws): c<r>=<count> ...` |

## Files and functions

| File | Symbol | Role |
|---|---|---|
| `patches/lot_map_probe_patch.cpp` | `LotMapProbePatch` | Install / Uninstall / UI |
| | `LooksLikeCamera`, `SameXYW`, `Same` | camera recognition |
| | `SetupMap` | ortho camera |
| | `Redraw<DrawFn>` | second draw into the map |
| | `OnGameDraw`, `OnFrameBoundary`, `OnPreReset` | lifecycle |
| | `CreateResources`, `ReleaseResources`, `Dump`, `WriteBmp` | resources and output |
| `d3d9_hook_registry.h` | `CallOriginal*` | device calls that bypass all hooks |
| `d3d9_extra_hooks.h` | `RawGetDepthStencilSurface`, `RawSetDepthStencilSurface` | real depth surface, no Depth Blur substitution |
| `gui.cpp` | `RenderApexFeature("LotMapProbe", ...)` under "Developer tools" | UI |

## Game addresses and patterns

None. The camera layout comes from Light Probe captures: VS c40..c43 = world view-projection in every scene draw
(LightProbe-m80), with other families using c0, c4, c180, c192, c216 (m70..m80); projection row2 = A*row3 + (0,0,0,-near)
with A = 1.00008 (m80).

## Interactions

- `Priority::First` on the draw hooks: it sees the game's draw before Night Lighting's bridge replaces it, so the map
  uses the game's original shaders (the bridge's replacement is not drawn into the map).
- Bypasses Depth Blur's INTZ swap (Raw depth calls) and every other hook (CallOriginal*), so Frame Profiler counters,
  Frame Capture and the probe do not see its extra draws.
- A device Reset cancels the capture (`preReset`).

## Known limitations

- Needs Live mode with a normal scene camera (the vote finds no camera otherwise).
- 160 m square fixed at capture start; only geometry that the game draws while the camera turns gets in.
- Roughly doubles the scene's draw calls for 4 s (the captures redrew 107k..304k draws over 240 frames).
- Roofs cover interiors from above; Sims move, so a static map cannot shade them (reasons for abandoning it).

## Pitfalls and failed approaches

Evidence: the four folders `MapaLote_1..4` in the S3SS folder (27/09 22:57..23:06, written by the Portuguese-label
version of the code) and NOTAS 27/09.

- **Replacing only c40..c43 is not enough.** The first test (MapaLote_1..3) skipped 75k..134k draws as "other camera":
  those families read the camera from c0, c4, c180, c192 or c216 and were drawn with the real perspective camera. Fix:
  replace every block whose x, y, w rows match the camera (`kCamBlocks` voting + register scan). The file header comment
  still says "c40..c43 replaced", which is outdated: the code replaces all matching blocks.
- **A block of zeros won the vote.** The second version (MapaLote_4, 23:06) elected an all-zero block that many draws
  have: every value in `info.txt` is `nan` (centre, heights, camera) and the "camera found" list covers almost every
  register. Fix: only blocks shaped like a camera may vote (`LooksLikeCamera`). No capture was made after this fix.
- **Height calibration is unverified.** MapaLote_1..3 report `upSign -1`, camera y -49.7 / -62.8 and heights around
  -230..-216 m, while lamps in the same town are at y ~60..90 (F8). The world-up sign and position decode in `SetupMap`
  have not been validated against a known height; check with a known object before trusting heights.
- Depth values span a tiny range (0.9949..0.9987 in MapaLote_1..3), so `altura.bmp` stretches min..max; use
  `altura.f32` for numbers.

## Testing in game

1. Dev build, Live mode. Apex tab > Performance > Developer tools > Lot Map Probe > Enabled.
2. "Capture lot map", close the menu within 2 s, orbit the camera slowly for ~4 s.
3. `S3SS_LOG.txt`: `[LotMapProbe] Saved to MapaLote_N (R draws redrawn, P% covered)`.
4. Open `info.txt` first: `nan` or a large "other camera" count means the camera vote failed.

## Open items

- Standalone: the planned renames (PLANO-SEPARACAO.md section 4) do not list `MapaLote_N`; it would land in
  `...\Apex Radiance\MapaLote_N` if it uses the Apex folder. TOML table `[patches.LotMapProbe]` is kept in `ApexRadiance.toml`.
- Validate `upSign`/camera position against a known height if the tool is revived.
- Update the stale header comment (c40..c43 only).
