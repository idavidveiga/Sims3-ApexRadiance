// Portable tests for the Apex-only Hair/Hats game-thread callback bridge.
// Does not require a TS3 installation or an unverified Mono ABI.
#include "cas_hair_native_bridge.h"
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <thread>
#include <vector>

using Bridge = ApexCasSchedule::HairNativeBridge;
using Plan = ApexCasSchedule::HairPopulationPlan;
using namespace std::chrono_literals;
struct State {
    std::uint64_t epoch = 1;
    std::vector<int> work;
    bool abort = false;
    int finishCount = 0;
};
static bool Current(void* p, std::uint64_t epoch) {
    return static_cast<State*>(p)->epoch == epoch;
}
static Plan::Outcome Store(void* p, Plan::Work w) {
    auto& s=*static_cast<State*>(p);
    s.work.push_back(static_cast<int>(w.index)+100);
    return Plan::Outcome::Committed;
}
static Plan::Outcome Part(void* p, Plan::Work w) {
    auto& s=*static_cast<State*>(p);
    s.work.push_back(static_cast<int>(w.index)+200);
    return s.abort ? Plan::Outcome::AbortSession : Plan::Outcome::Committed;
}
static Plan::Outcome Finish(void* p, Plan::Generation) {
    ++static_cast<State*>(p)->finishCount;
    return Plan::Outcome::Committed;
}
static Bridge::Callbacks Callbacks(State& state) {
    return {&state, &Current, &Store, &Part, &Finish};
}
static Bridge::SafetyGate Safe() { return {true,true,true}; }
static void TestRejectIncompleteGate() {
    Bridge bridge; State s;
    assert(!bridge.Begin(1,1,2,{},Callbacks(s)));
    assert(!bridge.Begin(1,1,2,{true,false,true},Callbacks(s)));
    assert(!bridge.Active() && s.work.empty());
    assert(!bridge.Begin(2,1,2,Safe(),Callbacks(s)));
}
static void TestOrderedGroupsAndFinishOnce() {
    Bridge bridge; State s;
    const auto gen=bridge.Begin(1,2,3,Safe(),Callbacks(s));
    assert(gen);
    auto r=bridge.OnSimulationTick(gen,1s,2);
    assert(r.dispatched && r.slice.processedStore==2 && !r.slice.completed);
    r=bridge.OnSimulationTick(gen,1s,6);
    assert(r.slice.processedParts==3 && r.slice.completed);
    assert(s.work==std::vector<int>({100,101,200,201,202}));
    assert(s.finishCount==1 && !bridge.Active());
    assert(bridge.OnSimulationTick(gen).refused && s.finishCount==1);
}
static void TestChangedEpochCancelsWithoutOldRows() {
    Bridge bridge; State s;
    const auto gen=bridge.Begin(1,0,3,Safe(),Callbacks(s));
    assert(gen);
    bridge.OnSimulationTick(gen,0ns,10);
    assert(s.work==std::vector<int>({200}));
    ++s.epoch;
    const auto result=bridge.OnSimulationTick(gen,1s,10);
    assert(result.canceled && s.work.size()==1);
    assert(!bridge.Active() && s.finishCount==0);
}
static void TestAbortRequiresExternalRebuild() {
    Bridge bridge; State s; s.abort=true;
    const auto gen=bridge.Begin(1,0,2,Safe(),Callbacks(s));
    assert(gen);
    auto r=bridge.OnSimulationTick(gen,1s,10);
    assert(r.needsOriginalRebuild && !bridge.Active());
    assert(bridge.NeedsOriginalRebuild());
    assert(s.work==std::vector<int>({200}));
    assert(bridge.OnSimulationTick(gen).refused);
    assert(bridge.Stop() && !bridge.NeedsOriginalRebuild());
}
static void TestForeignThreadCannotDispatchOrStealSession() {
    Bridge bridge; State s;
    const auto gen=bridge.Begin(1,0,2,Safe(),Callbacks(s));
    assert(gen);
    bool bad=false, foreignBegin=false, stopRejected=false;
    std::thread worker([&] {
        bad=bridge.OnSimulationTick(gen).wrongThread;
        foreignBegin=bridge.Begin(1,0,5,Safe(),Callbacks(s))==0;
        stopRejected=!bridge.Stop();
    });
    worker.join();
    assert(bad && foreignBegin && stopRejected && s.work.empty());
    auto r=bridge.OnSimulationTick(gen,1s,4);
    assert(r.slice.completed && s.work==std::vector<int>({200,201}));
}
static void TestReentrantChangeBlocksOldCommit() {
    Bridge bridge; State s;
    struct Reentry { Bridge* bridge; State* state; };
    Reentry entry{&bridge,&s};
    Bridge::Callbacks callbacks{
        &entry,
        [](void* p,std::uint64_t epoch) {
            auto& e=*static_cast<Reentry*>(p); return e.state->epoch==epoch;
        },
        [](void*,Plan::Work) { return Plan::Outcome::Committed; },
        [](void* p,Plan::Work) {
            auto& e=*static_cast<Reentry*>(p);
            e.state->work.push_back(1);
            ++e.state->epoch;
            assert(e.bridge->Stop());
            return Plan::Outcome::Committed;
        },
        [](void*,Plan::Generation) { assert(false); return Plan::Outcome::Committed; }
    };
    const auto gen=bridge.Begin(1,0,3,Safe(),callbacks);
    assert(gen);
    auto r=bridge.OnSimulationTick(gen,1s,6);
    assert(r.slice.stale && r.canceled && !bridge.Active());
    assert(s.work.size()==1);
}
int main() {
    TestRejectIncompleteGate();
    TestOrderedGroupsAndFinishOnce();
    TestChangedEpochCancelsWithoutOldRows();
    TestAbortRequiresExternalRebuild();
    TestForeignThreadCannotDispatchOrStealSession();
    TestReentrantChangeBlocksOldCommit();
    std::cout<<"PASS: 6 Hair/Hats native bridge test groups\n";
}
