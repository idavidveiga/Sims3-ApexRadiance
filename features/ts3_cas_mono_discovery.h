#pragma once
// Native CAS/MINT discovery policy. This is a STRICTLY OPT-IN resolver adapter,
// not a hook and not an address scanner. The caller supplies independently
// verified, simulation-thread-only TS3 Mono FindClass/GetNextMethod callbacks.
// Production code must NOT call those callbacks until their EA 1.69 entry,
// calling convention and script-host lifetime are independently established.
//
// The adapter never executes the target, alters MonoMethod / RuntimeMethod,
// touches ItemGrid or caches pointers beyond this single discovery operation.
#include "ts3_mono_method_identity.h"
#include <cstddef>
#include <cstdint>
#include <utility>

namespace ApexCasMono {

enum class DiscoveryStatus : std::uint8_t {
    Ready,
    Disabled,
    WrongThread,
    UnverifiedRuntime,
    MissingClass,
    BadEnumeration,
    NotFound,
    IdentityMismatch,
    ForeignClass,
    DuplicateTarget,
    TooManyMethods
};

struct DiscoveryGate {
    bool enabled = false;               // opt-in experimental feature
    bool onSimulationThread = false;    // independently verified caller context
    bool runtimeAbiVerified = false;    // native FindClass/EnumerateMethod ABI
    bool uiAssemblyVerified = false;    // original UI.dll ownership/integrity
};

struct Discovery {
    DiscoveryStatus status = DiscoveryStatus::Disabled;
    std::uint32_t method = 0;           // ephemeral MonoMethod*; NEVER a JIT entry
    std::uint32_t runtimeMethod = 0;    // observation only, may be zero
    std::size_t methodsVisited = 0;
};

// Lookup contracts:
// findClass("Sims3.UI.CAS", "CASHair") returns MonoClass* or zero;
// nextMethod(klass, &iterator) returns MonoMethod* or zero at end.
// The callbacks must be invoked ON the game's simulation thread with
// a verified original ABI (not from ImGui/D3D9/render thread).
// read(pointer, destination, length) copies whole span or returns false.
// Invalid, unsupported, duplicate, or modified metadata fails closed.
template <class FindClass, class NextMethod, class Read>
Discovery DiscoverHairPopulate(DiscoveryGate gate, FindClass&& findClass,
                              NextMethod&& nextMethod, Read&& read) {
    Discovery result;
    if (!gate.enabled) return result;
    if (!gate.onSimulationThread) {
        result.status = DiscoveryStatus::WrongThread;
        return result;
    }
    if (!gate.runtimeAbiVerified || !gate.uiAssemblyVerified) {
        result.status = DiscoveryStatus::UnverifiedRuntime;
        return result;
    }

    const std::uint32_t owner = findClass("Sims3.UI.CAS", "CASHair");
    if (!owner) {
        result.status = DiscoveryStatus::MissingClass;
        return result;
    }

    constexpr std::size_t kMaximumClassMethods = 2048;
    // In old Mono the iterator is an OPAQUE cookie: its numeric value
    // need not increase, even though each call must make progress.
    std::uint32_t cursor = 0;
    bool found = false;
    for (std::size_t i = 0; i < kMaximumClassMethods; ++i) {
        const std::uint32_t previous = cursor;
        const std::uint32_t candidate = nextMethod(owner, &cursor);
        if (!candidate) {
            result.status = found ? DiscoveryStatus::Ready :
                                    DiscoveryStatus::NotFound;
            return result;
        }
        ++result.methodsVisited;
        if (cursor == previous) {
            result.status = DiscoveryStatus::BadEnumeration;
            result.method = result.runtimeMethod = 0;
            return result;
        }
        // Cheap token check to avoid reading every method's strings/types.
        std::uint32_t token = 0;
        if (candidate > UINT32_MAX - 4 ||
            !read(candidate + 4, &token, sizeof(token))) {
            result.status = DiscoveryStatus::BadEnumeration;
            result.method = result.runtimeMethod = 0;
            return result;
        }
        if (token != kHairPopulateToken) continue;
        if (found) {
            result.status = DiscoveryStatus::DuplicateTarget;
            result.method = result.runtimeMethod = 0;
            return result;
        }

        std::uint32_t declaringClass = 0;
        if (candidate > UINT32_MAX - 8 ||
            !read(candidate + 8, &declaringClass, sizeof(declaringClass)) ||
            declaringClass != owner) {
            result.status = DiscoveryStatus::ForeignClass;
            return result;
        }
        auto inspected = InspectHairPopulate(candidate, read);
        if (!inspected.Matches()) {
            result.status = DiscoveryStatus::IdentityMismatch;
            return result;
        }
        found = true;
        result.method = candidate;
        result.runtimeMethod = inspected.runtimeMethod;
    }

    // Do not accept a method unless the iterator terminates unambiguously.
    // Returning a match from a truncated enumeration would permit duplicates.
    result.status = DiscoveryStatus::TooManyMethods;
    result.method = result.runtimeMethod = 0;
    return result;
}

} // namespace ApexCasMono
