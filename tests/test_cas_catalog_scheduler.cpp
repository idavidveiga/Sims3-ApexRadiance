// Standalone native-only scheduler tests. No EA game binaries or external patchers required.
// From repository root: clang++ -std=c++20 -Wall -Wextra -Werror -Ifeatures tests/test_cas_catalog_scheduler.cpp -o scheduler_test
#include "cas_catalog_scheduler.h"
#include <cassert>
#include <chrono>
#include <iostream>
#include <set>
using namespace ApexCasSchedule;
using namespace std::chrono_literals;

static void TestVisibleFirstAndScrollDirection() {
    Scheduler scheduler;
    auto generation = scheduler.Begin(100);
    scheduler.SetViewport(40, 6, 3, 2);
    for (std::size_t index = 40; index < 52; ++index) {
        auto work = scheduler.TakeNext();
        assert(work && work->index == index && work->generation == generation);
        assert(scheduler.Complete(*work));
    }
    scheduler.SetViewport(31, 6, 3, 2);
    for (std::size_t index = 31; index < 37; ++index) {
        auto work = scheduler.TakeNext();
        assert(work && work->index == index);
        assert(scheduler.Complete(*work));
    }
    for (std::size_t index = 25; index < 31; ++index) {
        auto work = scheduler.TakeNext();
        assert(work && work->index == index);
        assert(scheduler.Complete(*work));
    }
}

static void TestCancelAndStableIndices() {
    Scheduler scheduler;
    auto obsolete = scheduler.Begin(500);
    scheduler.SetViewport(100, 20, 5);
    auto queued = scheduler.TakeNext();
    assert(queued && queued->generation == obsolete);
    auto current = scheduler.Begin(40);
    assert(current != obsolete && !scheduler.Complete(*queued));
    std::set<std::size_t> seen;
    while (auto work = scheduler.TakeNext()) {
        assert(work->generation == current && seen.insert(work->index).second);
        assert(scheduler.Complete(*work));
    }
    assert(seen.size() == 40 && scheduler.IsComplete());
    assert(!scheduler.TakeNext());
    auto canceled = scheduler.Cancel();
    assert(canceled != current && scheduler.ItemCount() == 0);
}

static void TestTimeBudgetAndRequeue() {
    Scheduler scheduler;
    auto generation = scheduler.Begin(12);
    scheduler.SetViewport(0, 3);
    auto slice = scheduler.RunSlice(generation, 2ms, 3, [](Scheduler::WorkItem) { return true; });
    assert(slice.processed == 3 && !slice.completed && scheduler.Remaining() == 9);
    slice = scheduler.RunSlice(generation, 2ms, 3, [](Scheduler::WorkItem) { return false; });
    assert(slice.processed == 1 && scheduler.Remaining() == 9);
    auto item = scheduler.TakeNext();
    assert(item && item->index == 3 && scheduler.Complete(*item));
    // Simulate a category change from within the game callback.
    auto old = scheduler.CurrentGeneration();
    slice = scheduler.RunSlice(old, 2ms, 5, [&](Scheduler::WorkItem) {
        scheduler.Begin(2);
        return true;
    });
    assert(slice.generationChanged && scheduler.Remaining() == 2);
}

static void TestBoundsAndEmptyCategories() {
    Scheduler scheduler;
    const auto generation = scheduler.Begin(0);
    assert(scheduler.IsComplete() && !scheduler.TakeNext());
    assert(scheduler.RunSlice(generation, 1ms, 10, [](auto) { return true; }).completed);
    scheduler.Begin(3);
    scheduler.SetViewport(999999, 999999, 0, 999999);
    std::size_t completed = 0;
    while (auto item = scheduler.TakeNext()) {
        assert(item->index < 3 && scheduler.Complete(*item));
        ++completed;
    }
    assert(completed == 3);
}

int main() {
    TestVisibleFirstAndScrollDirection();
    TestCancelAndStableIndices();
    TestTimeBudgetAndRequeue();
    TestBoundsAndEmptyCategories();
    std::cout << "PASS: 4 CAS native scheduler test groups\n";
}
