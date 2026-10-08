# Native CAS viewport scheduler — implementation and integration

**Status:** independent C++20 scheduling core implemented and unit-tested. The EA 1.69 Win32 Apex diagnostic build is compiled separately; use [the read-only in-game smoke test](../../validation/cas-ea-169-diagnostic-smoke-test.md) to collect native runtime evidence. The native method bridge to the game's
`UI.dll` is **not implemented or validated**. This is not a ready-to-install optimization.

## Source and tests

- `features/cas_catalog_scheduler.h`: standalone `ApexCasSchedule::Scheduler`, no Mono or Sims 3 game APIs.
- `tests/test_cas_catalog_scheduler.cpp`: viewport priority, reverse-scroll prefetch, cancellation, stable logical
  indexes, failed item requeue, cooperative time budget, and bounds.

Compile/test the standalone core:

```sh
clang++ -std=c++20 -Wall -Wextra -Werror -pedantic -Ifeatures tests/test_cas_catalog_scheduler.cpp -o cas_scheduler_test
./cas_scheduler_test
```

The scheduler was also run through equivalent local C++20 tests during implementation. **This is not an x86
ApexRadiance.asi build**; no The Sims 3 game or Windows Mono ABI test has been run.

## Contract

```cpp
ApexCasSchedule::Scheduler schedule;
auto generation = schedule.Begin(catalogCount);
schedule.SetViewport(firstVisibleIndex, visibleSlotCount, columns, 2);

// Call on the game's UI/simulator thread, not on a worker thread.
// The callback must populate its logical slot without changing list order.
auto result = schedule.RunSlice(
    generation, std::chrono::milliseconds(2), 3,
    [&](ApexCasSchedule::Scheduler::WorkItem item) -> bool {
        // Future validated bridge only: populateLogicalSlot(item.index);
        return false; // placeholder here: do NOT attach this example to the live game
    }
);
```

- `Begin` creates a new generation whenever the age, gender, species, outfit, category, filters or backing
  CASP list changes. Old work is automatically invalidated.
- `SetViewport` reprioritizes the queue after the user scrolls; all indexes stay the original CAS ordering.
- `TakeNext` chooses visible cells first, then a bounded prefetch band in scroll direction, then the rest.
- `Complete` refuses stale generation results; `Requeue` handles temporary failures without removing slots.
- `RunSlice` respects a per-tick budget after each item. One expensive item may exceed the time budget.
- This module does not construct thumbnails, cache drawable handles, manage `WindowBase`, or move any rendering
  work to another thread.

## Blocking runtime integration

Apex must still independently discover and validate the target managed method's native JIT entry point on the
user's TS3 executable; the source packages alone do not disclose its runtime address or calling convention.
A `GameAddress` signature for the interpreter/JIT mechanism is **not** a verified per-method hook.

True visible-first also needs a proven stable-slot / placeholder mechanism for `ItemGrid`: appending a part out
of order would shift selections and break rows. In particular, a C++ queue must not call
`CASClothingCategory.AddGridItem` or `CASHair.AddHairTypeGridItem` at arbitrary positions until stable-slot
replacement is supported.

Before enabling a feature switch, independently verify the installed executable, runtime method signature,
compiler/JIT lifecycle, Detours trampoline, slot replacement, cancellation and third-party CAS mod interactions.
If any step fails, leave the game's original method untouched.

## Strict dependencies

**Only Apex's own native C++ code and the player's legally obtained game binaries may be used.**
No separate method patching framework, no additional script mod and no full replacement `UI.dll`.
