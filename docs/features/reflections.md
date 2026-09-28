# Reflections

> "Reflections" is a section of the Apex tab that groups the pond options of Night Lighting: lamp glints and glow on ponds
> at night and, with Depth Blur on, a screen-space reflection of the shore (trees, houses, lamps) on ponds. It is not a
> separate patch: the settings belong to the Night Lighting patch (`NightTerrainRelight`) and the rendering is Night
> Lighting's lake pass. Status: working. Both build flavours. It is unrelated to S3SS's own "Mirror Reflection Settings"
> patch (mirror objects), described at the end for disambiguation.

Implementation details of the water pass: [night-lighting/water.md](night-lighting/water.md). Related:
[night-lighting/README.md](night-lighting/README.md), [depth-blur.md](depth-blur.md), [edge-smoothing.md](edge-smoothing.md).

## Purpose

The game's pond/lake water reflects only a fixed sky cube and receives no lamp light, so ponds are dark at night and never
show the scenery around them (the ocean has a real planar reflection, lakes do not). The Reflections section exposes the
switch and strengths of the additive/premultiplied pass Night Lighting draws over lake water. It was split out of the Night
Lighting menu so the user finds it as its own feature.

## User-facing settings

UI: Apex tab > "Reflections" (collapsing header, open by default; tooltip: "Ponds glow and reflect nearby lamps at night;
with Depth Blur on, also the scenery on the shore. Part of Sims3 Settings Setter Apex Edition. Credits: @loinyx"). Drawn by
`ApexRenderReflectionsUI()` (declared in `apex_ui.h`, implemented in `patches/night_terrain_relight_patch.cpp`, called from
`gui.cpp` inside `ImGui::PushID("Reflections")`). If the Night Lighting patch is not enabled it shows only
"Needs Night Lighting enabled.".

Saved in `Documents\Electronic Arts\The Sims 3\S3SS\S3SS.toml`, table `[patches.NightTerrainRelight]` (the keys are
registered by `NightTerrainRelightPatch`; never rename them, saved configs use them).

| UI label (Apex Radiance menu, 2026-09-28) | TOML key | Type | Default | Range | Notes |
|---|---|---|---|---|---|
| Night Lights > Water > "Lamps glow on ponds" | `lagosRefletemLampadas` | bool | `true` | - | The lamp glow and glints on ponds. |
| Night Lights > Water > "Glow" | `brilhoNaAgua` | float | `1.0` | 0.1-3.0 | Lamp glow strength (PS c52.x), shown as 10-300%. |
| Effects > Water Reflections (switch + "Brightness") | `reflexoNoLago` | float | `1.0` | 0.0-3.0 | Shore reflection strength (PS c58.x). Switch off = 0 (the last value comes back when it is switched on); the slider shows 5-300%. Needs Night Lighting on (its lake pass draws it) and Depth Blur on (scene depth); the card shows "Needs ..." with a button that turns the missing one on. |

Night Lights > "Reset Night Lights" resets these too (`ResetDefaults`, unchanged). A change calls
`NotifySettingChanged()` (config save); the values are applied live every frame by the Night Lighting Present hook, no
reinstall:
`LotLightBridge::SetWaterFix(g_water || shoreOnly, g_water ? g_waterStrengthSetting : 0, g_waterReflSetting)` with
`shoreOnly = !g_water && g_waterReflSetting > 0 && DepthShare::Texture()`. Since 2026-09-28 the shore reflection no
longer depends on "Lamps glow on ponds": with the glow off, the lake pass still runs (when there is scene depth) with a
lamp strength of 0, so it adds the reflection alone. Before, `lagosRefletemLampadas = false` switched the whole pass,
reflection included, off. It still needs the Night Lighting patch installed: the pass lives in `LotLightBridge`'s draw
hooks, which only Night Lighting registers and feeds (lamp list, night level) and which its `Uninstall` shuts down.

## How it works

1. Night Lighting classifies the lake shaders by exact id (`kLakePs` {1344, `0x4F52846A`}, `kLakeVs` {1088,
   `0x23CCB61B`}, `shader_ids.h`).
2. On each lake draw (`DrawLake`, `lot_light_bridge.cpp`) the game's water is drawn unchanged, then a second pass with
   `water_lamps_ps.hlsl` on the same geometry: 16 lamps within 150 m (glints pow 250 + glow, clamped to 0.8, x "Lamp glow
   on water") and, when the scene depth is available, a screen-space ray march (up to 48 steps, 5 bisection steps) whose
   hit colour comes from the scene copy the game already binds for refraction (`s6`), weighted by fresnel x "Shore
   reflection" x hit confidence x fog. Blend ONE / INVSRCALPHA (premultiplied).
3. Scene depth = Depth Blur's INTZ texture (`DepthShare::Texture()`), used only if the currently bound depth-stencil is
   Depth Blur's surface. That is why the shore reflection needs Depth Blur on, and the game's MSAA ("Edge Smoothing" in the
   game options) off: with MSAA the depth is not shared. Without depth the pass draws the lamp glints only; the game's
   sky reflection stays.

Everything else (constants, rotated-lot projection, depth linearisation, history of failed approaches) is in
[night-lighting/water.md](night-lighting/water.md).

## Files and functions

| File | Symbol | Role |
|---|---|---|
| `apex_ui.h` | `void ApexRenderReflectionsUI();` | declaration for gui.cpp |
| `patches/night_terrain_relight_patch.cpp` | `ApexRenderReflectionsUI`, `NightTerrainRelightPatch::ResetReflectionDefaults`, `Hint`, settings `g_water`, `g_waterStrengthSetting`, `g_waterReflSetting`, Present hook | UI, TOML, live apply |
| `gui.cpp` | Apex tab (`BeginTabItem("Apex")`) | places the section between Night Lighting and Ambient Occlusion (combined build order) |
| `lot_light_bridge.cpp` | `DrawLake`, `EnsureWater`, `SetWaterFix`, `WaterStatus` | rendering |
| `water_lamps_ps.hlsl` / `water_lamps_hlsl.h` | shader | pass |
| `depth_share.h` | `DepthShare::Texture`, `Surface`, `SetInternalPass` | depth provider link |

## Game addresses and patterns

None patched. Shader recognition by exact id; lamp list from `FUN_006acf70` (`0x006ACF70`).

## Interactions

- **Night Lighting**: required (the section is inert without it). Water does not require the "Street lamps light inside
  lots" bridge; the lake branch runs before that gate.
- **Depth Blur**: required for the shore reflection (INTZ). The lake pass marks itself as an internal pass
  (`DepthShare::SetInternalPass`) so Depth Blur's "first UI draw" trigger ignores it.
- **Game MSAA**: must be off for the shore reflection.
- **HDR**: the combined build scaled the lamp glints by the HDR lamp gain; removed in the standalone, see
  [../removed-features.md](../removed-features.md).
- **S3SS "Mirror Reflection Settings"** (`patches/mirror_settings_patch.cpp`, patch name `MirrorSettings`, Patches tab,
  category Graphics, `VERSION_ALL`): a different system. It patches two floats in `.data`, `sfMirrorBaseDistance` and
  `sfMirrorDistanceScale` (base = scale address - 4), found through the pattern
  `F3 0F 10 05 ?? ?? ?? ?? 0F C6 C0 00 0F 59 D8 0F 5C D9` (patternOffset 4, reads the MOVSS operand), validates both
  values are within 0.001..1000, and writes the user's values. Fade formula (from its technicalDetails):
  `clamp01((mirrorSize x distanceScale - distance + baseDistance) / baseDistance)`. Settings in `[patches.MirrorSettings]`:
  `baseDistance` (10.0, 1-200), `distanceScale` (15.0, 0-200). Each mirror is a separate camera render, so larger values
  cost performance. It affects mirror objects only; it does not touch lake water, the Apex pass, or (as far as known,
  unverified) the ocean's planar reflection. No shared code or addresses with Reflections.
- **Ocean planar reflection**: the game's own (1024x1024 render target in the ocean shader's `s6`); not reused (fixed mirror
  at sea level, only rendered when the ocean is visible). Note for camera-voting code (PostScene): the camera
  view-projection block c40..c43 is mirrored in the water reflection pass (NOTAS 28/09).

## Known limitations

- Ponds/lakes with the exact lake shader only; the ocean and swimming pools are not affected.
- Screen-space: off-screen or hidden scenery is not reflected.
- No shore reflection with Depth Blur off or game MSAA on.

## Pitfalls and failed approaches

See [night-lighting/water.md](night-lighting/water.md): white-pink lake from oversized lamp radii, the fixed-distance
reflection, the 40 m fallback guess that painted black patches with MSAA on (m25), rotated-lot projection, Depth Blur
triggered by the pass.

## Testing in game

- Apex tab > Reflections: toggle "Ponds reflect lamps" at night by a pond; with Depth Blur on and game MSAA off, the shore
  appears in the water; set "Shore reflection" to 0 and only the lamp glints remain.
- Dev status line (Night Lighting > Developer > Status): `Water: water: active | draws with reflections: N`.
- Log: `[LotLightBridge] Agua: ativo`.

## Open items

- Depth with MSAA on (resolve or redraw depth).
- Decide in the standalone whether these keys stay in the Night Lighting table or move to their own section (currently
  `[patches.NightTerrainRelight]`).
