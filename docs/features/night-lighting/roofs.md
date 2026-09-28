# Roofs and roof snow

> **Status in the standalone:** in the v0.1.0 baseline (b84d5f1) exactly as described (`roof_ps.hlsl` is byte-identical);
> v0.1.0 has no HDR gain on `c52.x` and compiles the replacements at the first draw (no `PrecreatePs`).

> The game's roof shader has no lamp term at all (sun/moon + sky only), so roofs stay black next to a lit wall at night.
> Night Lighting replaces the summer roof pixel shader with an HLSL copy that adds the 16 most relevant outdoor lamps (and
> 16-tap shadows), and draws an additive lamp pass over snowy roofs. The snowy-roof pixel shader is also used by snow on
> stair tops; those draws are routed to the snow-relief fix instead. Status: working (summer roofs confirmed 24/09 after
> the strength and flicker fixes; snowy roofs installed 25/09, position fix m29). Both build flavours.

Related: [README](README.md), [snow.md](snow.md) (snow lying on objects), [fences.md](fences.md),
[water.md](water.md) (shares the lamp list and radius rule), [objects-and-rigs.md](objects-and-rigs.md) (the newer
per-pixel lamp law), [../../engine/shaders.md](../../engine/shaders.md).

## Purpose

- `LightProbe-telhado` (draw #652, DIP, 2776 triangles): `PS_278ED708` (ps_3_0) + `VS_278F5B10`. Light = sun/moon `c0` with
  a 4-tap shadow (`s5`), specular (pow 90), ambient cube `s1 x c6.x`, a 64x64 L8 mask (`s6`, uv `v5`) multiplying sun and
  sky, sky reflection cube `s0`. No lamp term, no lot/terrain light map, no rig lights. User: the roof "has no reaction to
  light", black next to a sconce that lights the wall. Second roof `LightProbe-telhado2`: same PS (hash 42e2c20d).
- Snowy roof (m23, `LightProbe-telhado-neve`): `PS_2793C3E8` (ps_3_0, 4992 bytes, 4 `rep` loops of noise for the snow
  normal) + `VS_27944BD8` (1344 bytes). Light = `sat(N.L) x c0 x shadow(s5, 4 taps, lrp v2.x) + texCUBE(s0, N) x c5.x`,
  times the mask `r6.w` (s7) and the snowy albedo `r6`; fog with v4. No lamps.

## User-facing settings

Saved in `[patches.NightTerrainRelight]` of `Documents\Electronic Arts\The Sims 3\S3SS\S3SS.toml`.

| UI label | TOML key | Type | Default | Range | Notes |
|---|---|---|---|---|---|
| Roofs receive lamp light | `telhadosComLuz` | bool | `true` | - | Main options. Summer roofs and snowy roofs. Read live every frame (`LotLightBridge::SetRoofFix`). |
| Roof light strength | `forcaNosTelhados` | float | `0.6` | 0.05-2.0 | Advanced > Walls and roofs (disabled when roofs are off). `lampParams.x` (PS c52.x). |

Roof lamps work without "Street lamps light inside lots": the roof branches run before that gate in `OnDrawInner`.
Combined build only: the roof strength was multiplied by the HDR lamp gain; see
[../../removed-features.md](../../removed-features.md).

## How it works

### Lamp list (shared with water)

- Every 20 frames (`LotLightBridge::OnPresent`), when roofs, water or per-pixel object lamps are on, `UpdateLampList`
  enumerates all lights (`FUN_006acf70` with a visitor; 15-byte prologue check) into `g_allLamps`: lights alive (`+0x100 &
  0x01`) and lit (`& 0x20`) that are street lamps (type `+0xB0 == 0xB`) or outdoor (`& 0x04` and room `+0x08 == 0`).
- Per lamp: head `+0x120`; visual radius `R = clamp(1.2 x sqrt(range), 2, 25)` with range = `+0x130` (97, 40, 100...; about
  7-12 m); colour = `F0(+0xF0) x intensity(+0x10) x fade(+0x20)`.
- `SelectLamps(x, z, maxScore)`: score = horizontal distance to the lamp - R; lamps with score > maxScore are ignored; keeps
  the 64 best candidates, then the 16 lowest scores into `g_lampData`: `[0..15]` = (head, R), `[16..31]` = (colour, 0),
  `[32]` = params. Roofs use `maxScore = 80` m, at the roof piece's world translation (VS `c8.w`, `c10.w`), so the choice
  never depends on the camera.

### Summer roofs (`DrawRoof`)

- Recognition by exact ids (`shader_ids.h`): PS `kRoofPs` {1136 bytes, FNV-1a `0x6EC87E3B`} (`PsClass::Roof`) AND VS
  `kRoofVs` {1192, `0x1F851ECB`} (VS class 1). The ids replaced the older `roof_ref.h` byte arrays (the game's bytecode is
  no longer embedded).
- The VS gives: world normal in TEXCOORD0, shadow position TEXCOORD1, fade COLOR1, fog TEXCOORD3, texture uv + world
  xz x 0.5 in TEXCOORD4 (`.xy`, `.zw`), mask uv + world y in TEXCOORD5 (`.xy`, `.w`), view vector TEXCOORD6, top-texture uv
  in COLOR0, camera in VS c11.
- Replacement `roof_ps.hlsl` (embedded as `kRoofHlsl` in `roof_ps_hlsl.h`, compiled at runtime as ps_3_0 with
  `d3dcompiler_47.dll` `D3DCompile` in `CompilePs`, about 353 instructions per the notes) = the game's lighting
  reproduced, plus:
  - 16-tap shadow (4x4 at offsets `(x - 1.5) x c2.y`) instead of 4, faded to 1 by COLOR1;
  - lamps: `p = (uv.z x 2, maskUv.w, uv.w x 2)`; per lamp `w = sat(1 - d^2/(R^2 + 1e-3))`,
    `wrap = sat((N.l/|l| + 0.5) / 1.5)`, `lamps += colour x w^2 x wrap`; `lamps x= c52.x`;
  - `diffuse = (ambient cube x c6.x + sat(N.L) x c0 x shadow + lamps) x mask(s6)`; the rest (albedo `s4 top x s2 x c3`,
    reflection with fresnel `sat((1-N.V)^3 + c7.x)`, specular mask `s3 x c4`, fog lerp and `x c8.x`, alpha
    `sat(lum - c5.x)`) as the game.
- Constants: `c20..c35` lampPos (xyz head, w radius), `c36..c51` lampCol, `c52` (x strength, y count). `DrawRoof` saves
  PS c20..c52 (33 registers), sets the replacement PS and constants, draws, restores.
- Precreated when the game creates the roof PS (`PrecreatePs`).

### Snowy roofs (`DrawRoofSnow`)

- Recognition: PS `kRoofSnowPs` {4992, `0x3CEB025E`} (`PsClass::RoofSnow`), no VS id check, but NOT when the VS is the
  snow-relief class 9 (stair snow, below).
- The game's roof is drawn unchanged, then (if at least one lamp was selected and `|VS c15.x| > 1e-6`) a second pass on the
  same geometry with `roof_snow_lamps_ps.hlsl` (`kRoofSnowLampsHlsl`, ps_3_0): blend ONE/ONE add, `ZWRITEENABLE` off,
  alpha test off, colour write RGB (0x7), separate alpha off. PS constants c20..c53 (34 registers) saved/restored.
- The pass recomputes the game's snowy albedo (first ~20 instructions of the game shader):
  `snow = sat(2 c6.z)`, `cover = sat(1.4 snow)`, `a = albedo(s4) x c3 x top(s6, N.y >= 0 ? COLOR0 : 0)`,
  `b = sat((a.r a.g a.b x 500 + 0.2)(snow + 1) + a)`, `s = snow texture(s3, TEXCOORD2.zw)`, `c = lerp(min(s, b), s, snow)`,
  `albedo = sat((1 - n.w)(c - a) + a)`, `mask = lerp(s7.x, 1, cover)`; output
  `mask x albedo x lamps x c52.x x c8.x x (1 - fog.w)`, same per-lamp law as the summer roof.
- **Position fix (m29).** The first version used TEXCOORD4.zw (world xz x VS `c19.x`); on some roofs `c19 = (0,0,0,0)`, so
  every pixel sat at the world origin while the lamps (PS c20+) were near the roof. Now xz = `COLOR0 x c53.x` where the VS
  writes `COLOR0 = world xz / VS c15.x` (`rcp r0.w, c15.x; mul o9.xy, r2.xzzw, r0.w`, used by the game for the top
  texture) and the C++ copies VS `c15.x` into PS `c53.x`; y = TEXCOORD5.w.

### Stair snow routing

The snow on stair tops (m50/m51, `LightProbe-neve-escada`: `VS_2E036438` / `PS_2E036820`, 500 triangles, 4 `rep` loops of
3D noise with `s1` permutation 256x256 and `s2` gradient 256x1) uses byte-for-byte the SAME pixel shader as snowy roofs
(`PS_2E036820 = PS_2793C3E8 = PS_27D4DAE0 = kRoofSnowPs`, 4992 bytes). Because the PS class was tested before the VS
classes, stair snow went to `DrawRoofSnow`, which reads the lamp positions from the roof VS constants and was wrong on
stairs (first test 25/09 10:43). Now:

- `ShaderPatches::IsSnowReliefVs` (VS class 9): TEXCOORD4 output with `.zw` written by `mul oT4.zw, rW.xyxz, cD.x` where
  `cD.x` is a def equal to 0.5 and `rW.x/.z` come from dp4 with `c8`/`c10`, AND an input `TEXCOORD2` (the snow's base
  position; roofs of the same family have none). Offline: only `VS_2E036438` matched among 161 VS.
- `OnDrawInner`: `if (RoofSnow && !g_curVsIsSnowRelief) DrawRoofSnow` ... later `if (g_curVsIsSnowRelief) DrawSnowRelief`.
- `DrawSnowRelief` patches the PS (`PatchSnowRelief`: after the single `mad rL.xyz, rCube(s0), cK.x, rS` outside any loop,
  add `atlas(v.zw x 2 x cA.xy + cA.zw) x cB.x`), with the fence option/strength; details in [snow.md](snow.md).

## Files and functions

| File | Function / symbol | Role |
|---|---|---|
| `lot_light_bridge.cpp` | `EnsureRoof`, `DrawRoof`, `EnsureRoofSnow`, `DrawRoofSnow`, `SelectLamps`, `ReadLamp`, `UpdateLampList`, `EnumerateLights`, `OnDrawInner` (order), `RoofStatus`, `SetRoofFix`, `PrecreatePs` | dispatch and constants |
| `roof_ps.hlsl` / `roof_ps_hlsl.h` (`kRoofHlsl`) | `main` | summer roof replacement |
| `roof_snow_lamps_ps.hlsl` / `roof_snow_lamps_hlsl.h` (`kRoofSnowLampsHlsl`) | `main` | snowy roof additive pass |
| `shader_ids.h` | `kRoofPs`, `kRoofVs`, `kRoofSnowPs` | exact ids (size + FNV-1a over DWORDs) |
| `shader_patches.cpp` | `IsSnowReliefVs`, `PatchSnowRelief` | stair snow |

The `*_hlsl.h` files are generated from the `.hlsl` files ("// Generated from roof_ps.hlsl", raw string `R"RAW(...)RAW"`);
only the first comment line differs (it names `shader_ids.h` instead of the old `roof_ref.h` / `roof_snow_ref.h`). The
`.hlsl` files are not in the vcxproj; the build uses only the header strings (compiled at runtime). No generator script was
found in the tree: keep the two in sync by hand.

## Game addresses and patterns

No game code is patched. Recognition is by exact shader id (roof PS/VS, snowy roof PS) and by VS pattern (stair snow).
Lamp enumeration: `FUN_006acf70` (`0x006ACF70`, stdcall(visitor), prologue `E8 2B 36 00 00 8B 4C 24 04 51 68 40 CF 6A 00`).

## Shader details

| Register | Summer roof (replacement) | Snowy roof pass |
|---|---|---|
| c0..c8 | game: sun colour, sun dir, shadow size, albedo tint, spec tint, alpha ref, ambient scale, fresnel bias, fog mix | c3 albedo tint, c6.z snow amount, c8.x output scale |
| c20..c35 | lamp head + radius | same |
| c36..c51 | lamp colour | same |
| c52 | x strength, y count | x strength |
| c53 | - | x = VS c15.x |
| samplers | s0 env cube, s1 ambient cube, s2 albedo, s3 spec mask, s4 top, s5 shadow, s6 mask | s3 snow, s4 albedo, s6 top, s7 mask |

## Interactions

- Water uses the same lamp list and radius rule ([water.md](water.md)).
- The lamp law here (radius `clamp(1.2 sqrt(range), 2, 25)`, `(1 - d^2/R^2)^2` x wrap) is NOT the bake-matched law used on
  objects and fences (`W = 0.4 x range`, `min(1, W cos/d^2)`); roadmap phase 3 plans one lamp model for all surfaces.
- Lamp colour: reads the tinted `F0` ([lamp-colour.md](lamp-colour.md)).

## Known limitations

- 16 lamps per roof piece, chosen within 80 m of the piece's origin.
- No wall occlusion for lamps (a lamp behind a chimney still lights the roof).
- Only the exact roof VS/PS ids are handled; other roof variants (if any) stay vanilla (none reported).
- Snowy roof pass is additive: it cannot darken, and lamps are multiplied by the mask and albedo of the game's formula as
  re-implemented (verify if the game's winter roof changes).

## Pitfalls and failed approaches

- First test with strength 1: roof almost white. A dedicated strength was added (first default 0.35, 0.05-2).
- Radius from the light bounds (`+0x134`, ~50 m) plus two lights per street lamp at the same place (`+0x130` = 97 and 40):
  roofs (and the lake) blew out to pinkish white. Fixed with the visual radius from `sqrt(range)`; the roof default then
  rose to 0.6.
- Choosing the 16 lamps nearest to the CAMERA every 20 frames made roofs flicker when zooming; selection is now per draw
  by the roof position (limit 80 m).
- Snowy roof position from TEXCOORD4.zw (VS c19 = 0 on some roofs): m29.
- PS class before VS class sent stair snow to the roof pass (10:43 test).

## Testing in game

- At night, a house with outdoor wall lamps and street lamps nearby: roof tiles near lamps lit, no flicker when zooming or
  rotating. Winter: snow on the roof lit around lamps.
- Status (dev): `Roofs: roofs: fixed | lamps on: N | draws fixed: N | with snow: N` ("waiting" until the first roof draw).
- Log: `[LotLightBridge] Telhados: ativo`, `[LotLightBridge] Telhados com neve: ativo` (or the compile error text).
- F7 on the roof (dev): PS c20+ should hold lamp positions near the roof; for snowy roofs check VS c15.x != 0.

## Open items

- Move roofs to the single lamp law (roadmap phase 3: "Telhados: passam para a mesma lei").
- Occlusion of roof lamps.
