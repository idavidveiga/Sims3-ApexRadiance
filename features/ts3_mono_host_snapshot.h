#pragma once
// Apex native, strictly READ-ONLY inspection of the embedded x86 TS3
// MonoScriptHost's readiness. The verified InitHeap instruction
// "83 3D <global> 00" reads a process-global pointer to the ScriptHost;
// its domain pointer is held at host+0xACC in old TS3 Mono.
//
// This is structural evidence ONLY: it does not prove either Mono lookup
// function's ABI, enable a hook, or authorize invoking managed methods.
// Only ephemeral pointer values are returned; callers must never retain
// or call them across a GC / world / simulation lifecycle change.
#include "ts3_cas_mono_sites.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace ApexCasMono {

enum class HostStatus : std::uint8_t {
    NotFound, InvalidExeBounds, InvalidInitHeap, GlobalOutsideImage,
    UnreadableGlobal, NotReady, UnreadableHost, MissingDomain, Ready
};

struct HostSnapshot {
    HostStatus status = HostStatus::NotFound;
    std::uint32_t globalRva = 0;
    std::uint32_t host = 0;
    std::uint32_t domain = 0;
    bool Ready() const noexcept { return status == HostStatus::Ready; }
};

inline constexpr std::uint32_t kMonoHostDomainOffset = 0xACCu;

// read(address, out, length) must copy the entire span or return false.
// x86 pointers and PE32 integer fields are little-endian. No direct
// pointer dereference, native function call, or memory mutation occurs.
template <class Read>
HostSnapshot InspectMonoHost(std::uint32_t imageBase,
                             std::uint32_t imageSize,
                             std::uint32_t initHeapAddress,
                             Read&& read) {
    HostSnapshot result;
    if (!initHeapAddress) return result;
    // Guard uint32 overflows without depending on a Windows PE header.
    const std::uint64_t imageEnd =
        static_cast<std::uint64_t>(imageBase) + imageSize;
    if (!imageBase || imageSize < 0x1000u ||
        imageEnd > static_cast<std::uint64_t>(UINT32_MAX) + 1u) {
        result.status = HostStatus::InvalidExeBounds;
        return result;
    }
    const auto inImage = [&](std::uint32_t address, std::size_t n) noexcept {
        return static_cast<std::uint64_t>(address) >= imageBase &&
               static_cast<std::uint64_t>(address) + n <= imageEnd;
    };
    const auto& pattern =
        kBridgePatterns[static_cast<std::size_t>(BridgeSite::ScriptHostInitHeap)];
    std::array<std::uint8_t, 15> prologue{};
    if (prologue.size() != pattern.length ||
        !inImage(initHeapAddress, prologue.size()) ||
        !read(initHeapAddress, prologue.data(), prologue.size()) ||
        !MatchesPattern(prologue, 0, pattern)) {
        result.status = HostStatus::InvalidInitHeap;
        return result;
    }

    std::uint32_t globalAddress = 0;
    std::memcpy(&globalAddress, prologue.data() + 2, sizeof(globalAddress));
    if (!inImage(globalAddress, sizeof(std::uint32_t)) ||
        (globalAddress & 3u) != 0u) {
        result.status = HostStatus::GlobalOutsideImage;
        return result;
    }
    result.globalRva = globalAddress - imageBase;
    if (!read(globalAddress, &result.host, sizeof(result.host))) {
        result.status = HostStatus::UnreadableGlobal;
        return result;
    }
    if (!result.host) {
        result.status = HostStatus::NotReady;
        return result;
    }
    if ((result.host & 3u) != 0 ||
        result.host > UINT32_MAX - kMonoHostDomainOffset -
                          static_cast<std::uint32_t>(sizeof(std::uint32_t))) {
        result.status = HostStatus::UnreadableHost;
        result.host = 0;
        return result;
    }
    if (!read(result.host + kMonoHostDomainOffset, &result.domain,
              sizeof(result.domain))) {
        result.status = HostStatus::UnreadableHost;
        result.host = 0;
        return result;
    }
    if (!result.domain || (result.domain & 3u) != 0) {
        result.status = HostStatus::MissingDomain;
        result.host = 0;
        result.domain = 0;
        return result;
    }
    result.status = HostStatus::Ready;
    return result;
}

inline const char* HostStatusName(HostStatus state) noexcept {
    switch (state) {
    case HostStatus::NotFound: return "no unique InitHeap function";
    case HostStatus::InvalidExeBounds: return "unsupported module bounds";
    case HostStatus::InvalidInitHeap: return "InitHeap bytes do not match";
    case HostStatus::GlobalOutsideImage: return "ScriptHost global pointer outside executable image";
    case HostStatus::UnreadableGlobal: return "ScriptHost global unreadable";
    case HostStatus::NotReady: return "ScriptHost not initialized";
    case HostStatus::UnreadableHost: return "ScriptHost object unreadable";
    case HostStatus::MissingDomain: return "Mono domain unavailable";
    case HostStatus::Ready: return "ScriptHost/domain structurally available (not ABI verified)";
    }
    return "invalid status";
}

} // namespace ApexCasMono
