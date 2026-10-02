#pragma once
#include <array>
#include <cstdint>

// Non-owning lookup front cache. The caller supplies the same synchronization
// as the authoritative map and clears this before replacing/releasing entries.
template <typename Value> class ShaderLookupCache {
    struct Slot { std::uint64_t key = 0; Value value{}; bool valid = false; };
    std::array<Slot, 32> slots_{};
    static std::size_t Index(std::uint64_t key) {
        return static_cast<std::size_t>((key ^ (key >> 9) ^ (key >> 17)) & 31);
    }
public:
    bool Find(std::uint64_t key, Value& value) const {
        const auto& slot = slots_[Index(key)];
        if (!slot.valid || slot.key != key) return false;
        value = slot.value;
        return true;
    }
    void Store(std::uint64_t key, const Value& value) { slots_[Index(key)] = {key, value, true}; }
    void Clear() { for (auto& slot : slots_) slot.valid = false; }
};
