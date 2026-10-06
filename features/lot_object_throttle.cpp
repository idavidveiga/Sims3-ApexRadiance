#include "lot_object_throttle.h"
#include "apex_log.h"
#include "entry_chain.h"
#include "game_addresses.h"
#include "game_version.h"
#include "s3ss_detect.h"
#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

constexpr size_t kOffObjBegin = 0x14;
constexpr size_t kOffObjEnd = 0x18;
constexpr size_t kOffDetailedViewRequested = 0xC1;
constexpr size_t kOffBulldozing = 0xC9;
constexpr int kScopeMsgBegin = 0x04C55E8C;
constexpr int kScopeMsgEnd = 0x04C55EFA;

using AddLotObjectsToScene_t = void(__thiscall*)(void* lot, char initialLoad, char alwaysVisibleOnly);
using UpdateObjectSceneNode_t = void(__thiscall*)(void* lot, void* obj, char initialLoad, char alwaysVisibleOnly);
using ScriptMessageScopeCtor_t = void*(__thiscall*)(void* scope, int beginMsg, int endMsg, void* lot);
using ScriptMessageScopeDtor_t = void(__fastcall*)(void* scope);
using PostRemoteMethodCall_t = char(__cdecl*)(int thread, void* lot, void* func, int a4, char initialLoad, char alwaysVisibleOnly);
using IsObjectLargeOrFlora_t = int(__cdecl*)(void* obj);

std::atomic<bool> g_running{false};
std::atomic<bool> g_externalOwner{false};
std::atomic<int> g_objectsPerWindow{LotObjectThrottle::kDefaultObjectsPerWindow};
std::atomic<int> g_delayMs{LotObjectThrottle::kDefaultDelayMs};

uintptr_t g_addLotObjectsEntry = 0;
UpdateObjectSceneNode_t g_updateObjectSceneNode = nullptr;
ScriptMessageScopeCtor_t g_scopeCtor = nullptr;
ScriptMessageScopeDtor_t g_scopeDtor = nullptr;
PostRemoteMethodCall_t g_postRemoteMethodCall = nullptr;
IsObjectLargeOrFlora_t g_isObjectLargeOrFlora = nullptr;

struct LotState {
    size_t next = 0;
    char startedDetailed = 0;
    char initialLoad = 0;
    char alwaysVisibleOnly = 0;
    bool needsPost = false;
    uint64_t lastWorkTick = 0;
    void* objBegin = nullptr;
};

std::unordered_map<void*, LotState> g_state;
std::mutex g_stateMtx;
std::mutex g_ctrl;

std::atomic<uint64_t> g_calls{0};
std::atomic<uint64_t> g_windows{0};
std::atomic<uint64_t> g_regularBuilt{0};
std::atomic<uint64_t> g_largeBuilt{0};
std::atomic<uint64_t> g_posts{0};
std::atomic<uint64_t> g_completed{0};
std::atomic<uint64_t> g_cancelled{0};
std::atomic<uint32_t> g_peakPendingLots{0};

bool S3SSOwnsObjectThrottle() {
    const S3SSDetect::Info info = S3SSDetect::Scan();
    if (!info.s3ssLoaded) return false;
    return S3SSDetect::S3SSPatchBoolSettingEnabled("LotStreamingOptimizations", "objectThrottle", true);
}

void EraseState(void* lot) {
    std::lock_guard<std::mutex> lk(g_stateMtx);
    g_state.erase(lot);
}

#pragma warning(push)
#pragma warning(disable : 4733)
bool ProbeLot(void* lot) {
    if (!lot) return false;
    __try {
        volatile uint8_t bulldozing = *(reinterpret_cast<volatile uint8_t*>(lot) + kOffBulldozing);
        volatile uint8_t detailed = *(reinterpret_cast<volatile uint8_t*>(lot) + kOffDetailedViewRequested);
        (void)bulldozing;
        (void)detailed;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
#pragma warning(pop)

void __fastcall Hook_AddLotObjectsToScene(void* lot, void*, char initialLoad, char alwaysVisibleOnly) {
    const auto next = reinterpret_cast<AddLotObjectsToScene_t>(
        EntryChain::Next(EntryChain::Site::LotAddObjectsToScene, EntryChain::Layer::LotObjectThrottle));

    if (!g_running.load(std::memory_order_acquire) || g_externalOwner.load(std::memory_order_acquire) || !lot) {
        next(lot, initialLoad, alwaysVisibleOnly);
        return;
    }

    g_calls.fetch_add(1, std::memory_order_relaxed);
    uint8_t* L = static_cast<uint8_t*>(lot);

    if (L[kOffBulldozing] != 0) {
        L[kOffDetailedViewRequested] = 0;
        EraseState(lot);
        g_cancelled.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    void** begin = *reinterpret_cast<void***>(L + kOffObjBegin);
    void** end = *reinterpret_cast<void***>(L + kOffObjEnd);
    const size_t count = (begin && end && end >= begin) ? static_cast<size_t>(end - begin) : 0;

    const char detailed = static_cast<char>(L[kOffDetailedViewRequested]);
    size_t idx = 0;
    char startedDetailed = detailed;
    {
        std::lock_guard<std::mutex> lk(g_stateMtx);
        const auto it = g_state.find(lot);
        if (it != g_state.end() && it->second.objBegin == reinterpret_cast<void*>(begin)) {
            idx = it->second.next;
            startedDetailed = it->second.startedDetailed;
        }
    }

    if (idx != 0 && detailed != startedDetailed) {
        EraseState(lot);
        g_cancelled.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (idx >= count) {
        EraseState(lot);
        return;
    }

    void* scope[2] = {nullptr, nullptr};
    g_scopeCtor(scope, kScopeMsgBegin, kScopeMsgEnd, lot);

    // First window: apartment/building shells, exterior geometry and outdoor flora must already be present before
    // SetActiveImpl's one-shot post-add fixups run. Only regular objects are throttled.
    if (idx == 0) {
        for (size_t i = 0; i < count; ++i) {
            if (begin[i] && g_isObjectLargeOrFlora(begin[i]) != 0) {
                g_updateObjectSceneNode(lot, begin[i], initialLoad, alwaysVisibleOnly);
                g_largeBuilt.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }

    const int quota = std::clamp(g_objectsPerWindow.load(std::memory_order_relaxed), 1, 256);
    size_t pos = idx;
    int built = 0;
    for (; pos < count && built < quota; ++pos) {
        if (begin[pos] && g_isObjectLargeOrFlora(begin[pos]) != 0) continue;
        g_updateObjectSceneNode(lot, begin[pos], initialLoad, alwaysVisibleOnly);
        ++built;
    }
    g_regularBuilt.fetch_add(static_cast<uint64_t>(built), std::memory_order_relaxed);

    while (pos < count && begin[pos] && g_isObjectLargeOrFlora(begin[pos]) != 0) ++pos;
    g_scopeDtor(scope);
    g_windows.fetch_add(1, std::memory_order_relaxed);

    if (pos < count) {
        uint32_t pending = 0;
        {
            std::lock_guard<std::mutex> lk(g_stateMtx);
            auto& st = g_state[lot];
            st.next = pos;
            st.startedDetailed = startedDetailed;
            st.initialLoad = initialLoad;
            st.alwaysVisibleOnly = alwaysVisibleOnly;
            st.needsPost = true;
            st.lastWorkTick = GetTickCount64();
            st.objBegin = reinterpret_cast<void*>(begin);
            pending = static_cast<uint32_t>(g_state.size());
        }
        uint32_t peak = g_peakPendingLots.load(std::memory_order_relaxed);
        while (pending > peak && !g_peakPendingLots.compare_exchange_weak(peak, pending, std::memory_order_relaxed)) {}
    } else {
        EraseState(lot);
        g_completed.fetch_add(1, std::memory_order_relaxed);
    }
}

void ResetPointers() {
    g_addLotObjectsEntry = 0;
    g_updateObjectSceneNode = nullptr;
    g_scopeCtor = nullptr;
    g_scopeDtor = nullptr;
    g_postRemoteMethodCall = nullptr;
    g_isObjectLargeOrFlora = nullptr;
}

} // namespace

namespace LotObjectThrottle {

bool Start(std::string* error) {
    std::lock_guard<std::mutex> guard(g_ctrl);
    if (g_running.load(std::memory_order_acquire)) return true;

    LOG_INFO(std::format("[LotObjectThrottle] Starting on {} ({} objects/window, {} ms delay)", GetGameVersionName(), ObjectsPerWindow(), DelayMs()));

    if (S3SSOwnsObjectThrottle()) {
        g_externalOwner.store(true, std::memory_order_release);
        g_running.store(true, std::memory_order_release);
        LOG_INFO("[LotObjectThrottle] Official Sims3SettingsSetter owns LotStreamingOptimizations.objectThrottle; Apex makes no object-throttle writes");
        return true;
    }

    std::string missing;
    if (!GameAddr::GroupAvailable("LotObjectThrottle", &missing)) {
        if (error) *error = GameAddr::NotAvailable(missing);
        return false;
    }

    g_addLotObjectsEntry = GameAddr::Get(GameAddr::Id::LotAddObjectsToScene);
    g_updateObjectSceneNode = reinterpret_cast<UpdateObjectSceneNode_t>(GameAddr::Get(GameAddr::Id::LotUpdateObjectSceneNode));
    g_scopeCtor = reinterpret_cast<ScriptMessageScopeCtor_t>(GameAddr::Get(GameAddr::Id::ScriptMessageScopeCtor));
    g_scopeDtor = reinterpret_cast<ScriptMessageScopeDtor_t>(GameAddr::Get(GameAddr::Id::ScriptMessageScopeDtor));
    g_postRemoteMethodCall = reinterpret_cast<PostRemoteMethodCall_t>(GameAddr::Get(GameAddr::Id::PostRemoteMethodCall));
    g_isObjectLargeOrFlora = reinterpret_cast<IsObjectLargeOrFlora_t>(GameAddr::Get(GameAddr::Id::IsObjectLargeOrFlora));

    {
        std::lock_guard<std::mutex> lk(g_stateMtx);
        g_state.clear();
    }
    g_externalOwner.store(false, std::memory_order_release);

    std::string hookError;
    if (!EntryChain::Install(EntryChain::Site::LotAddObjectsToScene, EntryChain::Layer::LotObjectThrottle,
                             reinterpret_cast<void*>(&Hook_AddLotObjectsToScene), &hookError)) {
        ResetPointers();
        if (error) *error = "Could not hook Lot::AddLotObjectsToScene: " + hookError;
        return false;
    }

    g_running.store(true, std::memory_order_release);
    LOG_INFO(std::format("[LotObjectThrottle] Active at {:#010x}; shells/flora stay synchronous", g_addLotObjectsEntry));
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> guard(g_ctrl);
    if (!g_running.load(std::memory_order_acquire)) return;

    if (g_externalOwner.load(std::memory_order_acquire)) {
        LOG_INFO("[LotObjectThrottle] Off in Apex; Sims3SettingsSetter remains the owner");
    } else {
        g_running.store(false, std::memory_order_release);
        EntryChain::Remove(EntryChain::Site::LotAddObjectsToScene, EntryChain::Layer::LotObjectThrottle);

        size_t forgotten = 0;
        {
            std::lock_guard<std::mutex> lk(g_stateMtx);
            forgotten = g_state.size();
            g_state.clear();
        }
        const uint64_t calls = g_calls.load(std::memory_order_relaxed);
        const uint64_t windows = g_windows.load(std::memory_order_relaxed);
        const uint64_t regular = g_regularBuilt.load(std::memory_order_relaxed);
        const uint64_t large = g_largeBuilt.load(std::memory_order_relaxed);
        const uint64_t posts = g_posts.load(std::memory_order_relaxed);
        LOG_INFO(std::format("[LotObjectThrottle] Stopped ({} calls, {} windows, {} regular objects, {} shells/flora, {} continuations posted; {} pending lot state(s) forgotten)",
                             calls, windows, regular, large, posts, forgotten));
        ResetPointers();
    }

    g_externalOwner.store(false, std::memory_order_release);
    g_running.store(false, std::memory_order_release);
}

void Tick() {
    // copies: Stop on another thread may clear the pointers while the posts below run (06/10 review)
    const auto post = g_postRemoteMethodCall;
    const auto entry = g_addLotObjectsEntry;
    if (!g_running.load(std::memory_order_acquire) || g_externalOwner.load(std::memory_order_acquire) || !entry || !post) return;

    struct Post {
        void* lot;
        char initialLoad;
        char alwaysVisibleOnly;
    };
    std::vector<Post> posts;

    const uint64_t now = GetTickCount64();
    const uint64_t delay = static_cast<uint64_t>(std::max(0, g_delayMs.load(std::memory_order_relaxed)));
    {
        std::lock_guard<std::mutex> lk(g_stateMtx);
        posts.reserve(g_state.size());
        for (auto& [lot, st] : g_state) {
            if (st.needsPost && (now - st.lastWorkTick) >= delay) {
                st.needsPost = false;
                posts.push_back({lot, st.initialLoad, st.alwaysVisibleOnly});
            }
        }
    }

    for (const Post& p : posts) {
        if (!ProbeLot(p.lot)) {
            EraseState(p.lot);
            g_cancelled.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        if (!g_running.load(std::memory_order_acquire)) return;
        post(1, p.lot, reinterpret_cast<void*>(entry), 0, p.initialLoad, p.alwaysVisibleOnly);
        g_posts.fetch_add(1, std::memory_order_relaxed);
    }
}

bool Running() { return g_running.load(std::memory_order_acquire); }
bool HandledByS3SS() { return Running() && g_externalOwner.load(std::memory_order_acquire); }

void SetObjectsPerWindow(int value) { g_objectsPerWindow.store(std::clamp(value, 1, 64), std::memory_order_relaxed); }
void SetDelayMs(int value) { g_delayMs.store(std::clamp(value, 0, 500), std::memory_order_relaxed); }
int ObjectsPerWindow() { return g_objectsPerWindow.load(std::memory_order_relaxed); }
int DelayMs() { return g_delayMs.load(std::memory_order_relaxed); }

std::string StatusText() {
    if (!Running()) return "Off";
    if (HandledByS3SS()) return "Handled by Sims3SettingsSetter";
    size_t pending = 0;
    {
        std::lock_guard<std::mutex> lk(g_stateMtx);
        pending = g_state.size();
    }
    return std::format("On: {} objects/window, {} ms delay; {} lot(s) pending", ObjectsPerWindow(), DelayMs(), pending);
}

} // namespace LotObjectThrottle
