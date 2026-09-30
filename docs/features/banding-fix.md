# Banding Fix (scene dither)

> Removes the colour steps (banding) of smooth gradients in the 3D world: lamp pools on walls and floors, room light
> fall-off, shadows. Patch name `SceneDither`, menu Image > Color > Banding and an Overview row, on by default, flagged experimental (2.2.1, the user's call; a "Still being tested" note on the tab);
> settings: Strength (`forca`, 0 - 1 step, default 1) and Moving grain (`graoEmMovimento`, off). Added 30/09/2026 on the user's report ("the colours are not uniform, mainly where there is light"; an
> interior wall showed rings around a lamp's pool of light). **First in-game test pending** (build 57a0a09d).

## Why the rings

The game draws the 3D scene straight into an 8-bit back buffer: 256 levels per channel. A lamp's pool of light is a long,
smooth gradient in the dark range, where one level is a large step of brightness, so every level boundary shows as a
ring; an OLED shows them sharply. Measured on lossless captures (`cor_1/2/6.bmp`): black does reach 0 and white 255 (no
lifted blacks or clipped range); in smooth dark areas 64-89% of neighbour steps are 1 level and 11-36% are 2 or more.
Wide gamut (the user's AW3225QF is DCI-P3) is not the cause: the game outputs sRGB, and reaching P3 would need an HDR /
scRGB output.

## Settings

| UI label | TOML key | Type | Default | Range | Notes |
|---|---|---|---|---|---|
| (card switch) | `enabled` | bool | true | | |
| Strength | `forca` | float | 1.0 | 0 - 1 | peak of the triangular grain in 8-bit steps (100% = +-1 step, the full TPDF; 0.5 - 3 until 30/09 night, when every surface got covered and the user asked for 0 - 100%); set per draw in the copy's amount constant |
| Moving grain | `graoEmMovimento` | bool | false | | a new pattern phase every frame (`cA.w` = frac(frame x golden ratio), added to the IGN input): high frame rates average the grain away (the user saw "micro specks" at 223%); at low ones a faint shimmer |
| Developer > Show surfaces without the fix in magenta | (not saved) | bool | false | | scene draws with no copy drawn flat magenta (a magenta shader of the same version) |

The first build (30/09, 57a0a09d) used a uniform grain of +-0.5 step; the user found it "a little better, not 100%". Since
7d5615d the grain is triangular (TPDF: the inverse CDF of the triangular distribution applied to the IGN value), which
leaves no noise modulation. GPU check (`scratchpad/dither/tpdftest.cpp`): a grey of 20.40 / 100.70 levels comes out as
20.355 / 100.657 on average (20 / 101 without), the level weights follow the triangle (about 1/8, 3/4, 1/8 on a level).
The AO composite adds the same grain (it rounds to 8 bits again). Since 30/09 night it follows the Banding Fix (its Strength and phase; none when
it is off) and only where the shade changed the pixel, with a shifted pattern: the same pattern added twice doubled the
grain (one of the causes of the "micro specks"). AO settings revision 5.

## Menu and Smooth gradients (30/09)

Color page, tab Banding (user's idea: everything against colour steps in one place): the Banding Fix card with Strength
and Picture's Smooth gradients (the deband post filter). The deband follows the Banding Fix switch, not Picture's: with
Picture off, Picture's pass runs with only the deband while the Banding Fix is on and Smooth gradients > 0 (never in a frame
without a copy of the scene, where it would smooth the menus). It is the only help for ps_2_x surfaces (the sky...).

In game (30/09 evening): "on many walls it changed almost nothing"; the magenta view turned the whole screen magenta
(a full-screen Z-on ps_2_x pass drawn over the scene with blending became opaque magenta) and the log showed only 109-139
copies made in the session, so most scene draws were not covered. The magenta view was replaced by "Show covered surfaces"
(a coarse 24-step grain on covered draws only) and the dev build logs the coverage every 20 s (first 12 times):
`[SceneDither] Last frame, 3D scene draws: N dithered, M ps_2_x (no pixel position), K ps_3_0 refused, L other | ...`.
The log then answered it: in the world 123-130 scene draws per frame were dithered and 166-172 were ps_2_x (0 ps_3_0
refused): more than half of the scene, the walls among them, drew with ps_2_x shaders.

## ps_2_x path (30/09 evening, 53d1091)

ps_2_x has no `vPos`. `ShaderPatches::AddDither2` reads the clip position from TEXCOORDk (k = the highest texture
coordinate the pixel shader does not use), computes the pixel `(ndc.x w/2 + w/2, -ndc.y h/2 + h/2)` (w, h from the
viewport, in the amount constant: `(amount, w/2, h/2, 0)`), then the same triangular grain; one constant per instruction
(ps_2_0 rule), the end writes `oC0` once. The copy is **ps_2_x** (`0xFFFF0201`): the grain does not fit the 64 arithmetic
slots of some ps_2_0 (519 of 2308 failed as ps_2_0). `AddScreenPosVs(vs, k)` redirects every `oPos` write to a free temp
and writes it to `oPos` and `oTk` (vs_1_1 / vs_2_x only: vs_3_0 pairs with ps_3_0). At creation the vertex copy for k = 7
is made; other k at the first draw. At the draw both copies are bound (a ps_2_x copy without a vertex copy = not
covered, counted as "vertex shader").

Offline (`scratchpad/dither/dithertest2.cpp`, native D3D9): ps_2_0 2996 / 3104 patched and created (108 with no free
texture coordinate, temp or constant); vs_2_0 4035 / 4338 (2961 with t7, 1074 with a lower k; 303 refused); a grey drawn
through a patched vs_2_0 + ps_2_0 pair gives the same means and triangle as ps_3_0 (20.355 / 63.971 / 100.657).
In-game coverage: the dev log line `[SceneDither] Last frame, 3D scene draws: ...` (first 12 times, every 20 s).

## How it works (`features/scene_dither.cpp`, `ShaderPatches::AddDither`)

- **The patch** (ps_3_0 only; pure function, tested offline): every write to `oC0` goes to a free temp `rO`; at the end
  `oC0.rgb = rO.rgb + (IGN(vPos) - 0.5) / 255` and `oC0.a = rO.a` (the alpha is the bloom mask: untouched). IGN =
  interleaved gradient noise `frac(52.9829189 * frac(dot(vPos, (0.06711056, 0.00583715))))`, a fixed per-pixel pattern
  (no time: nothing flickers). Two `def` constants and two temps above the shader's own; `dcl vPos.xy` added when
  missing. Refused (shader left alone): not ps_3_0, no colour write, subroutines / `ret`, relative constant addressing,
  no free register.
- **Creation:** a `CreatePixelShader` callback after every other one creates the game's shader itself
  (`D3D9Hooks::CallOriginalCreatePixelShader`, added for this) to learn its pointer, then the dithered copy; the pair is
  kept by the game shader's pointer (a reused address is overwritten at its next creation; everything is released when
  the feature is turned off). Shaders older than the feature get their copy at their first draw (`GetFunction`).
- **Draw:** DIP / DP callbacks after every other one: when render target 0 is the back buffer and `ZENABLE` is on (the
  3D scene), bind the copy, draw through `CallOriginalDraw*`, put the game's shader back (Skip). Night Lighting's
  replaced draws are re-issued through the device, so their patched shaders get their own copies.
- **Left alone on purpose:** the mouse-pick pass (its own 16x16 target, IDs and packed depth in the colour), shadows,
  reflections and bloom (their own targets), and every back-buffer draw with the depth test off: the UI copies the back
  buffer and redraws strips of it about 135 times per frame (frame capture of 24/09), where a dither would feed on itself.

## Coverage

`Shaders_Win32.precomp` (8018 unique pixel shaders): 4907 ps_3_0, all patched and accepted by a native D3D9 HAL device
(the strict validator); 3104 ps_2_0 and 7 ps_1_1 have no `vPos` and stay as they are (offline test,
`scratchpad\dither\dithertest.cpp`). By technique (`techver.pl`): most families have both ps_2_0 and ps_3_0 variants
(InteriorWall 13 / 8, InteriorFloor 26 / 32, Phong 255 / 444, SimSkin 97 / 654, TerrainLight 47 / 95...); ps_2_0 only:
Sky, Sky_Reflection, Ceiling, Rug, Foliage, pool water, SimEyes, FloorThickness, InteriorWall with strobe / black light.
Which variants the game really draws with is read in game: Developer > Debug views > Banding Fix ("Last frame, 3D scene
draws: N dithered, M without a copy").

## Pitfalls

- Never dither a data target (pick IDs, packed depth, light maps): the render-target and depth-test gate is what keeps
  them out. A new Apex pass that draws into the back buffer with the depth test on gets the dither too.
- The first session after installing may stutter a little more while DXVK builds pipelines for the copies (its state
  cache keeps them afterwards) *(expected, not measured)*.
