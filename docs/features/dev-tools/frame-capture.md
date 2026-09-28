# Frame Capture (Ctrl+Shift+F9)

> A draw-by-draw log of two consecutive frames written to `S3SS_FrameCapture.txt`: every render-target and
> depth-stencil switch, Clear, StretchRect, BeginScene, EndScene and every draw (including the UP variants) with its
> bound render targets, depth surface, shaders, first three textures, depth/stencil/blend/alpha-test/colour-write states
> and viewport; identical consecutive draws are folded into runs. Resources get short ids (`S12`, `T141`, `PS41`) with a
> one-line definition at first sight. Zero cost while idle. **Status: working, dev-only.** Registered as the patch
> `FrameCapture` ("Frame Capture (developer)"); the `APEX_REGISTER_FEATURE` is inside `#ifndef S3SS_PUBLIC`, so the public
> build has no such patch.

## Purpose

Understand the game's frame structure at the D3D9 level, which the pixel probe cannot show (it only sees screen-sized
draws covering one pixel). It was written on 24/09 for the Depth Blur ("find where the game finishes the 3D scene and
starts drawing its UI, and whether the scene depth buffer can be read") and later used for HDR output and every
post-scene effect. Facts established by the 24/09 capture (the file still on disk, 2381 lines, 1154 draws per frame):

- the scene is drawn straight into the back buffer with the auto depth-stencil `S1` (3840x2160 D24S8, 8x MSAA at the
  time: `Present params: ... ms=8`);
- readable depth: `INTZ: YES`, `DF24/DF16/RESZ: NO`, `D24S8 as a texture: YES` under DXVK -> the Depth Blur INTZ swap
  (`patches/depth_blur_patch.cpp` header comment "see S3SS_FrameCapture.txt analysis");
- the end of the scene: `StretchRect BACKBUFFER -> S12` (2048x1024 RT, texture `T141`, also the water refraction copy),
  a bloom chain of full-screen strips (`#973` into a 1024x1024 target, `#974..#976` ping-pong between `S13` and `S12`),
  then **`#977`**: `DP type=5` (triangle strip), 2 prims, into the back buffer, `z=0/0`, `blend=1`, `cw=7` (RGB only),
  sampling `T141`: the bloom composite. From `#978` on, back-buffer draws with Z off = the UI.
  `hdr_output.cpp` cites it: "the game adds its bloom (one full-screen DrawPrimitive strip right after the scene,
  FrameCapture 24/09 #977)". This is the rule PostScene / HDR use to find the scene/UI boundary (NOTAS "HDR (28/09)").
- the game's EndScene marker comes after all game draws; everything after it is the S3SS overlay.

## User-facing settings

| UI label | TOML | Type | Default | Notes |
|---|---|---|---|---|
| Frame Capture > Enabled | `[patches.FrameCapture] enabled` | bool | false (`enabledByDefault` not set; category "Experimental", `experimental = true`) | Apex tab > Performance > "Developer tools" (collapsing header, dev only, `gui.cpp`) |
| "Capture now" | - | button | | arms a capture (`Arm()`) |
| Hotkey **Ctrl+Shift+F9** | - | chord | | polled in `OnEndScene` while the patch is installed |
| "Status: ..." | - | text | | `Ready`, `Waiting for the next frame...`, `Capturing...`, `Capture saved (N lines) to S3SS_FrameCapture.txt`, `ERROR: could not write the file`, `ERROR: the game does not call IDirect3DDevice9::Present, capture cancelled` |

Saved in `S3SS.toml` through the patch system (`OptimizationPatch::SaveToToml` writes `enabled`); `gui.cpp` `IsApexPatch`
lists `FrameCapture` so it is shown in the Apex tab, not the Patches tab.

## How it works

`patches/frame_capture_patch.cpp`, all in an anonymous namespace (`CaptureState g`).

1. **Install** registers, all at `Priority::Last`, name `"FrameCapture"`: `RegisterBeginScene` (also calls
   `ExtraHooks::EnsureInstalled`), `RegisterSetRenderTarget`, `RegisterDrawIndexedPrimitive`, `RegisterDrawPrimitive`,
   `RegisterPresent`; adds `OnEndScene` to `RenderCallbacks::endSceneBeforeOverlay` (fired from the EndScene hook in
   `d3d9_hook.cpp` before the S3SS overlay draws); sets the four ExtraHooks observers (Clear, SetDepthStencilSurface,
   StretchRect, DrawPrimitiveUP/DrawIndexedPrimitiveUP). Log `[FrameCapture] Installed (press the button or
   Ctrl+Shift+F9 to capture)`.
2. **Arming**: button or hotkey -> `armed = true`. If 300 EndScenes pass while armed and no Present was ever seen, the
   capture is cancelled (the game must call `IDirect3DDevice9::Present`).
3. **Start** at the next Present (`OnPresentBoundary` -> `StartCapture`): clears state, writes the header
   (`WriteHeader`) and `==== FRAME 1 ====`.
4. **Recording** (`capturing = true`), `kFramesPerCapture = 2` frames:
   - events (`Event()`: flushes the pending draw run and definitions first): `BeginScene`,
     `SetRenderTarget[i] = <id>`, `SetDepthStencilSurface = <id>` (the surface the game **requested**, before any Depth
     Blur substitution), `Clear [COLOR Z STENCIL] rt0= ds= color=0x.. z= rects=`, `StretchRect <src> -> <dst> filter=<n>`,
     `---- Game's EndScene (everything below is the S3SS overlay) ----`;
   - draws (`OnDraw`, from DIP/DP hooks and the UP observer with kind `DIP`, `DP`, `DPUP`, `DIPUP`): a signature
     `kind type rt0 rt1 ds ps vs t0 t1 t2 z=ZENABLE/ZWRITE/fZFUNC st=STENCIL blend=ALPHABLEND atest=ALPHATEST cw=COLORWRITE(hex)
     vp=X,Y WxH`; consecutive draws with the same signature become one line `#a-#b <sig> xN prims=<sum>`;
   - at each Present: `==== Present (end of frame N, D draws) ====`, then `==== FRAME N+1 ====` or the file write.
5. **Write** (`WriteFile`): `Documents\Electronic Arts\<localized>\S3SS\S3SS_FrameCapture.txt`, truncated; at most
   `kMaxLines = 80000` lines, then `(capture truncated at 80000 lines)`. Log `[FrameCapture] Capture saved ...`.
6. **Uninstall** writes a capture in progress, unregisters everything and clears the observers.

### Ids and definitions

Every object gets an id the first time it appears: surfaces `S<n>`, textures `T<n>`, pixel shaders `PS<n>`, vertex
shaders `VS<n>` (counters restart with every capture; ids are per capture, not stable). Surfaces and textures get a
`[def]` line written just before the event/draw that introduced them:

```
    [def] S12 = 2048x1024 A8R8G8B8 usage=RT pool=0 ms=0 (texture level of T141)
    [def] T141 = tex2D 2048x1024 A8R8G8B8 usage=RT mips=1
    [def] S1 = 3840x2160 D24S8 usage=DS pool=0 ms=8
```
The back buffer is named `BACKBUFFER` (and a surface equal to it gets `<-- BACKBUFFER`). Shaders get no definition
(use the Light Probe to dump the bytecode of one).

### Header

`S3SS Frame Capture`, `Date:`, `Game version:` (`GetGameVersionName()`), `Device: adapter type behavior (PUREDEVICE)`,
`Present params: WxH fmt count ms msq swap windowed autoDS dsfmt flags interval`, `BACKBUFFER = WxH fmt ms`,
`Depth-stencil bound at the start: <id>`, `Readable depth support (depth-stencil texture):` with `CheckDeviceFormat`
results for INTZ, DF24, DF16 (DEPTHSTENCIL textures), RESZ (RENDERTARGET surface) and D24S8 as a texture, and the
legend line.

The file on disk from 24/09 was written by an older build with Portuguese labels (`Data`, `Versao do jogo`, `COR`,
`filtro`, `EndScene do jogo (daqui pra baixo e o overlay do S3SS)`); the current code writes the English labels above.

## Files and functions

| File | Symbol | Role |
|---|---|---|
| `patches/frame_capture_patch.cpp` | `FrameCapturePatch` (`OptimizationPatch("FrameCapture")`) | Install/Uninstall/RenderCustomUI |
| | `OnDraw`, `FlushRun`, `Event`, `FlushDefs`, `AddLine` | recording and run folding |
| | `SurfId`, `TexId`, `ObjId`, `NewId`, `FmtName`, `UsageStr` | ids and definitions |
| | `WriteHeader`, `WriteFile`, `StartCapture`, `OnPresentBoundary`, `OnEndScene`, `Arm` | lifecycle |
| | `ObserveClear`, `ObserveSetDepthStencil`, `ObserveStretchRect`, `ObserveDrawUP` | ExtraHooks observers |
| `d3d9_extra_hooks.cpp/.h` | `ExtraHooks::EnsureInstalled`, `Set*Observer` | Detours on vtable slots 34 StretchRect, 39 SetDepthStencilSurface, 40 GetDepthStencilSurface, 43 Clear, 83 DrawPrimitiveUP, 84 DrawIndexedPrimitiveUP; one observer slot each |
| `render_callbacks.h` | `endSceneBeforeOverlay` | EndScene callback slots (4) |
| `gui.cpp` | "Developer tools" header, `RenderApexFeature("FrameCapture", ...)` | UI (dev only) |

## Game addresses and patterns

None; D3D9 level only.

## Interactions

- **Owns ExtraHooks' single DrawUP observer slot.** `frame_profiler.cpp` notes it could not count UP draws for that
  reason. The Clear / SetDepthStencil / StretchRect observer slots are likewise single-owner: installing another
  observer would silently replace Frame Capture's (or vice versa).
- `Priority::Last` on the draw hooks: a draw that an earlier hook replaced and `Skip`ped (Night Lighting's bridge,
  blanked textures in the Light Probe) is not logged as the game's call; the replacement draw issued by the other module
  re-enters the registry and is logged instead (inferred from `d3d9_hook_registry.cpp`, where `Skip` ends the chain).
  The draw numbers therefore include mod draws.
- Post-scene effects (edge smoothing, AO, Depth Blur, HDR copies) draw inside the frame; with those enabled the capture
  shows their passes too. Capture with them off to see the vanilla frame.
- Draw indices (`#n`) count every draw including the overlay; they differ from the Light Probe's `#n`, which counts only
  screen-sized draws.

## Known limitations

- Only 3 texture stages (t0..t2) and render targets 0/1 are recorded; no shader constants (use the Light Probe).
- Ids are per capture; `#977` is the draw index in that 24/09 frame, not a constant of the game (the index changes with
  scene content; the rule is "the first full-screen z-off strip into the back buffer right after the scene").
- 80000-line cap (enough for two ~1200-draw frames).

## Pitfalls and failed approaches

- **The first z-off back-buffer draw is not always the UI.** HDR diagnostics (28/09) showed interiors have z-off draws in
  the middle of the scene; copying the scene at the first z-off draw lost the lamp light. The HDR copy is now refreshed at
  every z-on -> z-off transition (the last one wins) and the bloom strip right after the scene is included (NOTAS
  "Diagnosticos HDR_Diag_1/2"). Read the capture rather than assuming a single boundary.
- The 24/09 capture had 8x MSAA on; MSAA depth cannot be read in D3D9, so Depth Blur (and everything using its INTZ
  depth) requires the game's own Edge Smoothing off. Check `Present params ... ms=` in any new capture.
- The lake pass turning ZENABLE off made Depth Blur treat it as the first UI draw (review 25/09 ~03:30, item 1); fixed
  with `DepthShare::SetInternalPass`. A frame capture shows such mid-scene z-off draws immediately.

## Testing in game

1. Dev build. Apex tab > Performance > Developer tools > Frame Capture > Enabled.
2. Close the menu (or keep it: the overlay part is marked), press Ctrl+Shift+F9 or "Capture now".
3. `S3SS_LOG.txt`: `[FrameCapture] Capture saved (N lines) to S3SS_FrameCapture.txt`.
4. In the file, find `---- Game's EndScene` to separate game and overlay; search `StretchRect BACKBUFFER` for the scene
   copies, `type=5 ... prims=2 ... z=0/0` for full-screen passes.

## Open items

- Standalone rename (PLANO-SEPARACAO.md section 4): `Documents\Electronic Arts\<localized>\Apex Radiance\ApexRadiance_FrameCapture.txt`;
  the patch keeps its TOML table `[patches.FrameCapture]` in `ApexRadiance.toml`.
- The standalone must decide who owns the ExtraHooks observer slots (make them multi-subscriber, or keep Frame Capture as
  the only user).
- Optional: record all 16 samplers and the PS/VS pointers' content hash so ids can be matched with Light Probe captures.
