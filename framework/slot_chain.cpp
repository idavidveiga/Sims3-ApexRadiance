// Layered vtable-slot hooks (see slot_chain.h).
#include "slot_chain.h"
#include "apex_log.h"
#include "game_addresses.h"
#include "memory_patch.h"
#include <windows.h>
#include <intrin.h>
#include <format>
#include <iterator>
#include <mutex>

namespace SlotChain {

std::atomic<void*> g_next[static_cast<int>(Site::Count)][static_cast<int>(Layer::Count)] = {};

namespace {

constexpr int kSites = static_cast<int>(Site::Count);
constexpr int kLayers = static_cast<int>(Layer::Count);
constexpr int kMaxSlots = 2;

struct SiteInfo {
    const char* name;
    GameAddr::Id fn;        // the game function the slots hold
    GameAddr::Id firstSlot; // the first of `slots` consecutive slot ids
    int slots;
};
const SiteInfo kSiteInfo[kSites] = {
    {"ResourceMgr::FindProvider", GameAddr::Id::ResFindProvider, GameAddr::Id::ResFindProviderSlot0, 2},
    {"ResourceMgr::RegisterDatabase", GameAddr::Id::ResRegisterDb, GameAddr::Id::ResRegisterDbSlot, 1},
    {"ResourceSystem::RegisterDatabase", GameAddr::Id::ResRegisterDbDerived, GameAddr::Id::ResRegisterDbDerivedSlot, 1},
    {"ResourceMgr::SetDatabasePriority", GameAddr::Id::ResSetDbPriority, GameAddr::Id::ResSetDbPrioritySlot0, 2},
    {"ResourceMgr::DatabaseChanged", GameAddr::Id::ResDbChanged, GameAddr::Id::ResDbChangedSlot0, 2},
    {"RefPack stream write", GameAddr::Id::RefPackCompress, GameAddr::Id::RefPackCompressSlot, 1},
};
static_assert(std::size(kSiteInfo) == static_cast<size_t>(Site::Count), "kSiteInfo must list every Site in order");

struct SiteState {
    uintptr_t fn = 0;
    uintptr_t slot[kMaxSlots] = {};
    int slots = 0;
    void* hook[kLayers] = {}; // installed layers (nullptr = not installed)
};
SiteState g_state[kSites]; // guarded by g_mutex
std::mutex g_mutex;

// One aligned 4-byte slot: compare-exchange while its page is writable
bool SwapSlot(uintptr_t slot, uintptr_t expect, uintptr_t value) {
    if (slot & 3) return false;
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(slot), 4, PAGE_READWRITE, &old)) return false;
    const long prev = _InterlockedCompareExchange(reinterpret_cast<volatile long*>(slot), static_cast<long>(value), static_cast<long>(expect));
    DWORD tmp = 0;
    VirtualProtect(reinterpret_cast<void*>(slot), 4, old, &tmp);
    return static_cast<uintptr_t>(static_cast<unsigned long>(prev)) == expect;
}

// The pointer the slots hold now: the outermost installed layer's hook, else the game function
uintptr_t Outermost(const SiteState& s) {
    for (int l = 0; l < kLayers; l++)
        if (s.hook[l]) return reinterpret_cast<uintptr_t>(s.hook[l]);
    return s.fn;
}

// The next inner installed layer's hook after `layer`, else the game function
uintptr_t InnerOf(const SiteState& s, int layer) {
    for (int l = layer + 1; l < kLayers; l++)
        if (s.hook[l]) return reinterpret_cast<uintptr_t>(s.hook[l]);
    return s.fn;
}

// The nearest installed layer outside `layer`, or -1
int OuterOf(const SiteState& s, int layer) {
    for (int l = layer - 1; l >= 0; l--)
        if (s.hook[l]) return l;
    return -1;
}

bool Resolve(int site, std::string* error) {
    SiteState& s = g_state[site];
    const SiteInfo& info = kSiteInfo[site];
    if (s.fn) return true;
    if (!GameAddr::Resolved()) {
        if (error) *error = "the game code was not scanned yet";
        return false;
    }
    const uintptr_t fn = GameAddr::Get(info.fn);
    uintptr_t slot[kMaxSlots] = {};
    std::string missing;
    if (!fn) missing = GameAddr::Name(info.fn);
    for (int k = 0; k < info.slots; k++) {
        const auto id = static_cast<GameAddr::Id>(static_cast<int>(info.firstSlot) + k);
        slot[k] = GameAddr::Get(id);
        if (!slot[k]) missing += (missing.empty() ? "" : ", ") + std::string(GameAddr::Name(id));
    }
    if (!missing.empty()) {
        if (error) *error = GameAddr::NotAvailable(missing);
        return false;
    }
    s.fn = fn;
    s.slots = info.slots;
    for (int k = 0; k < info.slots; k++) s.slot[k] = slot[k];
    return true;
}

} // namespace

bool Install(Site site, Layer layer, void* hook, std::string* error) {
    const int si = static_cast<int>(site), li = static_cast<int>(layer);
    if (si < 0 || si >= kSites || li < 0 || li >= kLayers || !hook) {
        if (error) *error = "bad arguments";
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    SiteState& s = g_state[si];
    if (s.hook[li] == hook) return true;
    if (s.hook[li]) {
        if (error) *error = "another hook is installed for this layer";
        return false;
    }
    if (!Resolve(si, error)) return false;
    // Every slot must hold what Apex expects (the game function or the current outermost Apex layer)
    const uintptr_t outer = Outermost(s);
    for (int k = 0; k < s.slots; k++) {
        uint32_t v = 0;
        if (!MemPatch::ReadBytes(s.slot[k], &v, 4)) {
            if (error) *error = std::format("the vtable slot {:#010x} is not readable", s.slot[k]);
            return false;
        }
        if (v != outer) {
            if (error) *error = std::format("the vtable slot {:#010x} holds {:#010x}, expected {:#010x} (replaced by another module?)", s.slot[k], v, outer);
            return false;
        }
    }
    const uintptr_t inner = InnerOf(s, li);
    g_next[si][li].store(reinterpret_cast<void*>(inner), std::memory_order_release); // before anything can reach the hook
    const int o = OuterOf(s, li);
    if (o >= 0) {
        // An outer layer stays in the slots: it now calls this one
        s.hook[li] = hook;
        g_next[si][o].store(hook, std::memory_order_release);
    } else {
        int done = 0;
        while (done < s.slots && SwapSlot(s.slot[done], outer, reinterpret_cast<uintptr_t>(hook))) done++;
        if (done < s.slots) {
            for (int k = 0; k < done; k++) SwapSlot(s.slot[k], reinterpret_cast<uintptr_t>(hook), outer);
            if (error) *error = std::format("could not write the vtable slot {:#010x}", s.slot[done]);
            return false;
        }
        s.hook[li] = hook;
    }
    LOG_INFO(std::format("[SlotChain] {}: layer {} installed ({} slot(s); next = {:#010x}{})", kSiteInfo[si].name, li, s.slots, inner, o >= 0 ? ", inside an outer layer" : ""));
    return true;
}

void Remove(Site site, Layer layer) {
    const int si = static_cast<int>(site), li = static_cast<int>(layer);
    if (si < 0 || si >= kSites || li < 0 || li >= kLayers) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    SiteState& s = g_state[si];
    if (!s.hook[li]) return;
    const uintptr_t self = reinterpret_cast<uintptr_t>(s.hook[li]);
    const uintptr_t inner = InnerOf(s, li);
    const int o = OuterOf(s, li);
    if (o >= 0) {
        g_next[si][o].store(reinterpret_cast<void*>(inner), std::memory_order_release);
    } else {
        for (int k = 0; k < s.slots; k++)
            if (!SwapSlot(s.slot[k], self, inner))
                LOG_WARNING(std::format("[SlotChain] {}: the vtable slot {:#010x} was changed by another module; left as it is (it still reaches the removed layer, "
                                        "which keeps forwarding)",
                                        kSiteInfo[si].name, s.slot[k]));
    }
    s.hook[li] = nullptr; // g_next[si][li] keeps its value for threads still inside the removed hook
    LOG_INFO(std::format("[SlotChain] {}: layer {} removed", kSiteInfo[si].name, li));
}

bool Installed(Site site, Layer layer) {
    const int si = static_cast<int>(site), li = static_cast<int>(layer);
    if (si < 0 || si >= kSites || li < 0 || li >= kLayers) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state[si].hook[li] != nullptr;
}

uintptr_t GameFunction(Site site) {
    const int si = static_cast<int>(site);
    if (si < 0 || si >= kSites) return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state[si].fn;
}

} // namespace SlotChain
