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
#include "apex_log.h"
#include "memory_patch.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <string_view>
#include <unordered_map>
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

// Old Mono used by TS3 (x86). These are runtime object layout offsets, not game object offsets.
constexpr size_t kMethodKlass = 0x08;
constexpr size_t kMethodName = 0x18;
constexpr size_t kClassName = 0x34;
constexpr size_t kClassNamespace = 0x38;
constexpr size_t kArrayLength = 0x08;
constexpr size_t kArrayData = 0x10;

constexpr size_t kMaxEntries = 2048;
constexpr size_t kMaxBytes = 64u * 1024u * 1024u;
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

struct Entry {
    uint32_t result = 0;
    std::vector<uint8_t> bytes;
};

std::mutex g_cacheMutex;
std::unordered_map<Key, Entry, KeyHash> g_cache;
size_t g_cacheBytes = 0;

std::atomic<uint64_t> g_calls{0}, g_hits{0}, g_misses{0}, g_stores{0}, g_clears{0}, g_bytesReused{0};
std::atomic<uint64_t> g_resolverMatches{0};

bool ReadMethodIdentity(void* method, const char*& nameSpace, const char*& className, const char*& methodName) {
    nameSpace = className = methodName = nullptr;
    if (!method) return false;
    __try {
        void* const klass = *reinterpret_cast<void**>(static_cast<uint8_t*>(method) + kMethodKlass);
        if (!klass) return false;
        methodName = *reinterpret_cast<const char**>(static_cast<uint8_t*>(method) + kMethodName);
        className = *reinterpret_cast<const char**>(static_cast<uint8_t*>(klass) + kClassName);
        nameSpace = *reinterpret_cast<const char**>(static_cast<uint8_t*>(klass) + kClassNamespace);
        return methodName && className && nameSpace;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        nameSpace = className = methodName = nullptr;
        return false;
    }
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

bool CopyFromGame(const uint8_t* src, uint8_t* dst, uint32_t n) {
    if (!n) return true;
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CopyToGame(uint8_t* dst, const uint8_t* src, uint32_t n) {
    if (!n) return true;
    __try {
        std::memcpy(dst, src, n);
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
    g_cacheBytes = 0;
    g_clears.fetch_add(1, std::memory_order_relaxed);
}

void Invalidate(uint64_t compositorId, uint64_t patternHashId) {
    std::lock_guard<std::mutex> lock(g_cacheMutex);
    for (auto it = g_cache.begin(); it != g_cache.end();) {
        if (it->first.compositorId == compositorId && it->first.patternHashId == patternHashId) {
            g_cacheBytes -= it->second.bytes.size();
            it = g_cache.erase(it);
        } else {
            ++it;
        }
    }
}

uint32_t __cdecl Hook_GetPatternThumbnail(void* self, uint64_t compositorId, uint64_t patternHashId, void* data) {
    GetPatternThumbnail_t const original = g_getOriginal.load(std::memory_order_acquire);
    if (!original) return 0;
    if (!g_running.load(std::memory_order_acquire)) return original(self, compositorId, patternHashId, data);

    g_calls.fetch_add(1, std::memory_order_relaxed);
    uint32_t length = 0;
    uint8_t* bytes = nullptr;
    if (!ArrayInfo(data, length, bytes)) return original(self, compositorId, patternHashId, data);

    bool hashOk = false;
    const uint64_t inputHash = HashBytes(bytes, length, hashOk);
    if (!hashOk) return original(self, compositorId, patternHashId, data);
    const Key key{compositorId, patternHashId, inputHash, length};

    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        const auto it = g_cache.find(key);
        if (it != g_cache.end() && it->second.bytes.size() == length &&
            CopyToGame(bytes, it->second.bytes.data(), length)) {
            g_hits.fetch_add(1, std::memory_order_relaxed);
            g_bytesReused.fetch_add(length, std::memory_order_relaxed);
            return it->second.result;
        }
    }

    g_misses.fetch_add(1, std::memory_order_relaxed);
    const uint32_t result = original(self, compositorId, patternHashId, data);

    // Snapshot only successful calls. Copy outside the cache lock: touching Mono memory while holding our mutex could
    // stall another thumbnail request for no reason.
    if (result && length) {
        std::vector<uint8_t> snapshot(length);
        if (CopyFromGame(bytes, snapshot.data(), length)) {
            std::lock_guard<std::mutex> lock(g_cacheMutex);
            if (g_cache.size() >= kMaxEntries || g_cacheBytes + snapshot.size() > kMaxBytes) {
                g_cache.clear();
                g_cacheBytes = 0;
                g_clears.fetch_add(1, std::memory_order_relaxed);
            }
            auto [it, inserted] = g_cache.emplace(key, Entry{result, std::move(snapshot)});
            if (inserted) {
                g_cacheBytes += it->second.bytes.size();
                g_stores.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    return result;
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
    if (!native || !g_running.load(std::memory_order_acquire)) return native;

    const char *ns = nullptr, *klass = nullptr, *name = nullptr;
    if (!ReadMethodIdentity(method, ns, klass, name)) return native;
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

// Steam 1.67.2 address and an address-independent signature for the same old-Mono routine. On EA/unknown builds
// GameAddress uses the pattern and then validates the exact prologue before any write.
const GameAddress kLookupInternalCall{
    "mono_lookup_internal_call",
    {{GameVersion::Steam, 0x00E82680}},
    "81 EC 08 08 00 00 53 55 8B AC 24 14 08",
    0,
    {0x81, 0xEC, 0x08, 0x08, 0x00, 0x00, 0x53, 0x55, 0x8B, 0xAC, 0x24, 0x14, 0x08}
};

} // namespace

bool Start(std::string* error) {
    if (g_running.load(std::memory_order_acquire)) return true;

    const auto resolved = kLookupInternalCall.Resolve();
    if (!resolved) {
        if (error) *error = "Create-a-Style internal-call resolver was not found on this game build";
        return false;
    }

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
    g_running.store(true, std::memory_order_release);
    LOG_INFO(std::format("[FastCreateAStyle] Started: mono_lookup_internal_call at {:#010x}; waiting for UI.dll to request pattern thumbnails",
                         *resolved));
    return true;
}

void Stop() {
    if (!g_running.exchange(false, std::memory_order_acq_rel) && !g_hookInstalled) return;
    ClearCache();

    if (g_hookInstalled && g_lookupInternalCall) {
        std::lock_guard<std::recursive_mutex> transaction(DetourBatch::Lock());
        if (!DetourBatch::RemoveHooks({{reinterpret_cast<void**>(&g_lookupInternalCall), reinterpret_cast<void*>(&Hook_LookupInternalCall)}}))
            LOG_WARNING("[FastCreateAStyle] mono_lookup_internal_call detour could not be removed; its wrapper remains pass-through");
    }
    g_hookInstalled = false;
    LOG_INFO(std::format("[FastCreateAStyle] Stopped: {} calls, {} cache hits, {} misses, {} entries stored, {} bytes reused",
                         g_calls.load(), g_hits.load(), g_misses.load(), g_stores.load(), g_bytesReused.load()));
}

bool Running() { return g_running.load(std::memory_order_acquire); }

std::string StatusText() {
    if (!Running()) return "off";
    if (!g_getOriginal.load(std::memory_order_acquire))
        return "waiting for Create-a-Style to request a pattern thumbnail";

    size_t entries = 0, bytes = 0;
    {
        std::lock_guard<std::mutex> lock(g_cacheMutex);
        entries = g_cache.size();
        bytes = g_cacheBytes;
    }
    const uint64_t calls = g_calls.load(std::memory_order_relaxed);
    const uint64_t hits = g_hits.load(std::memory_order_relaxed);
    return std::format("{} of {} pattern thumbnails reused; {} cached ({:.1f} MB)", hits, calls, entries,
                       static_cast<double>(bytes) / (1024.0 * 1024.0));
}

} // namespace FastCreateAStyle
