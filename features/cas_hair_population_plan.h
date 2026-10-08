#pragma once
// Native Hair/Hats original-order population driver.
//
// Offline-only orchestration. Does not patch UI.dll, call Mono, or install any
// runtime hooks. Designed after the original EA 1.69 PopulateTypesGrid IL:
// featured Store items -> hair part groups (default + all optional presets)
// -> original terminal UI state updates.
//
// A PART is the smallest work unit on purpose. The original game calls
// ObjectDesigner.SetCASPart once, followed by default/preset selection,
// AddHairTypeGridItem and per-part button state. Splitting inside a part
// would allow ObjectDesigner / CAS context changes to invalidate that state.
//
// No shared game objects, enumerators, or pointers are retained. The future
// bridge must take immutable source snapshots, run on the simulator thread,
// and execute original operations in their original order. No performance
// improvement is claimed until that bridge is validated in-game.
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <thread>
#include <utility>

namespace ApexCasSchedule {

class HairPopulationPlan {
public:
    using Generation = std::uint64_t;
    enum class Phase : std::uint8_t {
        Inactive, FeaturedStore, PartGroups, Finalize, Finished
    };
    // A complete work unit never rolls back by itself. On a failed operation,
    // caller MUST return BlockedNoCommit only if there were *zero* mutations
    // to the grid/UI. Otherwise return AbortSession, and do not retry it.
    enum class Outcome : std::uint8_t { Committed, BlockedNoCommit, AbortSession };
    struct Work {
        Generation generation;
        std::size_t index; // source order, never a visual row index
        Phase phase;
    };
    struct Slice {
        std::size_t processedStore = 0;
        std::size_t processedParts = 0;
        std::size_t storeRemaining = 0;
        std::size_t partsRemaining = 0;
        bool completed = false;
        bool blocked = false;
        bool aborted = false;
        bool stale = false;
        bool wrongThread = false;
        Phase phase = Phase::Inactive;
    };

    // New category, Sim, outfit, content filter, wardrobe or preset epoch:
    // the caller invalidates pending work by calling Begin/Cancel on the
    // SAME simulator/UI thread. Rebuild source snapshots before Begin.
    Generation Begin(std::size_t storeCount, std::size_t partCount) noexcept {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        // Never permit a worker to steal an active CAS session's thread
        // ownership. Generation 0 is reserved as a failed Begin result.
        if (ActiveUnlocked() && !OnOwnerThread()) return 0;
        owner_ = std::this_thread::get_id();
        NextGeneration();
        storeCount_ = storeCount;
        partCount_ = partCount;
        storeIndex_ = partIndex_ = 0;
        phase_ = storeCount ? Phase::FeaturedStore :
                 partCount ? Phase::PartGroups : Phase::Finalize;
        return generation_;
    }

    // Reject off-thread cancellation while active. Never race callbacks.
    bool Cancel() noexcept {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        if (owner_ != std::thread::id{} && !OnOwnerThread()) return false;
        NextGeneration();
        storeCount_ = partCount_ = 0;
        storeIndex_ = partIndex_ = 0;
        phase_ = Phase::Inactive;
        return true;
    }

    Generation CurrentGeneration() const noexcept {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return generation_;
    }
    Phase CurrentPhase() const noexcept {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return phase_;
    }
    bool Active() const noexcept {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return ActiveUnlocked();
    }
    bool Finished() const noexcept {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return phase_ == Phase::Finished;
    }
    std::size_t StoreRemaining() const noexcept {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return storeCount_ - storeIndex_;
    }
    std::size_t PartRemaining() const noexcept {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        return partCount_ - partIndex_;
    }

    // The callbacks are called ON THE OWNER SIMULATOR THREAD ONLY:
    //
    // storeItem(index): all original Store-item layout/badge/click handlers,
    //   ordered as GetCASFeaturedStoreItems/FilterObjects enumerated them;
    //   filtered-out Store items return Committed (unit consumed).
    // partGroup(index): all default-preset, optional preset, selection and
    //   wardrobe button work for ONE part, using the original order/calls.
    //   Do NOT Sleep/yield inside this callback or inside AddHairTypeGridItem.
    // finalize(): EXACT original Tag, sort, save/undo and filter state update.
    //
    // A false/BlockedNoCommit callback must NOT have modified anything.
    // An aborted callback may have partially modified the UI; the caller
    // must rebuild the original grid rather than attempting partial retry.
    //
    // maxUnits and budget limit completed STORE or PART operations. One
    // expensive unit is permitted to exceed budget; it is never split.
    template <class StoreFn, class PartFn, class FinishFn,
              class Clock = std::chrono::steady_clock>
    Slice RunSlice(Generation expected, std::chrono::nanoseconds budget,
                   std::size_t maxUnits, StoreFn&& storeItem,
                   PartFn&& partGroup, FinishFn&& finalize) {
        std::lock_guard<std::recursive_mutex> lock(mutex_);
        Slice result;
        if (!OnOwnerThread()) {
            result.wrongThread = true;
            return Snapshot(result);
        }
        if (expected != generation_ || !Active()) {
            result.stale = true;
            return Snapshot(result);
        }
        if (Finished()) {
            result.completed = true;
            return Snapshot(result);
        }

        const auto started = Clock::now();
        std::size_t units = 0;
        while (units < maxUnits && expected == generation_) {
            if (units && Clock::now() - started >= budget) break;
            if (phase_ == Phase::FeaturedStore && storeIndex_ == storeCount_)
                phase_ = partCount_ ? Phase::PartGroups : Phase::Finalize;
            if (phase_ == Phase::PartGroups && partIndex_ == partCount_)
                phase_ = Phase::Finalize;
            if (phase_ == Phase::Finalize || phase_ == Phase::Finished)
                break;
            const Work work{expected,
                            phase_ == Phase::FeaturedStore ? storeIndex_ : partIndex_,
                            phase_};
            const Outcome state = (work.phase == Phase::FeaturedStore)
                ? storeItem(work) : partGroup(work);
            // A managed event can start another category from inside the
            // native callback. Never touch the new generation's cursors.
            if (expected != generation_) {
                result.stale = true;
                break;
            }
            if (state == Outcome::BlockedNoCommit) {
                result.blocked = true;
                break;
            }
            if (state == Outcome::AbortSession) {
                // There is no safe retry after a potentially partial append.
                // Leave the grid to the future native bridge's full fallback.
                NextGeneration();
                phase_ = Phase::Inactive;
                result.aborted = true;
                break;
            }
            if (work.phase == Phase::FeaturedStore) {
                ++storeIndex_;
                ++result.processedStore;
            } else {
                ++partIndex_;
                ++result.processedParts;
            }
            ++units;
        }
        if (expected == generation_) {
            if (phase_ == Phase::FeaturedStore && storeIndex_ == storeCount_)
                phase_ = partCount_ ? Phase::PartGroups : Phase::Finalize;
            if (phase_ == Phase::PartGroups && partIndex_ == partCount_)
                phase_ = Phase::Finalize;
            if (phase_ == Phase::Finalize && (units == 0 ||
                Clock::now() - started < budget)) {
                const Outcome completed = finalize(expected);
                if (expected != generation_) {
                    result.stale = true;
                } else if (completed == Outcome::Committed) {
                    phase_ = Phase::Finished;
                    result.completed = true;
                } else if (completed == Outcome::BlockedNoCommit) {
                    result.blocked = true;
                } else {
                    NextGeneration();
                    phase_ = Phase::Inactive;
                    result.aborted = true;
                }
            }
        }
        return Snapshot(result);
    }

private:
    Slice Snapshot(Slice result) const noexcept {
        result.phase = phase_;
        result.storeRemaining = StoreRemaining();
        result.partsRemaining = PartRemaining();
        return result;
    }
    bool ActiveUnlocked() const noexcept { return phase_ != Phase::Inactive; }
    bool OnOwnerThread() const noexcept {
        return owner_ == std::this_thread::get_id();
    }
    void NextGeneration() noexcept { if (++generation_ == 0) ++generation_; }

    // Use a recursive mutex because an original simulator callback may
    // synchronously reenter Begin/Cancel via category-switch UI events.
    // The lock protects *all* state and prevents any cross-thread data races.
    // It never permits callbacks to run on any thread but owner_.
    mutable std::recursive_mutex mutex_;
    std::thread::id owner_{};
    Generation generation_ = 0;
    Phase phase_ = Phase::Inactive;
    std::size_t storeCount_ = 0, partCount_ = 0;
    std::size_t storeIndex_ = 0, partIndex_ = 0;
};

} // namespace ApexCasSchedule
