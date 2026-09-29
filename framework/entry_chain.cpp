// Layered hooks on game function entries (see entry_chain.h).
#include "entry_chain.h"
#include "apex_log.h"
#include "game_addresses.h"
#include "memory_patch.h"
#include <windows.h>
#include <cstring>
#include <format>
#include <mutex>

namespace EntryChain {

std::atomic<void*> g_next[static_cast<int>(Site::Count)][static_cast<int>(Layer::Count)] = {};

namespace {

constexpr int kSites = static_cast<int>(Site::Count);
constexpr int kLayers = static_cast<int>(Layer::Count);
constexpr int kJmpLen = 5;
constexpr int kMaxPrologue = 8;

struct SiteInfo {
    const char* name;
    GameAddr::Id fn;
    uint8_t prologue[kMaxPrologue]; // the instructions moved to the trampoline (relocation-free, whole instructions)
    int len;
};
// Both encoders start with push ebp; mov ebp,esp; and esp,0FFFFFFF0h. No branch in .text lands on bytes 1..5 of either
// entry (checked in full.asm for the Frame Profiler's hand-made hooks of the same entries).
const SiteInfo kSiteInfo[kSites] = {
    {"DXT1 encoder", GameAddr::Id::DxtEncode1, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0}, 6},
    {"DXT5 encoder", GameAddr::Id::DxtEncode5, {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0}, 6},
};

struct SiteState {
    uintptr_t fn = 0;
    uint8_t* tramp = nullptr;
    uint8_t orig[kJmpLen] = {};
    void* hook[kLayers] = {}; // installed layers (nullptr = not installed)
};
SiteState g_state[kSites]; // guarded by g_mutex
std::mutex g_mutex;

uint8_t* g_trampPool = nullptr; // one RWX page, never freed
size_t g_trampUsed = 0;
constexpr size_t kTrampPool = 4096, kTrampSlot = 32;

void MakeJmp(uint8_t out[kJmpLen], uintptr_t from, uintptr_t to) {
    out[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(to - (from + kJmpLen));
    std::memcpy(out + 1, &rel, 4);
}

// The pointer the entry JMP targets now: the outermost installed layer's hook, or 0 (no layer: original bytes)
uintptr_t Outermost(const SiteState& s) {
    for (int l = 0; l < kLayers; l++)
        if (s.hook[l]) return reinterpret_cast<uintptr_t>(s.hook[l]);
    return 0;
}

// The next inner installed layer's hook after `layer`, else the trampoline
uintptr_t InnerOf(const SiteState& s, int layer) {
    for (int l = layer + 1; l < kLayers; l++)
        if (s.hook[l]) return reinterpret_cast<uintptr_t>(s.hook[l]);
    return reinterpret_cast<uintptr_t>(s.tramp);
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
    if (!fn) {
        if (error) *error = GameAddr::NotAvailable(GameAddr::Name(info.fn));
        return false;
    }
    uint8_t cur[kMaxPrologue] = {};
    if (!MemPatch::ReadBytes(fn, cur, static_cast<size_t>(info.len))) {
        if (error) *error = std::format("the entry {:#010x} is not readable", fn);
        return false;
    }
    if (std::memcmp(cur, info.prologue, static_cast<size_t>(info.len)) != 0) {
        if (error) *error = std::format("the entry bytes at {:#010x} changed (hooked by another module?)", fn);
        return false;
    }
    if (!g_trampPool) g_trampPool = static_cast<uint8_t*>(VirtualAlloc(nullptr, kTrampPool, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_trampPool || g_trampUsed + kTrampSlot > kTrampPool) {
        if (error) *error = "no memory for the trampoline";
        return false;
    }
    uint8_t* t = g_trampPool + g_trampUsed;
    g_trampUsed += kTrampSlot;
    std::memcpy(t, cur, static_cast<size_t>(info.len));
    MakeJmp(t + info.len, reinterpret_cast<uintptr_t>(t + info.len), fn + static_cast<uintptr_t>(info.len));
    FlushInstructionCache(GetCurrentProcess(), t, kTrampSlot);
    s.tramp = t;
    std::memcpy(s.orig, cur, kJmpLen);
    s.fn = fn;
    return true;
}

// Writes `bytes` over the entry if it still holds `expect` (every other thread suspended, none inside the bytes)
bool WriteEntry(const SiteState& s, const uint8_t bytes[kJmpLen], const uint8_t expect[kJmpLen], std::string* error) {
    uint8_t cur[kJmpLen] = {};
    if (!MemPatch::ReadBytes(s.fn, cur, kJmpLen) || std::memcmp(cur, expect, kJmpLen) != 0) {
        if (error) *error = std::format("the entry bytes at {:#010x} were changed by another module", s.fn);
        return false;
    }
    if (!MemPatch::WriteCodeSuspended(s.fn, bytes, kJmpLen)) {
        if (error) *error = std::format("could not write the entry at {:#010x} (a thread kept running its first instructions)", s.fn);
        return false;
    }
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
    const uintptr_t inner = InnerOf(s, li);
    g_next[si][li].store(reinterpret_cast<void*>(inner), std::memory_order_release); // before anything can reach the hook
    const int o = OuterOf(s, li);
    if (o >= 0) {
        // An outer layer keeps the entry: it now calls this one
        s.hook[li] = hook;
        g_next[si][o].store(hook, std::memory_order_release);
    } else {
        uint8_t expect[kJmpLen], jmp[kJmpLen];
        const uintptr_t cur = Outermost(s);
        if (cur) MakeJmp(expect, s.fn, cur);
        else std::memcpy(expect, s.orig, kJmpLen);
        MakeJmp(jmp, s.fn, reinterpret_cast<uintptr_t>(hook));
        if (!WriteEntry(s, jmp, expect, error)) return false;
        s.hook[li] = hook;
    }
    LOG_INFO(std::format("[EntryChain] {} ({:#010x}): layer {} installed (next = {:#010x}{})", kSiteInfo[si].name, s.fn, li, inner,
                         o >= 0 ? ", inside an outer layer" : (inner == reinterpret_cast<uintptr_t>(s.tramp) ? ", the game's code" : "")));
    return true;
}

bool Remove(Site site, Layer layer) {
    const int si = static_cast<int>(site), li = static_cast<int>(layer);
    if (si < 0 || si >= kSites || li < 0 || li >= kLayers) return true;
    std::lock_guard<std::mutex> lock(g_mutex);
    SiteState& s = g_state[si];
    if (!s.hook[li]) return true;
    const uintptr_t inner = InnerOf(s, li);
    const int o = OuterOf(s, li);
    if (o >= 0) {
        g_next[si][o].store(reinterpret_cast<void*>(inner), std::memory_order_release);
    } else {
        uint8_t expect[kJmpLen], bytes[kJmpLen];
        MakeJmp(expect, s.fn, reinterpret_cast<uintptr_t>(s.hook[li]));
        if (inner != reinterpret_cast<uintptr_t>(s.tramp)) MakeJmp(bytes, s.fn, inner);
        else std::memcpy(bytes, s.orig, kJmpLen);
        std::string error;
        if (!WriteEntry(s, bytes, expect, &error)) {
            uint8_t cur[kJmpLen] = {};
            if (MemPatch::ReadBytes(s.fn, cur, kJmpLen) && std::memcmp(cur, expect, kJmpLen) != 0) {
                // changed by someone else after us: left as it is (it may still reach the removed layer, which keeps forwarding)
                LOG_WARNING(std::format("[EntryChain] {}: {}; left as it is", kSiteInfo[si].name, error));
            } else {
                LOG_ERROR(std::format("[EntryChain] {}: layer {} not removed: {}", kSiteInfo[si].name, li, error));
                return false;
            }
        }
    }
    s.hook[li] = nullptr; // g_next[si][li] keeps its value for threads still inside the removed hook
    LOG_INFO(std::format("[EntryChain] {}: layer {} removed", kSiteInfo[si].name, li));
    return true;
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

void* Original(Site site) {
    const int si = static_cast<int>(site);
    if (si < 0 || si >= kSites) return nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state[si].tramp;
}

} // namespace EntryChain
