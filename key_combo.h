#pragma once
// A shortcut made of keys held together (preset shortcuts): allocation-free chords, including simultaneous letter keys.
#include <array>
#include <cstdint>
#include <bit>

struct KeyCombo {
    std::array<std::uint64_t, 4> keys{};
    static unsigned Normalize(unsigned vk) {
        if (vk == 0xA0 || vk == 0xA1) return 0x10; // Shift
        if (vk == 0xA2 || vk == 0xA3) return 0x11; // Ctrl
        if (vk == 0xA4 || vk == 0xA5) return 0x12; // Alt
        return vk;
    }
    void Add(unsigned vk) { vk = Normalize(vk); if (vk > 6 && vk < 256) keys[vk / 64] |= std::uint64_t{1} << (vk % 64); }
    bool Has(unsigned vk) const { return vk < 256 && (keys[vk / 64] & (std::uint64_t{1} << (vk % 64))); }
    unsigned Count() const { unsigned n = 0; for (auto k : keys) n += std::popcount(k); return n; }
    bool Empty() const { return Count() == 0; }
    bool HasMainKey() const {
        for (unsigned k = 7; k < 256; ++k) if (k != 0x10 && k != 0x11 && k != 0x12 && Has(k)) return true;
        return false;
    }
    // Letters, digits, Space and punctuation without Ctrl or Alt also type into the game's name fields (Sim, lot, save)
    bool TypingOnly() const {
        if (!HasMainKey() || Has(0x11) || Has(0x12)) return false;
        for (unsigned k = 7; k < 256; ++k) {
            if (k == 0x10 || !Has(k)) continue;
            const bool typing = (k >= 'A' && k <= 'Z') || (k >= '0' && k <= '9') || k == 0x20 ||
                                (k >= 0xBA && k <= 0xC0) || (k >= 0xDB && k <= 0xDF) || k == 0xE2;
            if (!typing) return false;
        }
        return true;
    }
    template<class Down> bool Matches(unsigned pressed, Down down) const {
        if (!Has(Normalize(pressed)) || !HasMainKey()) return false;
        // Modifiers are exact: plain A must never fire for Ctrl+A.
        for (unsigned k : {0x10u, 0x11u, 0x12u}) if (Has(k) != bool(down(k))) return false;
        for (unsigned k = 7; k < 256; ++k) if (Has(k) && !down(k)) return false;
        return true;
    }
    bool operator==(const KeyCombo&) const = default;
};
