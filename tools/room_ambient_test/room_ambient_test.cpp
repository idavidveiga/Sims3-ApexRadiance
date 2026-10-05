// Offline, read-only checks of the policy used by the room updater.
#include "features/room_ambient_policy.h"
#include <cstdio>
#include <limits>
#include <initializer_list>
#include <array>
#include <algorithm>

int main() {
    using namespace RoomAmbientPolicy;
    int checks = 0, failures = 0;
    auto check = [&](bool ok, const char* name) {
        ++checks;
        if (!ok) { ++failures; std::printf("FAIL: %s\n", name); }
    };
    for (float brightness : {0.0f, 0.105f, 0.344f, 0.5f, 1.0f}) {
        check(std::fabs(BackgroundShare(brightness, 1.0f) - brightness) < 1e-6f, "night matches room brightness");
        check(BackgroundShare(brightness, 0.0f) == 1.0f, "day unchanged");
        check(std::fabs(BackgroundShare(brightness, 0.5f) - (1.0f + brightness) / 2.0f) < 1e-6f, "dusk interpolation");
    }
    const float a[3] = {0.0317f, 0.0317f, 0.0317f};
    const float b[3] = {0.0169f, 0.0169f, 0.0169f};
    const float bad[3] = {std::numeric_limits<float>::quiet_NaN(), 0.0317f, 0.0317f};
    check(SameRgb(a, a), "unchanged ambient matched");
    check(!SameRgb(a, b), "recorded stale ambient differs from target");
    check(!SameRgb(a, bad), "invalid ambient never matched");
    check(NeedsAmbientSolve(0, true), "unknown or evicted ambient queued at rest");
    check(!NeedsAmbientSolve(0, false), "unknown ambient not requeued every scan");
    check(NeedsAmbientSolve(-2, true), "busy room delegated to deferred queue");
    check(!NeedsAmbientSolve(-1, true), "outside and roofless rooms untouched");
    check(!NeedsAmbientSolve(1, true), "direct retint needs no solve");
    for (int state = 0; state <= 5; ++state)
        check(SolverOwnsAmbient(state) == (state >= 1 && state <= 3), "solver ownership");
    check(RebuildRoomList(false, false, false, true, false), "same lot new story manager invalidates cache");
    check(RebuildRoomList(false, false, true, false, false), "lot switch invalidates cache");
    check(!RebuildRoomList(false, false, false, false, false), "stable rooms keep cache");
    check(!RebuildRoomList(false, false, false, true, true), "lazy scan defers manager rebuild");
    check(RebuildRoomList(false, true, false, true, true), "lazy cache eventually expires");
    check(RebuildRoomList(true, false, false, false, true), "world reset rebuilds empty list");
    check(AmbientUpdateDue(100, 0), "first group update immediately eligible");
    check(!AmbientUpdateDue(1100, 1000), "second solve does not bypass cooldown");
    check(!AmbientUpdateDue(3999, 1000), "pending update waits until deadline");
    check(AmbientUpdateDue(4000, 1000), "pending update eligible at deadline without another lamp event");
    check(AmbientUpdateDue(8000, 1000), "late update remains eligible");
    check(!AmbientUpdateDue(0x20u, 0xfffffff0u), "tick wrap preserves cooldown");
    check(AmbientUpdateDue(0xc00u, 0xfffffff0u), "tick wrap eventually releases pending update");
    const float lamp = 0.0653f, oldBase = 0.02f;
    for (float base : {0.0f, 0.01f, 0.08f}) {
        const float moved = MoveBackground(lamp + oldBase, oldBase, base);
        check(std::fabs(moved - (lamp + base)) < 1e-6f, "slider preserves lamp contribution including zero");
        check(std::fabs(MoveBackground(moved, base, oldBase) - (lamp + oldBase)) < 1e-6f, "background round trip has no double addition");
    }
    float total[4] = {};
    const float first[4] = {0.1f, 0.2f, 0.3f, 1.0f};
    const float second[4] = {0.4f, 0.5f, 0.6f, 1.0f};
    AccumulateAmbient(total, first, 2.0f, 1.0f, 2.0f);
    AccumulateAmbient(total, second, 1.0f, 1.0f, 1.0f);
    check(std::fabs(total[0] / 3.0f - 0.5f / 3.0f) < 1e-6f, "group uses original colours area and normalisation");
    check(std::fabs(total[3] / 3.0f - 1.0f) < 1e-6f, "group alpha is not normalised like RGB");
    float dark[4] = {}, black[4] = {};
    AccumulateAmbient(dark, black, 1.0f, 1.0f, 10.0f);
    check(dark[0] == 0 && dark[1] == 0 && dark[2] == 0, "unlit connected rooms reach zero");
    check(WindowRecheckDue(100, 100, 0), "window activation checked immediately after rearm");
    check(!WindowRecheckDue(2099, 100, 1), "window retry waits two seconds");
    check(WindowRecheckDue(2100, 100, 1), "late registry receives first retry");
    check(!WindowRecheckDue(6099, 100, 2), "window final retry waits six seconds");
    check(WindowRecheckDue(6100, 100, 2), "window final retry at deadline");
    check(!WindowRecheckDue(20000, 100, 3), "window checks stop after bounded passes");
    check(WindowRecheckDue(0x800u, 0xfffffff0u, 1), "window deadline survives tick wrap");
    check(LampRefreshDelay(true) == 120, "switch is reconciled after a short batch");
    check(LampRefreshDelay(false) == 300, "moving a lamp retains the drag debounce (the edit itself sends its rooms at once)");
    check(GatherAfterChange(1001, 1000), "gather started after final switch can be retained");
    check(!GatherAfterChange(999, 1000), "gather started before switch cannot be retained");
    check(!GatherAfterChange(1000, 1000), "same millisecond is ambiguous and requeues");
    check(GatherAfterChange(0x20u, 0xfffffff0u), "fresh gather comparison handles tick wrap");
    check(!GatherAfterChange(0xfffffff0u, 0x20u), "old gather before tick wrap cannot be retained");
    for (int state = -1; state <= 6; ++state)
        check(RetainFreshSolve(state) == (state == 2 || state == 3 || state == 5), "retain only post-gather solve states");
    check(AmbientMapsCompatible(1, 1, 42.85f, 42.85f), "colour-only update preserves map scale and wall ramp");
    check(!AmbientMapsCompatible(1, 0.5f, 42.85f, 42.85f), "normalisation change must solve again");
    check(!AmbientMapsCompatible(1, 1, 42.85f, 46.67f), "wall ramp change must solve again");
    check(!AmbientMapsCompatible(0, 0, 42.85f, 42.85f), "invalid zero normalisation falls back");
    check(!AmbientMapsCompatible(1, std::numeric_limits<float>::infinity(), 42.85f, 42.85f), "infinite normalisation falls back");
    check(!AmbientMapsCompatible(1, 1, std::numeric_limits<float>::quiet_NaN(), 42.85f), "invalid wall base falls back");
    check(!RigFallbackDue(2499, 1000, false), "busy lot waits before fallback");
    check(RigFallbackDue(2500, 1000, false), "busy lot keeps original furniture fallback");
    check(!RigFallbackDue(3000, 1000, true), "fallback does not repeat while waiting");
    check(RigFallbackDue(0x700u, 0xfffffff0u, false), "furniture fallback handles tick wrap");
    // Event-order model: a fresh solve may finish before or after the 120 ms reconciliation.
    // The production retention rule must not restart either ordering, including a second switch.
    for (const std::array<int, 4> states : {std::array<int, 4>{3, 2, 5, 1}, {5, 3, 2, 1}}) {
        int restarts = 0;
        for (int state : states) restarts += !RetainFreshSolve(state);
        check(restarts == 1, "only ungathered member requeues in either completion order");
        check(!GatherAfterChange(1001, 1100), "second switch invalidates retained first-switch gather");
    }
    // The native group still progresses with cached sources; no all-members-ready gate.
    for (float lampColour : {0.0f, 0.12f}) {
        const float source[4] = {0.0287f + lampColour, 0.0287f, 0.0287f, 1};
        float merged[4] = {};
        for (int room = 0; room < 4; ++room) AccumulateAmbient(merged, source, 1, 1, room + 1.0f);
        check(std::fabs(merged[0] / 10 - source[0]) < 1e-6f, "same map-scale group converges when on or off");
    }
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
