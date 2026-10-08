# Native CAS viewport scheduler — implementation and integration

**Status:** native C++20 scheduling core **and** the thread-bound Hair/Hats task lifecycle (`HairGridSession`) are implemented and tested. The latter supports ordered incremental append, category cancellation, delayed selection, terminal grid callbacks and rejection of wrong-thread ticks. **Neither is connected to `CASHair.PopulateTypesGrid(bool)` or the game's `ItemGrid`.** A safe native Mono interpreter and managed-method continuation bridge has not been validated and the feature is **not yet a playable optimization**.

## Source and tests

- `features/cas_catalog_scheduler.h`: standalone `ApexCasSchedule::Scheduler`, no Mono or Sims 3 game APIs.
- `tests/test_cas_catalog_scheduler.cpp`: viewport priority, reverse-scroll prefetch, cancellation, stable logical
  indexes, failed item requeue, cooperative time budget, and bounds.
- `features/cas_hair_grid_session.h`: concrete simulator-thread lifecycle for Hair/Hats, wrapping ordered append and selection/finalization.
- `tests/test_cas_hair_grid_session.cpp`: seven simulated-grid tests including reentrant cancellation, wrong-thread calls, ordered append and deferred selection.

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


**Hair/Hats ordered append contract (native-only, not wired to the game):**

```cpp
ApexCasSchedule::HairGridSession hair;
auto generation = hair.Begin(orderedHairParts.size(), currentlySelectedIndex);
// Only the original CAS simulator thread can tick the session. All callbacks
// must invoke VERIFIED existing game UI operations; no pointers are retained.
auto result = hair.Tick(
    generation, std::chrono::milliseconds(2), 3,
    [&](std::size_t i) -> bool {
        // Future game bridge: append original part i atomically in UI.dll order.
        // Do NOT call arbitrary Mono methods through an unverified ABI.
        return false;
    },
    [&](std::size_t i) -> bool {
        // Restore original selection only when its row exists.
        return false;
    },
    [&]() -> bool {
        // Issue UI.dll's actual final grid/filter notification once.
        return false;
    });
```

This is a **compile-time integration contract**, not a method hook. The three
callbacks are not implemented on EA 1.69. Their correct behavior, managed
object lifetimes and tick scheduling must be validated before calling Begin.

## EA 1.69 Hair/Hats original-order scheduling (phased, not per-preset)

**New candidate for the eventual live Hair/Hats bridge:** 
`features/cas_hair_population_plan.h` (`HairPopulationPlan`).
The game's *original* `CASHair.PopulateTypesGrid(bool)` has two
exception-protected enumerations: featured Store items first; then
`mPartsList`, where `ObjectDesigner.SetCASPart` is followed by
default/preset processing, possible selection changes and per-part
button updates. Its original terminal UI work updates the grid
`Tag`, sort/save/filter state and `mUndoOnDelete`.

The new driver therefore schedules **three phases in original order**:

1. Featured Store items, with the existing Store layout/filter logic
   retained by the future caller.
2. Complete **part groups**, each including the default hair item,
   any extra presets and their associated selection/wardrobe logic.
   No pause is permitted inside the original `AddHairTypeGridItem`
   or between `SetCASPart` and the associated presets.
3. Exactly one finalization step, preserving the original end-of-grid
   tags and buttons; never mark complete before this step succeeds.

The C++ driver uses a bounded number of *groups* per slice, not
per-preset `HairGridSession` work. This is intentional: retaining
`ObjectDesigner` state across unrelated simulator ticks could
corrupt thumbnails or link the wrong preset to an item. The older
`HairGridSession` remains a standalone-tested item scheduler, **not
the chosen canonical Hair/Hats integration approach**.

All work runs on the original simulator thread; generation changes
cancel obsolete work, and a failed group with possible UI changes
aborts the generation rather than duplicating items. Tests:
`tests/test_cas_hair_population_plan.cpp`. The first unit in a
slice may exceed the time budget; smoothness in the game remains
unproven. These operations are callbacks, **not wired into UI.dll**.

The exact original method metadata and hashes are covered by
`tools/verify_hair_ui_contract.py` and documented in
`docs/research/cas-hair-regression-2026-10-08.md`, including the
on-disk encrypted/obfuscated executable limitation.

## Blocking runtime integration

Apex must still independently validate the EA 1.69 native Mono class/method-lookup ABI,
simulator-thread task dispatch and an actual method-level managed continuation. The method is
interpreted as Mono MINT; it must **not** be treated as an x86 native JIT entrypoint.
The branch now includes an offline-tested strict method-discovery adapter and a read-only
incremental finder for `MonoScriptHost::FindClass`, `mono_class_get_methods`, `ProcessTasks`
and `InitHeap` patterns. They are **not wired to a playable hook or a managed replacement**.
A unique entry pattern does not prove calling convention, ownership or safety.

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
