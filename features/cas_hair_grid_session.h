#pragma once
// Hair/Hats native incremental UI adapter. It schedules work only: the caller
// MUST execute the game's existing append, selection and final-grid operations
// on the original CAS simulator thread. No Mono pointers or UI handles are
// retained. No runtime hook is installed here.
// Copyright (c) Apex Radiance contributors, MIT.
#include "cas_catalog_scheduler.h"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <thread>
#include <utility>

namespace ApexCasSchedule {

// Integration boundary for a future *verified* CASHair.PopulateTypesGrid bridge.
// Begin must be called on the CAS simulator thread after the original code has
// resolved a stable, ordered list of parts. Every Tick must run on THAT thread.
// An Append callback returning false MUST leave the grid unchanged: otherwise
// retrying would duplicate a visual item. A callback that changes category must
// call Begin or Cancel first, so subsequent work is discarded.
//
// This class intentionally contains no way to patch, discover or invoke a Mono
// method. That sensitive boundary must be separately validated for EA 1.69.
class HairGridSession {
public:
    using Generation = OrderedAppend::Generation;
    struct TickResult {
        std::size_t appended = 0;
        std::size_t remaining = 0;
        bool completed = false; // true only after the game's terminal callback succeeds
        bool blocked = false;
        bool stale = false;
        bool wrongThread = false;
        bool selectionPending = false;
    };

    // Selection is an original logical catalog index, never a reordered UI slot.
    Generation Begin(std::size_t count,
                     std::optional<std::size_t> selectedIndex = std::nullopt) {
        owner_ = std::this_thread::get_id();
        active_ = true;
        finalized_ = false;
        selectionDone_ = false;
        desired_ = selectedIndex && *selectedIndex < count ? selectedIndex
                                                           : std::nullopt;
        return queue_.Begin(count);
    }

    // The game's category/Sim/filter/outfit change must invalidate old work.
    // Reject wrong-thread cancellation so another thread cannot mutate the
    // live task while the simulation thread is inside a callback.
    bool Cancel() {
        if (active_ && !OnOwnerThread()) return false;
        queue_.Cancel();
        active_ = false;
        finalized_ = false;
        desired_.reset();
        selectionDone_ = false;
        return true;
    }

    Generation CurrentGeneration() const noexcept {
        return queue_.CurrentGeneration();
    }
    bool Active() const noexcept { return active_; }
    bool Finished() const noexcept { return active_ && finalized_; }
    std::size_t Remaining() const noexcept { return queue_.Remaining(); }

    // append(index) -> bool: commit exactly the given original index or
    // return false without making any change.
    // restoreSelection(index) -> bool: true if selection is now reflected in UI;
    // false means retry next tick without rebuilding rows.
    // finish() -> bool: perform the game's original terminal grid/filter update
    // exactly once; false means retry next tick. All three run on the caller's
    // simulator thread. They must not throw across a Mono/native boundary.
    template<class Append, class RestoreSelection, class Finish>
    TickResult Tick(Generation expected, std::chrono::nanoseconds budget,
                    std::size_t maxItems, Append&& append,
                    RestoreSelection&& restoreSelection, Finish&& finish) {
        TickResult result;
        if (!OnOwnerThread()) {
            result.wrongThread = true;
            return result;
        }
        if (!active_ || expected != queue_.CurrentGeneration()) {
            result.stale = true;
            return result;
        }
        if (finalized_) {
            result.completed = true;
            return result;
        }

        const auto slice = queue_.RunSlice(expected, budget, maxItems,
            [&](OrderedAppend::WorkItem work) {
                return append(work.index);
            });
        result.appended = slice.processed;
        result.remaining = queue_.Remaining();
        result.blocked = slice.blocked;
        if (slice.generationChanged || !active_ ||
            !queue_.IsCurrent(expected)) {
            result.stale = true;
            return result;
        }

        // Restore only when the selected item has physically been appended.
        // The callback may itself initiate another category: re-check the
        // generation before touching the completion state.
        if (desired_ && !selectionDone_ && queue_.NextIndex() > *desired_) {
            const bool restored = restoreSelection(*desired_);
            if (!active_ || !queue_.IsCurrent(expected)) {
                result.stale = true;
                return result;
            }
            if (restored) selectionDone_ = true;
        }
        result.selectionPending = desired_ && !selectionDone_;

        if (queue_.IsComplete() && !result.selectionPending) {
            const bool finished = finish();
            if (!active_ || !queue_.IsCurrent(expected)) {
                result.stale = true;
                return result;
            }
            if (finished) finalized_ = true;
        }
        result.completed = finalized_;
        return result;
    }

private:
    bool OnOwnerThread() const noexcept {
        return std::this_thread::get_id() == owner_;
    }

    OrderedAppend queue_;
    std::thread::id owner_{};
    std::optional<std::size_t> desired_;
    bool active_ = false;
    bool finalized_ = false;
    bool selectionDone_ = false;
};
} // namespace ApexCasSchedule
