# Indoor light on multi-story lots: investigation and roadmap (2026-10-01)

> User report: on two-story lots at night the unlit-room correction ("Rooms at Night") and the indoor light between
> stories sometimes fail: a room or a whole story keeps the wrong light, or furniture loses its colours. Leaving and
> entering the lot, or switching floors, sometimes fixes it. Wanted: floor switches stay instant without losing the
> corrections, especially the light between stories.
>
> Method: code and docs only (no game in this session). One first fix, then four agents in parallel (an adversarial
> review of the fix, furniture, the unlit-room colour, the story / floor pairing), then a synthesis where each proposal
> was accepted, narrowed or rejected below. Branch `fix/indoor-light-floors`, based on 2.5.2. **Nothing here was compiled
> or tested in game.**

## 1. The common cause

Several modules keep per-room or per-lamp state keyed by **addresses or by (tracker, story, room id)**, and assume the
key names the same thing for as long as the lot is loaded. It does not:

- a lot whose lighting is built again (camera away and back, a LOD change, a reload) gets **new story managers and new
  rooms**, while its tracker and tree levels stay; its lamps are registered again, often identical;
- freed blocks come back from the allocator, so a new manager, room or lamp can sit at an old address (the docs already
  record a story manager reallocated at the old address, level-light-share.md "Pitfalls").

Each stale entry then answers "nothing changed" for a room that is in fact new, so the room is never sent to gather or
solve again: it misses lamps, keeps a lamp switched off, or keeps an old ambient. A floor switch or a re-entry happens to
send the room through another, unfiltered path (the game's restart at 0x006C7451, a new build), which is why they "fix"
it. Two-story lots are hit more because most of this state exists only for light shared between stories.

## 2. Findings and what was done

| # | Where | Finding | Source | Status |
|---|---|---|---|---|
| 1 | `lamp_mark_filter.cpp` | Marks dropped by (tree level, light) for the whole session, also for rebuilt stories; never cleared on a world change | first study | **Fixed**: manager remembered per lamp (rebuilt story = mark), and a mark is dropped only when the room holds the lamp exactly when the game's gather filter takes it (`LevelLightShare::GameWouldTake`); world change clears it |
| 2 | same | The first fix used "lit" instead of the game's filter: lamps the gather always refuses would mark at every floor switch (the flicker the filter removes); game code ran under the filter's mutex | review | **Fixed** |
| 3 | same | Only the lamp's own room is checked; rooms of another story that took the lamp are not | unlit-room agent | **Fixed (conservative)**: a lamp any room of another story took always marks (`TakenByOtherStory`) |
| 4 | `level_light_share.cpp` `g_depSig` / `g_deps` | Lamp signatures of the rooms giving lamps to another story, keyed by (tracker, story, room), no manager check, never cleared: identical re-registered lamps looked unchanged, so the other story's rooms were never sent | review (best fit for "two-story only") | **Fixed**: manager stored, mismatch = first sight; cleared per lot on rebuild (`ForgetLot`) and on a world change |
| 5 | same, `g_ambOrig` / `g_ambApplied` / `g_ambQueuedAt` | The stacked-room merge averaged a new room with the previous build's values of its members (possibly from before its lamps went off) and took old "applied" values as current | unlit-room agent | **Fixed**: `ForgetLot` on rebuild |
| 6 | same, `g_lots` | Not cleared on a world change: a new world's lot at a reused tracker inherited counts, rooms near openings never sent | review | **Fixed** (cleared on the light tree thread) |
| 7 | same, stale-lamp scan `g_staleSent` | Keyed by room address with a hash of light pointers: a rebuilt room at the same address holding the same switched-off lamp was never sent; a room being solved erased its entry, so it could be re-sent every second | unlit-room agent | **Fixed**: manager + id stored; busy rooms keep their entry |
| 8 | `unlit_rooms.cpp` `g_baseRooms` | A record of the room that had this address before made `MoveBase` return "leave it", so a rebuilt lit room was neither moved nor solved again on a slider change; never cleared on a world change | both | **Fixed**: mismatch erases and falls through to "solve again at rest"; `UnlitRooms::OnWorldChanged` |
| 9 | `object_light_bridge.cpp` / `rig_tracker.cpp` | **Indoor furniture rigs (mode 0) were never gathered again by Apex**: "redo all rigs" (0x006B58F0) walks only the world light cells, and the room-rig regather accepted mode 1 only. Furniture kept the lamps of the moment its rig gathered, so with the lamps off it had no unlit slot for Rooms at Night to turn | furniture agent (best fit for "furniture loses its colours") | **Fixed**: every rig refresh collects the mode-0 rigs bound in the next 2 frames (`RigTracker::CollectRoomRigs`, no cost otherwise) and updates them through 0x006BBF90, 128 per frame. *Unverified*: that 0x006BBF90 runs the room gather for mode 0 as it does for mode 1 (docs: modes 0 and 1 share 0x006BBDE0) |
| 10 | `room_map_padding.cpp` | The pairing of a room light map with its basis maps was throttled per (basis map, shader): a story's floor map and object map share both, so one of them could stay unpaired and its furniture stay on the game's shader | furniture agent | **Fixed**: the light map is part of the key (its sampler learned per shader) |

## 3. Proposals rejected or deferred, and why

- **"Room 0 not holding the lamp: drop the mark anyway"** (review): rejected. It relies on an unverified guess about
  room 0 lists, and a wrong drop is exactly the bug.
- **The first theory as the whole story** (review): accepted that the mark filter alone does not explain "mostly
  two-story". It stays fixed, but findings 4, 5 and 9 are the better fits. The new counters (section 5) tell which one
  catches the bug in game.
- **Manager pointers as the only rebuild test** (review, story agent): a manager can come back at the same address.
  Kept as one signal; the real guards are the room checks (findings 1, 7). The robust answer is the per-story cache of
  section 4.
- **Merge only the lamp share of stacked rooms** (unlit-room agent, finding 5 of its report): agreed in principle (the
  merge scales the absolute unlit colour and mixes a dark room with a lit neighbour), deferred: it changes how atriums
  look and needs an in-game comparison first.
- **Act on mode-1 furniture** (furniture agent): deferred until the F6 mode histogram shows such draws.
- **Pre-seed a rebuilt room's ambient from a cache** (unlit-room agent): deferred. Restoring +0x110 / +0x120 over light
  maps that only a solve fills could show a correct colour on wrong texels; and the room ids of a rebuilt story must be
  shown to be stable first.
- **Record the stale-lamp send only when the queue accepted it** (unlit-room agent): deferred (the visitor does not know
  the queue's answer; small effect).

## 4. Roadmap

**Phase 1, test this branch** (dev build):
1. A two-story lot at night, some rooms with every lamp off: enter, leave, enter again; switch floors several times.
   Expected: rooms, stories and furniture right the first time; no need for "Refresh the lighting".
2. Developer status lines: "Rooms keep their light" (kept / story built again / room not as the lamp needs it),
   "Objects ... indoor rigs gathered again after a refresh", "Stories ... lots rebuilt".
3. Floor switch speed: the F6 recorder should show few re-solves on a switch where nothing changed (the filter still
   keeps most marks). If "room not as the lamp needs it" grows at every switch, report it: the rule is too strict.

**Phase 2, instrumentation** (from the furniture and story agents; small, dev build only):
- F6: a rig mode histogram of object draws (-1 / 0 / 1 / 2) and how many mode-1 rigs hold unlit-room lights.
- F6: per furniture rig, the age of its last gather and its unlit slots; a "stale rig" line when a mode-0 draw has no
  unlit slot while its room's lamps are all off.
- `[furniture] A miss`: a light map that path A could not pair.
- F8: per story, every floor object naming it and which one `LevelFor` chose.

**Phase 3, a per-story cache keyed by stable identities** (story agent's design; the real answer to "instant floor
switch without losing quality"):
- key (lot id `+0xC0/+0xC4`, story), never an address; entry: the story's manager, its own floor object and grid, the
  opening mask with a hash, the rooms near openings, a version;
- validated by ~8 reads per lookup (manager, floor object, grid pointer and size, tile array); a floor switch is then a
  cache hit and no opening mask is rebuilt (today every indoor gather rebuilds one or two full masks, and `OpeningCount`
  rebuilds 7 every 2 s per lot);
- a floor set / remove marks the entry dirty at once (no 1.5 s quiet); a rebuilt story whose mask hash is unchanged
  sends its rooms at once with the known cross lamps, so the first solve after re-entry is already right;
- `Cross` records (lot id, story, version) instead of a raw floor object, so the point test resolves through the cache
  and never counts a moved object as solid floor (today a lamp of another story goes dark through the opening until the
  room gathers again);
- `LotState`, the ambient merge and the lamp signatures keyed by lot id.
- Also: the floor registry should re-append an object reconstructed at an old address and prefer the most recently set
  own floor (story agent finding 2).

**Phase 4, quality**: the stacked-room merge of the lamp share only; the game's other basis-reading shaders (stairs,
instanced objects) given the floor test; mode-1 furniture if phase 2 shows it.
