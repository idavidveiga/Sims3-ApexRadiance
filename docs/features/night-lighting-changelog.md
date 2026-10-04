# Night Lighting refinement changelog

**Branch:** `feature/lot-lod-streaming`  
**Refinement window:** 2026-10-04  
**Lot-detail baseline used while testing:** distance **300** + maximum detailed lots **16**  
**Game build used for the captured validation:** EA 1.69.47.024017

This document records the current Night Lighting controls and the lighting/bloom corrections made during the 2026-10-04
refinement pass. It is intentionally separate from the general changelog and from the
[Lot Streaming changelog](lot-streaming-changelog.md).

## 1. Goal

The refinement had two requirements:

1. keep the Night Lighting improvements that make lamps affect ground, objects, walls, roofs, water and multiple stories;
2. stop those improvements from creating **daytime light/bloom** or from turning lit exterior walls/foundations into a
   large white bloom mask at night.

The work was therefore done per rendering path. No global bloom threshold was lowered or disabled, and the fixes do not
remove the game's normal night bloom.

## 2. Current user-facing Night Lighting controls

The current UI is divided by the surface or lighting system the setting affects. Values below are the defaults in the
current code.

### 2.1 Lamps / overall look

| Option | Default | Purpose |
|---|---:|---|
| **Light style** — Soft / Natural / Bright | Natural | Changes the related lamp-light strengths together, while each surface can still be fine-tuned in its own tab. |
| **Lamp color** | Warm/natural end of the slider | Adjusts the colour used by stock street lamps. |
| **Own color for lot lamps** | Off | Gives stock lot lamps a separate colour from street lamps. |
| **Lot lamp color** | Natural | Separate lot-lamp colour when the option above is enabled. |
| **Moonlight** | 100% | Controls how strongly the moon lights the world; 100% is the game's value. |

### 2.2 Stories

| Option | Default | Purpose |
|---|---:|---|
| **Outdoor light between floors** | On | Lets outdoor lamps reach stories above and below instead of stopping at one floor. |
| **Seamless walls between floors** | On | Removes a lighting step where exterior wall sections meet at a floor boundary. |
| **Every floor in full detail** | On | Keeps story lighting/detail available while changing floors. |
| **Indoor light between floors** | On | Lets indoor lamps reach through open floors and stairwells. |

### 2.3 Ground & Lots

| Option | Default | Purpose |
|---|---:|---|
| **Street lamps light lots** | On | Lets street-lamp light flow onto lot ground without a hard lot-border cut. |
| **Lot lamps light the street** | On | Lets outdoor lot lamps contribute to nearby world ground/streets. |
| **Smooth ground light** | On | Uses smoothed light maps instead of the game's blocky/speckled lamp-light steps. |
| **Ground brightness** | 100% | Lamp-light strength on grass, lots and patios. |
| **Roads and sidewalks** | 100% | Road/sidewalk lamp brightness relative to surrounding ground. |
| **Street lamp brightness** | 100% | Strength of street lamps in the rebuilt ground-light bake. |
| **Lot lamp brightness** | 100% | Strength of lot lamps in the ground-light bake/pass. |
| **Update at dusk** | On | Rebuilds the ground lighting after the game has switched the night lamps on. |
| **Delay after dusk** | 2.0 s | Waits briefly after dusk so the lamp state is stable before rebuilding. |

### 2.4 Objects

| Option | Default | Purpose |
|---|---:|---|
| **Lamps light objects** | On | Adds the intended lamp contribution to outdoor objects, including objects shadowed from moon/sun light. |
| **Brightness** | 100% | Strength of the extra object lamp-light contribution. |
| **Light stairs, railings, columns** | On | Includes object classes that the stock game often leaves without useful lamp light. |
| **Doors and windows stay lit** | On | Prevents exterior doors/windows from becoming darker than the lit wall/ground around them. |
| **Seamless light on pieces** | On | Gives modular/outdoor pieces a continuous per-pixel ground-light contribution. |
| **Seamless light brightness** | 100% | Strength of that per-pixel contribution. |
| **Fences and stairs catch light** | On | Makes fences, posts, stairs and their snow follow the illuminated ground. |
| **Fence brightness** | 100% | Strength of the fence/stair ground-light contribution. |
| **Smooth indoor light** | On, Experimental | Smooths directional room-light maps used by stairs, curtains and furniture. |

### 2.5 Buildings

| Option | Default | Purpose |
|---|---:|---|
| **Lamps light walls** | On | Makes exterior walls near lamps brighter than the stock game's very dim wall lighting. |
| **Brightness — Walls** | 200% | Strength of the wall lamp term; 100% is the game's dim wall value. |
| **Lamps light roofs** | On | Adds lamp light to roofs, which otherwise have no useful lamp term at night. |
| **Brightness — Roofs** | 60% | Strength of the roof lamp contribution. |

### 2.6 Rooms at Night

| Option | Default | Purpose |
|---|---:|---|
| **Adjust the background light** | On | Replaces the stock strong blue room glow with a controllable background light. |
| **Brightness** | 35% | Background-light intensity on room surfaces/furniture. |
| **Blue tint** | 20% | Keeps only the selected amount of the game's blue indoor tint. |
| **Refresh the lighting** | manual | Re-runs the room/lot/object lighting refresh when a loaded room is visibly stale. |

### 2.7 Water & Snow

| Option | Default | Purpose |
|---|---:|---|
| **Lamps glow on ponds** | On | Adds lamp glow/sparkle to pond water at night. |
| **Glow brightness** | 100% | Strength of the pond lamp glow. |
| **Walked-on sidewalks** | 50% | Controls how much sidewalk shows through the winter snow blend. |

The public labels, descriptions and the newly added Lot Streaming controls are kept in the same EN / PT-BR / ES / FR
translation system as the rest of the tool.

## 3. Day/night isolation fixes

### 3.1 Fence, stair and snow ground-light consumers

The ground-light contribution used by fences/stairs and their snow variants is now gated by the live night level:

- **full day** (`g_night <= 0.01`): Apex does not add the night ground-light term;
- **dawn/dusk**: the added term fades with `g_night`;
- **full night**: the configured strength is unchanged.

This prevents the smoothed night-light atlas from making these object families look artificially emissive in daylight.

### 3.2 Outdoor object draw path

The same rule was applied to the outdoor object ground-light replacement. The smoothed ground map can remain enabled for
terrain/roads/lots during the day while the object-specific night contribution stays neutral.

This was important because disabling **Smooth ground light** globally hid the symptom but also removed the desired
ground-light quality improvement.

### 3.3 ObjectLightBridge pre-draw rig boost

The Lighting + Bloom census proved that some outdoor object draws had **zero Apex draw claims** in daylight while lamps and
signs still bloomed. The source was earlier in the pipeline: `ObjectLightBridge` had already strengthened the object's
light record before the vanilla draw.

The bridge now receives the real night level:

- day: no Apex boost;
- twilight: boost multiplied by the live night blend;
- night: the original configured boost;
- cached object rigs are re-gathered only when the blend moves far enough to matter (about 0.1), plus the day/night
  boundary crossings, instead of every frame.

## 4. Smooth ground light no longer triggers unrelated relighting

**Smooth ground light** is a draw/map filtering choice. It now changes only whether the draw paths consume the
smoothed/raw maps.

It no longer:

- calls the generic full lighting refresh;
- re-solves terrain, lots, rooms or exterior walls;
- requests an object-rig re-gather.

This isolates the switch from unrelated wall/room state and prevents an A/B of ground-map smoothing from changing the
lighting solution itself.

## 5. Exterior walls and foundations: boosted RGB, vanilla bloom

### Problem

The wall option intentionally uses a default strength of **200%** so exterior walls close to lamps do not remain as dim as
the stock game. The stock ExteriorWall shader derives its output alpha (the scene bloom mask) from the final luminance.

Multiplying the wall lamp term therefore had two effects at once:

1. the desired brighter RGB wall lighting;
2. an unintended much larger bloom alpha, which could turn entire walls and foundations white in the bloom pass.

### Correction

For opaque ExteriorWall draws Apex now separates those results:

1. an alpha-only pass writes the **vanilla wall bloom alpha** using the original wall-light scale;
2. the authoritative RGB pass writes the configured Apex wall brightness with alpha disabled.

The alpha-only pass is prevented from writing depth or stencil. Blended/unsupported wall variants deliberately use the
previous one-pass fallback instead of risking a change to their compositing semantics.

The result keeps the intended **200% wall RGB lighting** while preventing that extra gain from widening the bloom mask.

Developer status reports:

- draws where vanilla bloom alpha was preserved;
- fallback wall draws.

## 6. Cinema/theatre facade: daytime bloom only

The remaining daytime bloom was traced with F7/Light Probe instead of changing global bloom.

### 6.1 Main marquee

EA 1.69 capture:

- VS: `BFFCCC56 / 1060`;
- PS: `D5ED0EF3 / 864`;
- outdoor rig mode 2.

The pixel shader writes its bloom alpha from final luminance minus a material threshold. In full daylight only, Apex
temporarily raises that **isolated alpha threshold** for this exact shader pair. RGB, depth and stencil remain the game's
output.

### 6.2 Narrow centre panel

After the main marquee was fixed, one narrow vertical panel still bloomed. The captured night material was:

- VS: `BFFCCC56 / 1060`;
- PS: `36F5E915 / 1296`;
- 2-primitive outdoor-object draw.

Day/night census correlation identified its day counterpart:

- VS: `BFFCCC56 / 1060`;
- PS: `4E570819 / 500`.

The day variant is now handled by the same principle, with additional safeguards:

- full day only;
- exact VS + PS;
- outdoor rig mode 2;
- the bytecode analyzer must prove that the threshold constant is used only by the final luminance-derived alpha write;
- the centre-panel variant is accepted only on a very small draw (no more than 4 primitives).

The captured night shader `36F5E915` is deliberately **not** in the day-guard list, so the theatre keeps its intended
night bloom.

## 7. Bloom safety rule

These corrections do **not** implement a global "disable bloom in daylight" rule.

That is intentional. Bloom is used by many valid TS3 materials. The current policy is:

- neutralise an Apex-added light term in daytime when Apex is the cause;
- preserve the game's original bloom alpha when Apex intentionally increases only RGB;
- for a stock material with a confirmed daytime defect, target the exact proven shader/material path and leave night
  variants untouched.

This keeps unrelated emissive materials and night lighting stable.

## 8. Diagnostics added during the refinement

### Lighting + Bloom census

The developer census records visible draw families for several frames and reports:

- day / twilight / night counts;
- whether Apex claimed the draw;
- shader hashes/sizes;
- rig mode;
- representative position/textures;
- **exact Apex claim source**, such as `ExteriorWallGain`, `OutdoorObjectGroundLight` or
  `CinemaMarqueeDayBloomGuard`.

This allowed a draw that was visually wrong to be separated from an earlier pre-draw rig modification.

### Bloom Alpha Probe

The probe captures the raw scene alpha before the game's bloom composite/UI and writes Day/Twilight/Night PNG + TXT
statistics. It was used to prove:

- the wall/foundation region was writing a very large night bloom mask before the wall-alpha correction;
- the corrected wall/base no longer saturates that region;
- the cinema facade was still writing isolated daytime bloom after the Apex object-day paths had already been neutralised.

### Light Probe / F7

F7 captures every draw covering one selected pixel, with shaders, constants and bound textures. It was used to isolate
the cinema materials without applying a broad object or global-bloom workaround.

## 9. Main implementation areas

| Area | Main code |
|---|---|
| live night-level / settings / UI | `patches/night_terrain_relight_patch.cpp` |
| object rig pre-draw light records | `features/object_light_bridge.cpp/.h` |
| per-draw terrain/lot/object/wall/roof/water handling | `features/lot_light_bridge.cpp/.h` |
| safe shader-pattern analysis/patches | `features/shader_patches.cpp/.h` |
| exact captured shader IDs | `shaders/shader_ids.h` |
| smoothed light maps / atlas | `features/lightmap_smooth.cpp/.h` |
| translations | `i18n/tr_lighting.cpp`, `i18n/tr_menu.cpp`, `i18n/tr_features.cpp` |
| focused subsystem notes | `docs/features/night-lighting/` |

## 10. Current validation target

The final lighting/bloom validation for this pass is deliberately small:

1. **day — cinema/theatre:** main marquee and narrow centre panel keep their visible colour/brightness but do not create
   the unwanted bloom halo;
2. **night — cinema/theatre:** the normal night lamp/bloom materials remain unchanged;
3. **night — previously tested building:** boosted exterior wall/foundation lighting remains visible without the former
   white bloom wash;
4. normal ground/object/roof/water lighting remains unchanged from the approved night behaviour.

If those checks pass, this lighting/bloom refinement is considered complete. Further lighting changes should be treated as
separate features/regressions instead of broadening these guards.

## 11. What was deliberately not changed

- no global bloom threshold;
- no global alpha clamp;
- no change to Advanced Rendering;
- no reduction of the approved night object/roof/water lighting;
- no change to the validated Lot Streaming 300 / 16 values;
- no blanket suppression of emissive materials;
- no modification of the cinema's captured night pixel shaders.
