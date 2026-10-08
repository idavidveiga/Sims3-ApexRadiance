// Deterministic integration tests for the native Hair/Hats UI-bound lifecycle.
// Uses a fake grid only; does NOT modify UI.dll or run inside The Sims 3.
#include "cas_hair_grid_session.h"
#include <cassert>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <optional>
#include <thread>
#include <vector>

using namespace ApexCasSchedule;
using namespace std::chrono_literals;

static void TestGridBatchesAndSelection() {
    HairGridSession session;
    const auto gen = session.Begin(7, 5);
    std::vector<std::size_t> grid;
    std::size_t selection = 999;
    int finishes = 0;
    auto append = [&](std::size_t index) {
        assert(index == grid.size());
        grid.push_back(index);
        return true;
    };
    auto select = [&](std::size_t index) {
        assert(index < grid.size());
        selection = index;
        return true;
    };
    auto finish = [&] { ++finishes; return true; };
    for (int step = 0; step < 2; ++step) {
        const auto tick = session.Tick(gen, 1s, 2, append, select, finish);
        assert(tick.appended == 2 && !tick.completed);
        assert(selection == 999 && finishes == 0);
    }
    auto tick = session.Tick(gen, 1s, 2, append, select, finish);
    assert(tick.appended == 2 && selection == 5 && finishes == 0);
    tick = session.Tick(gen, 1s, 2, append, select, finish);
    assert(tick.appended == 1 && tick.completed && finishes == 1);
    assert(grid.size() == 7 && session.Remaining() == 0);
    tick = session.Tick(gen, 1s, 100, append, select, finish);
    assert(tick.completed && tick.appended == 0 && finishes == 1);
}

static void TestFailureDoesNotReorder() {
    HairGridSession session;
    const auto gen = session.Begin(4);
    std::vector<std::size_t> indices;
    bool firstFailure = true;
    auto append = [&](std::size_t index) {
        if (index == 1 && firstFailure) {
            firstFailure = false;
            return false; // the game callback promises no side effects on failure
        }
        indices.push_back(index);
        return true;
    };
    auto tick = session.Tick(gen, 1s, 10, append, [](auto) { return true; },
                             [] { return true; });
    assert(tick.blocked && tick.appended == 1 && session.Remaining() == 3);
    tick = session.Tick(gen, 1s, 10, append, [](auto) { return true; },
                        [] { return true; });
    assert(tick.completed && indices == std::vector<std::size_t>({0,1,2,3}));
}

static void TestSelectionRetryAndCompletionRetry() {
    HairGridSession session;
    const auto gen = session.Begin(2, 1);
    int selectCalls = 0;
    int finishCalls = 0;
    auto select = [&](std::size_t index) {
        assert(index == 1);
        return ++selectCalls > 1;
    };
    auto finish = [&] { return ++finishCalls > 1; };
    auto tick = session.Tick(gen, 1s, 2, [](auto) { return true; },
                             select, finish);
    assert(tick.selectionPending && !tick.completed && finishCalls == 0);
    tick = session.Tick(gen, 1s, 2, [](auto) {
        assert(false && "all grid rows were already appended");
        return true;
    }, select, finish);
    assert(!tick.selectionPending && !tick.completed && finishCalls == 1);
    tick = session.Tick(gen, 1s, 2, [](auto) { return true; }, select, finish);
    assert(tick.completed && finishCalls == 2 && selectCalls == 2);
}

static void TestGenerationInvalidationOnCategorySwitch() {
    HairGridSession session;
    const auto old = session.Begin(50, 10);
    int called = 0;
    const auto tick = session.Tick(old, 1s, 100,
        [&](std::size_t index) {
            assert(index == 0);
            ++called;
            session.Begin(3, 2); // category changes inside managed append
            return true;
        },
        [](auto) { assert(false); return true; },
        [] { assert(false); return true; });
    assert(tick.stale && called == 1 && session.Remaining() == 3);
    const auto stale = session.Tick(old, 1s, 100,
        [](auto) { assert(false); return true; },
        [](auto) { assert(false); return true; },
        [] { assert(false); return true; });
    assert(stale.stale);
    const auto current = session.CurrentGeneration();
    int finished = 0;
    const auto now = session.Tick(current, 1s, 100,
        [](auto) { return true; },
        [](std::size_t selected) { assert(selected == 2); return true; },
        [&] { ++finished; return true; });
    assert(now.completed && finished == 1);
}

static void TestThreadOwnershipAndCancel() {
    HairGridSession session;
    const auto gen = session.Begin(2);
    bool rejectedCancel = false;
    bool rejectedTick = false;
    std::thread other([&] {
        rejectedCancel = !session.Cancel();
        const auto wrong = session.Tick(gen, 1s, 10,
            [](auto) { assert(false); return true; },
            [](auto) { assert(false); return true; },
            [] { assert(false); return true; });
        rejectedTick = wrong.wrongThread && wrong.appended == 0;
    });
    other.join();
    assert(rejectedCancel && rejectedTick && session.Remaining() == 2);
    assert(session.Cancel());
    assert(session.Tick(gen, 1s, 10,
        [](auto) { assert(false); return true; },
        [](auto) { return true; }, [] { return true; }).stale);
}

static void TestEmptyAndOutOfRangeSelection() {
    HairGridSession session;
    const auto gen = session.Begin(0, 12);
    int finished = 0;
    auto r = session.Tick(gen, 1s, 10,
        [](auto) { assert(false); return true; },
        [](auto) { assert(false); return true; },
        [&] { ++finished; return true; });
    assert(r.completed && finished == 1);
    const auto next = session.Begin(2, 777);
    r = session.Tick(next, 1s, 2,
        [](auto) { return true; },
        [](auto) { assert(false); return true; },
        [&] { ++finished; return true; });
    assert(r.completed && finished == 2);
}

static void TestReentrantCancelFromSelection() {
    HairGridSession session;
    const auto gen = session.Begin(1, 0);
    int terminal = 0;
    const auto r = session.Tick(gen, 1s, 1,
        [](auto) { return true; },
        [&](std::size_t) {
            assert(session.Cancel());
            return true;
        },
        [&] { ++terminal; return true; });
    assert(r.stale && terminal == 0 && !session.Active());
}

int main() {
    TestGridBatchesAndSelection();
    TestFailureDoesNotReorder();
    TestSelectionRetryAndCompletionRetry();
    TestGenerationInvalidationOnCategorySwitch();
    TestThreadOwnershipAndCancel();
    TestEmptyAndOutOfRangeSelection();
    TestReentrantCancelFromSelection();
    std::cout << "PASS: 7 native Hair/Hats grid lifecycle test groups\n";
}
