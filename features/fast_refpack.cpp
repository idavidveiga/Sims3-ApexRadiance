// Faster cache compression: the RefPack stream write answered by a fast compressor with the game's stream format (see
// fast_refpack.h, features/refpack_codec.h and docs/features/performance.md).
//
// Part of Apex Radiance. Credits: @loinyx
#include "fast_refpack.h"
#include "refpack_codec.h"
#include "slot_chain.h"
#include "game_addresses.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "imgui.h"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>

namespace FastRefPack {
namespace {

using SlotChain::Layer;
using SlotChain::Site;
using FnThis5 = uint64_t(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
using FnDecompress = uint32_t(__cdecl*)(uint8_t* dst, uint32_t capacity, const uint8_t* src, uint32_t srcSize);

constexpr int kPool = 4;
constexpr uint32_t kStartupChecks = 16;
constexpr uint32_t kKeepScratch = 1u << 20; // check buffers up to 1 MB stay allocated per context
constexpr uint32_t kNiceLen = 96;
constexpr uint64_t kPairGraceMs = 2000;

struct Slot {
    std::atomic<bool> busy{false};
    void* mem = nullptr; // RefPackCodec::ContextBytes(), allocated on first use, never freed
    RefPackCodec::Context ctx;
    uint8_t* scratch = nullptr;
    uint32_t scratchSize = 0;
};
Slot g_pool[kPool];

// The last counting run of this thread: its write must use the same compressor, flags and search depth (the buffer was
// sized by that run; the package writer counts with flags 2 and may write with other flags)
struct Pair {
    uint32_t src = 0, size = 0, flags = 0;
    int chain = 0;
    bool fast = false, valid = false;
};
thread_local Pair t_pair;

std::mutex g_ctrl;
std::atomic<bool> g_installed{false}; // the slot-chain layer (changed under g_ctrl)
std::atomic<bool> g_on{false};
std::atomic<bool> g_selfDisabled{false};
std::atomic<uint64_t> g_lastFastCountTick{0};
std::atomic<int> g_verifyEvery{kPublicBuild ? 0 : 1};
std::atomic<int> g_compareEvery{0};
std::atomic<int> g_chain{32};
std::atomic<uint32_t> g_seq{0};
FnDecompress g_decoder = nullptr; // the game's decompressor (0x004EB3B0), else Apex's translation
std::atomic<uint64_t> c_streams{0}, c_in{0}, c_out{0}, c_count{0}, c_passed{0}, c_paired{0}, c_overflow{0}, c_temp{0};
std::atomic<uint64_t> c_checked{0}, c_mismatch{0}, c_notCheckable{0}, c_compared{0}, c_cmpGame{0}, c_cmpFast{0};
std::atomic<uint64_t> g_ticks{0}, g_countTicks{0}, g_cmpGameTicks{0}, g_cmpFastTicks{0};
double g_qpcMs = 0.0;
std::mutex g_mismatchLock;
std::string g_lastMismatch;

uint64_t Qpc() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return static_cast<uint64_t>(q.QuadPart);
}

bool InitSlot(Slot& s) {
    if (s.mem) return true;
    s.mem = VirtualAlloc(nullptr, RefPackCodec::ContextBytes(), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!s.mem) return false;
    RefPackCodec::InitContext(s.ctx, s.mem);
    return true;
}

Slot* Acquire() {
    for (Slot& s : g_pool) {
        if (s.busy.exchange(true, std::memory_order_acquire)) continue;
        if (InitSlot(s)) return &s;
        s.busy.store(false, std::memory_order_release);
        return nullptr;
    }
    return nullptr;
}

// Decompresses the stream with the game's decoder and compares it with the source. False only on a real difference.
bool Check(Slot& slot, const uint8_t* stream, uint32_t streamSize, const uint8_t* src, uint32_t size) {
    uint8_t* buf = nullptr;
    bool own = false;
    if (size <= kKeepScratch) {
        if (!slot.scratch || slot.scratchSize < size) {
            if (slot.scratch) VirtualFree(slot.scratch, 0, MEM_RELEASE);
            const uint32_t want = size < 0x10000 ? 0x10000 : ((size + 0xFFFF) & ~0xFFFFu);
            slot.scratch = static_cast<uint8_t*>(VirtualAlloc(nullptr, want, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
            slot.scratchSize = slot.scratch ? want : 0;
        }
        buf = slot.scratch;
    } else {
        buf = static_cast<uint8_t*>(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        own = true;
    }
    if (!buf) {
        c_notCheckable.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    const uint32_t got = g_decoder ? g_decoder(buf, size, stream, streamSize) : RefPackCodec::Decompress(buf, size, stream, streamSize);
    bool ok = got == size;
    uint32_t at = 0;
    if (ok && std::memcmp(buf, src, size) != 0) {
        ok = false;
        while (at < size && buf[at] == src[at]) at++;
    }
    if (own) VirtualFree(buf, 0, MEM_RELEASE);
    if (!ok) {
        const std::string msg = std::format("{} bytes -> {}-byte stream (header {:02x}{:02x}); the {} decoder returned {}{}", size, streamSize, static_cast<unsigned>(stream[0]), static_cast<unsigned>(stream[1]),
                                            g_decoder ? "game's" : "Apex copy of the", got, got == size ? std::format(", first different byte at {}", at) : std::string());
        std::lock_guard<std::mutex> lock(g_mismatchLock);
        if (g_lastMismatch.empty()) LOG_ERROR("[FastRefPack] Verification mismatch: " + msg + ". The game's compressor was used; the feature turns itself off for this session.");
        g_lastMismatch = msg;
    }
    return ok;
}

// The stream write (layer FastCompress of the slot chain): thiscall(src, size, dst, capacity, flags), ret 14h
uint64_t __fastcall Hook_StreamWrite(void* self, void* edx, uint32_t src, uint32_t size, uint32_t dst, uint32_t capacity, uint32_t flags) {
    const FnThis5 next = reinterpret_cast<FnThis5>(SlotChain::Next(Site::RefPackCompress, Layer::FastCompress));
    if (!dst && (flags & 1)) return next(self, edx, src, size, dst, capacity, flags); // the size bound: no work
    const bool counting = dst == 0;
    Pair& pr = t_pair;
    bool fast;
    uint32_t useFlags = flags;
    int chain = g_chain.load(std::memory_order_relaxed);
    if (!counting && pr.valid && pr.src == src && pr.size == size) { // the write of a stream this thread just counted
        fast = pr.fast;
        pr.valid = false;
        if (fast) {
            useFlags = pr.flags; // same flags and depth as the counting run: the stream has exactly the counted size
            chain = pr.chain;
            c_paired.fetch_add(1, std::memory_order_relaxed);
        }
    } else {
        fast = g_on.load(std::memory_order_acquire) && !g_selfDisabled.load(std::memory_order_relaxed);
    }
    if (!fast) {
        if (counting) pr = Pair{src, size, flags, 0, false, true};
        c_passed.fetch_add(1, std::memory_order_relaxed);
        return next(self, edx, src, size, dst, capacity, flags);
    }
    Slot temp;
    Slot* slot = Acquire();
    if (!slot) { // all pooled contexts busy: a temporary one (rare)
        if (!InitSlot(temp)) {
            if (counting || capacity == 0 || capacity >= RefPackCodec::SizeBound(size)) {
                if (counting) pr = Pair{src, size, flags, 0, false, true};
                c_passed.fetch_add(1, std::memory_order_relaxed);
                return next(self, edx, src, size, dst, capacity, flags);
            }
            return RefPackCodec::kFailed;
        }
        c_temp.fetch_add(1, std::memory_order_relaxed);
        slot = &temp;
    }
    auto finish = [&] {
        if (slot == &temp) {
            VirtualFree(temp.mem, 0, MEM_RELEASE);
            if (temp.scratch) VirtualFree(temp.scratch, 0, MEM_RELEASE);
        } else {
            slot->busy.store(false, std::memory_order_release);
        }
    };
    RefPackCodec::Effort effort;
    effort.maxChain = chain;
    effort.niceLen = kNiceLen;
    effort.lazy = true;
    const uint64_t t0 = Qpc();
    const uint32_t r = RefPackCodec::Compress(slot->ctx, reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(src)), size, reinterpret_cast<uint8_t*>(static_cast<uintptr_t>(dst)),
                                              capacity, useFlags, effort);
    const uint64_t t1 = Qpc();
    if (counting) {
        pr = Pair{src, size, flags, chain, true, true};
        g_lastFastCountTick.store(GetTickCount64(), std::memory_order_relaxed);
        c_count.fetch_add(1, std::memory_order_relaxed);
        g_countTicks.fetch_add(t1 - t0, std::memory_order_relaxed);
        finish();
        return r;
    }
    if (r == RefPackCodec::kFailed) { // does not fit: the callers store the data uncompressed
        c_overflow.fetch_add(1, std::memory_order_relaxed);
        finish();
        return RefPackCodec::kFailed;
    }
    c_streams.fetch_add(1, std::memory_order_relaxed);
    c_in.fetch_add(size, std::memory_order_relaxed);
    c_out.fetch_add(r, std::memory_order_relaxed);
    g_ticks.fetch_add(t1 - t0, std::memory_order_relaxed);
    const uint32_t seq = g_seq.fetch_add(1, std::memory_order_relaxed);
    const int ve = g_verifyEvery.load(std::memory_order_relaxed);
    if (seq < kStartupChecks || (ve > 0 && seq % static_cast<uint32_t>(ve) == 0)) {
        const uint8_t* s = reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(src));
        if (!Check(*slot, reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(dst)), r, s, size)) {
            c_mismatch.fetch_add(1, std::memory_order_relaxed);
            g_selfDisabled.store(true);
            finish();
            if (capacity == 0 || capacity >= RefPackCodec::SizeBound(size)) return next(self, edx, src, size, dst, capacity, flags);
            return RefPackCodec::kFailed;
        }
        c_checked.fetch_add(1, std::memory_order_relaxed);
    }
    finish();
    const int ce = g_compareEvery.load(std::memory_order_relaxed);
    if (!kPublicBuild && ce > 0 && seq % static_cast<uint32_t>(ce) == 0) { // the game's compressor on the same data, counting only
        const uint64_t g0 = Qpc();
        const uint32_t gs = static_cast<uint32_t>(next(self, edx, src, size, 0, 0, useFlags & ~1u)); // same flags as ours; bit 0 off = compress, not the size bound
        const uint64_t g1 = Qpc();
        c_compared.fetch_add(1, std::memory_order_relaxed);
        c_cmpGame.fetch_add(gs, std::memory_order_relaxed);
        c_cmpFast.fetch_add(r, std::memory_order_relaxed);
        g_cmpGameTicks.fetch_add(g1 - g0, std::memory_order_relaxed);
        g_cmpFastTicks.fetch_add(t1 - t0, std::memory_order_relaxed);
    }
    return r;
}

void RemoveLayerLocked() {
    if (!g_installed.load()) return;
    SlotChain::Remove(Site::RefPackCompress, Layer::FastCompress);
    g_installed.store(false);
}

} // namespace

bool Start(std::string* error) {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (g_on.load()) return true;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    std::string missing;
    if (!GameAddr::GroupAvailable("FastCacheCompression", &missing)) return fail(GameAddr::NotAvailable(missing));
    if (g_qpcMs == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcMs = 1000.0 / static_cast<double>(f.QuadPart);
    }
    g_decoder = reinterpret_cast<FnDecompress>(GameAddr::Get(GameAddr::Id::RefPackDecompress));
    if (!g_installed.load()) {
        std::string err;
        if (!SlotChain::Install(Site::RefPackCompress, Layer::FastCompress, reinterpret_cast<void*>(&Hook_StreamWrite), &err)) return fail("Could not hook the RefPack stream write: " + err);
        g_installed.store(true);
    }
    g_selfDisabled.store(false);
    g_seq.store(0); // the startup checks run again after every start
    g_on.store(true, std::memory_order_release);
    LOG_INFO(std::format("[FastRefPack] On: the RefPack stream write {:#010x} answered through its vtable slot; streams checked with {} (the first {} always)",
                         SlotChain::GameFunction(Site::RefPackCompress), g_decoder ? std::format("the game's decoder {:#010x}", reinterpret_cast<uintptr_t>(g_decoder)) : std::string("Apex's copy of the decoder"),
                         kStartupChecks));
    return true;
}

void Stop() {
    {
        std::lock_guard<std::mutex> lock(g_ctrl);
        if (!g_on.load()) return;
        g_on.store(false, std::memory_order_release); // new streams go to the game's compressor from now on
        if (GetTickCount64() - g_lastFastCountTick.load() > kPairGraceMs) RemoveLayerLocked();
    }
    const Stats s = GetStats();
    LOG_INFO(std::format("[FastRefPack] Off ({} streams, {} -> {} bytes, {} checked, {} different, {} did not fit){}", s.streams, s.bytesIn, s.bytesOut, s.checked, s.mismatches,
                         s.overflows, s.installed ? "; the layer is removed once no counted stream can still be waiting for its write" : ""));
}

bool Running() { return g_on.load(std::memory_order_acquire); }

void Tick() {
    if (g_on.load(std::memory_order_relaxed)) return;
    if (GetTickCount64() - g_lastFastCountTick.load(std::memory_order_relaxed) <= kPairGraceMs) return;
    if (!g_installed.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (!g_on.load() && g_installed.load()) {
        RemoveLayerLocked();
        LOG_INFO("[FastRefPack] Layer removed");
    }
}

void SetVerifyEvery(int n) { g_verifyEvery.store(n < 0 ? 0 : n); }
int VerifyEvery() { return g_verifyEvery.load(); }
void SetCompareEvery(int n) { g_compareEvery.store(n < 0 ? 0 : n); }
int CompareEvery() { return g_compareEvery.load(); }
void SetChainDepth(int n) { g_chain.store(n < 4 ? 4 : (n > 256 ? 256 : n)); }
int ChainDepth() { return g_chain.load(); }

Stats GetStats() {
    Stats s;
    s.streams = c_streams.load();
    s.bytesIn = c_in.load();
    s.bytesOut = c_out.load();
    s.countingRuns = c_count.load();
    s.ms = static_cast<double>(g_ticks.load()) * g_qpcMs;
    s.countingMs = static_cast<double>(g_countTicks.load()) * g_qpcMs;
    s.passedThrough = c_passed.load();
    s.paired = c_paired.load();
    s.overflows = c_overflow.load();
    s.tempContexts = c_temp.load();
    s.checked = c_checked.load();
    s.mismatches = c_mismatch.load();
    s.notCheckable = c_notCheckable.load();
    s.compared = c_compared.load();
    s.comparedGameBytes = c_cmpGame.load();
    s.comparedFastBytes = c_cmpFast.load();
    s.comparedGameMs = static_cast<double>(g_cmpGameTicks.load()) * g_qpcMs;
    s.comparedFastMs = static_cast<double>(g_cmpFastTicks.load()) * g_qpcMs;
    s.selfDisabled = g_selfDisabled.load();
    s.gameDecoder = g_decoder != nullptr;
    s.installed = g_installed.load();
    std::lock_guard<std::mutex> lock(g_mismatchLock);
    s.lastMismatch = g_lastMismatch;
    return s;
}

std::string StatusText() {
    if (!Running()) return "Off";
    const Stats s = GetStats();
    if (s.selfDisabled) return "Turned itself off: a check found a stream the game could not read back (see ApexRadiance_LOG.txt)";
    if (!s.streams) return "On (nothing compressed yet)";
    std::string t = std::format("On: {} streams, {:.1f} MB -> {:.1f} MB ({:.0f}%) in {:.0f} ms; {} checked by decompressing, all equal", s.streams, static_cast<double>(s.bytesIn) / 1048576.0,
                                static_cast<double>(s.bytesOut) / 1048576.0, s.bytesIn ? 100.0 * static_cast<double>(s.bytesOut) / static_cast<double>(s.bytesIn) : 0.0, s.ms,
                                s.checked);
    if (s.compared && s.comparedGameBytes && s.comparedFastMs > 0.0)
        t += std::format("; vs the game on {}: size {:+.1f}%, {:.1f}x faster", s.compared,
                         100.0 * (static_cast<double>(s.comparedFastBytes) / static_cast<double>(s.comparedGameBytes) - 1.0), s.comparedGameMs / s.comparedFastMs);
    return t;
}

void RenderDeveloperUI() {
    if constexpr (kPublicBuild) return;
    if (!ImGui::GetCurrentContext()) return;
    const Stats s = GetStats();
    ImGui::TextUnformatted(("Faster cache compression: " + StatusText()).c_str());
    ImGui::TextDisabled("Streams %llu (%.1f ms), counting runs %llu (%.1f ms), writes after our counting run %llu; game's compressor %llu; did not fit (-1) %llu; temporary contexts %llu",
                        static_cast<unsigned long long>(s.streams), s.ms, static_cast<unsigned long long>(s.countingRuns), s.countingMs, static_cast<unsigned long long>(s.paired),
                        static_cast<unsigned long long>(s.passedThrough), static_cast<unsigned long long>(s.overflows), static_cast<unsigned long long>(s.tempContexts));
    ImGui::TextDisabled("Checks (%s): %llu equal, %llu different, %llu not checked (no memory); layer %s", s.gameDecoder ? "game's decoder" : "Apex's copy of the decoder",
                        static_cast<unsigned long long>(s.checked), static_cast<unsigned long long>(s.mismatches), static_cast<unsigned long long>(s.notCheckable),
                        s.installed ? "installed" : "not installed");
    if (s.compared)
        ImGui::TextDisabled("Compared with the game's compressor on %llu streams: game %llu bytes in %.1f ms, Apex %llu bytes in %.1f ms", static_cast<unsigned long long>(s.compared),
                            static_cast<unsigned long long>(s.comparedGameBytes), s.comparedGameMs, static_cast<unsigned long long>(s.comparedFastBytes), s.comparedFastMs);
    int every = g_verifyEvery.load();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderInt("Check 1 stream in N by decompressing##FrVerify", &every, 0, 64)) SetVerifyEvery(every);
    int cmp = g_compareEvery.load();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderInt("Also run the game's compressor on 1 stream in N (0 = never; adds its time)##FrCompare", &cmp, 0, 64)) SetCompareEvery(cmp);
    int chain = g_chain.load();
    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderInt("Search depth (candidates per position)##FrChain", &chain, 4, 256)) SetChainDepth(chain);
    if (!s.lastMismatch.empty()) ImGui::TextColored(ImVec4(0.91f, 0.44f, 0.42f, 1.0f), "Last difference: %s", s.lastMismatch.c_str());
}

} // namespace FastRefPack
