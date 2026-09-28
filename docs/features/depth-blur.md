# Depth Blur

> Distance blur (a simple depth-of-field without a near field) computed from the scene depth and applied to the
> finished 3D scene **before the game draws any UI**, so pie menus, tooltips, plumbobs and panels stay sharp. It is
> turned off, with a 0.3 s fade, while the game's map view is open. The module also owns the **INTZ depth swap** that
> makes the game's depth buffer readable (`DepthShare`), which Reflections (water screen-space reflection) reads too.
> Status: working, flagged `experimental` in the patch registry. Present in both build flavours; the Developer subsection
> (far plane, mask view, counters) exists only in the dev build (`kPublicBuild == false`).
> Patch name `DepthBlur`, UI in the **Apex** tab, settings in `[patches.DepthBlur]`.

## Purpose

ReShade-style DOF blurs the UI too, because it runs on the final frame (see the original request in the old scratchpad
`s3ss_feature_request_dof.md`: ReShade CinematicDOF blurred pie menus; REST, which can inject before a draw, crashed under
DXVK). Doing it inside the D3D9 hooks lets the blur run between the last scene draw and the first UI draw. The same
insertion point and depth access later served Edge Smoothing, the (now removed) Ambient Occlusion, the HDR sky boost and
the water reflections.

## User-facing settings

All live (read every frame; `Update()` only clears `pendingReinstall`, a reinstall would tear the depth swap down from the
wrong place). Saved by the patch system into `[patches.DepthBlur]` (combined build: `S3SS.toml`; standalone:
`ApexRadiance.toml`, same table name, see PLANO-SEPARACAO.md "Config schema"), plus the usual `enabled` key.

| UI label | TOML key | Type | Default | Range | Notes |
|---|---|---|---|---|---|
| Where the blur starts | `distancia` | float | 0.349 | 0.0 - 0.5 | Start of the ramp in the heuristic "linear depth" (see below). Small buttons **Near** 0.25, **Medium** 0.349, **Far** 0.45 (also registered as setting presets). |
| Strength | `forca` | float | 1.0 | 0 - 1 | Multiplies the blur factor (0 = none). |
| Off in map view | `offInMapView` | bool | true | | Fade the blur out while `MapView::IsOpen()`. Shows "Map view detection is not available on this game version." when the getter was not found. |
| Advanced > Transition | `transicao` | float | 0.20 | 0.01 - 0.5 | Ramp length after the start (lower = sharper transition). |
| Advanced > Blur size | `tamanho` | float | 1.5 | 0.5 - 6.0 | Total Gaussian spread, in half-resolution tap spacings (see Quality). |
| Advanced > Quality | `qualidade` | enum int | 2 (High) | Low / Medium / High / Ultra | Tap spacing and max pass count. |
| Advanced > Blur the sky | `blurSky` | bool | true | | Sky pixels (d >= 0.99999) get factor 1 (true) or 0 (false). |
| Advanced > Reset to defaults | | | | | `g.p = Params{}`: resets every field, including `farPlane` and `debugView`. |
| Developer > Far plane (dev build only) | `farPlane` | float | 1000.0 | 10 - 10000 | InputBox in the dev UI; clamped. Only a curve parameter of the heuristic below, NOT the game's far plane. |
| Developer > Show the blur mask (dev only) | `debugView` | bool | false | | Composite outputs the factor as grey (white = blurred). Runs even at strength 0 and in map view. |

Developer section also shows `Blurred frames: N | blur passes per frame: n` and `Map view: open/closed | fade x.xx`.

## How it works

### Enable / disable (who runs the depth swap)

`DepthBlurPatch::Install()` sets `g.blurOn = true`, calls `UpdateDepth()` and `PostScene::Add(PostScene::kDepthBlur, BlurEffect)`.
`Uninstall()` removes the effect, sets `blurOn = false`, calls `UpdateDepth()` again and releases the shaders.

`UpdateDepth()` (depth_blur_patch.cpp): the swap runs while `g.blurOn || g.requests > 0`.
- `StartDepth()` registers a registry **Present** hook (name `"DepthBlur"`, `Priority::First`) that calls
  `OnFrameBoundary`, and adds `OnPreReset` / `OnPostReset` to `RenderCallbacks::preReset` / `postReset`. Sets `g.active`.
- `StopDepth()` unregisters them and calls `ReleaseResources(g_pd3dDevice)`.
- `DepthShare::Request(bool)` (implemented in depth_blur_patch.cpp) increments / decrements `g.requests` and calls
  `UpdateDepth()`. Combined build requesters: Ambient Occlusion (`Install`/`Uninstall`) and HDR output's sky boost
  (`hdr_output.cpp`, `m_depthRequested`, only while `skyBoost > 0.001`). **Standalone: both are removed** (see
  [../removed-features.md](../removed-features.md)), so the swap runs only while Depth Blur is on. Reflections never
  request it; they use the texture only when it exists.

### Per frame

1. **Present (frame boundary, `OnFrameBoundary`).** If not `ready`, every `kRetryFrames` = 120 frames try
   `InitResources(dev)` (first try immediately: `retryCountdown = 0`). Records the backbuffer identity in `g.backBuffer`
   (vestigial since the trigger moved to PostScene on 26/09; `g.curRT0` likewise).
2. **`InitResources`** (runs at Present, between frames):
   - `ExtraHooks::EnsureInstalled(dev)`; failure -> status "ERROR: could not install the depth hooks".
   - Backbuffer must not be multisampled, else "Edge Smoothing is on: turn it off in the game's Options > Graphics".
   - `RawGetDepthStencilSurface` must return a surface with the backbuffer's size, no MSAA, format `D3DFMT_D24S8` or
     `D3DFMT_D24X8`, else "Waiting for the game (no depth buffer yet)" / "(depth buffer is not the screen's)". This
     surface is kept (AddRef held) as `g.origDS` = the game's auto depth-stencil.
   - Creates the INTZ texture: `CreateTexture(W, H, 1, D3DUSAGE_DEPTHSTENCIL, MAKEFOURCC('I','N','T','Z'), D3DPOOL_DEFAULT)`
     and its level-0 surface `g.intzSurf`. Failure -> "ERROR: the graphics card/driver does not support INTZ depth textures".
   - Two half-resolution render targets `halfA`, `halfB`: `((W+1)/2) x ((H+1)/2)`, `D3DFMT_A8R8G8B8` (the alpha channel
     carries the per-pixel blur factor between passes). Failure -> "ERROR: not enough video memory for the blur textures".
   - Compiles `PrepPS`, `BlurPS`, `CompositePS` (ps_3_0, `D3DCOMPILE_OPTIMIZATION_LEVEL3`, file name "depth_blur.hlsl").
   - `ExtraHooks::SetDepthSubstitution(SubstituteDS, ReportDS)`, `ready = true`, then
     `RawSetDepthStencilSurface(dev, intzSurf)` (the code assumes the auto depth-stencil is the one bound at Present).
     Log: `[DepthBlur] Resources ready (WxH, INTZ depth swapped in)`.
3. **During the frame** the game binds its auto depth-stencil through `SetDepthStencilSurface`; the ExtraHooks detour
   calls `SubstituteDS(requested)`, which returns `intzSurf` when `requested == origDS` (anything else passes through).
   `GetDepthStencilSurface` calls `ReportDS(actual)`, which returns `origDS` when `actual == intzSurf`. The game never
   sees the swap; the scene's depth lands in a texture that shaders can sample.
4. **PostScene trigger** (see "Shared machinery" below) calls `BlurEffect(dev)` third in the chain (order 30):
   - returns if `!blurOn || !ready || inBlur || internalPass`;
   - `StepMapFade()`: `dt` from QPC since the previous call (clamped to 0.1 s so a hitch does not skip the fade);
     `mapOpen = offInMapView && MapView::IsOpen()`; `mapFade` moves toward 1 (open) or 0 by `dt / 0.3 s`;
   - debug view: always runs; otherwise returns early (no GPU work at all) when `strength * (1 - mapFade) <= 0`;
   - `RunBlur(dev)`.
5. **`RunBlur`** (all draws are `DrawPrimitiveUP` quads, `D3DFVF_XYZRHW | D3DFVF_TEX1`, -0.5 pixel offset):
   1. `SavedState::Capture` (RT0, depth-stencil via `RawGetDepthStencilSurface`, PS, VS, declaration, FVF, stream 0,
      textures and 8 sampler states of s0..s2, 15 render states, PS constants c0..c3, viewport). No state block (CPU heavy).
   2. `StretchRect(backbuffer -> halfB, D3DTEXF_LINEAR)`: straight to half resolution, no full-res copy.
   3. `RawSetDepthStencilSurface(nullptr)`: the INTZ must not be bound while it is sampled. `SetPassStates`: Z off,
      blending off, cull none, sRGB write off, colour write 0xF; s0/s1 LINEAR, s2 POINT, clamp, no mips.
   4. Constants: c0 = (start, range, strength x (1 - mapFade), farPlane); c1 = (1/halfW, 1/halfH); c3 = (blurSky, debug).
      INTZ on s2.
   5. **Prep**: halfB -> halfA, rgb = colour, a = `BlurFactor(uv)`.
   6. **Blur**: `n = BlurIterations(spread, quality)` iterations of H then V (halfA -> halfB -> halfA), tap spacing
      `spread / sqrt(n)` in half-res texels ("n Gaussian passes of spacing s equal one pass of spacing s * sqrt(n)").
   7. **Composite** into the backbuffer: alpha blending SRCALPHA / INVSRCALPHA, colour write RGB only (the backbuffer
      alpha stays the game's), `CompositePS` samples halfA (s1) and the full-res factor from the INTZ.
   8. `SavedState::Restore` (RT0 first because SetRenderTarget resets the viewport; stream 0 restored because
      DrawPrimitiveUP clears it; depth-stencil restored with `RawSetDepthStencilSurface`, i.e. the INTZ again).
6. **Reset** (`HookedReset` in d3d9_hook.cpp fires `preReset` before the real Reset, `postReset` after a successful
   one): `OnPreReset` -> `ReleaseResources(dev)`: clears the substitution, re-binds `origDS` if the INTZ is bound,
   releases INTZ, half targets and the `origDS` reference (D3D9 Reset fails while default-pool references are held).
   Status "Recreating after a video change...". `OnPostReset` sets `retryCountdown = 0` so the next Present re-inits.

### Quality levels

`BlurIterations(spread, q) = clamp(ceil((spread / kQualitySpacing[q])^2), 1, kQualityMaxIterations[q])`.

| Quality | Max tap spacing (half-res px) | Max H+V iterations | Iterations at default spread 1.5 (computed) |
|---|---|---|---|
| 0 Low | 2.0 | 1 | 1 (spacing 1.5) |
| 1 Medium | 1.4 | 3 | 2 (spacing 1.06) |
| 2 High (default) | 1.0 | 6 | 3 (spacing 0.87) |
| 3 Ultra | 0.75 | 12 | 4 (spacing 0.75) |

The 13-tap kernel weights `W = {0.1963, 0.1745, 0.1216, 0.0662, 0.0280, 0.0092, 0.0024}` are a Gaussian of sigma about
2 taps (computed from W1/W0), so the total blur sigma is roughly `2 x spread` half-res pixels, about `4 x spread`
full-res pixels (computed, not measured).

### The blur factor and what "distance" means

```
d   = INTZ.r at uv (point)                 // device depth, 0..1
lin = d / (F - d * (F - 1))                // F = farPlane (1000)
f   = saturate((lin - start) / max(range, 1e-4))
if (d >= 0.99999) f = blurSky              // sky
factor = f * strength * (1 - mapFade)
```
This `lin` is a heuristic curve, not a metric distance. The game's projection is (nearly) infinite-far with a variable
near plane (see [../engine/camera-and-map-view.md](../engine/camera-and-map-view.md)): `d = A - near*A/z`, A = 1.00008,
near 0.2 - 0.3. Solving for view distance (A taken as 1): `z = near * (1 + lin*(F-1)) / (1 - lin)`. With near = 0.25 and
F = 1000 (computed, not measured in game):

| Setting value (`lin`) | View distance at near 0.25 |
|---|---|
| 0.25 (Near preset) | about 84 m |
| 0.349 (Medium, default) | about 134 m |
| 0.45 (Far preset) | about 205 m |
| 0.549 (default start + default transition = full blur) | about 305 m |

Because z is proportional to near, the same setting starts the blur 20% closer at near 0.2 and 20% further at near 0.3,
i.e. it shifts with zoom and camera height (inferred from the formula and the measured near range; not observed as a
complaint). Ambient Occlusion solved the same problem by converting to metres with `PostScene::CameraNear()` /
`CameraDepthA()`; Depth Blur never did.

### Shaders (embedded HLSL, `kShaderSource`)

| Entry | Input | Output | Notes |
|---|---|---|---|
| `PrepPS` | s0 = halfB (colour), s2 = INTZ | rgb = colour, a = factor | factor evaluated at half-res uv, point-sampled depth |
| `BlurPS` | s0 = the other half target | rgb = weighted sum, a = centre's factor | 13 taps `uv + cDir.xy * cTexel.xy * i`, weight `W[|i|] * (tap.a + 0.02)`: sharp taps (a ~ 0) weigh 0.02, so the foreground does not bleed into the blurred background (no halos) |
| `CompositePS` | s1 = halfA, s2 = INTZ | rgb = blurred colour, a = full-res factor | debug: returns (f, f, f, 1) |

Registers: c0 `cParams` (start, range, strength, far plane), c1 `cTexel`, c2 `cDir` (xy = direction * spacing),
c3 `cFlags` (x = blur sky, y = debug view).

### Map view detection (map_view.cpp)

The "Off in map view" option needs to know whether the game's map view (M key, or zooming all the way out) is open.
There is no D3D-level signal for it, so `MapView::IsOpen()` calls the game's own script binding:

1. `Resolve()` (once, `std::call_once`, on the first `Available()` / `IsOpen()` call, i.e. from the render thread at the
   first PostScene trigger): find the sections `.text`, `.rdata`, `.data` of `TS3W.exe` from the PE headers.
2. Search `.rdata` for the exact string `"ScriptCore.CameraController::Camera_IsMapViewModeEnabled"` including its
   terminator (1.67.2: at `0x010000A4`).
3. The script API registers native calls from a table of `{function pointer, name pointer}` pairs. Search `.data`
   (4-byte aligned; fallback `.rdata`) for the name's address; the dword **before** it is the function pointer. On
   1.67.2 the entry is at **`.data 0x0115DD40` (function) / `0x0115DD44` (name)**. (The code comment in
   `map_view.cpp:77` says "0x0115DD20"; that is a neighbouring entry, `{0x0073D620,
   "ScriptCore.CameraController::Camera_SetMotion"}`. The search does not use the constant, so the code is unaffected.)
4. Validate before trusting: the pointer must lie in `.text` with 0x60 bytes to spare, start with `E8` (call), push the
   cast id `68 89 DD 0F 11` (`push 110FDD89h`) and end in `8A 80 xx xx xx xx C3 32 C0 C3`
   (`mov al, [eax+disp32]; ret; xor al, al; ret`) within the first 0x50 bytes (`LooksLikeGetter`). Otherwise log
   `[MapView] Unexpected function at 0x..., map view detection off`.
5. Found: log `[MapView] Camera_IsMapViewModeEnabled at 0x0073e060`.
6. `IsOpen()` calls it as `bool __cdecl()` inside `__try/__except`; a fault logs `[MapView] Call faulted, map view
   detection off` and disables detection for the session.

What the function does (static disassembly, `engine_map\full.asm`, verified): `0x0073E060` calls `0x0096B390` (app),
`0x00F20C20` (world), `0x0096B6D0` (camera manager), then the manager's vfunc `+0x0C` with cast id `0x110FDD89` (camera
interface), and returns the byte at **camera + 0x8B9**, or 0 when any link is null. It only reads, so calling it from
the render thread is safe. The static call graph and data refs agree: `datarefs.tsv` has `0x0115DD40 -> 0x0073E060`.
Details of the camera object: [../engine/camera-and-map-view.md](../engine/camera-and-map-view.md).

The fade (`StepMapFade`) is a linear ramp of `kMapFadeSeconds = 0.3` s on `mapFade`, applied as
`strength x (1 - mapFade)`; at `mapFade = 1` Depth Blur does no GPU work at all.

### Standalone baseline

Per the split decision, the standalone takes Depth Blur and Edge Smoothing from the v0.1.0 commit `b84d5f1` ("Night
Remake alpha") **plus** the later SMAA and map-view fade. Differences between `b84d5f1` and `combined-final` for this
feature: `map_view.cpp` did not exist, `Params::offInMapView` / `mapFade` / `StepMapFade` were added, the status strings
were `S3SS_TR` pairs (now English only), and `post_scene.h` had `kSsao = 10` and no camera votes (`CameraNear`,
`CameraViewProj`, `CameraDepthA` were added later for Ambient Occlusion). Depth Blur itself does not use the camera
votes; the dev-only Frame Profiler reads `CameraViewProj` as a fallback, so check that dependency when porting.

## Shared machinery (post-scene chain, depth share, extra hooks)

Also summarised in [../architecture.md](../architecture.md); the details that matter for this feature:

### PostScene (post_scene.cpp / post_scene.h)

- **Chain in the standalone:** `kEdgeSmoothing = 20`, then `kDepthBlur = 30`. The combined build also had
  `kAmbientOcclusion = 10` (first); it is removed in the standalone ([../removed-features.md](../removed-features.md)).
  Effects are kept in a vector sorted with `std::stable_sort` by order; `Add` ignores duplicates.
- `PostScene::Add` registers the registry hooks (name `"PostScene"`, all `Priority::First`) with the first effect;
  `Remove` unregisters them with the last one. On first registration `g_done = true`, so nothing runs until the next
  Present.
  - Present -> `OnFrameBoundary`: refresh the backbuffer identity (`GetBackBuffer`), clear `g_sceneDraws`, `g_done`,
    near and view-projection votes.
  - SetRenderTarget -> track RT0 identity (`index == 0`).
  - DrawIndexedPrimitive and DrawPrimitive -> `OnGameDraw`.
- **`OnGameDraw`, the "scene done, before UI" detector:**
  - ignored when `g_done` or `DepthShare::InternalPass()`;
  - ignored unless RT0 is the backbuffer;
  - `D3DRS_ZENABLE != D3DZB_FALSE` -> a scene draw: `g_sceneDraws++`, and during the first 24 scene draws the camera
    votes (`VoteNear`, see the camera doc);
  - `ZENABLE == FALSE` with `g_sceneDraws >= 20` (`kMinSceneDraws`) -> the trigger: `g_done = true` first (a failure never
    retries within the frame), copy the effect list under the mutex, run each effect with the device. The effects run
    inside the game's draw call, before the game's draw executes.
  - Per post_scene.h and FrameCapture #977 (notes, HDR section 28/09), the first depth-off backbuffer draw after the
    scene is the **bloom composite** (a `DrawPrimitive` triangle strip of 2 primitives) when bloom is on, else the first
    UI draw. So the effects run **before the bloom is composited** over the scene.
- Effects draw with `DrawPrimitiveUP`, which is not a registry hook, so they never re-trigger PostScene. Their
  `SetRenderTarget` calls do go through the registry (the registry mutex is recursive): PostScene's own RT0 tracker
  follows them, and each effect restores RT0 at the end so the tracking ends correct.
- Registry order: draw hooks with `HookAction::Skip` stop the chain. lot_light_bridge's DIP/DP hooks register at the
  default `Priority::Normal` and return Skip for draws they replace; PostScene is at `Priority::First`, so it sees every
  game draw first. Its extra draws (lake/water pass, `draw()` lambda re-entering `DrawIndexedPrimitive`) are wrapped in
  `DepthShare::SetInternalPass(true/false)` because that pass turns ZENABLE off and unbinds the depth-stencil, which
  would otherwise look like the first UI draw (bug found in the 25/09 adversarial review, item 1: "the lake pass turns
  ZENABLE off -> Depth Blur thought it was the first UI draw and blurred mid-frame").

### DepthShare (depth_share.h, implemented at the end of depth_blur_patch.cpp)

| Function | Returns / does |
|---|---|
| `Texture()` | `g.intzTex` when `ready`, else null |
| `Surface()` | `g.intzSurf` when `ready` (level 0, the surface bound as depth-stencil while the scene renders) |
| `SetInternalPass(bool)` / `InternalPass()` | the flag above (render thread only, plain bool) |
| `Request(bool)` | reference-counted request to keep the swap running with Depth Blur off |
| `Status()` | Depth Blur's status string, shown by requesters ("Waiting for the scene depth: ...") |

Consumers must check that `Surface()` is the depth-stencil bound right now (via `ExtraHooks::RawGetDepthStencilSurface`)
before trusting the texture as the main scene's depth (AO and the water pass both do; reflections and UI passes bind
other depth surfaces), and must unbind it (`RawSetDepthStencilSurface(nullptr)`) while sampling it, restoring after.

### ExtraHooks (d3d9_extra_hooks.cpp / .h)

- Detours on device vtable slots not covered by the registry: 34 `StretchRect`, 39 `SetDepthStencilSurface`,
  40 `GetDepthStencilSurface`, 43 `Clear`, 83 `DrawPrimitiveUP`, 84 `DrawIndexedPrimitiveUP`.
- Installed once, on first `EnsureInstalled(dev)` (Depth Blur's `InitResources`, or Frame Capture), never removed; with
  no callback set each costs an atomic load.
- Guard: refuses to install if any of its slots shares code with a slot already detoured by d3d9_hook.cpp /
  d3d9_hook_registry.cpp (`owned = {16, 17, 23, 28, 37, 41, 42, 47, 65, 81, 82, 91, 92, 94, 106, 107, 109}`), log
  `[ExtraHooks] vtable[m] shares code with vtable[o], not installing` (protects against DXVK folding functions).
- Single-owner depth substitution (`SetDepthSubstitution(substitute, report)`), observer slots used only by Frame
  Capture, and `RawSet/RawGetDepthStencilSurface` that bypass the substitution.
- PLANO-SEPARACAO.md 2f: official S3SS hooks neither slot 39 nor 40, so the swap has no competitor; S3SS's ImGui uses
  state blocks, which do not include the depth-stencil binding, so its overlay neither sees nor disturbs the INTZ.

### RenderCallbacks (render_callbacks.h)

Three arrays of 4 atomic slots (`endSceneBeforeOverlay`, `preReset`, `postReset`), fired from d3d9_hook.cpp.
`Add` silently does nothing when all 4 slots are taken. In the combined build five modules add a `preReset` callback
(Ambient Occlusion, Depth Blur, Edge Smoothing, Lot Map Probe, Night Lighting's `LightmapSmooth::OnPreReset`), so with
all five on, the last one to install is never called before Reset and its default-pool resources make Reset fail
(found while writing these docs, from the code; not observed). The standalone without AO has exactly four.

## Files and functions

| File | Function / symbol | Role |
|---|---|---|
| patches/depth_blur_patch.cpp | `DepthBlurPatch` (Install/Uninstall/RenderCustomUI), `APEX_REGISTER_FEATURE` | patch, settings, UI |
| | `StartDepth`, `StopDepth`, `UpdateDepth` | depth swap lifetime |
| | `InitResources`, `ReleaseResources`, `SubstituteDS`, `ReportDS` | INTZ swap |
| | `BlurEffect`, `StepMapFade`, `RunBlur`, `BlurIterations`, `SavedState`, `SetPassStates` | the effect |
| | `OnFrameBoundary`, `OnPreReset`, `OnPostReset` | Present / Reset |
| | `namespace DepthShare { ... }` | implementation of depth_share.h |
| depth_share.h | `DepthShare::*` | interface |
| post_scene.cpp/.h | `PostScene::Add/Remove`, `OnGameDraw`, `OnFrameBoundary` | trigger chain |
| d3d9_extra_hooks.cpp/.h | `ExtraHooks::*` | depth-stencil detours |
| render_callbacks.h | `RenderCallbacks::preReset/postReset` | Reset slots |
| map_view.cpp/.h | `MapView::IsOpen/Available` | map view flag |
| gui.cpp | Apex tab: `RenderApexFeature("DepthBlur", "Depth Blur")` after Ambient Occlusion, followed by the note "Ambient Occlusion, Depth Blur and Edge Smoothing need the game's own Edge Smoothing off" | UI placement |

## Game addresses and patterns

Depth Blur itself patches no game code. It depends on:

| Address | What | How found / verified |
|---|---|---|
| `0x0073E060` | `ScriptCore.CameraController::Camera_IsMapViewModeEnabled` (returns camera + 0x8B9) | found at run time through the name string (`.rdata 0x010000A4`) and the `{function, name}` binding table entry (`.data 0x0115DD40/44`), shape-checked (see "Map view detection"); verified statically and in the log `[MapView] Camera_IsMapViewModeEnabled at 0x0073e060`. |
| device vtable 34/39/40/43/83/84 | D3D9 methods (DXVK `d3d9.dll`) | ExtraHooks, guard above |

## Interactions

- **Edge Smoothing (order 20)** runs before Depth Blur on the same trigger, so the blur works on the anti-aliased image.
  Edge Smoothing does not need the depth swap. Both need the game's MSAA ("Edge Smoothing" in Options > Graphics) off.
- **Reflections** (lot_light_bridge.cpp water pass): screen-space ray march against `DepthShare::Texture()` when it
  exists and `Surface()` is bound; otherwise lamps only (no scenery reflection). So scenery reflections on ponds need
  Depth Blur on in the standalone (the notes: "Without Depth Blur: the approximate reflection stays").
- **Picture filters** (hdr_output.cpp, `[qol.picture]`, SDR only in the standalone): the pass runs at the end of
  EndScene and separates scene from UI by comparing the final frame with a **scene copy** taken at the depth-tested ->
  depth-off transition in its own `Priority::First` draw hooks (a pixel counts as UI where it differs by more than 1/64).
  When bloom is on, the copy is taken after the bloom strip, i.e. after PostScene already ran, so the blurred pixels are
  scene. When there is no bloom strip, the copy and PostScene fire on the same draw and their order is the registry sort
  order of two `Priority::First` hooks (`std::sort`, not stable; PLANO-SEPARACAO.md 2d). If the copy runs first, the
  blurred pixels differ from the copy and would be treated as UI (inferred from the code, not observed). The combined
  build's HDR output and HDR sky boost (which requested the depth) do not exist in the standalone.
- **Frame Profiler** reads `PostScene::CameraViewProj` as a camera-motion fallback; it is only captured while at least
  one PostScene effect is on.
- **S3SS overlay:** drawn in S3SS's EndScene, after the trigger, so it is never blurred (both hook orders,
  PLANO-SEPARACAO.md 2d).

## Known limitations

- Needs the game's multisampling off: multisampled depth cannot be sampled in D3D9, and the auto depth-stencil must be
  D24S8/D24X8 at screen size.
- The start distance is in a heuristic unit that scales with the camera near plane (see above).
- Colour is blurred at half resolution; the mask is full resolution.
- Trigger position: the effect runs before the bloom composite. In interiors the game has depth-off backbuffer draws
  in the middle of the scene (HDR diagnostic 28/09: a scene copy taken at the first depth-off draw missed most lamp
  light and 98.7% of the screen counted as UI). PostScene still fires on the first depth-off draw after 20 scene draws,
  so in some interiors the blur may run before the scene is complete (inferred from that diagnostic; HDR was changed to
  recopy at every transition, PostScene was not).
- Map view detection needs `Camera_IsMapViewModeEnabled`; on other game versions the option does nothing.

## Pitfalls and failed approaches

- **Lake pass mid-frame trigger** (25/09 review): any extra pass that turns ZENABLE off on the backbuffer must be
  wrapped in `DepthShare::SetInternalPass`.
- **Game MSAA on** (notes, "Lago preto", m16+ captures showed samples=8): Depth Blur could not share the depth, the water
  fell back to a 40 m guess sampled at the wrong place and made black patches at night. With no depth, reflections now
  use lamps only.
- **Spreading taps apart** makes dotted artefacts; repeat tight passes instead (comment in `RunBlur`).
- **Full state blocks** were avoided on purpose (CPU heavy); save exactly the states touched, and remember
  `SetRenderTarget` resets the viewport and `DrawPrimitiveUP` clears stream 0.
- **Do not call `Uninstall`/reinstall from `Update()`**: it would tear down the swap from the wrong thread.
- **Projection model:** early notes (26/09) said the far plane is infinite (A = 1) and near sometimes 1.0 or 10; both
  were rounding in 3-digit captures, corrected on 27/09-28/09 (A = 1.00008, near 0.2 - 0.3). `farPlane` = 1000 is
  unrelated to the game's far plane; it only shapes the curve.
- **Held references across Reset:** `origDS` is AddRef'd; it must be released in `preReset` (done in `ReleaseResources`).

## Testing in game

- Apex tab > Depth Blur: status "Active". Developer (dev build): "Blurred frames" increases; "blur passes per frame"
  matches the quality table; toggle **Show the blur mask** (white = blurred).
- Open a pie menu over a blurred background: the menu must be sharp.
- Press M (or zoom fully out): "Map view: open", fade goes to 1.00 within 0.3 s and the blur disappears; closing fades it
  back.
- Change resolution / alt-tab (Reset): status shows "Recreating after a video change..." then "Active" again.
- Log (`S3SS_LOG.txt` in the combined build, `ApexRadiance_LOG.txt` in the standalone): `[DepthBlur] Installed`,
  `[ExtraHooks] Installed (StretchRect, Set/GetDepthStencilSurface, Clear, DrawPrimitiveUP, DrawIndexedPrimitiveUP)`,
  `[DepthBlur] Resources ready (WxH, INTZ depth swapped in)`, `[MapView] Camera_IsMapViewModeEnabled at 0x0073e060`.
- Frame Capture (dev) lists StretchRect / depth-stencil binds / DrawPrimitiveUP through the ExtraHooks observers.

## Open items

- Express the start distance in metres through `PostScene::CameraNear()` / `CameraDepthA()` (idea, as the removed AO did).
- Decide whether PostScene should fire at the last depth-tested -> depth-off transition like the HDR/Picture scene copy
  (needs a way to run effects after the fact, e.g. at the next draw or at EndScene).
- Make the Picture scene copy and PostScene order explicit (priorities instead of equal `Priority::First`).
- Raise `RenderCallbacks::kSlots` or log when `Add` finds no free slot.
