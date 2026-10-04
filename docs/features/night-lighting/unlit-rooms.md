# Rooms at Night (unlit rooms)

Part of Night Lighting (2026-09-29). Source: `features/unlit_rooms.cpp`, `features/unlit_rooms.h`; settings and card in
`patches/night_terrain_relight_patch.cpp` (`DrawRoomsCard`, Lighting > Buildings tab, after the Buildings card).

## Purpose

With every lamp of a room off, the game does not leave the room dark: it lights it with a fixed blue ambient (user,
29/09: "when I turn off every light of a room or lot it still looks very bright inside"; "the original game makes it
super blue"). Sims3SettingsSetter's "Brady Bunch BEGONE" sets that colour (all zero: far too dark, says the user) and
its `disableFillLights` removes the fill light on furniture. This card controls both, in between.

## User-facing settings

`[patches.NightTerrainRelight]` in `ApexRadiance.toml`.

| UI label (Lighting > Buildings > Rooms at Night) | TOML key | Type | Default | Range | Notes |
|---|---|---|---|---|---|
| Adjust the background light | `comodosEscurosSemLuz` | bool | `true` | - | Off: the code reads the game's colours again (the patches are removed). |
| Brightness | `luzQueSobraNosComodos` | float | `0.35` | 0.1..0.8 (10–80%) | Share of the game's unlit-room colour, on walls, floors and furniture (its [NoLight] and fill rig lights and the ambient cube, x night level). |
| Blue tint | `azulNosComodos` | float | `0.0` | 0..1 | 0 = a grey of the same luminance, 1 = the game's blue. Since 30/09 the same on furniture (its [NoLight] and fill rig lights and the ambient cube, x night level), no longer scaled by "On furniture". |
| (removed 30/09) | `efeitoNosMoveis` | | | | "On furniture", how far furniture followed the Brightness, was removed: furniture now follows Brightness and Blue tint exactly as the walls (see the last section). The key is ignored if still in a file; it had replaced `luzSuaveNosMoveis` earlier the same day. |

Applied live: `UnlitRooms::Set` every frame; 0.6 s after the last change every room of every loaded lot lights again
(`LevelLightShare::RelightAllRooms`) and the rigs in the world light cells gather again
(`ObjectLightBridge::RequestRigRefresh`). Room-mode rigs (furniture inside) keep their fill until they gather again (a
lamp of the room switched, the object moved, the lot loaded). Reset sets the defaults above. The defaults use 35% brightness and neutral (0%) blue tint; the slider bounds are 10–80% for brightness and 0–100% for blue.

## How it works (reverse engineering, TS3W.exe Steam 1.67.2)

- `FUN_006a0f50` (the room's ambient, state 0 of the room solve, called from `FUN_006a18b0`): when the room's light list
  (`room+0xC8..+0xCC`) is empty it sets the normalisation `room+0x160` to 1 and the ambient `room+0x110` and `+0x120` to
  the vector at `0x011D0B60` or `0x011D0B40` (`mov ecx, imm32` at `0x006A0F94` / `0x006A0F9B`, through the identity
  `FUN_00f626f0`); which one depends on `FUN_00c63140(worldMgr, x, z)`: the lot at that place, its type (vfunc+0x40 == 1)
  and its byte `+0x430`. Both vectors are in `.bss` (written at run time; no code writes them by address: the value
  comes from elsewhere, unverified where).
- With lamps, the end of `FUN_006a0f50` calls `FUN_006a00a0` twice (`0x006A13F0`, `0x006A1410`, results in `+0x110` and
  `+0x120`): the same colour (the same two `mov ecx` at `0x006A00C1` / `0x006A00C8`) times `[0x011D0B88]`; when the
  lamps' light sums under `0.0005` (`0x00FE3500`) the room gets that colour alone, else the lamps' light plus the
  shortfall when the colour is brighter. So dim rooms are topped up to the blue too.
- Walls and floors add `light map alpha x room ambient` (the wall PS: `mad r0.xyz, r0.w, c4, r0`), so the colour covers
  the whole unlit room.
- The furniture fill light: `FUN_006ba340` (fills a rig's light slots, from `FUN_006bba50`) calls `FUN_006b7e70` when the
  rig is room-mode (`rig+0x1D4 == 0`) or has flag `0x20` in `rig+0x224`, while the byte `0x01158D5C` is set
  (`cmp byte [0x01158D5C], 0` at `0x006BA57C`; the static is 1 in the file). `FUN_006b7e70` sums the rig's lights into a
  main direction and, when that is strong enough, shifts the slots and adds a light opposite to it with the colour
  `[0x011D0E10]` x the amount (`movaps xmm2, [0x011D0E10]` at `0x006B816A`); that vector is set once to
  `(0.8, 0.8, 1.0, 0.8)` (`0x00F9D514` = 0.8).
- Apex points the 6 reads at its own values (all six or none; the originals are kept and put back when the option goes
  off): the two colours = `light x (grey + blue x (game - grey))` with grey = the colour's Rec. 709 luminance, the fill
  colour the same way from `(0.8, 0.8, 1.0)` x `fill`, and the gate byte = `fill > 0`. The game's colours are read
  through the original pointers every 2 s (a mod that changed them, or all zero: the default `(0.15, 0.15, 0.30)` shown
  by Sims3SettingsSetter; the status says "(default)" then).

## Game addresses and patterns

| Id | Steam | Pattern | Check |
|---|---|---|---|
| `RoomAmbient` | `0x006A0F50` | prologue `55 8B EC 83 E4 F0 81 EC 04 01 00 00 53 56 8B F1 8B 86 C8 00 00 00 3B 86 CC 00 00 00 57 75` | 1 match |
| `UnlitColourA` / `B` | `0x006A0F95` / `0x006A0F9C` | in `RoomAmbient` (0x80): `84 C0 B9 ?? ?? ?? ?? 75 05 B9 ?? ?? ?? ?? E8` +3 / +10 | `B9` before each, same values as the `Dim` pair, A != B |
| `DimAmbient` | `0x006A00A0` | prologue `55 8B EC 83 E4 F0 83 EC 3C 8B C1 8B 50 14 8B 40 10 8B 0D` | 1 match |
| `DimColourA` / `B` | `0x006A00C2` / `0x006A00C9` | in `DimAmbient` (0x40): same pattern | as above |
| `FillGate` | `0x006BA57E` | `80 3D ?? ?? ?? ?? 00 74 26 8B CF E8` +2 | `80 3D` before, `00` after |
| `FillColour` | `0x006B816D` | `0F 28 15 ?? ?? ?? ?? 0F 58 C1 0F 59 C5 0F 57 C9` +3 | `0F 28 15` before, 16-byte aligned target |

## Interactions

- Sims3SettingsSetter "Brady Bunch BEGONE" / `disableFillLights`: with this card on, Apex's values win (the code reads
  Apex's vectors); with it off, whatever S3SS set applies again. If S3SS patched the same six reads after Apex started,
  the patch fails (the bytes are not what Apex saw) and the log says so.
- Light between stories (level-light-share): a room whose own lamps are off but that takes lamps through an opening has
  lamps in its list, so it is lit by them (and topped up by this colour only when they are dim).
- The stacked-rooms ambient merge (level-light-share part 4) merges the colour these functions produced.

## Testing in game

- A room at night, every lamp off: with the card on the room keeps a faint grey light (Light left 35%, Blue 20%); 100%
  / 100% / 100% must look exactly like the game; the switch off must too.
- Development build: Developer > Status "Rooms at night: on | the game's unlit-room colours (...) ... | Apex's (...) ...
  | furniture fill (...) | relights: N". The log line `[UnlitRooms] Ready: unlit-room colours at 0x11d0b60 (...)` shows
  the game's values at start (".. not set yet: default" when still zero).

## Open items

- Where the game writes the two colours (and whether they change with the time of day): read them in game (status).
- Whether room-mode rigs gather again on a relight of their room (the furniture fill after a slider change).

## Applying a change (29/09, second build)

User: "it takes forever to apply", "the controls do not respond", "it took very long for the room on the main lot".
The first build relit every room of every loaded lot after each change (`RelightAllRooms`), one room at a time through
the game's queue. Now:

- The game sets the shader parameter `InteriorBuildingAmbientColor` (id `[0x01158AEC]`) to a pointer to room+0x110
  when it draws a room (0x0069EB70, 0x006A06BF; the parameter entry: +0x0C = the data pointer). A room lit by the unlit
  colour alone holds exactly that colour at +0x110 (and +0x120). `Retint` visits every room of every loaded lot
  (`LevelLightShare::ForEachRoom`, stories -4..7, the room list rebuilt at most every 3 s) and gives each room holding
  one of the colours set before (the game's or Apex's, 12 kept per slot) the new one: walls and floors change at the
  next frame, every 50 ms at most while a slider moves.
- Rooms whose lamps were topped up to the colour (lamps plus the shortfall, FUN_006a00a0) hold a mix only a new solve
  gives: at rest (400 ms after the last change) rooms with lamps and an ambient at most 1.25x the brightest known colour
  are sent to gather again (defer: not while solved).
- Object rigs gather again after each retint (at most 4 per second), at rest, and 1.5 s later (after those solves).
- Status: "changes applied N, rooms given the new colour at once N, rooms topped up from their lamps solved again N".
- Marked Experimental in the menu.

Furniture (user: "the light is not applied to furniture"; F7 Capture_057 on a sofa in an unlit room): the sofa is drawn
by the indoor-object shader (Apex's smooth room light patch, PS 2FE1CD18). Its light is the room map (smoothed) x c20,
an irradiance cube (s0) x c12.w, the vertex lights (v0) and the rig's four directional lights c0..c3 / colours c4..c7
(0.0945 0.104 0.170, 0.0136 grey, 0.042 0.042 0.105, 0.021 0.042 0.063: bluish). The room ambient at +0x110 is not one
of them, so the retint does not reach furniture directly. Which rig light carries the blue (the fill light of
FUN_006b7e70, window lights, the sky) is not verified yet: test with "Soft light on furniture" at 0%.

## The colour as a base under the lamps (29/09, third build)

User: "the controls should set the room's tone also with a lamp on, or turning that lamp off changes the whole room".
FUN_006a00a0 (the top-up; its two calls 0x006A13F0 -> room+0x110 with pow 1, 0x006A1410 -> +0x120 with pow 0) goes
through `BaseHook` while Rooms at Night is on: the unlit colour C (slot by the lot test FUN_00c63140, times the scalar
[0x011D0B88]) is always added to the lamps' ambient instead of only topping it up. lamp' is recovered from the game's
result (sums over x, y, z with weight 1.0 [0x0107A538]; top-up happened iff sum(out) < sum(C), then
sum(lamp') = (sum(out) - sum(C)^2) / (1 - sum(C))). Each room's C and results are kept (`g_baseRooms`), so a slider
change moves lit rooms at once as well (value + new C - old C, only while the room still holds what BaseHook wrote).
Turning it on sends every lit room once (at rest) for a solve with the base; off sends the based rooms back to the
game's top-up. Status: "base under the lamps: on (given in solves N, moved at once N)".

Furniture, captures 061-065 (several objects in the dark room): all drawn by Apex's indoor-object shader (VS 2B1A5700),
with the same three rig lights in every room (directions c0..c2, colours c4 0.0945 0.104 0.170 / c5 0.042 0.042 0.105 /
c6 0.021 0.042 0.063). In that shader the rig-light diffuse was replaced by the smoothed room map (r21 x c20; since 30/09 the brighter of the two), so the
rig colours reach only the specular; the rest of the object light is the irradiance cube s0 x c12.w (0.25) and the
vertex lights v0 (VS). Next: which of the cube and v0 carries the blue, then scale it by the Rooms at Night values for
indoor objects.

## Furniture, all object shaders (30/09, fourth build)

Captures 066-079: the objects that responded to nothing are drawn by the game's own object shaders in room mode
(RigTracker mode 0; most on the upper floor, where Apex's indoor-object shader does not apply because no room light map
with directional maps is found). Their light is the rig: slot 0 the sun or moon (0.0945 0.104 0.170, the same in every
room), the other slots lamps or dim bluish window / sky lights, plus the ambient cube x c12.w and the vertex lights.
`OnDrawInner` (lot_light_bridge.cpp) now wraps every room-mode object draw while Rooms at Night acts on furniture:
`AnalyzeRigPs` (per pixel shader, cached) finds the rig chain c4..c7 and the cube weight; slot 0 and the dim bluish
slots (b > 1.15 r, luma < 0.2) of PS c4..c7 and of the VS vertex lights get `UnlitRooms::FurnitureColour` (the walls'
Brightness and Blue tint, times "On furniture"), the cube weight x `FurnitureAmbient`; the draw is made there and every
constant restored. Lamps keep their colour. Apex's indoor-object shader also tints its cube: after the cube read,
`lrp(tintConst.x, cube, luma(cube))` (PatchIndoorBasis, `tintConst` set every draw, 1 = unchanged).
The game's own shaders get the same tint since 30/09 (user: "the only thing missing on the furniture that works is the blue
tint"): the guard draws them with a copy made once per shader by `ShaderPatches::PatchCubeTint` (after the cube read whose
rgb is weighted by c.w: `dp3 Tmp.x, cube, cL(luma)`, `lrp cube.xyz, cT.x, cube, Tmp.x`, cT / cL above the shader's own
constants), with cT.x = `FurnitureTint()`; skipped while the tint is 1. Offline check over the captured shaders: 6 of 6
with an ambient cube weight patched, all valid (scratch test, not in the repo). Status (ObjectStatus): "Rooms at Night on
furniture: N draws (M with the blue tint in the game's shader)". Not tested in game yet.
Limits: needs RigTracker (Doors and windows stay lit) and the
lot light bridge (Street lamps light lots) running.

## Furniture Blue tint, final (30/09, second multi-agent study; not tested in game yet)

User: "the Blue tint still does not work on furniture". Findings (captures 079-094, decompile, verified by two adversarial
verifiers):
- The blue on furniture at night is only the game's three [NoLight] rig lights (CustomLightRigging.ini, added by
  `0x006BB3E0` to room-mode rigs, x0.21 in the captured rooms) and the fill light (`0x006B7E70`, (0.8 0.8 1.0), slot 1,
  w = 0.8 x strength). The ambient cube of room-mode objects is CASDiffuseProbe, flat grey (0.1935 0.2004 0.1935), so the
  cube tint (PatchCubeTint copy, path A tintConst) had nothing to act on. The walls' colour (room+0x110) never reaches the
  furniture shaders; v0 = 0 and fog is black in every captured furniture draw; the room light map term is lamp light only.
- Room-mode rigs have no sun (`0x006BBDE0`: start slot = (mode == 1)): slot 0 is the strongest room light, and the guard
  used to treat it as the sun, dimming and greying a lamp there (087: the red lamp at (1.79 0 0)).
- The furniture tint was multiplied by "On furniture" (at 57.9% the slider moved furniture only between 0.42 and 1).
- Path A (Apex's indoor-object shader) replaced the whole rig diffuse by the basis light (lamp light only), so unlit
  furniture there had only the grey cube (b/r 1.000 at Blue 0% and 100%; the installed 02:59 asi emits only
  'mul D, Acc, cStr.x').
- Matte furniture shaders (dp3_sat between the chain steps, e.g. EB0C8C35 / 25F66827) were not recognised by AnalyzeRigPs.

Changes:
- `FurnitureTintNow() = 1 + (Blue - 1) x night`; "On furniture" scales the brightness only (`FurnitureEffect`);
  `FurnitureActive` = night and (brightness effect or Blue < 100%).
- `IsUnlitLight(colour, dir)`: an unlit-room light is the fill (w > 0) or one of the [NoLight] lights (by direction, within
  1e-3; PS c0..c3 for c4..c7, VS vl-4.. for the vertex lights). Exact tests only (final review): a colour fallback ("dim
  and bluish") also took blue, purple and dim cool-white lamps (greyed on furniture, and counted twice on path A); over
  every room-mode draw of captures 075-094 it was never needed (fill by w 18 slots, [NoLight] by direction 46).
  `FurnitureColour(rgb, dir)` turns only those: lamps keep colour and strength in every slot. The guard reads PS c0..c7.
- Path A: PatchIndoorBasis points the rig diffuse chain's c4..c7 at 4 new constants (`diffuseConst` = cS+7..cS+10) and ends
  with 'mad D, Acc, cStr.x, D'. DrawIndoorObject fills them with the rig's unlit-room lights (lamp slots 0), so path-A
  furniture = the unlit-room lights (turned by Brightness and Blue) + the basis light (lamps per pixel, as at 02:59); the
  specular chain keeps c4..c7 (lamp highlights as before) and the vertex lights stay zeroed (overflow lamps are in the
  basis maps). A first try, max(rig diffuse, basis), was rejected by both verifiers: it brought the per-object rig lamp
  back next to lamps (HDR 1.79 against the 8-bit basis maps).
- AnalyzeRigPs follows the chain through its accumulator when the four steps are not consecutive (strict: unswizzled
  colour constant, replicated multiplier, unswizzled accumulator; census 60 of 628 game shaders, all real chains).
- BicubicSetup's first instruction read two constant registers (native D3D9 refuses that): split into mul + add.
- Menu: Blue tint "0% is neutral grey, 100% is the game's blue, on walls and furniture"; On furniture "How much furniture
  follows the Brightness; 0% keeps the game's" (PT/ES/FR updated).
Expected: at Blue 0% unlit furniture light is exactly grey, at 100% the game's; furniture follows less strongly than the
walls (the environment specular, DefaultSpecProbe, is ~25-35% of its blue channel and is left alone). Rooms whose rigs
gathered enough lamp light have no [NoLight] slots, so their furniture has no game blue for the slider to change.

## Furniture in dark rooms at any hour (30/09 morning; not tested in game yet)

User (a session at dawn / by day, night level 0 to 0.33 in the log): "sometimes the brightness stops working on the
objects" while walls follow it. The furniture part was scaled by the night level. Now the guard reads the rig first
(PS c0..c7) and, when it holds a [NoLight] light with luma > 0.01 (`IsDarkRoomLight`: the game adds those only to a dark
room, FUN_006bb3e0), sets `SetDrawDark(true)` for that draw: `NightNow()` is then 1, so Brightness and Blue tint act fully
on furniture in a dark room at any hour; lit rooms by day keep the game's look (no [NoLight], night level ~0). The F6
recorder now adds a line every 100 ms: room-mode draws (Rooms at Night not acting / no rig chain / turned / Apex
indoor-object shader), rig slots turned and lamps kept, whether it acts, the brightness and blue values, and the night
level.

## Brightness fixes and the Refresh button (30/09, F6 recording 092629; installed build 871930f4, not tested in game yet)

User: "the first slider leaves the room extremely dark, even at 100%" and "the room's brightness goes into a very high
range if I keep moving it". The room tracer showed two bugs in `RetintUnlit` (the per-room retint while a slider moves):

- **Wrong colour family.** The retint told a room's family (the first colour 0x011D0B60, (0.01 0.01 0.01) with S3SS's
  "Brady Bunch" setting, or the second, (0.15 0.15 0.30)) from the colour the room held. Near Brightness 0 both families
  are almost black and match each other within the tolerance: dragging to 0.001 and back left every unlit room of the
  played lot CF2DEA20 and of lot 51A4D8B0 in the first family, 16x too dark ((0.0019 0.0019 0.0019) at Brightness 0.19
  instead of (0.030 0.030 0.034)); rooms of the first family could go the other way (15-30x too bright). Now the family
  comes from the game's own lot test (`SlotOfRoom`: FUN_00c63140 on the room's lot id +0x10 / +0x14, as the top-up uses,
  cached per lot for 5 s); a room holding any known colour of either family gets its own family's new colour.
- **Lit rooms taken for unlit ones.** A lit room's value (its lamps' share + the base) came within the tolerance of a
  colour set during the drag: rooms of 20 and 29 lamps (lot E4606BD0, story 1 room 14 and story 3 room 8) were retinted as
  unlit, lost their lamps' share and followed the unlit colour from then on (+0x120 left behind). Now only a room with an
  empty light list (or a roofless one, as before) is matched by colour; a room with lamps is moved by `MoveBase` or solved
  again once the change rests.

**Refresh the lighting** row (user: "a button to recalculate these lights when they bug"): at the bottom of the Rooms at
Night card, always shown, with the shortcut chip (Ctrl+Shift+G / 3 / F9 by preset). It calls `NightLighting::RefreshAll`
(terrain, lot stories, every room of every loaded lot, object rigs now and again 2 s later, `UnlitRooms::RigsAgainIn`).
`RequeueAllRooms` now also sends basement stories (-4..-1; it used to stop at 0..7).

Seen in the same recording, left as they are: at the first floor switch 11 furniture parts drew one frame (16 ms) on the
game's shader, because the story's new room light map (326E1F60) was drawn before a basis-reading draw paired it with its
directional maps (RoomMapPadding learns pairings from those draws). 68 of 459 room-mode parts stay on path B (mostly
PS 27F98E68 and 27E3DBB0: vertex-light and normal-mapped variants), 2 untouched.

**One Brightness for walls and furniture (30/09, later the same morning; user: "the furniture brightness and the
brightness itself get in each other's way and bug things, they should be one thing").** The "On furniture" slider
(`efeitoNosMoveis`) is gone: `FurnitureShareNow() = 1 + (Brightness - 1) x night`, `FurnitureActive()` = on and at night
(or in a dark room), `UnlitRooms::Set(on, light, blue)`. The user had it at 38.5% with Brightness 8.6%: furniture kept
x0.65 of the game's unlit-room light while the walls kept x0.09, so furniture glowed in dark rooms. Now both keep x0.09 at
that setting (raise the Brightness for brighter furniture and walls together); at 100% everything is the game's.

**Furniture's ambient cube takes the room's colour (30/09, F7 110-113; built a6a253e2, not installed yet).** User: "the blue
tint again is not applied to almost any furniture" (captures: sofa on Apex's indoor-object shader, rug on the game's shader,
Blue tint 98% then 3.9%). The sofa's turned rig lights did follow the slider ((0.032 0.035 0.057) -> (0.036 0.036 0.037)),
but with every lamp of the room off most of the sofa is lit by the ambient cube alone (the three [NoLight] lights are
directional), and the cube (CASDiffuseProbe) is grey: pulling it towards its grey changed nothing. Walls, floors and the
rug take the room's colour ((0.051 0.051 0.101) at Brightness 33.9%). Both cube patches (PatchIndoorBasis for path A,
PatchCubeTint for path B) now end with `mul cube.xyz, cube, tintConst.yzww`; `UnlitRooms::FurnitureCubeColour` sets
.yzw = the chroma of the game's second unlit colour (0x011D0B40, (0.15 0.15 0.30): x0.93 0.93 1.86, luma 1) by the Blue
tint (1, 1, 1 at 0%, by day or while Rooms at Night does not act). The .yzw MUST be set wherever tintConst is set (0 would
make the cube black). Checked offline over 349 captured shaders (scratch harness like tools\basis_test): 4 PatchIndoorBasis
and 31 PatchCubeTint results, all disassemble, the mul right after the lrp. Path B now takes the tinted copy also when only
the colour differs from 1. Not changed: how bright the cube is (its weight x the Brightness).

**Furniture follows the square root of the Brightness (30/09, user's choice of three).** After the merge, furniture at
Brightness 33.9% kept x0.34 of the game's unlit-room light and a sofa (lit mostly by the grey cube and the three directional
[NoLight] lights) looked far darker than the rug beside it (walls, floors and rugs take the room's colour directly).
`FurnitureShareNow() = 1 + (sqrt(Brightness) - 1) x night`: 100% = the game, 33.9% -> 0.58, 8.6% -> 0.29, 0% -> 0. The
walls keep the Brightness as it is. Built together with the cube colour, not installed yet.

**Automatic refresh after any lighting setting (30/09; built 2f441b8d, not installed yet).** User: "whenever any setting of
the mod's lights changes, do the F9 refresh automatically". `RequestAutoRefresh` (night_terrain_relight_patch.cpp) is called
by `Edit` (every Night Lights card, Rooms at Night, the lamp colour, Reset Night Lights), `ApplyTableLive` when a setting
differed (profiles, looks, undo), `ReinstallNow` and the "Upper floors light the ground" switch (`NightLighting::RefreshSoon`);
the Present hook runs `NightLighting::RefreshAll("a setting changed")` once, 1 s after the last change (a dragged slider keeps
pushing it back; never while a reinstall is due). Same work as the shortcut: terrain rebuild, lot stories, every room of the
loaded lots (basements too), object rigs now and 2 s later.

## Room Synchronisation Candidate (2026-10-01)

`2.5.2-room-sync-test` supersedes the square-root furniture rule above: furniture's background light now uses the same
linear Brightness as room ambient. Day/night interpolation and the dark-room override remain; lamps retain their own
colour and strength. Equal multipliers do not guarantee identical pixel brightness across different materials/shaders.

The 14:06 recording holds room ambient at 0.0317 after Brightness reaches 0.105 (target about 0.0169), until the global
refresh completes. A no-lamp ambient absent from the 64-colour history previously never requested a solve. Unknown or
merged ambient now requests one after a change settles, including when disabling the option. Busy rooms go through the
existing queue/deferred-solve path rather than being retinted in the middle of a solve. Outdoor/roofless rooms are excluded.
Lit-room ownership is validated before the unchanged-base shortcut, so an externally changed result can be rebuilt.

Room enumeration compares the loaded story-manager pointers as well as lot trackers. A topology change discards merged
ambient caches and rearms a settled refresh. A once-per-second known-colour reconciliation catches streamed rooms and rooms
previously busy; unchanged scans do not request object-rig refreshes. World changes clear room-base and lot-family records.

Offline checks in `tools/room_ambient_test` exercise the production policy: linear background factors, finite RGB matching,
unknown/busy-room solve decisions, and manager/lot cache invalidation. These are not in-game integration tests. Floor
switches, initial night loads, open stairwells, lots without lamps, enabling/disabling, and rapid slider drags still require
runtime validation. No new binary hooks, map dimensions, or sampler assumptions were introduced by this candidate.

### Test005 recovery and coordinated controls (2026-10-01)

Recording session 17-58-56 shows some rooms following the slider while connected lit rooms lag. A room skipped in states 1-3 now keeps the recovery pending without invalidating its active solve. Once the target settles, periodic reconciliation retries unknown or unbased idle rooms, once per target and (address, manager, id). Failed queue requests remove their provisional sent record, allowing a later retry. Retarget and world-change reset the sent records. Disabling Rooms at Night retains a base record until its restoring solve can actually be requested; recovery also runs while disabled if work remains.

Reused room addresses with a mismatching manager/id discard their old base record. An unchanged merged base is accepted only when the original source, actual merged ambient and second ambient still match validated cache ownership; external changes retain the native-solve fallback. A failed live movement keeps recovery armed even before the control settles. Connected slider changes stage the whole compatible ambient group instead of writing its members individually (level-light-share.md). No colour-family inference or lamp-contribution guessing is introduced.

Offline production-code harnesses pass the busy-to-idle transition, per-target deduplication, failed queue acknowledgement, identity reuse, stale-record removal, disabling during a deferred change, merged ownership and background deltas. Runtime confirmation remains required; tests use controlled native-state fixtures, not a running TS3W process.

### Release 2.5.3 state
The release carries the test005 recovery path and subsequent performance changes retained in test007/test008. The user's gameplay feedback reported Rooms at Night working well in these tests; this is scenario-specific feedback, not exhaustive game-version coverage. The adjustment row no longer shows Experimental by explicit user request. Brightness, Blue tint, config keys and reset defaults are unchanged.

### Reliability audit (2026-10-03)

The player reported an intermittent response in 2.5.6, then confirmed that both controls worked after restarting with the
new build. `unlit_rooms.cpp`, `room_ambient_policy.h`, `level_light_share.cpp` and `object_light_bridge.cpp` are identical
between the published 2.5.5 and 2.5.6 tags. The audit does not establish which runtime event caused that earlier symptom.
Three pre-existing updater weaknesses were reproduced offline and corrected:

- A lit-room base used to be erased when its restoring solve was requested, before the room queue acknowledged it.
  If queuing failed while the feature was disabled, the next pass no longer knew that this lit room needed restoration.
  The base now survives rejection and is retired only on a successful acknowledgement of the same manager/id. A fresh
  result captured by `NoteBase` cancels retirement, so an older acknowledgement cannot erase the new base.
- The `2e-4` RGB tolerance for recognising owned colours also served as the already-at-target test. Small accepted
  slider steps, particularly with the first lot family's grey `.01` base, could therefore be skipped. The early exit now
  also requires a tight target comparison; the conservative ownership tolerance and native-solve fallback are retained.
- An unset periodic deadline (`0`) was compared as an ordinary tick deadline. Above the signed tick range (about
  24.9 days of Windows uptime), world-reset reconciliation and late-base reads could fail to start. Unset deadlines are
  now explicitly immediately eligible; real deadlines still use wrap-safe signed comparisons.

The maintained harness in `tools/room_ambient_test/unlit_rooms_recovery_test.cpp` includes the actual production updater,
with controlled x86 room buffers, time, native queue outcomes, group service responses and patch-write fixtures. It
reproduced the rejected restoration, small-step and high-uptime failures before their corresponding fixes. After the
fixes, 1,269 checks pass, plus 89 existing policy checks (1,358 total, zero failures). Coverage includes native top-up
fixtures, both lot families and ambient samples, 0%-100% slider sweeps, busy-to-idle recovery, history eviction, repeated
targets, identity reuse, world resets, late game-base initialisation, partial patch rollback, disabled recovery, clock
wrap, daylight/night/dark-room furniture factors, neutral and blue cube colour, and preservation of actual lamp RGB.
An unchanged reconciliation also verifies that it does not request another object-rig refresh.

The connected-group tests cover delegation to the group updater; they do not execute the native group's solves or GPU
draws. Binary hooks, actual lot enumeration, material response, other mods and DXVK still require a running-game test.
No shader, lighting formula, default, setting key or saved-profile layout changed. Test instructions and the reproducible
runner are documented in `tools/room_ambient_test/README.md`. Offline checks and a successful build do not guarantee
every possible runtime state.

### Inherited dark grey base (2026-10-03, running-game inspection)

The installed recovery binary was verified against its build SHA-256. Only one Apex ASI was loaded alongside the
official Sims3SettingsSetter. With Brightness and Blue tint both saved at 1.0, read-only inspection confirmed:

| Colour | Original RGB | RGB through Apex's patched reader |
| --- | --- | --- |
| First family (`0x011D0B60`) | .01, .01, .01 | .01, .01, .01 |
| Second family (`0x011D0B40`) | .01, .01, .01 | .01, .01, .01 |

S3SS's separate `[settings.'BradyBunchBlue RGB']` saves that same grey (.01, .01, .01). This saved debug setting is
independent of `[patches.BradyBunchBegone].enabled`: S3SS applies saved settings when registered, and its Vector3 setter
writes their three RGB components. Disabling BBB therefore does not necessarily restore the game's blue ambient.
Apex currently inherits the original pointers' colour rather than overriding another mod's saved RGB. At 100% it
passes this inherited colour unchanged; Blue tint interpolates between that colour and its own luminance grey, which
is identical when all RGB components are equal. The standard second-family (.15, .15, .30) has luma .16083, about
16.083 times the inherited grey's .01. This is an ambient-component comparison, not a total screen-brightness ratio.

The maintained reference test reproduces the live RGB and fourth components, verifies scaling exactly once, and then
restores the standard second-family blue without promoting the legitimately grey first family. It passes 63 checks
against both v2.5.5 production source and the current source, with identical numerical brightness responses. The full
current suite now passes 1,332 updater checks plus 89 policy checks (1,421 total). The current compatibility behaviour
explains why the slider cannot brighten past the externally darkened baseline or visibly restore blue in that session;
it does not imply that every prior intermittent report has the same cause. No new brightness formula or external
configuration change was made during this inspection.

After the player explicitly requested disabling this S3SS adjustment, only the saved `BradyBunchBlue RGB` table was
removed from the local S3SS TOML. `BradyBunchBegone` remains disabled and every other parsed setting was verified
unchanged; the original config was backed up in folder `336-disable-s3ss-ambient-rgb`. No live memory was changed.
The running session retains the already-applied RGB until restart. Confirmation that native blue is restored on the
next startup and visibly follows the controls is pending. This local fix does not change Apex's compatibility policy.

## Room controls and S3SS compatibility (2026-10-03)

Brightness now has a player-facing range of 10-80%, with the existing 35% default. Registered settings and the menu share those bounds; legacy config/profile values are clamped on load without renaming their keys. Blue tint defaults and resets to 0%; explicit saved values remain valid.

The compatibility action in the Rooms at Night card lets the player explicitly correct this conflict. Enabling the feature does not edit S3SS. On action, official S3SS must be loaded and Apex creates a content-specific backup in its data folder before disabling the saved `settings.BradyBunchBlue RGB` override. Invalid or unsupported values are left untouched. Other TOML values and patch switches are preserved; ordinary standalone sections retain comments and layout. Alternate TOML layouts use semantic formatting. A concurrent config change observed before writing aborts the write. The UI calls the override disabled; technically, Apex removes that saved entry so S3SS falls back to its default room-light colour.

If S3SS already applied that RGB, Apex uses the original blue baseline (0.15, 0.15, 0.30) only for the second room family when its current RGB exactly matches the saved override. Native globals, alpha, and the legitimate first grey family are preserved. Walls and furniture use the same effective baseline. A successful explicit correction updates the current session when Rooms at Night is on; otherwise, the next enable uses the corrected baseline. S3SS reads its default room-light colour after the game restarts. The module scan and config write happen only when the player invokes the action, never during feature activation or per-frame updates.
