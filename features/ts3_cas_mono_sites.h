#pragma once
// EA/Steam The Sims 3 x86 native Mono bridge-site EVIDENCE finder.
// Matches well-documented function entry patterns against an already-loaded
// executable image in BOUNDED chunks. Patterns are public factual bytes,
// not dependent on nor copying external mod-loader implementation.
// NO pointer is callable; NO memory is read/dereferenced by this header,
// NO hook/patch. A unique signature is not an ABI proof.
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace ApexCasMono {

enum class BridgeSite : std::uint8_t {
    ScriptHostFindClass, MonoClassGetMethods, ScriptHostProcessTasks,
    ScriptHostInitHeap, Count
};
inline constexpr std::size_t kSiteCount = static_cast<std::size_t>(BridgeSite::Count);

struct BridgePattern {
    BridgeSite site;
    std::string_view name;
    std::string_view bytes; // fixed-width hex byte pairs; ?? is a wildcard
    std::size_t length;
};
// These are function entry candidates from independent TS3 Mono 1.2.3.1
// x86 architecture research. Only an exact match + unique occurrence counts
// as evidence; no site here authorizes a native CALL, jmp, detour or ABI.
// The InitHeap entry contains an absolute pointer to g_ScriptVM at +2.
inline constexpr std::array<BridgePattern, kSiteCount> kBridgePatterns{{
    {BridgeSite::ScriptHostFindClass, "MonoScriptHost::FindClass",
     "83 EC 1C 53 8B 5C 24 24 33 C0 55 56 89 44", 14},
    {BridgeSite::MonoClassGetMethods, "mono_class_get_methods",
     "57 8B 7C 24 0C 85 FF 75 ?? 33 C0 5F C3 56 8B 74 24 0C F6 46 14 01 75 ?? 56 E8 ?? ?? ?? ?? 83 C4 04 8B 07 85 C0 75 ?? 56 E8 ?? ?? ?? ?? 83 C4 04 83 7E 70", 49},
    {BridgeSite::ScriptHostProcessTasks, "MonoScriptHost::ProcessTasks",
     "55 8B EC 83 E4 F8 83 EC 50 53 55 56 8B D9 8B 83 ?? ?? ?? ?? 8D B3 ?? ?? ?? ?? 57 50 8B CE", 28},
    {BridgeSite::ScriptHostInitHeap, "MonoScriptHost::InitHeap",
     "83 3D ?? ?? ?? ?? 00 74 ?? B0 01 C3 57 8B 3D", 15}
}};
inline constexpr std::size_t kMaxBridgePatternLength = 49;

inline int Hex(char c) noexcept {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

// Pure bounded matcher; no external state, no reads beyond data.
inline bool MatchesPattern(std::span<const std::uint8_t> bytes,
                           std::size_t start, const BridgePattern& pattern) noexcept {
    if (start > bytes.size() || pattern.length > bytes.size() - start) return false;
    std::size_t offset = 0;
    std::size_t pos = 0;
    while (pos < pattern.bytes.size() && offset < pattern.length) {
        while (pos < pattern.bytes.size() && pattern.bytes[pos] == ' ') ++pos;
        if (pos + 1 >= pattern.bytes.size()) return false;
        const char hi = pattern.bytes[pos], lo = pattern.bytes[pos + 1];
        if (!(hi == '?' && lo == '?')) {
            const int a = Hex(hi), b = Hex(lo);
            if (a < 0 || b < 0 || bytes[start + offset] != ((a << 4) | b))
                return false;
        }
        ++offset;
        pos += 2;
        if (pos < pattern.bytes.size() && pattern.bytes[pos] != ' ') return false;
    }
    while (pos < pattern.bytes.size() && pattern.bytes[pos] == ' ') ++pos;
    return offset == pattern.length && pos == pattern.bytes.size();
}

struct BridgeMatch {
    std::size_t count = 0;
    std::uint32_t firstRva = 0;
    bool Unique() const noexcept { return count == 1 && firstRva != 0; }
};

class BridgeSignatureEvidence {
public:
    // chunk contains up to (max_pattern_len-1) bytes of previously read
    // contiguous executable memory, followed by this chunk's bytes.
    // chunkBaseRva is the RVA of chunk[0], not of new work.
    // Only matches with an ending byte in the new portion are counted;
    // therefore a match crossing chunk boundaries is counted exactly once.
    void Observe(std::span<const std::uint8_t> chunk,
                 std::uint32_t chunkBaseRva, std::size_t prefixBytes) noexcept {
        if (prefixBytes > chunk.size()) return;
        for (std::size_t k=0; k<kSiteCount; ++k) {
            const auto& pattern = kBridgePatterns[k];
            if (chunk.size() < pattern.length) continue;
            for (std::size_t i=0; i <= chunk.size()-pattern.length; ++i) {
                if (i + pattern.length <= prefixBytes) continue;
                if (!MatchesPattern(chunk, i, pattern)) continue;
                auto& hit = hits_[k];
                if (hit.count++ == 0) {
                    if (i <= UINT32_MAX - chunkBaseRva)
                        hit.firstRva = chunkBaseRva + static_cast<std::uint32_t>(i);
                }
            }
        }
    }
    const BridgeMatch& Get(BridgeSite site) const noexcept {
        return hits_[static_cast<std::size_t>(site)];
    }
    bool AllUnique() const noexcept {
        for (const auto& hit : hits_) if (!hit.Unique()) return false;
        return true;
    }
private:
    std::array<BridgeMatch, kSiteCount> hits_{};
};

} // namespace ApexCasMono
