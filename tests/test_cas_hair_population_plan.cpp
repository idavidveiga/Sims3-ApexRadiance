// Synthetic original Hair/Hats execution replay for the phased native plan.
// No EA DLL is built, patched or distributed.
#include "cas_hair_population_plan.h"
#include <cassert>
#include <chrono>
#include <cstddef>
#include <iostream>
#include <thread>
#include <vector>

using ApexCasSchedule::HairPopulationPlan;
using Plan = ApexCasSchedule::HairPopulationPlan;
using namespace std::chrono_literals;

static void TestStorePrecedesAtomicPartGroups() {
    Plan driver;
    auto generation = driver.Begin(3, 4);
    std::vector<int> record;
    int terminal = 0;
    auto store = [&](Plan::Work work) {
        assert(work.phase == Plan::Phase::FeaturedStore);
        record.push_back(100 + static_cast<int>(work.index));
        return Plan::Outcome::Committed;
    };
    auto part = [&](Plan::Work work) {
        assert(work.phase == Plan::Phase::PartGroups);
        // Each whole original part includes its default + extra preset(s).
        record.push_back(200 + static_cast<int>(work.index));
        record.push_back(300 + static_cast<int>(work.index));
        return Plan::Outcome::Committed;
    };
    auto finish = [&](Plan::Generation) {
        assert(driver.PartRemaining() == 0);
        ++terminal;
        return Plan::Outcome::Committed;
    };
    auto t = driver.RunSlice(generation, 1s, 2, store, part, finish);
    assert(t.processedStore == 2 && t.processedParts == 0);
    assert(!t.completed && terminal == 0);
    t = driver.RunSlice(generation, 1s, 2, store, part, finish);
    assert(t.processedStore == 1 && t.processedParts == 1);
    t = driver.RunSlice(generation, 1s, 3, store, part, finish);
    assert(t.processedParts == 3 && t.completed && terminal == 1);
    assert(record == std::vector<int>({100,101,102,200,300,201,301,202,302,203,303}));
    t = driver.RunSlice(generation, 1s, 100, store, part, finish);
    assert(t.completed && terminal == 1 && t.processedParts == 0);
}

static void TestNoSplitWithinPartGroup() {
    Plan driver;
    const auto g = driver.Begin(0, 3);
    std::vector<int> order;
    auto t = driver.RunSlice(g, 0ns, 3,
        [](auto) { assert(false); return Plan::Outcome::Committed; },
        [&](Plan::Work w) {
            order.push_back(static_cast<int>(w.index));
            order.push_back(1000 + static_cast<int>(w.index));
            return Plan::Outcome::Committed;
        },
        [](auto) { return Plan::Outcome::Committed; });
    assert(t.processedParts == 1 && t.partsRemaining == 2);
    assert(!t.completed && order == std::vector<int>({0,1000}));
}

static void TestBlockedUnitNeverSkipped() {
    Plan driver;
    const auto g = driver.Begin(1, 2);
    int attempts = 0;
    std::vector<std::size_t> parts;
    auto st = [](auto) { return Plan::Outcome::Committed; };
    auto pt = [&](Plan::Work w) {
        if (w.index == 0 && ++attempts == 1)
            return Plan::Outcome::BlockedNoCommit;
        parts.push_back(w.index);
        return Plan::Outcome::Committed;
    };
    auto t = driver.RunSlice(g, 1s, 4, st, pt,
                             [](auto) { return Plan::Outcome::Committed; });
    assert(t.blocked && t.processedStore == 1 && t.processedParts == 0);
    t = driver.RunSlice(g, 1s, 4, st, pt,
                        [](auto) { return Plan::Outcome::Committed; });
    assert(t.completed && parts == std::vector<std::size_t>({0, 1}));
}

static void TestReentrantNewCategoryInvalidatesOldCallbacks() {
    Plan driver;
    const auto old = driver.Begin(4, 8);
    int calls = 0;
    auto t = driver.RunSlice(old, 1s, 20,
        [&](Plan::Work work) {
            assert(work.index == 0);
            ++calls;
            driver.Begin(0, 2);
            return Plan::Outcome::Committed;
        },
        [](auto) { assert(false); return Plan::Outcome::Committed; },
        [](auto) { assert(false); return Plan::Outcome::Committed; });
    assert(t.stale && calls == 1 && driver.PartRemaining() == 2);
    t = driver.RunSlice(old, 1s, 20,
        [](auto) { assert(false); return Plan::Outcome::Committed; },
        [](auto) { assert(false); return Plan::Outcome::Committed; },
        [](auto) { assert(false); return Plan::Outcome::Committed; });
    assert(t.stale);
}

static void TestAbortNeverRetriesPartialUiMutation() {
    Plan driver;
    const auto g = driver.Begin(0, 2);
    int partCalls = 0;
    const auto fail = driver.RunSlice(g, 1s, 20,
        [](auto) { assert(false); return Plan::Outcome::Committed; },
        [&](auto) {
            ++partCalls;
            return Plan::Outcome::AbortSession;
        },
        [](auto) { assert(false); return Plan::Outcome::Committed; });
    assert(fail.aborted && driver.CurrentPhase() == Plan::Phase::Inactive);
    const auto again = driver.RunSlice(g, 1s, 20,
        [](auto) { return Plan::Outcome::Committed; },
        [&](auto) { ++partCalls; return Plan::Outcome::Committed; },
        [](auto) { return Plan::Outcome::Committed; });
    assert(again.stale && partCalls == 1);
}

static void TestOwnerThreadAndCancel() {
    Plan driver;
    const auto g = driver.Begin(1, 2);
    bool wrongThread = false;
    bool cancelRejected = false;
    std::thread other([&] {
        wrongThread = driver.RunSlice(g, 1s, 10,
            [](auto) { assert(false); return Plan::Outcome::Committed; },
            [](auto) { assert(false); return Plan::Outcome::Committed; },
            [](auto) { assert(false); return Plan::Outcome::Committed; }).wrongThread;
        cancelRejected = !driver.Cancel();
    });
    other.join();
    assert(wrongThread && cancelRejected && driver.StoreRemaining() == 1);
    assert(driver.Cancel() && !driver.Active());
    assert(driver.RunSlice(g, 1s, 1,
        [](auto) { return Plan::Outcome::Committed; },
        [](auto) { return Plan::Outcome::Committed; },
        [](auto) { return Plan::Outcome::Committed; }).stale);
}

static void TestFinishRetryAndEmptyCategory() {
    Plan driver;
    const auto g = driver.Begin(0, 0);
    int ends = 0;
    auto complete = [&](Plan::Generation) {
        return ++ends == 2 ? Plan::Outcome::Committed
                           : Plan::Outcome::BlockedNoCommit;
    };
    auto tick = driver.RunSlice(g, 0ns, 0,
        [](auto) { assert(false); return Plan::Outcome::Committed; },
        [](auto) { assert(false); return Plan::Outcome::Committed; },
        complete);
    assert(tick.blocked && !tick.completed && ends == 1);
    tick = driver.RunSlice(g, 0ns, 0,
        [](auto) { assert(false); return Plan::Outcome::Committed; },
        [](auto) { assert(false); return Plan::Outcome::Committed; },
        complete);
    assert(tick.completed && ends == 2);
    tick = driver.RunSlice(g, 1s, 10,
        [](auto) { return Plan::Outcome::Committed; },
        [](auto) { return Plan::Outcome::Committed; },
        complete);
    assert(tick.completed && ends == 2);
}

int main() {
    TestStorePrecedesAtomicPartGroups();
    TestNoSplitWithinPartGroup();
    TestBlockedUnitNeverSkipped();
    TestReentrantNewCategoryInvalidatesOldCallbacks();
    TestAbortNeverRetriesPartialUiMutation();
    TestOwnerThreadAndCancel();
    TestFinishRetryAndEmptyCategory();
    std::cout << "PASS: 7 original-order Hair/Hats population plan groups\n";
}
