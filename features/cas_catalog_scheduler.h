#pragma once
// Apex-native CAS catalogue scheduler. No game object, managed pointer, or UIImage is stored.
// This header provides scheduling only; all UI work must be performed by its caller on the game thread.
// Independent implementation by @idavidveiga; repository MIT license.
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace ApexCasSchedule {
class Scheduler {
public:
    using Generation = std::uint64_t;
    enum class ItemState : std::uint8_t { Pending, InFlight, Complete };
    struct WorkItem { Generation generation; std::size_t index; };
    struct SliceResult { std::size_t processed = 0; bool generationChanged = false; bool completed = false; };

    Generation Begin(std::size_t count) {
        NewGeneration();
        state_.assign(count, ItemState::Pending);
        outstanding_ = count;
        cursor_ = first_ = visible_ = 0;
        columns_ = 1;
        prefetchRows_ = 2;
        direction_ = 1;
        return generation_;
    }
    Generation Cancel() {
        NewGeneration();
        state_.clear();
        outstanding_ = cursor_ = 0;
        return generation_;
    }
    Generation CurrentGeneration() const noexcept { return generation_; }
    bool IsCurrent(Generation g) const noexcept { return g == generation_; }
    bool IsComplete() const noexcept { return outstanding_ == 0; }
    std::size_t Remaining() const noexcept { return outstanding_; }
    std::size_t ItemCount() const noexcept { return state_.size(); }
    ItemState StateAt(std::size_t i) const { return state_.at(i); }

    // All indexes remain the original game's logical indexes. Direction controls which
    // adjacent band is prefetched first; visible items are always prioritized.
    void SetViewport(std::size_t first, std::size_t count,
                     std::size_t columns = 1, std::size_t prefetchRows = 2) {
        if (first < first_) direction_ = -1;
        else if (first > first_) direction_ = 1;
        first_ = std::min(first, state_.size());
        visible_ = std::min(count, state_.size() - first_);
        columns_ = std::max<std::size_t>(1, columns);
        prefetchRows_ = std::min<std::size_t>(prefetchRows, 16);
    }
    std::optional<WorkItem> TakeNext() {
        const std::size_t n = state_.size();
        if (!n || !outstanding_) return std::nullopt;
        const std::size_t last = first_ + visible_;
        const std::size_t band = prefetchRows_ > n / columns_ ? n : prefetchRows_ * columns_;
        if (auto item = TakeRange(first_, last)) return item;
        if (direction_ >= 0) {
            if (auto item = TakeRange(last, BoundedAdd(last, band, n))) return item;
            if (auto item = TakeRange(first_ > band ? first_ - band : 0, first_)) return item;
        } else {
            if (auto item = TakeRange(first_ > band ? first_ - band : 0, first_)) return item;
            if (auto item = TakeRange(last, BoundedAdd(last, band, n))) return item;
        }
        for (std::size_t step = 0; step < n; ++step) {
            const auto i = cursor_ < n ? cursor_ : 0;
            cursor_ = (i + 1 == n ? 0 : i + 1);
            if (state_[i] == ItemState::Pending) {
                state_[i] = ItemState::InFlight;
                return WorkItem{generation_, i};
            }
        }
        return std::nullopt;
    }
    bool Complete(WorkItem work) {
        if (!IsCurrent(work.generation) || work.index >= state_.size() ||
            state_[work.index] != ItemState::InFlight) return false;
        state_[work.index] = ItemState::Complete;
        --outstanding_;
        return true;
    }
    bool Requeue(WorkItem work) {
        if (!IsCurrent(work.generation) || work.index >= state_.size() ||
            state_[work.index] != ItemState::InFlight) return false;
        state_[work.index] = ItemState::Pending;
        return true;
    }
    // The callback must commit to the original logical slot, on the game's UI/simulator
    // thread. This budget is cooperative: a single expensive part may exceed it.
    template<class BuildFn, class Clock = std::chrono::steady_clock>
    SliceResult RunSlice(Generation g, std::chrono::nanoseconds budget,
                         std::size_t maxItems, BuildFn&& build) {
        SliceResult result;
        if (!IsCurrent(g)) { result.generationChanged = true; return result; }
        const auto start = Clock::now();
        for (std::size_t i = 0; i < maxItems && IsCurrent(g); ++i) {
            if (i && Clock::now() - start >= budget) break;
            auto item = TakeNext();
            if (!item) break;
            const bool ok = build(*item);
            if (!IsCurrent(g)) break;
            if (ok) Complete(*item);
            else Requeue(*item);
            ++result.processed;
            if (!ok) break;
        }
        result.generationChanged = !IsCurrent(g);
        result.completed = IsCurrent(g) && IsComplete();
        return result;
    }

private:
    void NewGeneration() noexcept { if (++generation_ == 0) ++generation_; }
    static std::size_t BoundedAdd(std::size_t a, std::size_t b, std::size_t maximum) noexcept {
        return b > maximum - a ? maximum : a + b;
    }
    std::optional<WorkItem> TakeRange(std::size_t a, std::size_t b) {
        for (std::size_t i = a; i < b; ++i) {
            if (state_[i] == ItemState::Pending) {
                state_[i] = ItemState::InFlight;
                return WorkItem{generation_, i};
            }
        }
        return std::nullopt;
    }
    Generation generation_ = 0;
    std::vector<ItemState> state_;
    std::size_t outstanding_ = 0, cursor_ = 0, first_ = 0, visible_ = 0;
    std::size_t columns_ = 1, prefetchRows_ = 2;
    int direction_ = 1;
};

// Ordered append scheduler for Hair/Hats category grids that do not have
// verified support for sparse slot insertion. Unlike Scheduler, this class
// NEVER changes the order of calls to the caller's original append action.
// The callback is restricted to the game's own UI/simulator thread and must
// complete one logical item before returning true. On failure the same item
// is retried in a later tick without skipping its original position.
// No managed object, UIImage, game pointer or global thread is retained here.
class OrderedAppend {
public:
    using Generation = std::uint64_t;
    struct WorkItem { Generation generation; std::size_t index; };
    struct SliceResult {
        std::size_t processed = 0;
        bool generationChanged = false;
        bool completed = false;
        bool blocked = false;
    };

    Generation Begin(std::size_t itemCount) noexcept {
        AdvanceGeneration();
        count_ = itemCount;
        next_ = 0;
        return generation_;
    }
    Generation Cancel() noexcept {
        AdvanceGeneration();
        count_ = next_ = 0;
        return generation_;
    }
    Generation CurrentGeneration() const noexcept { return generation_; }
    std::size_t ItemCount() const noexcept { return count_; }
    std::size_t NextIndex() const noexcept { return next_; }
    std::size_t Remaining() const noexcept { return count_ - next_; }
    bool IsComplete() const noexcept { return next_ == count_; }
    bool IsCurrent(Generation g) const noexcept { return g == generation_; }

    template<class AppendFn, class Clock = std::chrono::steady_clock>
    SliceResult RunSlice(Generation expected, std::chrono::nanoseconds budget,
                         std::size_t maxItems, AppendFn&& append) {
        SliceResult result;
        if (!IsCurrent(expected)) {
            result.generationChanged = true;
            return result;
        }
        const auto start = Clock::now();
        while (result.processed < maxItems && next_ < count_ && IsCurrent(expected)) {
            // One item is allowed even with a zero budget, exactly as with
            // the existing scheduler. Subsequent items obey the time limit.
            if (result.processed && Clock::now() - start >= budget) break;
            const WorkItem work{expected, next_};
            const bool ok = append(work);
            if (!IsCurrent(expected)) break;
            if (!ok) {
                result.blocked = true;
                break; // append did not commit; retry this same index later
            }
            ++next_;
            ++result.processed;
        }
        result.generationChanged = !IsCurrent(expected);
        result.completed = IsCurrent(expected) && IsComplete();
        return result;
    }

private:
    void AdvanceGeneration() noexcept {
        if (++generation_ == 0) ++generation_;
    }
    Generation generation_ = 0;
    std::size_t count_ = 0;
    std::size_t next_ = 0;
};
} // namespace ApexCasSchedule
