#pragma once
// Indoor light through openings: the rooms that lost a source room's lamps at their gather gather again a little later
// (level_light_share.cpp ShareInnerLamps). While a player drags a lamp the game takes it out of its room's registry until it
// is put down; a neighbour's gather that falls in between finds no lamp in the source room, and no later edit of the source
// room needs to send it again (13/10 in game: the lamp put down came back in room 0, the next room stayed dark). Such a room
// gathers again after kDelayMs[0], and again with the next delay while it keeps losing them, kTries times at most.
// POD and the standard library only, no game reads: the offline harness (tools/roofed_openings_test) tests it directly.
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <vector>

namespace InnerRetry {

constexpr int kTries = 4;
constexpr uint32_t kDelayMs[kTries] = {700, 1500, 3000, 6000};
constexpr uint32_t kForgetMs = 15000; // a retry sent this long ago whose room never gathered: dropped
constexpr size_t kMax = 128;

struct Key {
    uintptr_t tracker = 0;
    int story = 0, id = 0;
    bool operator==(const Key& o) const { return tracker == o.tracker && story == o.story && id == o.id; }
};
struct Entry {
    Key key;
    uint32_t due = 0, sentAt = 0;
    int tries = 0;         // retries sent so far
    bool pending = false;  // waiting for its due time (else sent, waiting for its gather)
    std::vector<int> lost; // the source rooms whose lamps it lost
};

// The source rooms a gather lost: taken at the previous gather, not at this one (sorted, unique)
inline std::vector<int> Lost(std::vector<int> before, std::vector<int> now) {
    std::sort(before.begin(), before.end());
    before.erase(std::unique(before.begin(), before.end()), before.end());
    std::sort(now.begin(), now.end());
    std::vector<int> out;
    std::set_difference(before.begin(), before.end(), now.begin(), now.end(), std::back_inserter(out));
    return out;
}

inline bool Reached(uint32_t now, uint32_t at) { return static_cast<int32_t>(now - at) >= 0; }

class List {
public:
    // A gather of `k` at `now` (ms) still missing the lamps of `lost` (the sources it took before and not since; empty: it
    // misses none). Returns the delay of the gather it schedules (ms), 0 when none. Once its tries are spent the entry goes
    // (Has() false): the caller forgets those sources.
    uint32_t Gathered(const Key& k, const std::vector<int>& lost, uint32_t now) {
        const auto it = std::find_if(e_.begin(), e_.end(), [&](const Entry& e) { return e.key == k; });
        if (lost.empty()) {
            if (it != e_.end()) e_.erase(it); // its lamps are back (or it never had any)
            return 0;
        }
        if (it == e_.end()) {
            if (e_.size() >= kMax) e_.erase(e_.begin()); // (the oldest)
            Entry e;
            e.key = k, e.due = now + kDelayMs[0], e.pending = true, e.lost = lost;
            e_.push_back(std::move(e));
            return kDelayMs[0];
        }
        if (it->pending) { // already waiting: it waits for these too
            for (int s : lost)
                if (std::find(it->lost.begin(), it->lost.end(), s) == it->lost.end()) it->lost.push_back(s);
            return 0;
        }
        // a retry's gather that lost them again (the lamp still lifted)
        if (it->tries >= kTries) {
            e_.erase(it);
            return 0;
        }
        it->due = now + kDelayMs[it->tries], it->pending = true, it->lost = lost;
        return kDelayMs[it->tries];
    }
    bool Has(const Key& k) const { return std::any_of(e_.begin(), e_.end(), [&](const Entry& e) { return e.key == k; }); }
    // The entries due at `now`, marked sent (their tries counted); sent ones whose room never gathered are dropped
    std::vector<Entry> TakeDue(uint32_t now) {
        std::vector<Entry> out;
        std::erase_if(e_, [&](const Entry& e) { return !e.pending && Reached(now, e.sentAt + kForgetMs); });
        for (Entry& e : e_)
            if (e.pending && Reached(now, e.due)) {
                e.pending = false, e.tries++, e.sentAt = now;
                out.push_back(e);
            }
        return out;
    }
    size_t Size() const { return e_.size(); }

private:
    std::vector<Entry> e_;
};

} // namespace InnerRetry

// The brightness of a lamp shining out of a roofed room (level_light_share.cpp InnerNormScale): the game multiplies an indoor
// room's summed light by its normalisation (room+0x160) and an outdoor room's by nothing, so the lamp's colour in the
// solving room is scaled by the source room's factor over the solving room's (1 for an outdoor one). A factor that is not
// finite or lies outside 0.05 .. 50 counts as 1; the scale stays within 0.05 .. 20.
namespace InnerLight {
inline float Norm(float n) { return std::isfinite(n) && n >= 0.05f && n <= 50.0f ? n : 1.0f; }
inline float NormScale(float src, float dst, bool dstOutdoor) { return std::clamp(Norm(src) / (dstOutdoor ? 1.0f : Norm(dst)), 0.05f, 20.0f); }
} // namespace InnerLight
