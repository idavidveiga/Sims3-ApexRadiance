#pragma once
// Apex-only Hair/Hats game-thread adapter for the phased original-order driver.
//
// Unlike the scheduler alone, this adapter owns the lifecycle of a population
// session and runs its callbacks from a caller-supplied GAME/SIMULATION tick.
// It cannot patch CASHair.PopulateTypesGrid or authenticate the Mono ABI:
// callers must supply a separately verified method interception, stable native
// callback context and a game-thread tick. No managed/GC pointers are retained
// by this class. In an ordinary Apex build no callbacks are registered.
//
// One work unit = complete Store item OR complete hair part with all presets.
// Never yield inside AddHairTypeGridItem or an ObjectDesigner operation.
#include "cas_hair_population_plan.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>

namespace ApexCasSchedule {

class HairNativeBridge {
public:
    using Plan = HairPopulationPlan;
    using Outcome = Plan::Outcome;
    using Work = Plan::Work;
    using Generation = Plan::Generation;

    struct SafetyGate {
        bool originalUiMethodVerified = false;
        bool interpreterAbiVerified = false;
        bool simulationThreadVerified = false;
    };
    // The context must be native-owned and kept alive until Stop or completion.
    // The validity callback must detect category/Sim/filter/content epoch changes.
    // Never store MonoObjects, raw UI grid items, Mono enumerators or UIImage here.
    struct Callbacks {
        void* context = nullptr;
        bool (*current)(void*, std::uint64_t) = nullptr;
        Outcome (*store)(void*, Work) = nullptr;
        Outcome (*part)(void*, Work) = nullptr;
        Outcome (*finish)(void*, Generation) = nullptr;
    };
    struct Result {
        Plan::Slice slice{};
        bool dispatched = false;
        bool refused = false;
        bool canceled = false;
        bool needsOriginalRebuild = false;
        bool wrongThread = false;
    };

    // Called only from a verified interception of the original PARENT method
    // on the game's simulation thread, with immutable source counts.
    // Zero is reserved as failure; the original method must run untouched.
    Generation Begin(std::uint64_t contextEpoch,
                     std::size_t featuredStoreCount,
                     std::size_t completePartCount,
                     SafetyGate gate, Callbacks callbacks) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (!gate.originalUiMethodVerified || !gate.interpreterAbiVerified ||
            !gate.simulationThreadVerified || !callbacks.context ||
            !callbacks.current || !callbacks.store || !callbacks.part ||
            !callbacks.finish)
            return 0;
        if (owner_ != std::thread::id{} &&
            owner_ != std::this_thread::get_id())
            return 0;
        if (!callbacks.current(callbacks.context, contextEpoch)) return 0;
        const auto generation = plan_.Begin(featuredStoreCount, completePartCount);
        if (!generation) return 0;
        owner_ = std::this_thread::get_id();
        callbacks_ = callbacks;
        epoch_ = contextEpoch;
        active_ = true;
        rebuildRequired_ = false;
        return generation;
    }

    // Must be called by a REAL simulator/UI tick, never Apex's render thread.
    // A complete part group may exceed budget, but will never split.
    Result OnSimulationTick(Generation generation,
                            std::chrono::nanoseconds budget =
                                std::chrono::milliseconds(2),
                            std::size_t maxUnits = 4) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        Result out;
        if (owner_ != std::this_thread::get_id()) {
            out.wrongThread = true;
            return out;
        }
        if (!active_ || generation != plan_.CurrentGeneration()) {
            out.refused = true;
            return out;
        }
        if (!callbacks_.current(callbacks_.context, epoch_)) {
            // The NEW CAS category owns its grid now. Do not append old rows
            // and never run an old-session fallback over a new category.
            plan_.Cancel();
            active_ = false;
            callbacks_ = {};
            out.canceled = true;
            return out;
        }
        out.slice = plan_.RunSlice(generation, budget, maxUnits,
            [this](Work w) { return callbacks_.store(callbacks_.context, w); },
            [this](Work w) { return callbacks_.part(callbacks_.context, w); },
            [this](Generation g) {
                return callbacks_.finish(callbacks_.context, g);
            });
        out.dispatched = true;
        if (out.slice.aborted) {
            // Some original UI operations may already have committed.
            // Do not automatically call the original parent recursively
            // from inside the failed callback. The managed bridge must
            // schedule ONE clean original-method rebuild instead.
            rebuildRequired_ = true;
            active_ = false;
            callbacks_ = {};
            out.needsOriginalRebuild = true;
        } else if (out.slice.completed) {
            active_ = false;
            callbacks_ = {};
        } else if (out.slice.stale) {
            // Another category may have been started reentrantly.
            out.canceled = true;
        }
        return out;
    }

    // Explicit category switch / CAS close. Owner thread only.
    bool Stop() {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (owner_ != std::thread::id{} &&
            owner_ != std::this_thread::get_id()) return false;
        if (!plan_.Cancel()) return false;
        active_ = false;
        callbacks_ = {};
        rebuildRequired_ = false;
        return true;
    }
    bool NeedsOriginalRebuild() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return rebuildRequired_;
    }
    bool Active() const {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return active_;
    }

private:
    // Synchronous callbacks may reenter Begin/Stop. As in HairPopulationPlan,
    // recursive locking permits invalidation without overlapping work units.
    mutable std::recursive_mutex mutex_;
    Plan plan_;
    std::thread::id owner_{};
    Callbacks callbacks_{};
    std::uint64_t epoch_ = 0;
    bool active_ = false;
    bool rebuildRequired_ = false;
};

} // namespace ApexCasSchedule
