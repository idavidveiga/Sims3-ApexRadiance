> Current RC `2.5.4-rc-edge-only`: the System page is Edge Smoothing only. Display/fluency was retired, including its runtime controllers and vendor SDK headers. Conditional MSAA conflicts remain. Night Lighting uses the approved Ready balance layout: Subtle, Soft and Natural, with explicit Custom status, scope help and choice undo. No setting is automatically changed on load.

# Apex Radiance: developer documentation

Apex Radiance ("Apex Radiance for The Sims 3"; file `ApexRadiance.asi`; by @loinyx; formerly "Sims3 Settings Setter
Apex Edition" / "S3SS Apex", file `S3SSApex.asi`) is a native mod for The Sims 3
(Steam 1.67.2, `TS3W.exe`, 32-bit). It is an ASI plugin loaded by Ultimate ASI Loader. It hooks the game's Direct3D 9
device (the game runs on the official DXVK 3.1.1 `d3d9.dll`) and patches game code in memory. It started as a fork of
sims3fiend's Sims3SettingsSetter (S3SS) and is now a standalone ASI that runs next to an unmodified official S3SS.
Its files live in `Documents\Electronic Arts\The Sims 3\Apex Radiance\` (`ApexRadiance.toml`, `ApexRadiance_LOG.txt`).

These documents are written for future maintainers, especially Claude sessions. With them you should not need to
re-derive anything from the long Portuguese notes. Start with [../CLAUDE.md](../CLAUDE.md), then
[architecture.md](architecture.md) and [workflow.md](workflow.md).

**Code baseline.** Unless stated otherwise, file references point to the frozen combined build
`%USERPROFILE%\Desktop\S3SS-dev\Sims3SettingsSetter\` (git tag `combined-final`, commit 45e36e2). The standalone keeps
the same module and file names (see the standalone section of [architecture.md](architecture.md)). Game addresses are for
`TS3W.exe` 1.67.2 Steam, image base 0x00400000. Anything marked *(unverified)* has not been confirmed at runtime or in
the disassembly.

**Scope decision (2026-09-28).** HDR output, Native HDR and Ambient Occlusion exist in the combined build but are
removed from the standalone. Their findings are kept in [removed-features.md](removed-features.md). Picture filters
stay, as an SDR-only module.

## Contents

### General
| Document | What it covers |
|---|---|
| [features/bug-reports.md](features/bug-reports.md) | Current RC restores the 2.5.3 session/capture/list/help page; current save guards, retry, storage and centered notices remain. Superseded redesign notes are retained as history. |
| [architecture.md](architecture.md) | Loading, D3D9 device hooks, hook registry (priorities, Skip), extra hooks, render callbacks, post-scene trigger chain, INTZ depth share, patch system and TOML settings, logger, build flavours, per-frame flow, threads, the standalone split |
| [workflow.md](workflow.md) | Build commands, install, the user's standing rules, diagnosis with F7/F8/profiler, release process |
| [removed-features.md](removed-features.md) | HDR output, Native HDR, Ambient Occlusion, Smooth Streaming, Script GC Scheduler, Service Frame Budget: what they were, where the code is, findings worth keeping |

### Engine reverse engineering (TS3W.exe)
| Document | What it covers |
|---|---|
| [engine/main-loop-and-services.md](engine/main-loop-and-services.md) | Main loop 0xECA960, ServiceManager, JobManager, ResourceSystem, SimService, TextureCompositor, thread model |
| [engine/lot-loading-and-streaming.md](engine/lot-loading-and-streaming.md) | Lot loading, LOD, world streaming, package IO |
| [engine/terrain-and-light-bake.md](engine/terrain-and-light-bake.md) | Terrain chunks, the light bake FUN_00C292B0, story gate 0xC294D9, flags and countdowns, world atlas |
| [engine/room-light-maps.md](engine/room-light-maps.md) | Room light maps and the lot light solve |
| [engine/light-objects-and-rigs.md](engine/light-objects-and-rigs.md) | Light object layout and classes, light manager, object rigs |
| [engine/shaders.md](engine/shaders.md) | Shaders_Win32.precomp, shader families, constants, LightingTweaks tanh, how Apex identifies shaders |
| [engine/camera-and-map-view.md](engine/camera-and-map-view.md) | Camera, projection, map view 0x73E060 |
| [engine/mono-gc.md](engine/mono-gc.md) | Mono / Boehm GC and the simulation thread |
| [engine/timers-and-sleeps.md](engine/timers-and-sleeps.md) | Clock, sleeps, frame limiter, Smooth Patch sites |
| [engine/game-versions.md](engine/game-versions.md) | Game builds (Steam 1.67.2, EA app 1.69.47), the encrypted EA .text, the signature table of every Night Lights address and how it is resolved at run time |

### Features
| Document | Feature |
|---|---|
| [features/night-lighting/README.md](features/night-lighting/README.md) | **Night Lighting** (`[patches.NightTerrainRelight]`): overview, all settings, module map |
| [features/night-lighting/lot-light-pass.md](features/night-lighting/lot-light-pass.md) | Lot light pass: max(lot map, atlas) on the ground |
| [features/night-lighting/world-atlas-and-smoothed-maps.md](features/night-lighting/world-atlas-and-smoothed-maps.md) | World atlas and smoothed light maps |
| [features/night-lighting/terrain-relight.md](features/night-lighting/terrain-relight.md) | Story gate patch, dusk rebuild, automatic terrain relight reconciliation |
| [features/night-lighting/level-light-share.md](features/night-lighting/level-light-share.md) | Outdoor lamps on every floor, lamp-floor wall test |
| [features/night-lighting/walls.md](features/night-lighting/walls.md) | Exterior walls |
| [features/night-lighting/floors.md](features/night-lighting/floors.md) | Floors |
| [features/night-lighting/roads.md](features/night-lighting/roads.md) | Roads and sidewalks |
| [features/night-lighting/roofs.md](features/night-lighting/roofs.md) | Roofs and roof snow |
| [features/night-lighting/water.md](features/night-lighting/water.md) | Lakes, ponds, ocean, pools |
| [features/night-lighting/foliage.md](features/night-lighting/foliage.md) | Bushes, trees, plants |
| [features/night-lighting/objects-and-rigs.md](features/night-lighting/objects-and-rigs.md) | Objects and rigs: per-pixel lamps, bake-matched falloff, max rule |
| [features/night-lighting/fences.md](features/night-lighting/fences.md) | Fences and railings, per-pixel |
| [features/night-lighting/snow.md](features/night-lighting/snow.md) | Snow on ground, floors, sills, stairs, fence tops |
| [features/night-lighting/lamp-colour.md](features/night-lighting/lamp-colour.md) | Lamp colour |
| [features/reflections.md](features/reflections.md) | Reflections |
| [features/picture-filters.md](features/picture-filters.md) | Picture filters (SDR colour and image controls) |
| [features/edge-smoothing.md](features/edge-smoothing.md) | Edge Smoothing: SMAA 1x and FXAA |
| [features/ambient-occlusion.md](features/ambient-occlusion.md) | Ambient Occlusion (GTAO) |
| [features/banding-fix.md](features/banding-fix.md) | Banding Fix: dither of the scene pixel shaders |
| [features/depth-blur.md](features/depth-blur.md) | Depth Blur (off in map view) |
| [features/frame-profiler.md](features/frame-profiler.md) | Frame Profiler (dev build only): sampling, per-service and per-hook timing, hitches |
| [features/performance.md](features/performance.md) | Performance: Faster Game File Lookups (`ResourceLookupCache`, the FindProvider cache and the package list / database classes behind it) with Remember Missing Files (`ResourceLookupMisses`, absent answers and write epochs) and Faster File Lists (`FileListCache`, GetKeyList 0x4B1AE0), Lot Lighting While Moving (`LotLightingMotion`, the lot lighting budget 0xADB120), Wall Shading While Moving (`WallShadingWhileMoving`, the wall AO step 0x68B810), Faster Texture Compression (`FastTextureCompression`, the CPU DXT1 / DXT5 encoders 0x6152F0 / 0x6154B0 reverse-engineered and rewritten bit-identically) and Faster Cache Compression (`FastCacheCompression`, the RefPack stream write 0x4EC200, compressor and decompressor), Spread New Objects Over Frames (`SceneNodeBudget`, the scene's pending-node drain 0x6E4130) and Faster Object Lookups (`ObjectLookupIndex`, the object tree walk behind 0xC62D40); the shared vtable-slot, entry and call chains with the profiler; offline tests `tools/dxt_test`, `tools/refpack_test` |
| [changes-since-0.1.0.md](changes-since-0.1.0.md) | Lighting changes after v0.1.0 and the re-add order (fences first) |

### Developer tools (dev build only)
| Document | Tool |
|---|---|
| [features/dev-tools/light-probe.md](features/dev-tools/light-probe.md) | Light Probe, Ctrl+Shift+F7 |
| [features/dev-tools/light-diag.md](features/dev-tools/light-diag.md) | Light Diag, Ctrl+Shift+F8 |
| [features/dev-tools/frame-capture.md](features/dev-tools/frame-capture.md) | Frame Capture, Ctrl+Shift+F9 |
| [features/dev-tools/lot-map-probe.md](features/dev-tools/lot-map-probe.md) | Lot Map Probe |
| [features/dev-tools/census.md](features/dev-tools/census.md) | Shader census, false colour, offline coverage tests |

## Primary sources behind these docs

- Code: the combined tree above.
- `%USERPROFILE%\Desktop\S3SS-dev\NOTAS-ILUMINACAO.md`: chronological lab notebook in Portuguese, every capture and
  root cause. Later entries supersede earlier ones.
- `PASSO3-PLANO.md` (per-pixel lamp plan and critique), `ROADMAP-NIGHT-REMAKE.md`, `PLANO-SEPARACAO.md` (standalone split).
- Static RE of TS3W.exe: `re\out` (Ghidra decompile) and the session scratchpad `engine_map\` (call graph, strings,
  service tables, profiler targets).
- `README.md` of the combined build (user-facing feature list).

When these docs and the code disagree, the code is right; fix the doc.

## Release 2.5.3 documentation
The candidate sources incorporate test007/test008 lighting work and the approved grouped Performance menu. See ui.md and features/performance.md for menu organization, features/night-lighting/level-light-share.md and unlit-rooms.md for coordinated room updates, terrain-relight.md for paced terrain updates, and objects-and-rigs.md for the painting correction and its remaining gameplay checks. Public distributions omit Developer tools. Removing Experimental labels was a user decision, not an additional safety or gameplay verdict.

## 2.5.4 hotfix

The hotfix includes the verified summer multi-pass terrain correction for consistent lighting across lot/world boundaries, prioritized visible-lamp activation/colour/intensity changes and bounded visible-lot arrival refreshes. The user accepted the latest private lighting-response build and reported improved perceived performance; measured gameplay latency/FPS were not supplied. See [terrain-relight.md](features/night-lighting/terrain-relight.md#254-hotfix-validation) for validation and limits, [lot-light-pass.md](features/night-lighting/lot-light-pass.md) for the shader variant, [ui.md](ui.md) for page/whole-mod defaults, and [bug-reports.md](features/bug-reports.md) for one-click capture and centered notices. Public binaries exclude Developer tools.

## Local Report interface test (unreleased)

`2.5.4-test-report-ui` implements the complete direct Capture / Saved files workflow in both build flavours. The private Developer workspace is still excluded from the public binary. See [bug-reports.md](features/bug-reports.md#current-local-test-direct-capture-and-saved-files) for the exact controls, file/retry behavior, removal recovery, limitations and gameplay checklist. Published 2.5.4 assets are unchanged. No installation or upload is automatic.

Private RC temporal candidate, 2026-10-02: `2.5.4-rc-temporal-pool-test` retains the report-library RC UI and adds targeted pool jitter protection and phase-aware temporal reprojection. Gameplay validation is pending; not installed or published. See [features/edge-smoothing.md](features/edge-smoothing.md) for evidence, fallback scope, quality tradeoff and checks.

Current private RC: `2.5.4-rc-unified-dev-mode`. Temporal smoothing and the experimental native MSAA combination are removed. High/Ultra color edge detection is retained. Keep the game’s own Edge Smoothing off for the mod’s smoothing and depth-based effects. Private candidate; no publication or installation.

## Unified build and optional developer mode (2026-10-02)

One ASI contains the player features and optional developer tools. Enable developer mode in Settings > Menu after confirmation, then restart the game. Default off. Profiles optionally include Development; the save option is hidden in normal mode and appears when importing a profile containing it. Profiles never start measurements or recordings automatically. See [developer-mode.md](features/developer-mode.md). FXAA is the first smoothing method; Anti-aliasing precedes Window. The current RC is private.

- [Persistent game anti-aliasing warning](ui-aa-compatibility.md): private RC notice, affected effects and inline help.
