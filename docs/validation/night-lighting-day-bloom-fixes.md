# Daytime bloom and wall bloom fixes: validation

The feature is described in [features/night-lighting/day-bloom-fixes.md](../features/night-lighting/day-bloom-fixes.md).
It must:

- Give every outdoor lamp term the shared daytime curve `LampDay(night)` (3% of the night strength by day, exact at
  night), leave snow on objects to the game at a night level of 0.01 or less, and keep every full-night value bit exact
  (2.12.3; until 2.12.2 fences and outdoor objects were left to the game by day).
- Write the game's own bloom alpha on opaque lit exterior walls, and Apex's brighter lamp term in RGB only.
- Change only the facade's bloom threshold constant, only for the exact shader pairs whose threshold the bytecode proves.
- Never refresh the lighting when *Smooth ground light* is switched.

## Automated tests

| Harness | What it checks | Run | Backend |
|---|---|---|---|
| [`tools/terrain_lighting_test/check.cpp`](../../tools/terrain_lighting_test/README.md) (`Policies`) | `LampDay` floor, exact 1 at night, smoothstep values, monotonic over 1001 levels; `SurfaceLampGain`, `WallLampScale`, `LampScale`, `LotPass` and `SolveLampDay` follow the curve before full night and keep the previous values at night | `run.ps1 -Captures <dir>` (the policy part needs no capture) | None |
| `tools/terrain_lighting_test/run_resource_checks.ps1` | Extracted production `DrawWallGain`, lot branch, `DrawObjectLamp`, `DrawInstanced`, `NativeWallScale` on a fake D3D9: curve strengths, no full-daylight gate, the kept rig x the curve (rgb only) and restored, every constant restored | `run_resource_checks.ps1` | Fake D3D9 |

The room part (`RoofedDay`, `BasisLightHook`, `SweepDayRooms` in `level_light_share.cpp`) has no offline harness.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-04 | fork `integrate/upstream-v2.6.0-stable` | Lighting + Bloom census, bloom alpha captures, F7 captures, EA 1.69.47 | Wall and foundation bloom mask no longer saturated after the alpha pass; the facade's marquee and centre panel without daytime bloom | In game (fork) |

## In-game test plan

1. Day, a cinema/theatre facade. **Expected:** the marquee and the narrow centre panel keep their colour and brightness
   without a bloom halo.
2. Night, the same facade. **Expected:** check whether its normal night bloom is still there (see *Open checks*).
3. Night, a building with lamps on its walls. **Expected:** the brighter wall and foundation light without a white bloom
   wash.
4. Day, fences, snowy fence tops and outdoor objects near switched-off lamps. **Expected:** the game's daylight look.
5. Noon, a lamp left on (the red garden post of 2026-10-12): columns, window frames, story-2 window glass, the lot
   ground, walls, roofs and water near it. **Expected:** no visible lamp colour (3% of night); F7 on a column shows
   *lamp strength* about 0.03 x the night value, or *the game's rig lamp colours x 0.0300*; status *daytime curve:
   outdoor lamps x 0.030*. Interiors lit by indoor lamps unchanged.
6. Dusk, the same lamp. **Expected:** walls, columns, ground and fences brighten together, without one surface jumping
   ahead; window glass and rooms change once the night has settled (log `Daytime curve: N roofed rooms`).
7. Night. **Expected:** identical to 2.12.2 (full-night values are unchanged).
5. Switch *Smooth ground light* off and on. **Expected:** no relighting of rooms, lots or terrain in the log.

## Confirmed in game

- The fork's checks on the EA app build 1.69 (table above).

## Open checks

- The facade at night on Steam: the guard has no night-level test and also matches the night pixel shaders, so its night
  bloom may be suppressed.
- Every item on Steam 1.67.2.
