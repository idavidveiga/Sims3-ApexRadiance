# Banding Fix (scene dither)

> Removes the colour steps (banding) of smooth gradients in the 3D world: lamp pools on walls and floors, room light
> fall-off, shadows. Patch name `SceneDither`, menu Image > Color (the first card) and an Overview row, on by default;
> one setting, Strength (`forca`, 0.5 - 3 steps, default 1). Added 30/09/2026 on the user's report ("the colours are not uniform, mainly where there is light"; an
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
| Strength | `forca` | float | 1.0 | 0.5 - 3 | peak of the triangular grain in 8-bit steps; set per draw in the copy's amount constant |
| Developer > Show surfaces without the fix in magenta | (not saved) | bool | false | | scene draws with no copy drawn flat magenta (a magenta shader of the same version) |

The first build (30/09, 57a0a09d) used a uniform grain of +-0.5 step; the user found it "a little better, not 100%". Since
7d5615d the grain is triangular (TPDF: the inverse CDF of the triangular distribution applied to the IGN value), which
leaves no noise modulation. GPU check (`scratchpad/dither/tpdftest.cpp`): a grey of 20.40 / 100.70 levels comes out as
20.355 / 100.657 on average (20 / 101 without), the level weights follow the triangle (about 1/8, 3/4, 1/8 on a level).
The AO composite adds the same grain (it rounds to 8 bits again).

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
**Open: are the walls ps_2_x?** If so, the next step is a ps_2_x path (a VS copy that passes the screen position).

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
