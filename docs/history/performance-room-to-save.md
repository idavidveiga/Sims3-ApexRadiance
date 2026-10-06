# Room to Save: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/performance/room-to-save.md](../features/performance/room-to-save.md).

### 2026-10-05: a reserve of address space for world saves

**Context:** commit `3328a4b`. A memory study (2026-09-30) found that what runs out first in a long session is one large
free block of the 32-bit address space, not memory. Error 12 is the world save (0x00C6D460, called at 0x00AAC320)
failing.

**Finding:** a reserved, uncommitted block costs nothing but address space and can be handed back at the moment the save
needs it; the game's own trim (0x00733E70) frees its idle resource cache without losing anything in use.

**Outcome:** a 128 MB reserve taken at start, let go before every world save and when the largest free block falls under
160 MB, taken again when 640 MB are free; before every save and under 320 MB the game's resource caches are shrunk on
the thread of their per-frame update (its call at 0x007377F7 is wrapped). The same commit limited the top detail class on
every story to the priority lot when *High quality on every lot* is on.

### 2026-10-05: the EA app build

**Context:** commit `733a7c5`.

**Outcome:** the ResourceSystem update call, the shrink of both caches and the world save call get signatures (each
unique on Steam). Steam keeps its fixed addresses and byte checks; other builds check the calls' targets. The EA code is
encrypted on disk, so the signatures there are confirmed by the startup scan's log.

### 2026-10-06: Experimental badge

**Context:** commit `7232c3b`.

**Outcome:** marked Experimental in the menu, still on by default. Released in 2.7.0.
