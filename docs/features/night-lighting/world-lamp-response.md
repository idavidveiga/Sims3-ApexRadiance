# World Lamp Response

Published in 2.5.6. The player confirmed the colour response in the
2.5.5-world-lamp-test2 scene. This is qualitative acceptance, not an instrumented
latency/FPS result or confirmation of every world, weather and streaming case.

## Purpose

The F8 captures identify the affected lamps as type 11 with lot ID zero. The
previous ReadLotLamp rejected every zero lot ID, so neither change tracking nor
the terrain bake snapshot could reconcile their changes. ReadLotLamp now admits
world-owned type-11 outdoor lamps in the existing enumeration. Other world light
classes remain excluded. Existing lot-owned lamp rules remain unchanged.

World lamps share snapshot group zero. Observed value edits of existing lamps
request reconciliation, including simultaneous recolouring of many lamps.
Streaming additions/removals remain excluded. A native lit-bit-only dusk/dawn
transition is not classified as a priority world value edit.
The day guard no longer drops a priority world-lamp edit: the night indicator
does not establish that an observed world lamp is off.

## How it works

`WorldLampPolicy::Eligible` requires a live world-owned type-11 lamp, but not the
lot-room-known bit 0x04. Lot-owned types 3..6/11 retain room-known and room-zero
checks. `AcceptEdit` excludes streaming additions/removals from priority edits.
The existing enumeration builds both the current bake snapshot and direct lamp
list; a changed list invalidates the exact-position selection memo.

`NightTerrainRelight::NoteEdit` coalesces edits. After the priority debounce
(80 ms quiet, or 500 ms from the first edit), a pending world edit requests
`ObjectLightBridge::RequestRigRefresh` once, independently of terrain completion.
The existing render-thread refresh invalidates all native rigs, not just those
near the edited lamp. The pending request resets on world change/re-enable.
Terrain reconciliation keeps the existing local queue, limits and full-rebuild
fallback; it does not promise an atomic same-frame update of every surface.

When a locally released chunk finishes through the native LOD path instead of
ChunkRenderThunk, FinishFlight now sends NoteChunkRendered before publishing
completion. This prevents the smoothing cache from relying on round-robin
hash discovery for that completed chunk. Resolution, filters and budgets are
unchanged. The unsuccessful six-second lamp-driven window retry was removed;
the existing startup window checks and indoor room invalidation are retained.

## Pitfalls and failed approaches

- The first world-lamp test removed only the zero-lot exclusion. Its remaining
  room-known gate still rejected captured world flags 0x73/0xF3. Testing only the
  lot ID did not cover the complete production eligibility rule.
- The earlier six-second window retry did not resolve the reported world-post
  delay and was removed. Do not restore it as a terrain/rig response fix.
- The 01:09 F8 identifies the post's two lamps at (881.9,62.2,1250.1) and
  (882.1,64.2,1250.4), with green current colour. F7 shows green direct lamp
  constants, a blue native rig before replacement, and blue road output.
  Direct per-pixel selection alone therefore does not establish correct native
  rigs or terrain maps. No change to lamp strength/falloff was justified by this.

## Code map

| File | Responsibility |
|---|---|
| `features/world_lamp_policy.h` | World eligibility and priority edit guard |
| `features/lot_light_bridge.cpp` | Enumeration, snapshots, diffs and direct lamp memo |
| `patches/night_terrain_relight_patch.cpp` | Debounce, terrain decisions and coalesced rig request |
| `features/terrain_chunk_relight.cpp` | Local queue completion, including alternate LOD notification |
| `features/object_light_bridge.cpp` | Existing native rig refresh on the render thread |
| `tools/world_lamp_test/check.cpp` | Eligibility/guard regression checks |

## Validation and testing

Offline: tools/world_lamp_test/check.cpp checks world eligibility and the edit
guard (58,221 checks), including captured flags and unchanged lot rules.
This does not validate native bake timing or GPU output.
In game: recolour the same world post in daytime and at night, disable/enable it,
pan while editing, then check dusk/dawn and lot streaming. Record F7/F8 and a
session covering the edit and ground response. Verify indoor lights separately.
No FPS gain or instantaneous response is established by the offline tests.
The final Release x86 build and translation checks passed. The player reported
the test2 response as correct before publication. Still measure the global rig
refresh cost and compare repeated edits, streaming, dusk/dawn and indoor lights.

## Private response review (2026-10-04)

A native full terrain rebuild can finish before the 80 ms edit debounce. The
terrain-covered path previously called `FinishEdit`, ending the pending edit
before its independent world-lamp rig request was consumed. Objects could retain
their older native light despite terrain completion. `FinishEdit` now consumes
that request too; the normal debounce uses the same helper. Each pending request
is consumed once, and a world reset still discards the previous world's request.
This changes update correctness, not the lamp colour, gain, falloff or selection.

On world change, the direct lamp pool, selection memo generation, GPU rows and
enumeration scratch are cleared alongside terrain maps and bake snapshots. The
next lamp read is requested immediately. New geometry cannot receive cached
roof/water/object lamp rows from the preceding world while enumeration is pending
or unsuccessful. The current-world animated-lamp count resets too. New lamp
observations still follow the original streaming rules.

Regression checks extract the production edit completion, world-change and lamp
selection functions and substitute engine memory/timing. They cannot establish
visual latency or global rig-refresh cost. The player reported the latest supplied
terrain test build as excellent before this additional review; that is qualitative
acceptance of the tested scene, not validation of these follow-up changes.
