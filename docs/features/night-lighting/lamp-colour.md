# Lamp colour (stock pink -> warm white)

> **Status in the standalone:** in the v0.1.0 baseline (b84d5f1) exactly as described; only the status text changed later
> (Portuguese in v0.1.0).

> The game's stock street lamps and lot lamps have a pink base colour (1, 0.75, 0.79). It barely shows on green grass but
> turns snow and pale walls pink. Night Lighting intercepts the two places where a light's colour is written and turns
> exactly that stock pink into a warm white (1, 0.80, 0.62) of the same luminance, leaving every other colour (including
> colours picked in Build mode) alone. Status: working. Both build flavours; always installed with Night Lighting (the
> slider at 0 makes it a no-op).

Module: the lamp-colour part of `object_light_bridge.cpp` (`ObjectLightBridge::InstallLampColour`, `TintStockColour`,
`LampColourSet`, `LampColourSetScript`). Related: [objects-and-rigs.md](objects-and-rigs.md),
[snow.md](snow.md), [../../engine/light-objects-and-rigs.md](../../engine/light-objects-and-rigs.md).

## Purpose

- Measured (LightProbe-m12 with snow, m14 without): the lot light map is identical with and without snow, mean
  (0.313 / 0.267 / 0.275); in lamp-lit areas G/R = 0.754 and B/R = 0.793, i.e. the lamps' stock colour (1 / 0.75 / 0.79).
  On white snow the pools of lamp light look pink.
- After the first fix (creation path only), `S3SS_LightDiag-neve-praca.txt` + m17 still showed 56 street lamps (type 11)
  and 46 lot lamps (type 3) with (1 / 0.75 / 0.79): the lamp object's script sets the colour after creation.
- Other stock colours seen in the F8 dumps (G/R, B/R): 0.98/0.85 (warm), 1.01/1.18 (bluish), 0.81/0.56 (orange), 1/1 (white),
  0.99/1.06 (slight lilac). These are not touched.

## User-facing settings

Saved in `Documents\Electronic Arts\The Sims 3\S3SS\S3SS.toml`, table `[patches.NightTerrainRelight]`.

| UI label | TOML key | Type | Default | Range | Notes |
|---|---|---|---|---|---|
| Lamp colour | `luzDasLampadasNatural` | float | `1.0` | 0.0-1.0 | Main options. 0 = the game's pink ("%.2f (pink)" below 0.5), 1 = warm white. Blend factor between the stock colour and warm white. Applies when a save loads (lights are tinted when their colour is written). Reset to defaults: 1.0. |

Passed every frame by the Present hook (`ObjectLightBridge::SetLampTint(luzDasLampadasNatural)`, clamped to 0..1) and once
at install before `InstallLampColour()`.

## How it works

### Where a light's colour lives

| Offset | Meaning |
|---|---|
| `light+0xF0` | base colour (r, g, b) |
| `light+0xE0` | intensity x colour while lit |
| `light+0x10` | intensity |
| `light+0x20` | fade (night fade-in) |
| `light+0x100` | flags (0x01 alive, 0x04 room known, 0x20 lit, 0x40 enabled) |

Both colour writers below fill `+0xF0` and `+0xE0`.

### The two writers (both intercepted)

1. **Creation, from the light definition:** `FUN_006bda90(light, def+0x10)` (thiscall, reads only rgb). It has 7 call
   sites, one per light type constructor. Mapping inferred from the address ranges of the constructors created by the
   light factory `FUN_006ac590` (`re/out/dump/fn/006ac590.c`):

   | Call site | Constructor (factory case) |
   |---|---|
   | `0x006C047D` | `FUN_006c0450`, type 3 "Lighting/BareBulb" |
   | `0x006C051D` | `FUN_006c04f0`, type 0xB (street lamp, "Lighting/BareBulb") |
   | `0x006C05C1` | `FUN_006c0590`, type 5 "Lighting/ShadedLamp" |
   | `0x006C1251` | `FUN_006c1220`, type 6 "Lighting/TubeLight" |
   | `0x006C15D1` | `FUN_006c15a0`, type 9 "Lighting/RectangleAreaLight" |
   | `0x006C1891` | `FUN_006c1860`, type 10 "Lighting/DiscAreaLight" |
   | `0x006C1B11` | `FUN_006c1ae0`, type 4 "Lighting/Spot" |

   (Window lights, types 7 and 8, have no such call.) Each call is redirected to `LampColourSet(light, rgb)`, which copies
   rgb into a local, applies `TintStockColour`, and calls the original `0x006BDA90`.
2. **The lamp object's script:** `FUN_006b0b50(objId, r, g, b)` calls `FUN_006bc3e0(light, rgba)` for every light of the
   object (called by `FUN_006b1850`). Both the stock colour and the colour chosen in Build mode pass here. The call at
   `0x006B0BDE` is redirected to `LampColourSetScript(light, rgba)`: the game passes (r, g, b, r); the buffer must be
   16-byte aligned because `FUN_006bc3e0` reads it with `MOVAPS` (`alignas(16)`, review 25/09 item 2); after tinting, w is
   set back to r; then the original `0x006BC3E0` is called.

Every redirect is a rel32 rewrite of an `E8` call, validated first (`E8` + current target). If any of the 8 sites does not
match, all are restored and the log says `[ObjectLightBridge] Cor das lampadas: chamada nao confere`.

### The transform (`TintStockColour`)

```
t = luzDasLampadasNatural; skip if t <= 0 or r <= 0.05
g' = g / r, b' = b / r; skip unless |g' - 0.75| < 0.03 and |b' - 0.79| < 0.03   (the stock pink only)
lumPink = 0.2126 + 0.7152 g' + 0.0722 b'
lumWarm = 0.2126 + 0.7152 x 0.80 + 0.0722 x 0.62
k = r x lumPink / lumWarm
warm = (k, 0.80 k, 0.62 k)                       ; same luminance as the original
colour += (warm - colour) x t
```

Counter `g_tinted` = lights corrected.

### How the colour is then used

- Terrain light bake (`FUN_00C292B0`): reads the base colour `+0xF0` (`0xC2950F`); the stamp in the world light maps, the
  smoothed maps and the atlas are therefore warm white after a rebuild (see [terrain-relight.md](terrain-relight.md)).
- Object rigs: `vfunc+0x10` records use `F0` (see [objects-and-rigs.md](objects-and-rigs.md); `BoostRec` uses `+0xF0`).
- Roofs, water and per-pixel object/fence lamps: `ReadLamp` uses `F0 x intensity x fade` (`lot_light_bridge.cpp`).
- Room/lot light solve (walls, floors, lot grass): uses the light's own evaluation `vfunc+0x4C`; which colour field it reads
  is not verified here, but the lot map in m12/m14 carried the stock ratios, so it follows the light's colour.

## Files and functions

| File | Function | Role |
|---|---|---|
| `object_light_bridge.cpp` | `InstallLampColour`, `UninstallLampColour`, `SetLampTint`, `LampColourStatus` | install / status |
| | `TintStockColour`, `LampColourSet`, `LampColourSetScript`, `RedirectCall` | transform and thunks |
| `patches/night_terrain_relight_patch.cpp` | `Install` (calls `SetLampTint` then `InstallLampColour`), `Uninstall` (`UninstallLampColour`), Present hook | lifecycle |

## Game addresses and patterns

| Address | What | Verification |
|---|---|---|
| `0x006BDA90` | set light colour from definition, `thiscall(light, const float* rgb)` | 7 call sites validated (`E8` + target) |
| `0x006C047D`, `0x006C051D`, `0x006C05C1`, `0x006C1251`, `0x006C15D1`, `0x006C1891`, `0x006C1B11` | its callers | rel32 rewritten |
| `0x006BC3E0` | script colour setter, `thiscall(light, const float* rgba)` (MOVAPS: 16-byte aligned) | call at `0x006B0BDE` validated |
| `0x006B0B50` | `FUN_006b0b50(objId, r, g, b)` (caller of the above), called by `FUN_006b1850` | RE |

## Interactions

- Every lamp-lit surface of Night Lighting reads the tinted colour; nothing else is needed.
- Colours chosen by the player in Build mode differ from the stock ratios and are left alone (unless the player picks
  exactly the stock pink).
- S3SS has no patch at these sites (PLANO-SEPARACAO.md: "No S3SS byte overlap found" for the object_light_bridge sites).

## Known limitations

- Takes effect when a light's colour is written: loading a save, placing a lamp, the script re-applying a colour. Moving
  the slider does not re-tint lights that already exist; reload the save.
- Only the one stock pink is recognised (tolerance 0.03 on both ratios).
- Uninstall restores the call sites but not the colours already written; they revert only when rewritten (reload).

## Pitfalls and failed approaches

- Hooking only creation (`FUN_006bda90`) was not enough: the script writes the colour afterwards (m17, 25/09 01:40).
- Passing an unaligned stack buffer to `FUN_006bc3e0` (it uses MOVAPS) could crash; fixed with `alignas(16)` (review
  25/09 ~03:30).

## Testing in game

- Load a snowy save at night: the pools of lamp light on snow must look warm white, not pink. Slider at 0 + reload: pink
  again.
- Status (dev, Developer > Status): `Lamp colour: active | lights with corrected colour: N` (N grows while lights are
  created on load).
- F8 (dev): `S3SS_LightDiag.txt` lists each light's colour; stock lamps should show ratios about 0.80 / 0.62.
- Log: `[ObjectLightBridge] Cor das lampadas: instalado (criacao + script)`.

## Open items

- Optional per-type colours (street lamps cooler, garden lamps warmer) are listed as an extra in ROADMAP-NIGHT-REMAKE.md
  (phase 8), not implemented.
