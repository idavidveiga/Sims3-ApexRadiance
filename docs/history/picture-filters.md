# Picture filters: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/picture-filters.md](../features/picture-filters.md). HDR output is described in
[removed-features.md](../removed-features.md).

### 2026-09-28: grading inside HDR output

**Context:** the combined build added grading controls to HDR output (raw notes "HDR (28/09)"): exposure, contrast,
blacks, expansion start, temperature, deband.

**Finding:** deband by random sampling would add noise; fixed rings (2 x 8 directions) with a threshold, applied to the
scene only, keep UI edges sharp.

**Outcome:** deterministic deband kept.

### 2026-09-28: Picture split from HDR

**Context:** raw notes "HDR nativo e saida HDR (28/09, noite)". The grade had to work with or without HDR.

**Finding:** the grade could share the HDR pass in `hdr_output.cpp/.h` (`HdrOutput::RenderPictureUI`, `PictureParams`,
shader `HdrPS` with constants c0..c15 including HDR-only `cNits`, `cSky`, `cCal`, `cLook.y` sRGB option, `cColor.xy`,
`cGrade.w` expansion start, `cWb.w` output mode). `HdrOutput::BeforeOverlay` ran from `HookedEndScene` right after
`RenderCallbacks::endSceneBeforeOverlay`; `HdrOutput::OnEndScene` ran after the S3SS overlay, before `original_EndScene`.
`HdrOutput::BeforeCreateDevice` could parse the whole TOML early when the device was created before the config load.

**Outcome:** a "Picture" section on the Display tab with its own table `[qol.picture]`; old grading values were read from
`[qol.hdr]` when `[qol.picture]` was missing (`ReadGrade`), and `SaveToToml` stopped writing grade keys to `[qol.hdr]`.
The combined build's README still listed the controls under "HDR > Advanced" (stale).

### 2026-09-28: 98.7% of the screen treated as UI

**Context:** HDR_Diag_1/2: grading never applied in interiors.

**Finding:** the first version copied the scene at the first depth-off draw after the scene. Interiors have depth-off
draws in the middle of the scene, so the copy missed most of the lamp light and the mask marked almost everything as UI.
Separately, hooks at `Normal` priority never saw draws that Night Lighting replaced and ended with `Skip`. Copying before
the bloom strip (FrameCapture 24/09 #977: one full-screen 2-primitive strip right after the scene) would leave bloom
outside the copy.

**Outcome:** the copy is retaken at every depth-on to depth-off transition (last wins), after the bloom strip, with hooks
that see every draw.

### 2026-09-28: carve-out for the standalone

**Context:** HDR output and Native HDR were removed from the standalone (plan `PLANO-SEPARACAO.md`, section 2d).

**Finding:** kept: `PictureParams`, the Picture part of the shader, the scene-copy machinery, `BeforeOverlay` logic, the
frame / scene / chain targets, shader and queries, the SDR path of `OnEndScene`, `[qol.picture]` read and write with the
`[qol.hdr]` fallback (moved to the one-time `S3SS.toml` migration), the UI. Dropped: the HDR aux shaders (BrightPS,
DownPS, UpPS, ControlPS), the glow and limiter targets, `s_lampGain`, `HdrNative::Update`, display queries, the depth
request of the sky boost, HDR_Diag, the device-creation path (`BeforeCreateDevice`, `AfterCreateDevice`,
`ApplyColorSpace`, `AfterReset`, `QueryDisplay`), DXVK and DXGI headers, and `HdrOutput::LampGain()` in the lot light
bridge. For SDR, running at Apex's own EndScene is correct in either hook order with S3SS.

**Outcome:** `features/picture.cpp` (SDR only, shader `PicturePS`, constants renumbered to c0..c12, log prefix
`[Picture]`, registry name `"Picture"`), precompiled on a background thread.

### 2026-09-30: Smooth gradients moves to the Banding tab

**Context:** the Color page gained a Banding tab grouping everything against colour steps.

**Finding:** the deband is the only help for ps_2_x surfaces such as the sky.

**Outcome:** Smooth gradients follows the Banding Fix switch (`Effective()`): with Picture off the pass runs with only
the deband, never in a frame without a scene copy. At the time, *Reset Picture* kept the deband value.

### 2026-09-30: draw-hook order after the post-scene trigger

**Context:** in the combined build, Picture's copy and the post-scene trigger were both `Priority::First` draw hooks and could fire on the same
draw in an undefined order; post-scene pixels would then be treated as UI.

**Finding:** an explicit priority removes the race.

**Outcome:** by 2.1.0, Picture's draw hooks run at priority 10: after the post-scene trigger, before `Early` (25) and every feature
that may skip a draw (`Normal`, 50).

### 2026-10-03: restore-default buttons removed; Film tones and Color mixer collapsed

**Context:** commit 1bb2291 removed page and card restore-default controls; commit 986cabb grouped rare controls.

**Finding:** per-row reset (changed dots) remains.

**Outcome:** *Reset Picture* (`PictureParams{}` keeping `enabled`, `compare` and the deband) and *Reset mixer* (all six to
100%) are gone. Film tones and Color mixer moved into collapsed *Advanced* groups.

### 2026-10-03: filtered screenshots and loading frames

**Context:** commits fc88731, 917f1d8 and 4107eee added the filtered player screenshot and fixed captures taken with the
menu open.

**Finding:** with the menu open the capture fired before the Picture pass and missed the grade; on loading frames the
pass could wait on shader compilation.

**Outcome:** the bootstrap runs Picture once and fires `filteredSceneBeforeOverlay` before the menu; the pass returns
until shader precompilation completes.

### 2026-10-05: Filters tab

**Context:** commit 95f059e added a Filters tab to the Color page with eight stackable looks (Technicolor 1 and 2, DPX
Cineon, Colourfulness, Night Mode, Emphasize, Prism, 3DFX), written from scratch with no third-party shader code.

**Finding:** every filter fits in the existing Picture pass as a dynamic branch on its own flag, so a filter that is
off costs nothing; the scene depth and camera are requested only while a depth filter is on. Settings in
`[qol.picture.filters]` are covered by profiles and Undo.

**Outcome:** kept. Commit b2c4eea rebuilt the tab as one card per filter (21 filters at the time) and added Vintage,
Cross-process, Black and white, Glow, Halation, Dreamy, Light leaks, Film grain, CRT, Cartoon, Sun rays and Fake HDR;
Emphasize gained automatic focus and a zone depth relative to the focus distance.

### 2026-10-05: Sun rays, Cartoon and Light leaks removed

**Context:** Sun rays (beams from the game's sun, found by a 1x1 search pass around the projected sun light and blocked
by the scene depth; commit 57b7610 stopped it blinking), Cartoon and Light leaks.

**Outcome:** removed (commits aa20747 and 1cb36ef): shaders, the sun search pass, settings and cards; RigTracker went
back to its Night Lighting-only form. Film grain stopped moving (the same grain every frame) and lost its Moving grain
switch. The tab was regrouped into Film looks, Color and mood, Light and detail, Camera, and Retro and style.

### 2026-10-05: pie menu, neighbour taps and the hard UI mask

**Context:** with the strong new filters, UI pieces showed through: the pie menu turned grey with Emphasize, panels and
text shadows were half filtered, and filters that read around the pixel pulled button colours into the world.

**Finding:** the pie menu's 3D Sim portrait is a short depth-tested run drawn over the UI, and copying the scene after
it made the menu part of the scene. The soft mask `saturate(difference x 64)` half-filtered translucent UI. Deband,
Prism, sharpening, the CRT warp and the 3DFX soft pixels read their neighbours from the finished frame.

**Outcome:** commit b90410f: after the first copy, a run under 20 depth-tested draws no longer re-copies the scene, and
an Emphasize zone depth saved in metres by the first test builds is reset on load. Commit 749b789: neighbour taps read
the scene copy. Commit a509f62: a hard UI mask (any difference of one 8-bit step is UI) and a rebuilt Film grain (smooth
value noise in two sizes, multiplying the brightness, strongest in the midtones).

### 2026-10-06: Filmic pass, Tint, Levels, LUT, fog, Auto exposure, Adaptive sharpening, Color-blind mode

**Context:** commits 3b24b79 and fb4f06a.

**Outcome:** the Filters tab reached its 26 filters, all off by default, with an Accessibility section for Color-blind
mode and a LUTs folder for PNG look-up strips.

### 2026-10-06: the pie menu's backing box and its Sim portrait

**Context:** with the pie menu open, the filters left an unfiltered box around it, and fog and Emphasize showed a grey
square where its Sim portrait is.

**Finding:** the backing box is a faint veil a few levels darker than the scene, so the hard mask kept it as unfiltered
picture. The portrait clears a square of the live depth before the pass reads it. A frame capture with the pie menu
open showed about 25 full-screen copies of the back buffer at the end of the frame, with the depth test on, `ALWAYS`
and no depth write, counted as more scene.

**Outcome:** commit 3375aee: only a clear change (4 to 24 levels, smoothly) counts as UI; under a faint veil the scene
copy is filtered and the veil applied again as the ratio frame / scene. Commit cf467d2: fog and Emphasize read a depth
copy taken with the scene copy. Commit 701fe64: a depth-tested draw with `ALWAYS` and no depth write is not scene.

### 2026-10-06: Relight removed

**Context:** commit 9bd719e added Relight: up to 4 lights of the player's own, fixed in the world. Each scene pixel's
position came from the depth copy and the inverted camera view-projection (computed on the CPU in double precision,
relative to the eye), its normal from the neighbours, with each light's angle and windowed falloff and 12-step
screen-space shadows. Lights were placed at the world point under the screen's centre (a 1x1 depth read back once).

**Outcome:** reverted the same day (commit 7619418) at the maintainer's decision; not part of any release.

### 2026-10-06: Smooth gradients off by default, on the Banding Fix page

**Context:** commit 410e521 gave the Banding Fix its own page.

**Outcome:** *Smooth gradients* moved with it, and its default changed from 100% to 0% (off).
