# AO receiver checks

Read-only shader-package and native D3D9 checks. Run from the repository root in an x86 VS developer prompt:

```bat
cl /nologo /O2 /EHsc /std:c++20 /Ifeatures /Ishaders tools\ao_receiver_test\ao_receiver_test.cpp features\shader_patches.cpp d3d9.lib d3dcompiler.lib user32.lib /Fe:%TEMP%\ao_receiver_test.exe
%TEMP%\ao_receiver_test.exe "C:\Games\Hydra\The Sims 3\Game\Bin\Shaders_Win32.precomp"
```

The harness reads the package and the actual AO HLSL source. It validates unique Sim material pairs against a native D3D9 HAL device, checks failure leaves input unchanged, and renders SM2/SM3 alpha-tested masks. It also renders the actual composite at 100%, 0% and 50%, separate hair/body intensities, shade caps, transparent-hair coverage and foreground-depth rejection. Shader alpha is preserved for alpha testing; mask depth and coverage use G32R32F. Transparent layered hair remains a screen-composite approximation, not per-layer colour separation. No shader bytecode or screenshots are written.

To regenerate only size/hash identifiers from technique ownership:

```bat
node tools\ao_receiver_test\generate_ids.cjs "C:\Games\Hydra\The Sims 3\Game\Bin\Shaders_Win32.precomp" shaders\sim_receiver_ids.h
```

The generator excludes PS bytecode shared with non-Sim techniques. It stores no game bytecode. Re-run native checks after generation. Exact fingerprints deliberately fail closed on unrecognized variants; they do not prove game coverage.

Shader-generation checks include opaque and blended body/hair under SM2 and SM3, alpha rejection, zero/partial opacity, independent body/hair strength and foreground rejection. Both opaque and blended copies are validated across all 1912 recognized shader pairs. Unsupported layouts leave both inputs unchanged. Recognized SRCALPHA/INVSRCALPHA ADD body overlays use the same coverage target as blended hair; the hair toggle only gates hair draws.

## Production replay order

Run `run_runtime_checks.ps1 -OutDir <scratch>` from PowerShell with the installed x86 Visual Studio toolchain. The runner extracts the exact production receiver block into scratch and compiles it with the production shader patcher. It creates a hidden 16x16 native D3D9 device and classifies synthetic materials by their bytecode, without reading or writing game files. An optional `-SourcePath <source-root>` or patch path checks a baseline with its matching `features/shader_patches.cpp`.

The fixture invokes the Early-hook replay before the original draw, matching the dispatcher's actual order. It checks blended body alpha with the hair option off, fresh LESS geometry, equal-depth non-Sim rejection, foreground rejection, unchanged original colour/depth, state/shader/target/viewport restoration, disabled controls, unsupported blend/viewport skips and an injected mask-shader setter failure. Replays preserve the original depth comparison; widening LESS to LESSEQUAL would tag coplanar fragments that the original colour draw rejects.

The nearest-depth revision also tests two overlapping triangles in one draw, both triangle orders and both body/hair classes, mixed classes across draws, alpha rejection, RGB-disabled/alpha-only skips and the scissor RECT. Opaque mask RG now holds nearest body/hair depths, with clear value 1 and MIN blending; transparent RG remains signed depth/alpha coverage. The native fixture verifies G32R32F post-pixel blending capability and renders a MIN operation. Snapshot checks include SRCBLEND, DESTBLEND and BLENDOP. The composite retains original AO when both opaque classes match final depth within its tolerance.

2026-10-04 coverage fix: the expanded shader/composite suite passes 9388 checks, with 1796 opaque and 1796 blended pairs accepted and 116 layouts safely refused in each path. The old generator accepted only 112 blended pairs and failed 8 native render assertions. The extracted production replay passes 66 checks; its unchanged baseline fails the blended-body and coplanar-LESS cases. These fixtures validate the demonstrated defects, not the specific lip material in a screenshot or real-world frame times. Confirm the reported mouth region in game and under DXVK.

2026-10-04 nearest-depth revision: 9400 shader/composite checks and 223 extracted production replay checks pass with zero failures on native D3D9 and the game's DXVK 3.1.1 x86 DLL. The fixture tests all eight depth comparisons: opaque LESS/LESSEQUAL are accepted, other opaque comparisons retain original AO, and blended receivers keep their existing selection. The preceding replay stores the farther depth when near geometry precedes far geometry in the same draw; the original colour draw still selects the nearest geometry. The earlier baseline scissor snapshot also detects rectangle loss, and colour-disabled queries previously received blended receiver replays. These failures justify the corrections independently of the screenshot's lip material. Re-test the same Sim in game; no FPS or mouth-coverage guarantee follows from these fixtures.

Known limits: nonstandard transparency equations, unknown/custom shader overrides and refused interpolator layouts retain original shading. Devices without G32R32F post-pixel blending retain original scene AO with the existing warning. Coplanar opaque body/hair classes are ambiguous and also keep original AO. The shared blended target retains the last accepted layer, not exact multilayer colour separation. Replays add GPU/CPU cost; query copies without RGB writes are excluded, but native in-game query behavior, DXVK and gameplay cost still require validation before release.
