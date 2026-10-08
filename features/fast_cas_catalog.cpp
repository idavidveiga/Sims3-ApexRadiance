// Create-a-Sim catalogue: bounded cache of native preset metadata lookups.
// Does not cache or borrow UIImage handles. The CAS grid/preset list is still owned by UI.dll.
//
// Verified source assemblies: user-supplied Sims3 gameplay.package and scripts.package.
// UI.dll: CASClothingCategory.PopulateTypesGrid / PopulateGrid, CASHair.PopulateTypesGrid,
// CASMakeup.PopulatePartsGrid and CASTattoo.PopulateTattooGrid.
// SimIFace.dll: Sims3.SimIFace.CAS.ICASUtils.PartDataNumPresets, PartDataGetPresetId,
// PartDataAddDesignPreset and PartDataRemoveDesignPreset.
//
// Part of Apex Radiance. Experimental CAS research by @idavidveiga.
#include "fast_cas_catalog.h"
#include "fast_create_a_style.h"
#include "apex_log.h"
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <unordered_map>

namespace FastCasCatalog {
namespace {
// Native x86 layout of Sims3.SimIFace.ResourceKey: TypeId:uint32, GroupId:uint32, InstanceId:uint64.
struct ResourceKey {
    uint32_t typeId;
    uint32_t groupId;
    uint64_t instanceId;
};
static_assert(sizeof(ResourceKey) == 16);
// The assembly's sequential ResourceKey fields are TypeId:uint32, GroupId:uint32, InstanceId:uint64.
// This verifies only the C++ layout, not how TS3's embedded Mono marshals value types into native ICalls.
static_assert(offsetof(ResourceKey, typeId) == 0);
static_assert(offsetof(ResourceKey, groupId) == 4);
static_assert(offsetof(ResourceKey, instanceId) == 8);

struct PresetIdKey {
    ResourceKey resource;
    uint32_t index;
};
struct KeyHash {
    size_t operator()(const ResourceKey& k) const noexcept {
        const uint64_t h = k.instanceId ^ (uint64_t(k.groupId) << 32) ^ k.typeId;
        return static_cast<size_t>(h ^ (h >> 32));
    }
    size_t operator()(const PresetIdKey& k) const noexcept {
        const size_t h = (*this)(k.resource);
        return h ^ (static_cast<size_t>(k.index) * 0x9E3779B1u);
    }
};
struct KeyEqual {
    bool operator()(const ResourceKey& a, const ResourceKey& b) const noexcept {
        return a.typeId == b.typeId && a.groupId == b.groupId && a.instanceId == b.instanceId;
    }
    bool operator()(const PresetIdKey& a, const PresetIdKey& b) const noexcept {
        return a.index == b.index && (*this)(a.resource,b.resource);
    }
};

using CountFn = uint32_t(__cdecl*)(void*, ResourceKey);
using GetIdFn = uint32_t(__cdecl*)(void*, ResourceKey, uint32_t);
using AddFn = uint32_t(__cdecl*)(void*, ResourceKey, void*);
using RemoveFn = void(__cdecl*)(void*, ResourceKey, uint32_t);
std::atomic<CountFn> g_count{nullptr};
std::atomic<GetIdFn> g_id{nullptr};
std::atomic<AddFn> g_add{nullptr};
std::atomic<RemoveFn> g_remove{nullptr};
std::atomic<bool> g_enabled{false};
std::atomic<bool> g_cacheHealthy{false}; // one mismatch shuts off reuse for the rest of the session
std::atomic<uint64_t> g_countCalls{0}, g_idCalls{0}, g_hits{0}, g_validationFailures{0}, g_invalidations{0};
std::atomic<uint64_t> g_nativeCalls{0}, g_nativeNanoseconds{0}, g_firstHitChecks{0};
constexpr size_t kMaxCountEntries = 4096;
constexpr size_t kMaxIdEntries = 16384;
constexpr auto kTTL = std::chrono::seconds(30);
constexpr uint64_t kCheckEveryHits = 32;

template<class T> struct Cached {
    T result;
    std::chrono::steady_clock::time_point stored;
    bool verified = false; // require two equal real results before skipping any native calls
};
std::mutex g_mutex;
std::unordered_map<ResourceKey, Cached<uint32_t>, KeyHash, KeyEqual> g_counts;
std::unordered_map<PresetIdKey, Cached<uint32_t>, KeyHash, KeyEqual> g_ids;

template<class F>
uint32_t MeasureNative(F&& f) {
    const auto start = std::chrono::steady_clock::now();
    const uint32_t result = f();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    g_nativeCalls.fetch_add(1, std::memory_order_relaxed);
    g_nativeNanoseconds.fetch_add(static_cast<uint64_t>(elapsed), std::memory_order_relaxed);
    return result;
}

void DisableCacheOnMismatch(const char* method) {
    if (!g_cacheHealthy.exchange(false, std::memory_order_acq_rel)) return;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_counts.clear();
        g_ids.clear();
    }
    LOG_WARNING(std::format("[FastCasCatalog] {} produced inconsistent metadata; all CAS cache hits disabled (native pass-through)", method));
}

void Invalidate(ResourceKey key) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_counts.erase(key);
    for (auto it = g_ids.begin(); it != g_ids.end();) {
        if (KeyEqual{}(it->first.resource, key)) it = g_ids.erase(it);
        else ++it;
    }
    g_invalidations.fetch_add(1, std::memory_order_relaxed);
}

uint32_t __cdecl HookCount(void* self, ResourceKey key) {
    const CountFn original = g_count.load(std::memory_order_acquire);
    if (!original) return 0;
    if (!g_enabled.load(std::memory_order_acquire) || !g_cacheHealthy.load(std::memory_order_acquire))
        return original(self,key);
    g_countCalls.fetch_add(1,std::memory_order_relaxed);
    const auto now = std::chrono::steady_clock::now();
    uint32_t result=0;
    bool hit=false, verified=false;
    std::chrono::steady_clock::time_point cachedAt{};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it=g_counts.find(key);
        if(it!=g_counts.end() && (now-it->second.stored)<kTTL) {
            result=it->second.result;
            hit=true;
            verified=it->second.verified;
            cachedAt=it->second.stored;
        }
    }
    if(hit) {
        const auto n=g_hits.fetch_add(1,std::memory_order_relaxed)+1;
        if(verified && n % kCheckEveryHits) return result;
        if(!verified) g_firstHitChecks.fetch_add(1,std::memory_order_relaxed);
        const uint32_t actual=MeasureNative([&] { return original(self,key); });
        if(actual==result) {
            if(!verified) {
                std::lock_guard<std::mutex> lock(g_mutex);
                auto it=g_counts.find(key);
                if(it!=g_counts.end() && it->second.result==result && it->second.stored==cachedAt)
                    it->second.verified=true;
            }
            return result;
        }
        g_validationFailures.fetch_add(1,std::memory_order_relaxed);
        DisableCacheOnMismatch("PartDataNumPresets");
        return actual;
    }
    result=MeasureNative([&] { return original(self,key); });
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if(g_counts.size()>=kMaxCountEntries) g_counts.clear();
        g_counts[key]={result,now};
    }
    return result;
}

uint32_t __cdecl HookId(void* self, ResourceKey key, uint32_t index) {
    const GetIdFn original=g_id.load(std::memory_order_acquire);
    if(!original) return 0;
    if(!g_enabled.load(std::memory_order_acquire) || !g_cacheHealthy.load(std::memory_order_acquire))
        return original(self,key,index);
    g_idCalls.fetch_add(1,std::memory_order_relaxed);
    const PresetIdKey cacheKey{key,index};
    const auto now=std::chrono::steady_clock::now();
    uint32_t result=0;
    bool hit=false, verified=false;
    std::chrono::steady_clock::time_point cachedAt{};
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        auto it=g_ids.find(cacheKey);
        if(it!=g_ids.end() && (now-it->second.stored)<kTTL) {
            result=it->second.result;
            hit=true;
            verified=it->second.verified;
            cachedAt=it->second.stored;
        }
    }
    if(hit) {
        const auto n=g_hits.fetch_add(1,std::memory_order_relaxed)+1;
        if(verified && n % kCheckEveryHits) return result;
        if(!verified) g_firstHitChecks.fetch_add(1,std::memory_order_relaxed);
        const uint32_t actual=MeasureNative([&] { return original(self,key,index); });
        if(actual==result) {
            if(!verified) {
                std::lock_guard<std::mutex> lock(g_mutex);
                auto it=g_ids.find(cacheKey);
                if(it!=g_ids.end() && it->second.result==result && it->second.stored==cachedAt)
                    it->second.verified=true;
            }
            return result;
        }
        g_validationFailures.fetch_add(1,std::memory_order_relaxed);
        DisableCacheOnMismatch("PartDataGetPresetId");
        return actual;
    }
    result=MeasureNative([&] { return original(self,key,index); });
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        if(g_ids.size()>=kMaxIdEntries) g_ids.clear();
        g_ids[cacheKey]={result,now};
    }
    return result;
}

uint32_t __cdecl HookAdd(void* self, ResourceKey key, void* preset) {
    const AddFn original=g_add.load(std::memory_order_acquire);
    if(!original) return 0;
    const uint32_t result=original(self,key,preset);
    if(g_enabled.load(std::memory_order_acquire)) Invalidate(key);
    return result;
}
void __cdecl HookRemove(void* self, ResourceKey key, uint32_t index) {
    const RemoveFn original=g_remove.load(std::memory_order_acquire);
    if (!original) return;
    original(self,key,index);
    if(g_enabled.load(std::memory_order_acquire)) Invalidate(key);
}

template<typename F> void* Bind(std::atomic<F>& slot, F target, void* wrapper, const char* name) {
    F expected=nullptr;
    slot.compare_exchange_strong(expected,target);
    if(slot.load(std::memory_order_acquire)!=target) {
        LOG_WARNING(std::format("[FastCasCatalog] {} native target changed; ignored",name));
        return reinterpret_cast<void*>(target);
    }
    LOG_INFO(std::format("[FastCasCatalog] Resolved {} at {:#010x}",name,reinterpret_cast<uintptr_t>(target)));
    return wrapper;
}
} // namespace

bool Start(std::string* error) {
#ifndef APEX_CAS_PRESET_CACHE_PILOT
    // The original game's embedded Mono InternalCall x86 ABI has not been verified against TS3W.exe.
    // Prevent even manual enabling from installing an untested native detour in a regular build.
    if (error) *error = "CAS preset profiling unavailable until the native TS3 Mono ABI is verified";
    return false;
#endif
    if(g_enabled.load(std::memory_order_acquire)) return true;
    // Resolver is shared with Faster Create-a-Style to prevent a competing Detours hook on the same Mono entry.
    if(!FastCreateAStyle::AcquireResolver(error)) return false;
    g_cacheHealthy.store(true,std::memory_order_release);
    g_enabled.store(true,std::memory_order_release);
    LOG_INFO("[FastCasCatalog] Experimental preset metadata cache enabled; awaiting ICASUtils resolution");
    return true;
}
void Stop() {
    if(!g_enabled.exchange(false,std::memory_order_acq_rel)) return;
    g_cacheHealthy.store(false,std::memory_order_release);
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_counts.clear();
        g_ids.clear();
    }
    FastCreateAStyle::ReleaseResolver();
    LOG_INFO(std::format("[FastCasCatalog] Off: count={} id={} repeated={} firstHitVerified={} nativeCalls={} nativeTime={:.3f}ms invalidations={} discrepancies={}",
        g_countCalls.load(),g_idCalls.load(),g_hits.load(),g_firstHitChecks.load(),g_nativeCalls.load(),
        static_cast<double>(g_nativeNanoseconds.load()) / 1'000'000.0,
        g_invalidations.load(),g_validationFailures.load()));
}
bool Running() { return g_enabled.load(std::memory_order_acquire); }
void* MaybeWrap(const char* ns,const char* klass,const char* name,void* native) {
    if(!Running() || !native || !ns || !klass || !name) return native;
    if(std::strcmp(ns,"Sims3.SimIFace.CAS") || std::strcmp(klass,"ICASUtils")) return native;
    if(!std::strcmp(name,"PartDataNumPresets")) return Bind(g_count,reinterpret_cast<CountFn>(native),reinterpret_cast<void*>(&HookCount),name);
    if(!std::strcmp(name,"PartDataGetPresetId")) return Bind(g_id,reinterpret_cast<GetIdFn>(native),reinterpret_cast<void*>(&HookId),name);
    if(!std::strcmp(name,"PartDataAddDesignPreset")) return Bind(g_add,reinterpret_cast<AddFn>(native),reinterpret_cast<void*>(&HookAdd),name);
    if(!std::strcmp(name,"PartDataRemoveDesignPreset")) return Bind(g_remove,reinterpret_cast<RemoveFn>(native),reinterpret_cast<void*>(&HookRemove),name);
    return native;
}
std::string StatusText() {
    if(!Running()) return "Off";
    if(!g_cacheHealthy.load(std::memory_order_acquire))
        return "Self-disabled: metadata mismatch; native pass-through (restart or toggle off/on to retest)";
    if(!g_count.load(std::memory_order_acquire) && !g_id.load(std::memory_order_acquire))
        return "Waiting for CAS preset lookups";
    size_t counts=0,ids=0;
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        counts=g_counts.size(); ids=g_ids.size();
    }
    return std::format("count={} id={} repeats={} native={:.2f}ms; cache: {} counts / {} IDs; mismatches={}",
        g_countCalls.load(),g_idCalls.load(),g_hits.load(),
        static_cast<double>(g_nativeNanoseconds.load()) / 1'000'000.0,
        counts,ids,g_validationFailures.load());
}
} // namespace FastCasCatalog
