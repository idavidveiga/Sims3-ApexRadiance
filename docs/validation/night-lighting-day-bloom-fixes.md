# Daytime bloom and wall bloom fixes: validation

The feature is described in [features/night-lighting/day-bloom-fixes.md](../features/night-lighting/day-bloom-fixes.md).
It must:

- Leave fences, snow on objects and outdoor objects to the game at a night level of 0.01 or less.
- Write the game's own bloom alpha on opaque lit exterior walls, and Apex's brighter lamp term in RGB only.
- Change only the facade's bloom threshold constant, only for the exact shader pairs whose threshold the bytecode proves.
- Never refresh the lighting when *Smooth ground light* is switched.

## Automated tests

None offline.

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
4. Day, fences, snowy fence tops and outdoor objects near lamps. **Expected:** the game's daylight look.
5. Switch *Smooth ground light* off and on. **Expected:** no relighting of rooms, lots or terrain in the log.

## Confirmed in game

- The fork's checks on the EA app build 1.69 (table above).

## Open checks

- The facade at night on Steam: the guard has no night-level test and also matches the night pixel shaders, so its night
  bloom may be suppressed.
- Every item on Steam 1.67.2.
