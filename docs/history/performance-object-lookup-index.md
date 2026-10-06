# Faster Object Lookups: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/performance/object-lookup-index.md](../features/performance/object-lookup-index.md).

### 2026-09-29: validated index (plan candidate C8)

**Context:** plan section 2.2: 0x00C5FA60 is 13.6% of the samples of lot-lighting-dominated hitches (the hot leaf), with
0x00C62D55 / 0x00C60D76 on the stack in 16%; the lookup has 233 direct callers including the script natives.

**Finding:** the Layer mutators are reached only through the Layer vtable, but the WorldManager roots vector is written
by eight non-virtual methods and Layers load their children in 0x00AAA190. Hooking eight direct-called methods by their
entries, and proving there is no other writer, was judged not verifiable enough. Every caller's `visited` argument was
checked site by site to be 0.

**Outcome:** a validated cache with no mutator hooks; shipped experimental, off by default; merged with C6 in v1.5.0.

### 2026-10-02: on by default (2.5.5)

**Outcome:** default changed to on. See [group history](performance.md).

### 2026-10-05: object service ID index

**Context:** commit `b0a95b7`. The object service's map by ID (find 0x00939100) walks one of 1033 bucket chains per
lookup, with dozens of objects per chain in a populated world.

**Finding:** the map has exactly one insert (0x00939170) and one erase (0x00938D00), both leaves, and nothing else writes
its count (+0x1028), so an index kept by those two hooks can be checked against the count on every answer.

**Outcome:** `ObjectIdMap`, an open-addressing side index started with Faster Object Lookups; answers only while the
counts match, the first 256 checked against the game's walk. Released in 2.7.0.
