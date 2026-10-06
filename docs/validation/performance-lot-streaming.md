# Lot Streaming: validation

The feature is described in [features/performance/lot-streaming.md](../features/performance/lot-streaming.md). It must:

- Write `WorldManager+0xDC` (distance) and `+0xE4` (detailed lots) only after reading the expected native value, keep
  them only while Apex still owns them, and restore only the fields it still owns.
- Enable the native Lot LoD transition throttle and set the camera threshold (`+0xEC`) to 5.0, restoring only values
  still equal to what Apex wrote.
- Write the visibility branch (0x74 -> 0xEB) only when it reads 0x74, and restore it only when Apex wrote it.
- Hold the map-view "skip lot streaming" gate (`+0x258`) only while the map is open and 1 s after, then restore it.
- Make no write at all for a part that official Sims3SettingsSetter already handles.
- Keep building and apartment shells, large exterior geometry and flora in the first object window.

## Automated tests

None offline.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-04 | fork `feature/lot-lod-streaming` | Controlled probes, EA 1.69.47.024017 | Distance 200 and 300 cut off at metrics of about 40,000 and 90,000 (squared distance); 16 detailed lots at a dense point that saturated at 8; no crash or runaway allocation at 300 + 16 (private memory up to about 1566 MB, largest free block about 1790 MB) | In game (fork) |
| 2026-10-04 | fork `feature/lot-lod-streaming` | A/B in one session at 300 + 16 | Off: 149.3 transitions a minute, 87 same-lot reversals within 5 s, 48 within 2 s. Smooth lot streaming on: 99.2 a minute, 18 and 3 | In game (fork) |
| 2026-10-06 | `ba456a5` | Offline signature search in Steam TS3W.exe | The six object-throttle addresses found, each unique | None |

## In-game test plan

1. Turn on Extended lot detail at 300 and 16 and pan over a dense neighbourhood. **Expected:** lots keep full detail
   farther away and up to 16 at once; the log shows the values applied.
2. Turn it off. **Expected:** the game's 70 and 8 come back.
3. Smooth lot streaming on, pan quickly. **Expected:** fewer lots switching detail back and forth.
4. Keep lot visibility stable on, rotate the camera in place. **Expected:** no lots loading or unloading from the angle
   alone.
5. Pause lot streaming in map view on, open and close the map. **Expected:** no lot detail streaming while the map is
   open; streaming resumes about 1 s after it closes.
6. With official Sims3SettingsSetter's LotStreamingOptimizations on. **Expected:** each matching row shows "Handled by
   Sims3SettingsSetter" and Apex makes no write.
7. Spread lot objects while loading on, enter a lot. **Expected:** objects appear over a few frames; shells and flora at
   once.

## Confirmed in game

- The fork's controlled probes and the A/B above, on the EA app build 1.69.

## Open checks

- Steam 1.67.2: every part in game; its addresses were found offline (signatures), not confirmed in game.
- Memory use at 300 + 16 in large worlds on Steam.
- Spread lot objects while loading together with lamp edits (lamps arriving one by one relit the rooms).
