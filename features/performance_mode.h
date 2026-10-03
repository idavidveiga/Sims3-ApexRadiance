#pragma once
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <vector>

namespace PerformanceMode {
inline std::atomic<bool> enabled{true};
inline bool Enabled() { return enabled.load(std::memory_order_relaxed); }
inline void SetEnabled(bool on) { enabled.store(on, std::memory_order_relaxed); }

// Read current device values, not cached state. Adjacent registers share one query.
template <typename Read> void ReadConstantPair(Read read, unsigned a, unsigned b, float* outA, float* outB, bool optimized) {
    if (optimized && ((a < b && b - a == 1) || (b < a && a - b == 1))) {
        float rows[8];
        if (read(std::min(a, b), rows, 2)) {
            std::memcpy(outA, rows + (a < b ? 0 : 4), 4 * sizeof(float));
            std::memcpy(outB, rows + (a < b ? 4 : 0), 4 * sizeof(float));
            return;
        }
    }
    read(a, outA, 1);
    read(b, outB, 1);
}

// Caller clears the index before erasing entries. Inserts preserve map-node addresses.
template <typename Map> typename Map::value_type* EntryAt(Map& map, std::size_t cursor, std::vector<typename Map::value_type*>& index, bool optimized) {
    if (map.empty()) return nullptr;
    cursor %= map.size();
    if (!optimized) {
        auto it = map.begin();
        std::advance(it, cursor);
        return &*it;
    }
    if (index.size() != map.size()) {
        index.clear();
        index.reserve(map.size());
        for (auto& entry : map) index.push_back(&entry);
    }
    return index[cursor];
}

template <std::size_t N, std::size_t M> void BuildLampRows(const float (&selected)[M][4], int count, float (&rows)[1 + 2 * N][4]) {
    static_assert(M >= 32 && N <= 16);
    std::memset(rows, 0, sizeof(rows));
    rows[0][3] = 1e-4f;
    for (std::size_t k = 0; k < N; k++) rows[1 + 2 * k][0] = rows[1 + 2 * k][2] = 1e6f;
    for (int k = 0; k < count && k < static_cast<int>(N); k++) {
        const float* pr = selected[k];
        const float* col = selected[16 + k];
        const float r = pr[3] > 0.1f ? pr[3] : 0.1f;
        rows[1 + 2 * k][0] = pr[0];
        rows[1 + 2 * k][1] = pr[1];
        rows[1 + 2 * k][2] = pr[2];
        rows[1 + 2 * k][3] = 1.0f / (r * r);
        rows[2 + 2 * k][0] = col[0];
        rows[2 + 2 * k][1] = col[1];
        rows[2 + 2 * k][2] = col[2];
    }
}
}
