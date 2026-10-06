# Faster Scripts: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/performance/faster-scripts.md](../features/performance/faster-scripts.md).

### 2026-10-05: NaN tests inline

**Context:** commit `a1de9ab`. The Mono interpreter's floating-point compare and branch handlers call `msvcr80!_isnan`
for both operands before every comparison.

**Finding:** 25 handlers share two block shapes; the test can be done in place with `fucomip st0, st0` and `jnp`, keeping
the registers, the x87 stack and the flags, and the bytes after the old return address.

**Outcome:** the handlers are patched by pattern (any build), with every other thread suspended outside the changed
bytes.

### 2026-10-05: type object cache

**Context:** commit `fc6caf6`. `mono_type_get_object` takes a lock and searches on every call.

**Finding:** the answer for a (domain, type) pair does not change while the domain lives, except for TypeBuilder types
whose answer is the class's `reflection_info`.

**Outcome:** a lock-free cache of the game's own answers, invalidated by `mono_domain_free`, with the first 256 answers
checked against the game.

### 2026-10-06: Experimental badge

**Context:** commit `7232c3b`.

**Outcome:** marked Experimental in the menu, still on by default. Released in 2.7.0.
