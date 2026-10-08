#pragma once
// Offline-testable TS3 x86 read-only CALL-rel32 candidate cross-reference scan.
//
// This is a BYTE-PATTERN OBSERVATION ONLY. An 0xE8 byte does not prove an
// instruction boundary, the callee's identity, or the x86 Mono ABI.
// No memory writes, hooks, trampolines, third-party patchers or game objects.
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>

namespace Ts3MonoXref {

struct Reference {
    std::uint32_t callerRva = 0;
    std::uint32_t candidateRva = 0;
};

inline bool DecodeDirectCall(const std::uint8_t* code, std::size_t available,
                             std::uint32_t callerRva,
                             std::uint32_t& targetRva) noexcept {
    if (!code || available < 5 || code[0] != 0xE8) return false;
    // Decode little-endian rel32 as a 32-bit *bit pattern*.
    // x86 address arithmetic is modulo 2^32 (negative rel32 values work).
    const std::uint32_t displacement =
        std::uint32_t{code[1]} |
        (std::uint32_t{code[2]} << 8) |
        (std::uint32_t{code[3]} << 16) |
        (std::uint32_t{code[4]} << 24);
    targetRva = callerRva + std::uint32_t{5} + displacement;
    return true;
}

// Scan a contiguous, already safely copied block of loaded PE .text bytes.
// "targets" contains possible function prologue RVAs from a separate scan.
// A caller can prepend exactly the last four *contiguous readable* bytes
// of the preceding block to detect CALLs crossing a chunk boundary.
// This iteration reports no duplicates: no complete CALL can start
// in the preceding block's final four bytes.
// No result is proof of a JIT method's identity or executable control flow.
template<class Emit>
std::size_t VisitDirectCallsToCandidates(
    std::span<const std::uint8_t> bytes,
    std::uint32_t firstByteRva,
    std::span<const std::uint32_t> targets,
    Emit&& report) {
    if (targets.empty() || bytes.size() < 5) return 0;
    std::size_t found = 0;
    for (std::size_t offset = 0; offset <= bytes.size() - 5; ++offset) {
        if (bytes[offset] != 0xE8) continue;
        std::uint32_t destination = 0;
        if (!DecodeDirectCall(bytes.data() + offset, bytes.size() - offset,
                              firstByteRva + static_cast<std::uint32_t>(offset),
                              destination))
            continue;
        for (const auto target : targets) {
            if (destination == target) {
                ++found;
                report(Reference{
                    firstByteRva + static_cast<std::uint32_t>(offset),
                    target
                });
                break;
            }
        }
    }
    return found;
}

} // namespace Ts3MonoXref
