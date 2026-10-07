# Exterior floor lookup checks

Extracts the lower-wall floor lookup and `LevelForPoint` from production, comparing it with the original lookup from
the v2.10.0 source. Native manager/floor accesses are mocked; the fixture does not run lighting geometry or the game.

Run from an installed MSVC x86 environment or let the runner initialize it:

```powershell
./tools/exterior_lookup_test/run.ps1 -ReferenceSource <v2.10.0-level_light_share.cpp> [-OutDir <scratch-folder>]
```

The reference must be the unmodified v2.10.0 file. The runner fails if extraction boundaries change. Compiler output
goes to the scratch folder; the executable prints results without writing files. Covered cases: repeated alternating
stories/lamps, warm single-story lookup, floor replacement between points, missing manager/floor and recovery, bounded
slot eviction and the existing read-only self-check. Full search counts are workload counts, not gameplay timings.
