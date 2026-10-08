// Faster Create-a-Style: native pattern-thumbnail cache.
//
// Reverse engineered from the official 1.67 gameplay.package / scripts.package:
//   UI.dll: Sims3.UI.CAS.CASCompositorController.PopulateMaterialsBinGridTimeslicedTask
//   Sims3Metadata.dll: Pattern.BuildPatternCache / GetPatternsForCategory
//   SimIFace.dll: ObjectDesigner.GetPatternThumbnail -> IWorld.ObjectDesigner_GetPatternThumbnail
//
// The CASt browser already yields once per pattern, but it can still ask ObjectDesigner for the same finished preview
// again. We intercept Mono's internal-call resolver and substitute only the three ObjectDesigner pattern-thumbnail
// internal calls. The original native target is always retained and remains the source of truth on a cache miss.
//
// Part of Apex Radiance. Research and code by @idavidveiga.
#include "fast_create_a_style.h"
#include "fast_cas_catalog.h"
#include "apex_log.h"
#include "memory_patch.h"
#include "game_version.h"
#include <windows.h>
#include <atomic>
#include <array>
#include <optional>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace FastCreateAStyle {
namespace {

using LookupInternalCall_t = void*(__cdecl*)(void* method);
using GetPatternThumbnail_t = uint32_t(__cdecl*)(void* self, uint64_t compositorId, uint64_t patternHashId, void* data);
using CreateLargePatternThumbnail_t = void(__cdecl*)(void* self, uint64_t compositorId, uint64_t patternHashId, void* data);
using ClearLargePatternThumbnail_t = void(__cdecl*)(void* self);

LookupInternalCall_t g_lookupInternalCall = nullptr;
std::atomic<GetPatternThumbnail_t> g_getOriginal{nullptr};
std::atomic<CreateLargePatternThumbnail_t> g_createOriginal{nullptr};
std::atomic<ClearLargePatternThumbnail_t> g_clearOriginal{nullptr};
std::atomic<bool> g_running{false};
bool g_hookInstalled = false;
std::mutex g_resolverMutex;
unsigned g_resolverClients = 0;

// Old Mono used by TS3 (x86). These are runtime object layout offsets, not game object offsets.
constexpr size_t kMethodKlass = 0x08;
constexpr size_t kMethodName = 0x18;
constexpr size_t kClassName = 0x34;
constexpr size_t kClassNamespace = 0x38;
// MonoArray on a 32-bit Mono runtime: MonoObject (8), bounds ptr (4), max_length (4), vector (offset 16).
// The previous offset 0x08 incorrectly read the bounds pointer as the length.
constexpr size_t kArrayLength = 0x0C;
constexpr size_t kArrayData = 0x10;

constexpr size_t kMaxEntries = 2048;
constexpr uint32_t kMaxArrayBytes = 4u * 1024u * 1024u; // a CASt thumbnail should be far smaller; corrupt layouts fail closed.

struct Key {
    uint64_t compositorId = 0;
    uint64_t patternHashId = 0;
    uint64_t inputHash = 0;
    uint32_t length = 0;

    bool operator==(const Key& o) const {
        return compositorId == o.compositorId && patternHashId == o.patternHashId &&
               inputHash == o.inputHash && length == o.length;
    }
};

struct KeyHash {
    size_t operator()(const Key& k) const noexcept {
        uint64_t h = k.compositorId ^ (k.patternHashId + 0x9E3779B97F4A7C15ull + (k.compositorId << 6) + (k.compositorId >> 2));
        h ^= k.inputHash + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        h ^= static_cast<uint64_t>(k.length) * 0x9E3779B185EBCA87ull;
        return static_cast<size_t>(h ^ (h >> 32));
    }
};

std::mutex g_cacheMutex;
std::unordered_set<Key, KeyHash> g_cache;

std::atomic<uint64_t> g_calls{0}, g_hits{0}, g_stores{0}, g_clears{0};
std::atomic<uint64_t> g_resolverMatches{0};

// Do not send raw Mono pointers to strcmp(): corrupt/unknown runtime layouts may not
// reference a NUL-terminated string. Copy small printable identifiers under SEH first.
bool CopyIdentifier(const char* src, char* dest, size_t capacity) {
    if (!src || !dest || capacity < 2) return false;
    __try {
        for (size_t i = 0; i + 1 < capacity; ++i) {
            const unsigned char c = static_cast<unsigned char>(src[i]);
            if (c == 0) { dest[i] = '\0'; return i > 0; }
            if (c < 0x20 || c > 0x7e) return false;
            dest[i] = static_cast<char>(c);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return false; // oversized identifiers are not trustworthy
}

bool ReadMethodIdentity(void* method, char* nameSpace, char* className, char* methodName, size_t capacity) {
    if (!method || !nameSpace || !className || !methodName || capacity < 2) return false;
    nameSpace[0] = className[0] = methodName[0] = '\0';
    const char* ns = nullptr;
    const char* klassName = nullptr;
    const char* methodText = nullptr;
    __try {
        void* const klass = *reinterpret_cast<void**>(static_cast<uint8_t*>(method) + kMethodKlass);
        if (!klass) return false;
        methodText = *reinterpret_cast<const char**>(static_cast<uint8_t*>(method) + kMethodName);
        klassName = *reinterpret_cast<const char**>(static_cast<uint8_t*>(klass) + kClassName);
        ns = *reinterpret_cast<const char**>(static_cast<uint8_t*>(klass) + kClassNamespace);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return CopyIdentifier(ns, nameSpace, capacity) &&
           CopyIdentifier(klassName, className, capacity) &&
           CopyIdentifier(methodText, methodName, capacity);
}

bool ArrayInfo(void* array, uint32_t& length, uint8_t*& bytes) {
    length = 0;
    bytes = nullptr;
    if (!array) return false;
    __try {
        const int32_t n = *reinterpret_cast<const int32_t*>(static_cast<const uint8_t*>(array) + kArrayLength);
        if (n < 0 || static_cast<uint32_t>(n) > kMaxArrayBytes) return false;
        length = static_cast<uint32_t>(n);
        bytes = static_cast<uint8_t*>(array) + kArrayData;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t HashBytes(const uint8_t* p, uint32_t n, bool& ok) {
    // FNV-1a 64. The hash is of the caller-provided buffer before the game touches it: two calls with the same IDs but
    // materially different request data cannot share an entry.
    uint64_t h = 1469598103934665603ull;
    ok = true;
    __try {
        for (uint32_t i = 0; i < n; ++i) {
            h ^= p[i];
            h *= 1099511628211ull;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
        return 0;
    }
    return h;
}

void ClearCache() {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    g_cache.clear();
    g_clears.fetch_add(1, std::memory_order_relaxed);
}

void Invalidate(uint64_t compositorId, uint64_t patternHashId) {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    for (auto it = g_cache.begin(); it != g_cache.end();) {
        if (it->compositorId == compositorId && it->patternHashId == patternHashId)
            it = g_cache.erase(it);
        else ++it;
    }
}

uint32_t __cdecl Hook_GetPatternThumbnail(void* self, uint64_t compositorId,
                                          uint64_t patternHashId, void* data) {
    const GetPatternThumbnail_t original = g_getOriginal.load(std::memory_order_acquire);
    if (!original) return 0; // unreachable unless the embedded runtime changes unexpectedly
    if (g_running.load(std::memory_order_acquire)) {
        g_calls.fetch_add(1, std::memory_order_relaxed);
        uint32_t length = 0;
        uint8_t* bytes = nullptr;
        if (ArrayInfo(data, length, bytes)) {
            bool hashOk = false;
            const uint64_t inputHash = HashBytes(bytes, length, hashOk);
            if (hashOk) {
                const Key key{compositorId, patternHashId, inputHash, length};
                std::lock_guard<std::mutex> lock(g_cacheMutex);
                if (g_cache.find(key) != g_cache.end()) {
                    // Observe duplicates only. The returned uint32 is a native thumbnail
                    // handle, whose ownership/lifetime is not verified: NEVER replay it.
                    g_hits.fetch_add(1, std::memory_order_relaxed);
                } else {
                    if (g_cache.size() >= kMaxEntries) {
                        g_cache.clear();
                        g_clears.fetch_add(1, std::memory_order_relaxed);
                    }
                    g_cache.insert(key);
                    g_stores.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }
    // Always call the game. Profiling-only until native handle lifetime and
    // the exact TS3 Mono x86 InternalCall ABI have been independently validated.
    return original(self, compositorId, patternHashId, data);
}

void __cdecl Hook_CreateLargePatternThumbnail(void* self, uint64_t compositorId, uint64_t patternHashId, void* data) {
    CreateLargePatternThumbnail_t const original = g_createOriginal.load(std::memory_order_acquire);
    if (!original) return;
    original(self, compositorId, patternHashId, data);
    if (g_running.load(std::memory_order_acquire)) Invalidate(compositorId, patternHashId);
}

void __cdecl Hook_ClearLargePatternThumbnail(void* self) {
    ClearLargePatternThumbnail_t const original = g_clearOriginal.load(std::memory_order_acquire);
    if (original) original(self);
    if (g_running.load(std::memory_order_acquire)) ClearCache();
}

void* __cdecl Hook_LookupInternalCall(void* method) {
    void* const native = g_lookupInternalCall ? g_lookupInternalCall(method) : nullptr;
    if (!native) return native;

    char ns[96], klass[96], name[96];
    if (!ReadMethodIdentity(method, ns, klass, name, sizeof(ns))) return native;
    // Another Performance feature can use the same resolver; never chain a second Detours layer on this entry.
    if (void* replacement = FastCasCatalog::MaybeWrap(ns, klass, name, native); replacement != native)
        return replacement;
    if (!g_running.load(std::memory_order_acquire)) return native;
    if (std::strcmp(ns, "Sims3.SimIFace") != 0 || std::strcmp(klass, "IWorld") != 0) return native;

    if (std::strcmp(name, "ObjectDesigner_GetPatternThumbnail") == 0) {
        GetPatternThumbnail_t expected = nullptr;
        g_getOriginal.compare_exchange_strong(expected, reinterpret_cast<GetPatternThumbnail_t>(native));
        if (g_getOriginal.load(std::memory_order_acquire) != reinterpret_cast<GetPatternThumbnail_t>(native)) {
            LOG_WARNING("[FastCreateAStyle] GetPatternThumbnail resolved to a different native target; leaving this lookup untouched");
            return native;
        }
        g_resolverMatches.fetch_add(1, std::memory_order_relaxed);
        LOG_INFO(std::format("[FastCreateAStyle] IWorld::ObjectDesigner_GetPatternThumbnail -> {:#010x}; cache wrapper installed",
                             reinterpret_cast<uintptr_t>(native)));
        return reinterpret_cast<void*>(&Hook_GetPatternThumbnail);
    }
    if (std::strcmp(name, "ObjectDesigner_CreateLargePatternThumbnail") == 0) {
        CreateLargePatternThumbnail_t expected = nullptr;
        g_createOriginal.compare_exchange_strong(expected, reinterpret_cast<CreateLargePatternThumbnail_t>(native));
        if (g_createOriginal.load(std::memory_order_acquire) != reinterpret_cast<CreateLargePatternThumbnail_t>(native)) return native;
        g_resolverMatches.fetch_add(1, std::memory_order_relaxed);
        return reinterpret_cast<void*>(&Hook_CreateLargePatternThumbnail);
    }
    if (std::strcmp(name, "ObjectDesigner_ClearLargePatternThumbnail") == 0) {
        ClearLargePatternThumbnail_t expected = nullptr;
        g_clearOriginal.compare_exchange_strong(expected, reinterpret_cast<ClearLargePatternThumbnail_t>(native));
        if (g_clearOriginal.load(std::memory_order_acquire) != reinterpret_cast<ClearLargePatternThumbnail_t>(native)) return native;
        g_resolverMatches.fetch_add(1, std::memory_order_relaxed);
        return reinterpret_cast<void*>(&Hook_ClearLargePatternThumbnail);
    }
    return native;
}

// Experimental EA 1.69 resolver gate. The player's live TS3.exe log from
// 2026-10-08 established exactly one mono_lookup_internal_call candidate:
// loaded-image RVA 0xA826A0, including its 13-byte original prologue and
// FOUR real x86 CALL rel32 references. The callers each push a MonoMethod*
// and restore ESP by 4 bytes, which establishes the resolver's cdecl shape.
// This is specifically NOT a JIT method compiler or a Hair UI hook.
// The native ICASUtils *callee* ABI is separately experimental, so normal
// builds cannot install this hook.
std::optional<uintptr_t> VerifiedExperimentalResolver(std::string* error) {
#ifndef APEX_CAS_PRESET_CACHE_PILOT
    if (error) *error = "Native CAS ICall pilot is not compiled in this build";
    return std::nullopt;
#else
    if (g_gameVersion != GameVersion::EA || g_exeTimestamp != 0x6707155Cu) {
        if (error) *error = "Native CAS ICall pilot accepts EA 1.69.47 only";
        return std::nullopt;
    }
    // Other Mono method replacers alter the call chain and method state.
    // Do not attach a competing detour or silently promise interoperability.
    if (GetModuleHandleW(L"MonoPatcher.asi") ||
        GetModuleHandleW(L"Sims3MonoModder.asi")) {
        if (error) *error =
            "Remove MonoPatcher.asi / Sims3MonoModder.asi for the isolated "
            "experimental CAS preset cache (no change installed)";
        return std::nullopt;
    }
    const HMODULE module = GetModuleHandleW(nullptr);
    if (!module) return std::nullopt;
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    constexpr uintptr_t kEntryRva = 0xA826A0u;
    constexpr std::array<BYTE,13> kPrologue{{
        0x81,0xEC,0x08,0x08,0x00,0x00,0x53,0x55,
        0x8B,0xAC,0x24,0x14,0x08
    }};
    BYTE* image = nullptr;
    size_t imageSize = 0;
    if (!MemPatch::GetModuleInfo(module, &image, &imageSize) ||
        kEntryRva > imageSize || kPrologue.size() > imageSize-kEntryRva ||
        base != reinterpret_cast<uintptr_t>(image) ||
        !MemPatch::ValidateBytes(
            reinterpret_cast<const void*>(base+kEntryRva),
            kPrologue.data(),kPrologue.size())) {
        if (error) *error = "EA 1.69 ICall resolver prologue differs; original unchanged";
        return std::nullopt;
    }
    // Exact EA 1.69 caller RVAs from the user-provided in-game native log.
    // Check true x86 CALL targets, not random byte-pattern coincidences.
    constexpr std::array<uintptr_t,4> kCallRvas{{
        0x98A28Cu,0xA6454Cu,0xA84DDFu,0xA9931Eu
    }};
    for (uintptr_t rva : kCallRvas) {
        if (rva>imageSize || 5>imageSize-rva) return std::nullopt;
        std::array<BYTE,5> call{};
        if (!MemPatch::ReadBytes(base+rva,call.data(),call.size()) ||
            call[0]!=0xE8) {
            if (error) *error = "EA 1.69 ICall resolver caller bytes differ";
            return std::nullopt;
        }
        int32_t rel=0;
        std::memcpy(&rel,call.data()+1,sizeof(rel));
        if (static_cast<intptr_t>(rva+5)+static_cast<intptr_t>(rel) !=
            static_cast<intptr_t>(kEntryRva)) {
            if (error) *error = "EA 1.69 ICall resolver caller target differs";
            return std::nullopt;
        }
    }
    LOG_INFO(std::format(
        "[CAS ICall Pilot] EA169 resolver checked: RVA {:#x}, 13 original "
        "bytes, 4 independent caller targets. Experimental callee ABI.",
        kEntryRva));
    return base+kEntryRva;
#endif
}

} // namespace

bool AcquireResolver(std::string* error) {
    std::lock_guard<std::mutex> guard(g_resolverMutex);
    if (g_resolverClients) {
        ++g_resolverClients;
        return true;
    }
    // A failed detach leaves the resolver hook in place but pass-through while off. Re-enable that existing layer
    // instead of trying to attach a second Detours layer to the same entry.
    if (g_hookInstalled) { g_resolverClients = 1; return true; }

    const auto resolved = VerifiedExperimentalResolver(error);
    if (!resolved) return false;

    g_lookupInternalCall = reinterpret_cast<LookupInternalCall_t>(*resolved);
    {
        std::lock_guard<std::recursive_mutex> transaction(DetourBatch::Lock());
        if (!DetourBatch::InstallHooks({{reinterpret_cast<void**>(&g_lookupInternalCall), reinterpret_cast<void*>(&Hook_LookupInternalCall)}})) {
            if (error) *error = "Could not hook the Create-a-Style internal-call resolver";
            g_lookupInternalCall = nullptr;
            return false;
        }
    }

    g_hookInstalled = true;
    g_resolverClients = 1;
    LOG_INFO(std::format("[FastCreateAStyle] Started: mono_lookup_internal_call at {:#010x}; waiting for UI.dll to request pattern thumbnails",
                         *resolved));
    return true;
}

void ReleaseResolver() {
    std::lock_guard<std::mutex> guard(g_resolverMutex);
    if (!g_resolverClients) return;
    if (--g_resolverClients) return;
    if (g_hookInstalled && g_lookupInternalCall) {
        if (DetourBatch::RemoveHooks({{reinterpret_cast<void**>(&g_lookupInternalCall),
                                       reinterpret_cast<void*>(&Hook_LookupInternalCall)}})) {
            g_hookInstalled = false;
        } else {
            LOG_WARNING("[FastCreateAStyle] Resolver detach failed; retained as pass-through");
        }
    }
}

bool Start(std::string* error) {
#ifndef APEX_ENABLE_UNVERIFIED_TS3_MONO_ICALLS
    // The original game's embedded Mono InternalCall x86 ABI has not been verified against TS3W.exe.
    // Prevent even manual enabling from installing an untested native detour in a regular build.
    if (error) *error = "CASt thumbnail profiling unavailable until the native TS3 Mono ABI is verified";
    return false;
#endif
    if (g_running.load(std::memory_order_acquire)) return true;
    if (!AcquireResolver(error)) return false;
    g_running.store(true, std::memory_order_release);
    return true;
}

void Stop() {
    if (!g_running.exchange(false, std::memory_order_acq_rel)) return;
    ClearCache();
    ReleaseResolver();
    LOG_INFO(std::format("[FastCreateAStyle] Profiling stopped: {} requests, {} repeated inputs, {} distinct inputs (no native thumbnail handles reused)",
                         g_calls.load(), g_hits.load(), g_stores.load()));
}

bool Running() { return g_running.load(std::memory_order_acquire); }

std::string StatusText() {
    if (!Running()) return "off";
    if (!g_getOriginal.load(std::memory_order_acquire))
        return "waiting for CASt native thumbnail requests";
    size_t entries = 0;
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        entries = g_cache.size();
    }
    return std::format("profiling only: {} of {} requests repeated; {} signatures observed; thumbnails not replayed",
                       g_hits.load(), g_calls.load(), entries);
}

} // namespace FastCreateAStyle
