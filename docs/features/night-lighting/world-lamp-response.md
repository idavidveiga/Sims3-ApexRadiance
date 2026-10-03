# World Lamp Response Test

Build: 2.5.5-world-lamp-test. Runtime validation pending.

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

When a locally released chunk finishes through the native LOD path instead of
ChunkRenderThunk, FinishFlight now sends NoteChunkRendered before publishing
completion. This prevents the smoothing cache from relying on round-robin
hash discovery for that completed chunk. Resolution, filters and budgets are
unchanged. The unsuccessful six-second lamp-driven window retry was removed;
the existing startup window checks and indoor room invalidation are retained.

Follow-up (test2 source, not built): world type-11 lamps do not need the lot-room
known flag (0x04). Captured flags 0x73/0xF3 were rejected by that remaining gate
in test1. Lot-owned lamps retain the existing room-known/outdoor requirements.
F7 shows green direct per-pixel constants alongside a blue native rig on the same
post. A coalesced observed world edit now requests the existing native rig refresh
after edit debounce, independently of terrain completion. It is not repeated
while the same terrain edit waits. This uses the existing global rig invalidation;
its in-game cost must be measured, and it does not guarantee same-frame output.

Offline: tools/world_lamp_test/check.cpp checks world eligibility and the edit
guard (58,221 checks), including captured flags and unchanged lot rules.
This does not validate native bake timing or GPU output.
In game: recolour the same world post in daytime and at night, disable/enable it,
pan while editing, then check dusk/dawn and lot streaming. Record F7/F8 and a
session covering the edit and ground response. Verify indoor lights separately.
No FPS gain or instantaneous response is established by the offline tests.
