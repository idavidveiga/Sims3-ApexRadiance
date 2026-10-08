#pragma once
// Apex ASI only: safely snapshot original Hair/Hats managed CIL from a
// separately verified MonoMethodHeader on the real simulation thread.
// No raw game pointer is stored. No managed method is replaced here.
// The distinct EA 1.69 / alternate UI.dll method token must already have
// passed ts3_cas_mono_discovery + original method-identity verification.
#include "cas_hair_native_il.h"
#include <cstddef>
#include <cstdint>
#include <string>

namespace ApexCasNativeIl {
// Takes raw CIL/EH byte pointers supplied by a VERIFIED Mono-method header
// adapter, copies them via MemPatch::ReadBytes, checks SHA256 and creates an
// owned candidate. Fail closed if any byte/ABI/identity/thread gate is missing.
//
// Does NOT call a Mono function, install a detour, or change managed memory.
// Output lifetime is owned by the caller; never pass .il.data() to MINT
// without an independent, validated ownership/translation integration.
bool PrepareFromVerifiedRuntimeMemory(
    std::uintptr_t originalIL, std::size_t ilBytes,
    std::uintptr_t originalEH, std::size_t ehBytes,
    Variant verifiedVariant,
    bool methodIdentityVerified,
    bool simulationThreadVerified,
    bool runtimeAbiVerified,
    Rewritten& result,
    std::string* error=nullptr);
} // namespace ApexCasNativeIl
