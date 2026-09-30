# Ambient Occlusion

> Soft shade where things meet (under furniture, in corners, where walls meet the floor, around houses and trees),
> computed from the scene depth right after the game finishes the 3D scene and before bloom and the UI. GTAO at full
> resolution, deterministic (a still camera never changes it), with a composite that keeps lamp-lit and bright surfaces
> and their colour. Brought back on the user's request on 30/09/2026 after the earlier AO lines were removed (history in
> [../removed-features.md](../removed-features.md)).
> Status: released in 2.1.0 (user in game: "ficou incrível"); off by default. Since 30/09 afternoon no longer flagged
> experimental: the page shows a performance note instead. Patch name `AmbientOcclusion`, menu page Image > Ambient
> Occlusion, settings in `[patches.AmbientOcclusion]`.

## Purpose

The user's verdict on the last AO (28/09): dirty / too dark, weak / barely visible, and dots or bands, both indoors and
outdoors. The study of 30/09 (lab `gtaolab.cpp`, session scratchpad `ao\`) compared the shipped HBAO with a new GTAO on
six saved frames of the game (depth dumps `Documents\...\S3SS\Profundidade\profundidade_N_3840x2160.f32` + colour):

- **Clean:** bit-identical with a still camera; the shift metric (image moved 1-4 px) as good as the old HBAO.
- **Stronger at contact, less spread:** a flat floor or wall gets exactly no shade (the visibility is a ratio to the
  unoccluded arc), so the shaded share of an indoor frame fell from 64% to 46%, all of it where things meet.
- **Composite (d):** a dead zone drops faint shade, a multi-bounce term keeps bright surfaces bright and coloured (no
  grey film), lamp-lit pixels keep part of their light. Bright pixels lose 3.6x less light than with the old composite.

## User-facing settings

| UI label | TOML key | Type | Default | Range | Notes |
|---|---|---|---|---|---|
| (card switch) | `enabled` | bool | false | | experimental |
| Strength | `forca` | float | 1.0 | 0 - 2 | multiplies the contact and large strengths (1 = the lab's set); 0 = no GPU work |
| Advanced > Reach | `alcance` | float | 1.0 | 0.5 - 2 | scales the three radii |
| Advanced > Keep lamp light | `protegerLuz` | float | 0.5 | 0 - 1 | share of light kept by bright pixels (luma 0.35 -> 0.75) |
| Quality (visible) | `qualidade` | enum | 2 (High) | stored 0 Low, 1 Medium, 2 High, 3 Ultra, 4 Very Low | 4 / 6 / 8 / 12 / 2 slices; the menu shows Very Low .. Ultra (`kQualityShown`), stored indices kept from 2.1.0 |
| Advanced > Show the shade alone | (not saved) | bool | false | | the shade in grey (also on the Developer page) |

All live (read every frame). The old combined build's keys (`intensidade`, `raioM`, `visualizar`) are not read.

## How it works (`patches/ambient_occlusion_patch.cpp`)

Runs first in the post-scene chain (`PostScene::kAmbientOcclusion = 10`, before Edge Smoothing 20 and Depth Blur 30),
only when the bound depth-stencil is `DepthShare::Surface()` (the main scene). `Install` asks for the INTZ depth swap
(`DepthShare::Request`) and for the camera (`PostScene::WantCamera`).

Passes per frame (W x H = the back buffer):
1. `StretchRect` of the back buffer into a colour copy.
2. `LinearizePS`: device depth -> 1/z in 1/m (`(A - d) / (near A)`, sky 0) into level 0 of a 9-level R32F pyramid padded
   to a multiple of 256.
3. `DownPS` x 8: each level = the 2x2 average of 1/z (sky left out), rendered into a one-level target and copied in.
4. `GtaoPS` (full resolution, G16R16F out: R = visibility, G = 1/z for the blur). Per pixel: normal from the neighbour with
   the smaller depth step per axis; SLICES slices at angle `(s + b1) pi / SLICES`, 4 geometric steps per side from 2 px
   (at 4K, scaled with the height) to the large radius (at most 30% of the height), each step reads the pyramid
   bilinearly within the nearest level to its spacing minus 2 (sampler s2 MIPFILTER POINT; trilinear until 30/09); step offset `b2` plus a golden-ratio phase per half-slice; `b1`, `b2`
   from a 4x4 Bayer. Two horizons per side from the same samples: contact (0.6 m, strength 1.2) and large (2.0 m near /
   2.5 m far, strength 0.5 near / 0.8 far, only what the contact one does not cover; near -> far between 20 and 40 m).
   XeGTAO falloff (full to 38.5% of R, 0 at R) on a distance whose depth part counts 1.3x. Cosine-weighted arcs with the
   projected normal, exact `acos` (the fast fit biased the result by 0.5%, see Verified). Isolated pixels (both
   neighbours on an axis more than 1% away in depth: leaf edges, thin rails) fade out; the shade fades out 150-400 m.
   Samples off the screen count as sky.
5. `BlurPS` x 4: depth-aware (3% of z) box 0.5 1 1 1 0.5 H and V (contains each Bayer offset once), then tent 1 2 3 2 1.
6. `CompositePS` over the copy, colour write RGB: `v = 1 - sat((1 - ao - 0.05) / 0.95)`, per channel
   `m = MultiBounce(v, min(0.9, colour^2.2))` (Jimenez 2016), `m = lerp(m, 1, sat((luma - 0.35) * 2.5) * keep)`.

Camera: near, A and tanX / tanY from `PostScene` (vertex-constant votes over the first 24 scene draws of each frame, only
while an effect asked for them: the combined build's code, see engine/camera-and-map-view.md); fallback near 0.25,
A 1.00008, `tanY = 1/4.293`.

Resources: pyramid R32F (4K: 3840x2304, 9 levels) + 8 one-level targets, two G16R16F screen targets, a colour copy.
Needs R32F filtering and G16R16F render targets (checked at start), and the game's own Edge Smoothing off (paused while
the back buffer is multisampled). Released at `preReset`, rebuilt at the next frame; rebuilt when the back buffer size
changes without a Reset. GPU cost from timestamp queries (card chip, Developer line). fxc slots: GtaoPS ~505, BlurPS 73,
CompositePS 35, DownPS 25, LinearizePS 8.

## Verified

- **The GPU shader against the CPU lab** (`scratchpad\aonew\gpucheck.cpp`: the patch's HLSL on a D3D9 HAL device, the
  same saved depth and colour, the lab's Gtao + Denoise(1) + Composite(3)): scenes 1, 2, 3, 6: raw AO mean difference
  0.08-0.11 of 255 levels, filtered 0.26-0.28 (G16R16F rounding), composite 0.06-0.09 levels (max 2-4); a second run is
  bit-identical. With the fast acos fit the raw AO was 1.3 levels off (68% of pixels > 1 level): exact `acos` costs 7
  slots.
- Not verified yet: the look in game, the GPU cost at 4K, foliage in the wind, alpha-blended surfaces (glass, particles
  do not write depth: the shade of what is behind shows through), water, thin railings, daytime scenes (the lab frames
  are night scenes, where most pixels are dark or lamp-lit and the composite protects them: the effect is subtle there).

## Plan (stages)

1. **This build:** GTAO + composite (d), experimental, Strength / Reach / Keep lamp light / Quality.
2. **Ambient share:** the Apex-patched shaders write how much of each pixel's light is ambient (a second render target),
   so the AO darkens only ambient light and can be stronger without dirtying lamp-lit surfaces (interior walls, floors,
   furniture first, then terrain, exterior walls, Sims, foliage). The back-buffer alpha is not free (bloom uses it).

## Interactions

- Compare with the game (shortcut) turns it off with Night Lighting, Depth Blur and Edge Smoothing.
- Depth Blur and Reflections read the same INTZ depth; the swap runs while any of them needs it.
- Picture's scene copy is taken after the post-scene effects, so Color filters apply on top of the shade.

## Pitfalls and failed approaches

See [../removed-features.md](../removed-features.md) (Ambient Occlusion): half resolution (the root of every "micro
dots" report), per-frame noise with temporal accumulation (twinkling on leaves), radii in near units (shade breathing with
zoom), HBAO with a plain multiply (dirty and grey, weak once toned down). Do not bring any of those back.

## Cost and quality levels (30/09, `scratchpad\aonew\aotime.cpp`)

The in-game shaders on this machine's GPU (RTX 4070 Ti SUPER, native D3D9, 3840x2160, scenes 1 / 2 / 6 of the lab; in game
DXVK: compare the levels, not the absolute ms). "Image" = mean difference of the final picture from the 2.1.0 High, in
levels of 255; "shift" = AO change when the image moves 1 px (the stability metric of the lab).

| Level | Slices | Total ms | Image vs 2.1.0 High | Shift |
|---|---|---|---|---|
| 2.1.0 High (trilinear) | 8 | 3.9 - 4.3 | 0 | 0.28 - 0.51 |
| Very Low | 2 | 1.3 | 0.10 - 0.22 | 0.57 - 1.07 |
| Low | 4 | 1.8 - 2.0 | 0.08 - 0.17 | 0.39 - 0.73 |
| Medium | 6 | 2.5 - 2.65 | 0.07 - 0.13 | 0.33 - 0.59 |
| High (default) | 8 | 3.0 - 3.2 | 0.03 - 0.05 | 0.28 - 0.50 |
| Ultra | 12 | 4.1 - 4.5 | 0.06 - 0.11 | 0.23 - 0.41 |

Fixed part about 0.55 ms (scene copy + pyramid 0.26, blur 0.2, composite 0.09). Tried and dropped: fewer steps (3: the
look moves 3-4 levels), box-only blur (saves 0.1 ms, less stable), a coarser (mip 1) or finer (mip 3) read level (no
gain), the fast acos (no gain), an R16F copy of the pyramid for the march (no gain). The cost was the trilinear R32F
filtering (two bilinear lookups per tap, 64 taps per pixel), not the arithmetic.
