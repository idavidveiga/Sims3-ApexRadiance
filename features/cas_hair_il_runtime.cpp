// Apex-only bridge from guarded Mono memory snapshots to the original CIL
// relocation. Does not install a method hook or make speculative Mono calls.
#include "cas_hair_il_runtime.h"
#include "memory_patch.h"
#include <cstdint>
#include <limits>
#include <vector>

namespace ApexCasNativeIl {
bool PrepareFromVerifiedRuntimeMemory(
    std::uintptr_t originalIL, std::size_t ilBytes,
    std::uintptr_t originalEH, std::size_t ehBytes,
    Variant verifiedVariant,
    bool methodIdentityVerified,
    bool simulationThreadVerified,
    bool runtimeAbiVerified,
    Rewritten& result,
    std::string* error) {
    result={};
    if(!methodIdentityVerified || !simulationThreadVerified ||
       !runtimeAbiVerified || verifiedVariant==Variant::Unknown) {
        if(error)*error="Missing Mono method ownership, simulator thread or ABI proof";
        return false;
    }
    // Exact byte bounds are mandatory. A MonoMethodHeader is not a PE fat
    // method and its EH clauses are not necessarily a serialized 52-byte
    // CLI section: callers MUST provide a confirmed original CLI EH blob.
    if(ilBytes!=1621 || ehBytes!=52 || !originalIL || !originalEH ||
       originalIL>std::numeric_limits<std::uintptr_t>::max()-ilBytes ||
       originalEH>std::numeric_limits<std::uintptr_t>::max()-ehBytes) {
        if(error)*error="Unsupported original CIL/EH buffer bounds";
        return false;
    }
    std::vector<std::uint8_t> il(ilBytes), eh(ehBytes);
    if(!MemPatch::ReadBytes(originalIL,il.data(),il.size()) ||
       !MemPatch::ReadBytes(originalEH,eh.data(),eh.size())) {
        if(error)*error="Original managed CIL/EH not readable";
        return false;
    }
    // BuildHairParent checks both original SHA-256 digests, exact
    // instruction call sites, try/finally scopes and independent output
    // SHA-256 golden references. No writable game memory is touched.
    return BuildHairParent(il,eh,verifiedVariant,result,error);
}
} // namespace ApexCasNativeIl
